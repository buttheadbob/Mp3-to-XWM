// SPDX-License-Identifier: AGPL-3.0-or-later
/* WMA version 2 encoder for xWMA.
 *
 * xWMA files carry no codec extra data; decoders assume flags 0x1F: exponents
 * are Huffman coded, the bit reservoir is on and block lengths are signalled.
 * This encoder always uses full length blocks (one block per frame), mid/side
 * stereo, and fixed-size packets in which frames run on from one packet into
 * the next.
 *
 * The derivation of the codec parameters from the format (exponent_bands, the
 * block size, noise coding, coefficient table and byte offset choices in ctx_init,
 * total_gain_to_bits and av_log2) follows FFmpeg libavcodec/wma.c,
 * Copyright (c) 2002-2007 The FFmpeg Project, LGPL-2.1-or-later, used here under
 * the AGPL as THIRD_PARTY.md explains.
 */
#include "wma_enc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitwriter.h"
#include "mdct.h"
#include "wma_tables.h"

static constexpr double PI         = 3.14159265358979323846;
static constexpr int    FLAGS2     = 0x1F;
static constexpr int    BLOCK_MIN_BITS = 7;
static constexpr int    MAX_BANDS  = 32;
static constexpr int    MAX_N      = 2048;
static constexpr int    EXP_MIN    = -60;  /* decoder exponent table covers -60..95 */
static constexpr int    EXP_MAX    = 95;
static constexpr int    MAX_FRAMES_PER_PACKET = 13;

typedef struct {
    const wma_coef_table *t;
    uint16_t int_table[400];  /* first code index for each level */
} coef_coder;

typedef struct {
    wma_params p;
    int    frame_len_bits, n;          /* frame length N = block length */
    int    nb_block_sizes, blk_bits;   /* block size signalling */
    int    byte_offset_bits;
    bool   use_noise_coding;
    int    n_high_bands;               /* noise coding flags per channel */
    int    nbands, bands[MAX_BANDS];   /* exponent bands for the full block */
    int    coefs_end, cutoff;
    coef_coder coder[2];               /* [0] mid or mono, [1] side */
    float  window[2 * MAX_N];
    mdct_plan mdct;
    float  in[2 * MAX_N];              /* windowed MDCT input */
    int    packet_bits;                /* payload bits per packet (after header) */
    int    header_bits;
    int    frame_target, frame_max, frame_min;
} wma_ctx;

static int av_log2(unsigned v)
{
    int n = -1;
    while (v) {
        v >>= 1;
        n++;
    }
    return n < 0 ? 0 : n;
}

static void setup_coder(coef_coder *c, const wma_coef_table *t)
{
    c->t = t;
    int code = 2;
    for (int k = 0; k < t->n_levels; k++) {
        c->int_table[k] = (uint16_t)code;
        code += t->levels[k];
    }
}

/* Exponent band sizes for block size index k (block_len = N >> k), as the decoder derives them. */
static int exponent_bands(const wma_ctx *s, int k, int *out)
{
    int block_len = s->n >> k, sr = s->p.sample_rate;
    int a = s->frame_len_bits - BLOCK_MIN_BITS - k;
    const uint8_t *table = nullptr;
    if (a < 3) {
        if (sr >= 44100)
            table = wma_exponent_band_44100[a];
        else if (sr >= 32000)
            table = wma_exponent_band_32000[a];
        else if (sr >= 22050)
            table = wma_exponent_band_22050[a];
    }
    if (table) {
        for (int i = 0; i < table[0]; i++)
            out[i] = table[i + 1];
        return table[0];
    }
    int j = 0, lpos = 0;
    for (int i = 0; i < 25; i++) {
        int pos = ((block_len * 2 * wma_critical_freqs[i]) + (sr << 1)) / (4 * sr);
        pos <<= 2;
        if (pos > block_len)
            pos = block_len;
        if (pos > lpos)
            out[j++] = pos - lpos;
        if (pos >= block_len)
            break;
        lpos = pos;
    }
    return j;
}

static int ctx_init(wma_ctx *s, const wma_params *p, char *err, int err_size)
{
    *s = (wma_ctx){.p = *p};
    int sr = p->sample_rate, ch = p->channels;
    if ((sr != 44100 && sr != 48000) || ch < 1 || ch > 2 || p->bitrate <= 0 || p->block_align <= 0) {
        snprintf(err, (size_t)err_size, "unsupported format %d Hz, %d channels", sr, ch);
        return -1;
    }
    s->frame_len_bits = 11;
    s->n = 1 << s->frame_len_bits;

    /* Block sizes the decoder expects to be signalled (flags2 bits 3-4). */
    int nb = ((FLAGS2 >> 3) & 3) + 1;
    if (p->bitrate / ch >= 32000)
        nb += 2;
    int nb_max = s->frame_len_bits - BLOCK_MIN_BITS;
    if (nb > nb_max)
        nb = nb_max;
    s->nb_block_sizes = nb + 1;
    s->blk_bits = av_log2((unsigned)(s->nb_block_sizes - 1)) + 1;

    float bps = (float)p->bitrate / (float)(ch * sr);
    s->byte_offset_bits = av_log2((unsigned)(int)(bps * s->n / 8.0 + 0.5)) + 2;

    /* Noise coding and the high frequency limit, as the decoder derives them. */
    float bps1 = ch == 2 ? bps * 1.6f : bps;
    float high_freq = sr * 0.5f;
    s->use_noise_coding = true;
    if (bps1 >= 0.61f)
        s->use_noise_coding = false;
    else
        high_freq *= 0.4f;

    s->nbands = exponent_bands(s, 0, s->bands);
    s->coefs_end = s->n - (s->n * 9) / 100;
    if (s->use_noise_coding) {
        int high_band_start = (int)((s->n * 2 * high_freq) / sr + 0.5);
        int pos = 0;
        for (int i = 0; i < s->nbands; i++) {
            int start = pos, end = pos + s->bands[i];
            pos = end;
            if (start < high_band_start)
                start = high_band_start;
            if (end > s->coefs_end)
                end = s->coefs_end;
            if (end > start)
                s->n_high_bands++;
        }
    }

    int table = 2;
    if (bps1 < 0.72f)
        table = 0;
    else if (bps1 < 1.16f)
        table = 1;
    setup_coder(&s->coder[0], &wma_coef_tables[table * 2]);
    setup_coder(&s->coder[1], &wma_coef_tables[table * 2 + 1]);

    /* Audio bandwidth: wider as the bit rate per channel grows. */
    double per_ch = (double)p->bitrate / ch;
    double cutoff_hz = per_ch >= 64000 ? 20000 : per_ch >= 48000 ? 18000 : per_ch >= 32000 ? 16500 : 15000;
    s->cutoff = (int)(cutoff_hz / (sr * 0.5) * s->n);
    if (s->cutoff > s->coefs_end)
        s->cutoff = s->coefs_end;

    for (int i = 0; i < 2 * s->n; i++)
        s->window[i] = (float)sin((i + 0.5) * PI / (2.0 * s->n));

    s->header_bits = 4 + 4 + s->byte_offset_bits + 3;
    s->packet_bits = p->block_align * 8 - s->header_bits;
    /* Frames per packet that keep the stated bit rate: samples per packet / N. */
    double samples_per_packet = (double)p->block_align * 8.0 * sr / p->bitrate;
    double frames_per_packet = samples_per_packet / s->n;
    s->frame_target = (int)(s->packet_bits / frames_per_packet);
    s->frame_max = (1 << (s->byte_offset_bits + 3)) - 1;
    if (s->frame_max > s->packet_bits - 1)
        s->frame_max = s->packet_bits - 1;
    s->frame_min = s->packet_bits / MAX_FRAMES_PER_PACKET + 1;
    if (s->frame_target > s->frame_max)
        s->frame_target = s->frame_max;
    if (s->frame_target < s->frame_min) {
        snprintf(err, (size_t)err_size, "packet size too small for this bit rate");
        return -1;
    }
    if (!mdct_init(&s->mdct, s->n)) {
        snprintf(err, (size_t)err_size, "out of memory");
        return -1;
    }
    return 0;
}

/* ---- One frame ---------------------------------------------------------- */

typedef struct {
    float coef[2][MAX_N];   /* scaled MDCT coefficients per coded channel */
    int   exps[2][MAX_BANDS];
    float rel[2][MAX_BANDS]; /* 10^((e - e_max)/16) */
    int   exp_bits[2];
    bool  coded[2];
    int   q[2][MAX_N];
} frame_state;

/* Huffman code for a run of zeros followed by a level, or 0 for escape. */
static inline int coef_code(const coef_coder *c, int run, int level)
{
    if (level <= c->t->n_levels && run < c->t->levels[level - 1])
        return c->int_table[level - 1] + run;
    return 0;
}

static int total_gain_to_bits(int g)
{
    return g < 15 ? 13 : g < 32 ? 12 : g < 40 ? 11 : g < 45 ? 10 : 9;
}

/* Quantizes channel ch at gain g. Returns coefficient bits, or -1 if a level does not fit. */
static int quantize(const wma_ctx *s, frame_state *f, int ch, int g, bool store)
{
    const coef_coder *cc = &s->coder[ch == 1 && s->p.channels == 2];
    double gain = pow(10.0, g / 20.0);
    int nb = total_gain_to_bits(g), limit = 1 << nb;
    int bits = 0, run = 0, pos = 0;
    for (int b = 0; b < s->nbands && pos < s->coefs_end; b++) {
        double inv = 1.0 / (f->rel[ch][b] * gain);
        int end = pos + s->bands[b];
        if (end > s->coefs_end)
            end = s->coefs_end;
        for (; pos < end; pos++) {
            int q = 0;
            if (pos < s->cutoff) {
                double v = f->coef[ch][pos] * inv;
                if (!(fabs(v) + 0.4054 < limit))  /* also false for NaN; checked before the cast */
                    return -1;
                q = (int)(fabs(v) + 0.4054);
                if (v < 0)
                    q = -q;
            }
            if (store)
                f->q[ch][pos] = q;
            if (!q) {
                run++;
                continue;
            }
            int code = coef_code(cc, run, abs(q));
            bits += cc->t->bits[code] + 1;
            if (code == 0)
                bits += nb + s->frame_len_bits;
            run = 0;
        }
    }
    if (run)
        bits += cc->t->bits[1];
    return bits;
}

static int header_bits(const wma_ctx *s, const frame_state *f, bool reset, int g)
{
    int bits = s->blk_bits * (reset ? 3 : 1);
    int ch = s->p.channels;
    if (ch == 2)
        bits++;
    bits += ch;
    bool any = false;
    for (int c = 0; c < ch; c++) {
        if (f->coded[c]) {
            any = true;
            bits += f->exp_bits[c] + (s->use_noise_coding ? s->n_high_bands : 0);
        }
    }
    if (any)
        bits += 7 * ((g - 1) / 127 + 1);
    return bits;
}

/* Picks band exponents for channel ch from a masking estimate of its band levels. */
static void choose_exponents(const wma_ctx *s, frame_state *f, int ch)
{
    double level[MAX_BANDS];
    int pos = 0;
    double peak = 0;
    for (int b = 0; b < s->nbands; b++) {
        int end = pos + s->bands[b];
        double e = 0;
        int cnt = 0;
        for (int i = pos; i < end && i < s->cutoff; i++, cnt++)
            e += (double)f->coef[ch][i] * f->coef[ch][i];
        level[b] = cnt ? sqrt(e / cnt) : 0;
        if (level[b] > peak)
            peak = level[b];
        pos = end;
    }
    /* Masking threshold per band. The bands follow the critical bands, so a masker spreads
     * to higher bands at -12 dB per band and to lower bands at -25 dB per band. The
     * threshold never drops below 60 dB under the frame's peak band or below about
     * -100 dBFS (white noise of rms 1e-5 gives coefficients of 1e-5 * sqrt(N) * 32768). */
    static constexpr double UP = 0.2512;    /* -12 dB */
    static constexpr double DOWN = 0.0562;  /* -25 dB */
    double floor_level = peak * 1e-3;
    double abs_floor = 1e-5 * sqrt((double)s->n) * 32768.0;
    if (floor_level < abs_floor)
        floor_level = abs_floor;
    double masked[MAX_BANDS];
    for (int b = 0; b < s->nbands; b++) {
        double m = floor_level;
        for (int j = 0; j < s->nbands; j++) {
            double att = j <= b ? pow(UP, b - j) : pow(DOWN, j - b);
            if (level[j] * att > m)
                m = level[j] * att;
        }
        masked[b] = m;
    }
    double top = peak > 0 ? 16.0 * log10(peak) : 0;
    int prev = 36, emax = EXP_MIN;
    for (int b = 0; b < s->nbands; b++) {
        int e = (int)lround(16.0 * log10(masked[b] > 0 ? masked[b] : 1e-30) - top) + 80;
        if (e < prev - 60)
            e = prev - 60;
        if (e > prev + 60)
            e = prev + 60;
        if (e < EXP_MIN)
            e = EXP_MIN;
        if (e > EXP_MAX)
            e = EXP_MAX;
        f->exps[ch][b] = e;
        prev = e;
        if (e > emax)
            emax = e;
    }
    int bits = 0;
    prev = 36;
    for (int b = 0; b < s->nbands; b++) {
        bits += wma_scalefactor_bits[f->exps[ch][b] - prev + 60];
        prev = f->exps[ch][b];
        f->rel[ch][b] = (float)pow(10.0, (f->exps[ch][b] - emax) / 16.0);
    }
    f->exp_bits[ch] = bits;
}

/* Pads a frame that codes in too few bits: adds +-1 values at unused positions, quietest
 * bands first. Frames only get here when quantization is at its finest, where one step
 * is far below audibility. Larger levels (escape coded) are used while plain values
 * could not cover the deficit. */
static void add_padding(const wma_ctx *s, frame_state *f, int g, int deficit)
{
    for (int ch = 0; ch < s->p.channels && deficit > 0; ch++) {
        const coef_coder *cc = &s->coder[ch == 1 && s->p.channels == 2];
        int cost1 = cc->t->bits[coef_code(cc, 0, 1)] + 1;
        int esc_level = cc->t->n_levels + 1;
        int cost_esc = cc->t->bits[0] + total_gain_to_bits(g) + s->frame_len_bits + 1;

        int order[MAX_BANDS], start[MAX_BANDS + 1];
        start[0] = 0;
        for (int b = 0; b < s->nbands; b++) {
            order[b] = b;
            start[b + 1] = start[b] + s->bands[b];
        }
        for (int i = 1; i < s->nbands; i++)
            for (int j = i; j > 0 && f->rel[ch][order[j]] < f->rel[ch][order[j - 1]]; j--) {
                int t = order[j];
                order[j] = order[j - 1];
                order[j - 1] = t;
            }
        int free_left = 0;
        for (int i = 0; i < s->coefs_end; i++)
            free_left += f->q[ch][i] == 0;
        for (int k = 0; k < s->nbands && deficit > 0; k++) {
            int b = order[k];
            int end = start[b + 1] < s->coefs_end ? start[b + 1] : s->coefs_end;
            for (int i = start[b]; i < end && deficit > 0; i++) {
                if (f->q[ch][i])
                    continue;
                bool esc = deficit > cost1 * free_left;
                f->q[ch][i] = esc ? esc_level : 1;
                deficit -= esc ? cost_esc : cost1;
                free_left--;
            }
        }
    }
}

/* Bits of channel ch from its stored q values (used after padding). */
static int coded_bits(const wma_ctx *s, const frame_state *f, int ch, int g)
{
    const coef_coder *cc = &s->coder[ch == 1 && s->p.channels == 2];
    int nb = total_gain_to_bits(g), bits = 0, run = 0;
    for (int i = 0; i < s->coefs_end; i++) {
        int q = f->q[ch][i];
        if (!q) {
            run++;
            continue;
        }
        int code = coef_code(cc, run, abs(q));
        bits += cc->t->bits[code] + 1 + (code == 0 ? nb + s->frame_len_bits : 0);
        run = 0;
    }
    return bits + (run ? cc->t->bits[1] : 0);
}

static void write_coefs(const wma_ctx *s, const frame_state *f, int ch, int g, bitwriter *bw)
{
    const coef_coder *cc = &s->coder[ch == 1 && s->p.channels == 2];
    int nb = total_gain_to_bits(g), run = 0;
    for (int i = 0; i < s->coefs_end; i++) {
        int q = f->q[ch][i];
        if (!q) {
            run++;
            continue;
        }
        int level = abs(q);
        int code = coef_code(cc, run, level);
        bw_put(bw, cc->t->bits[code], cc->t->codes[code]);
        if (code == 0) {
            bw_put(bw, nb, (uint32_t)level);
            bw_put(bw, s->frame_len_bits, (uint32_t)run);
        }
        bw_put(bw, 1, q < 0);  /* the decoder negates: its sign bit 1 restores a negative value of this MDCT */
        run = 0;
    }
    if (run)
        bw_put(bw, cc->t->bits[1], cc->t->codes[1]);
}

static void write_frame(const wma_ctx *s, const frame_state *f, bool reset, int g, bitwriter *bw)
{
    int ch = s->p.channels;
    if (reset) {
        bw_put(bw, s->blk_bits, 0);  /* previous block: full length */
        bw_put(bw, s->blk_bits, 0);  /* this block: full length */
    }
    bw_put(bw, s->blk_bits, 0);      /* next block: full length */
    if (ch == 2)
        bw_put(bw, 1, 1);            /* mid/side stereo */
    bool any = false;
    for (int c = 0; c < ch; c++) {
        bw_put(bw, 1, f->coded[c]);
        any |= f->coded[c];
    }
    if (!any)
        return;
    int v = g - 1;
    for (; v >= 127; v -= 127)
        bw_put(bw, 7, 127);
    bw_put(bw, 7, (uint32_t)v);
    if (s->use_noise_coding)
        for (int c = 0; c < ch; c++)
            if (f->coded[c])
                bw_put(bw, s->n_high_bands, 0);
    for (int c = 0; c < ch; c++) {
        if (!f->coded[c])
            continue;
        int prev = 36;
        for (int b = 0; b < s->nbands; b++) {
            int code = f->exps[c][b] - prev + 60;
            bw_put(bw, wma_scalefactor_bits[code], wma_scalefactor_codes[code]);
            prev = f->exps[c][b];
        }
    }
    for (int c = 0; c < ch; c++)
        if (f->coded[c])
            write_coefs(s, f, c, g, bw);
}

/* Encodes one frame into bw within [min_bits, max_bits]. */
static int encode_frame(const wma_ctx *s, frame_state *f, bool reset, int min_bits, int max_bits, bitwriter *bw)
{
    int ch = s->p.channels;

    for (int c = 0; c < ch; c++) {
        f->coded[c] = true;
        choose_exponents(s, f, c);
    }

    /* Smallest gain (finest quantization) whose frame fits in max_bits. */
    int lo = 1, hi = 400, best = -1;
    while (lo <= hi) {
        int g = (lo + hi) / 2, bits = header_bits(s, f, reset, g);
        bool ok = true;
        for (int c = 0; c < ch && ok; c++) {
            int b = quantize(s, f, c, g, false);
            ok = b >= 0;
            bits += b;
        }
        if (ok && bits <= max_bits) {
            best = g;
            hi = g - 1;
        } else {
            lo = g + 1;
        }
    }
    if (best < 0)
        return -1;
    int bits = header_bits(s, f, reset, best);
    for (int c = 0; c < ch; c++)
        bits += quantize(s, f, c, best, true);

    /* Near silent frames code in very few bits; pad them up to the minimum. */
    for (int tries = 0; tries < 8 && bits < min_bits; tries++) {
        add_padding(s, f, best, min_bits - bits);
        bits = header_bits(s, f, reset, best);
        for (int c = 0; c < ch; c++)
            bits += coded_bits(s, f, c, best);
    }
    if (bits < min_bits || bits > max_bits)
        return -1;
    size_t start = bw->bits;
    write_frame(s, f, reset, best, bw);
    return (int)(bw->bits - start);
}


/* ---- Stream -------------------------------------------------------------- */

struct wma_encoder {
    wma_ctx     s;
    frame_state f;
    float      *in[2];         /* input window: samples [in_start, in_start + in_len) */
    int64_t     in_start;
    size_t      in_len, in_cap;
    int64_t     total_in;      /* samples fed so far */
    int64_t     next_frame;
    int64_t     last_packet_started;
    long long   balance;       /* bits saved so far against the target */
    bitwriter   stream;        /* frame bits not yet sent in a packet */
    uint64_t    base;          /* stream position of stream.data[0], in bits */
    uint64_t   *ends;          /* end positions of frames not yet assigned to a packet */
    size_t      ends_len, ends_cap;
    uint64_t    prev_end;      /* end of the last frame assigned to a packet */
    int64_t     packet;        /* next packet number */
    uint64_t    decoded;       /* decoded bytes after the packets so far */
    uint32_t   *dpds;
    size_t      dpds_len, dpds_cap;
    uint8_t    *pkt;
    wma_packet_sink sink;
    void       *sink_ctx;
};

/* MDCT of a frame (input samples (frame - 1) N .. (frame + 1) N) into f->coef, mid/side for stereo. */
static void analyse(wma_encoder *e, int64_t frame)
{
    wma_ctx *s = &e->s;
    frame_state *f = &e->f;
    int n = s->n;
    int64_t first = (frame - 1) * n;  /* the decoder drops the first frame as codec delay */
    float scale = 32768.0f;           /* the decoder's reconstruction gives the input level back */
    for (int c = 0; c < s->p.channels; c++) {
        for (int i = 0; i < 2 * n; i++) {
            int64_t t = first + i - e->in_start;
            float x = (t >= 0 && t < (int64_t)e->in_len) ? e->in[c][t] : 0.0f;
            if (!isfinite(x))
                x = 0.0f;  /* never let NaN or infinity reach the quantizer */
            else if (x > 1.0f || x < -1.0f)
                x = x > 0 ? 1.0f : -1.0f;  /* full scale is the most the format can carry */
            s->in[i] = x * s->window[i];
        }
        mdct_forward(&s->mdct, s->in, f->coef[c]);
        for (int i = 0; i < n; i++)
            f->coef[c][i] *= scale;
    }
    if (s->p.channels == 2) {
        for (int i = 0; i < n; i++) {
            float m = (f->coef[0][i] + f->coef[1][i]) * 0.5f;
            float d = (f->coef[0][i] - f->coef[1][i]) * 0.5f;
            f->coef[0][i] = m;
            f->coef[1][i] = d;
        }
    }
}

/* Sends the next packet: its header, then the frame bits that fall into it. */
static int emit_packet(wma_encoder *e, char *err, int err_size)
{
    wma_ctx *s = &e->s;
    uint64_t lo = (uint64_t)e->packet * (uint64_t)s->packet_bits, hi = lo + (uint64_t)s->packet_bits;
    uint64_t total = e->base + e->stream.bits;
    /* A frame that started in an earlier packet carries on into this one. */
    bool carry = e->packet > 0 && e->ends_len > 0 && e->prev_end < lo;
    int bit_offset = carry ? (int)(e->ends[0] - lo) : 0;
    size_t ends = 0;
    while (ends < e->ends_len && e->ends[ends] <= hi)
        ends++;
    int field = (int)ends + (carry ? 0 : 1);
    if (ends == 0 || field > 15 || bit_offset >= (1 << (s->byte_offset_bits + 3))) {
        snprintf(err, (size_t)err_size, "packet %lld layout invalid", (long long)e->packet);
        return -1;
    }
    memset(e->pkt, 0, (size_t)s->p.block_align);
    bitwriter pk = {.data = e->pkt, .cap = (size_t)s->p.block_align};
    bw_put(&pk, 4, (uint32_t)(e->packet & 15));
    bw_put(&pk, 4, (uint32_t)field);
    bw_put(&pk, s->byte_offset_bits + 3, (uint32_t)bit_offset);
    bw_copy(&pk, e->stream.data, (size_t)(lo - e->base), (size_t)((hi < total ? hi : total) - lo));

    e->decoded += (uint64_t)ends * (uint64_t)s->n * (uint64_t)s->p.channels * 2u;
    if (e->decoded > UINT32_MAX) {  /* the dpds table holds 32-bit byte counts */
        snprintf(err, (size_t)err_size, "audio too long for the xWMA format (over %d minutes)",
                 (int)(UINT32_MAX / (2u * (uint64_t)s->p.channels * (uint64_t)s->p.sample_rate) / 60));
        return -1;
    }
    if (e->dpds_len == e->dpds_cap) {
        size_t cap = e->dpds_cap ? e->dpds_cap * 2 : 1024;
        uint32_t *p = realloc(e->dpds, cap * sizeof *p);
        if (!p) {
            snprintf(err, (size_t)err_size, "out of memory");
            return -1;
        }
        e->dpds = p;
        e->dpds_cap = cap;
    }
    e->dpds[e->dpds_len++] = (uint32_t)e->decoded;
    if (e->sink(e->pkt, (size_t)s->p.block_align, e->sink_ctx) != 0) {
        snprintf(err, (size_t)err_size, "error writing output file");
        return -1;
    }

    e->prev_end = e->ends[ends - 1];
    memmove(e->ends, e->ends + ends, (e->ends_len - ends) * sizeof *e->ends);
    e->ends_len -= ends;
    e->packet++;
    /* drop the whole bytes of frame bits that are now sent */
    size_t drop = hi <= total ? (size_t)((hi - e->base) / 8) : 0;
    bw_drop_front(&e->stream, drop);
    e->base += (uint64_t)drop * 8;
    return 0;
}

static int encode_next_frame(wma_encoder *e, char *err, int err_size)
{
    wma_ctx *s = &e->s;
    uint64_t start = e->base + e->stream.bits;
    int64_t packet = (int64_t)(start / (uint64_t)s->packet_bits);
    bool reset = packet != e->last_packet_started;  /* first frame that starts in this packet */
    e->last_packet_started = packet;

    long long allowed = s->frame_target + e->balance / 4;
    if (allowed > s->frame_max)
        allowed = s->frame_max;
    if (allowed < s->frame_min + 64)  /* room for padding to overshoot by one value */
        allowed = s->frame_min + 64;

    analyse(e, e->next_frame);
    int bits = encode_frame(s, &e->f, reset, s->frame_min, (int)allowed, &e->stream);
    if (bits < 0 || e->stream.failed) {
        snprintf(err, (size_t)err_size, "could not fit frame %lld in the bit budget", (long long)e->next_frame);
        return -1;
    }
    e->balance += s->frame_target - bits;
    if (e->ends_len == e->ends_cap) {
        size_t cap = e->ends_cap ? e->ends_cap * 2 : 64;
        uint64_t *p = realloc(e->ends, cap * sizeof *p);
        if (!p) {
            snprintf(err, (size_t)err_size, "out of memory");
            return -1;
        }
        e->ends = p;
        e->ends_cap = cap;
    }
    e->ends[e->ends_len++] = e->base + e->stream.bits;
    e->next_frame++;

    /* A packet is complete once the frame bits run past its end. */
    while (e->base + e->stream.bits > (uint64_t)(e->packet + 1) * (uint64_t)s->packet_bits)
        if (emit_packet(e, err, err_size) != 0)
            return -1;

    /* The next frame needs input from (next_frame - 1) N on; drop what is older. */
    int64_t keep_from = (e->next_frame - 1) * s->n;
    if (keep_from > e->in_start) {
        size_t drop = (size_t)(keep_from - e->in_start);
        if (drop > e->in_len)
            drop = e->in_len;
        for (int c = 0; c < s->p.channels; c++)
            memmove(e->in[c], e->in[c] + drop, (e->in_len - drop) * sizeof(float));
        e->in_len -= drop;
        e->in_start += (int64_t)drop;
    }
    return 0;
}

wma_encoder *wma_encoder_create(const wma_params *p, wma_packet_sink sink, void *ctx, char *err, int err_size)
{
    wma_encoder *e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(err, (size_t)err_size, "out of memory");
        return nullptr;
    }
    if (ctx_init(&e->s, p, err, err_size) != 0) {
        free(e);
        return nullptr;
    }
    bw_init(&e->stream);
    e->sink = sink;
    e->sink_ctx = ctx;
    e->last_packet_started = -1;
    e->in_cap = 3 * (size_t)e->s.n;
    e->pkt = malloc((size_t)p->block_align);
    bool ok = e->pkt != nullptr;
    for (int c = 0; c < p->channels; c++)
        ok = ok && (e->in[c] = malloc(e->in_cap * sizeof(float))) != nullptr;
    if (!ok) {
        snprintf(err, (size_t)err_size, "out of memory");
        wma_encoder_free(e);
        return nullptr;
    }
    return e;
}

int wma_encoder_feed(wma_encoder *e, const float *const *pcm, size_t samples, char *err, int err_size)
{
    size_t done = 0;
    while (done < samples) {
        size_t take = e->in_cap - e->in_len;
        if (take > samples - done)
            take = samples - done;
        for (int c = 0; c < e->s.p.channels; c++)
            memcpy(e->in[c] + e->in_len, pcm[c] + done, take * sizeof(float));
        e->in_len += take;
        e->total_in += (int64_t)take;
        done += take;
        /* a frame can be coded once the input reaches (frame + 1) N */
        while (e->total_in >= (e->next_frame + 1) * e->s.n)
            if (encode_next_frame(e, err, err_size) != 0)
                return -1;
    }
    return 0;
}

int wma_encoder_finish(wma_encoder *e, char *err, int err_size)
{
    /* frames until every input sample is out; the last ones read past the end as silence */
    int64_t frames = (e->total_in + e->s.n - 1) / e->s.n + 1;
    while (e->next_frame < frames)
        if (encode_next_frame(e, err, err_size) != 0)
            return -1;
    while (e->ends_len > 0)
        if (emit_packet(e, err, err_size) != 0)
            return -1;
    return 0;
}

const uint32_t *wma_encoder_dpds(const wma_encoder *e, size_t *count)
{
    *count = e->dpds_len;
    return e->dpds;
}

void wma_encoder_free(wma_encoder *e)
{
    if (!e)
        return;
    mdct_free(&e->s.mdct);
    bw_free(&e->stream);
    free(e->in[0]);
    free(e->in[1]);
    free(e->ends);
    free(e->dpds);
    free(e->pkt);
    free(e);
}
