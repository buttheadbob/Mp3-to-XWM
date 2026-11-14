// SPDX-License-Identifier: AGPL-3.0-or-later
/* MPEG audio layer I, II and III decoder (ISO/IEC 11172-3, 13818-3 and the MPEG-2.5
 * extension), including free format streams.
 *
 * Plain floating point implementation of the reference decoding steps. Layers I and II:
 * bit allocation, scale factors and dequantization of subband samples. Layer III: Huffman
 * decoding, requantization, reordering, stereo processing, alias reduction and IMDCT.
 * All layers end in the polyphase synthesis filter bank.
 */
#include "mp3dec.h"

#include <math.h>
#include <string.h>

#include "mp3_tables.h"

static constexpr double PI = 3.14159265358979323846;

/* ---- Tables built once ------------------------------------------------------ */

typedef struct {
    int16_t child[2];  /* > 0: next node, <= 0: leaf holding -symbol */
} huff_node;

static huff_node g_huff[15][512];  /* big value tables, see mp3_huff_lens */
static huff_node g_quad[2][32];    /* count1 tables A and B */
static float     g_pow43[8207];    /* x^(4/3); 15 + 2^13 - 1 is the largest value */
static float     g_window[512];    /* synthesis window D */
static float     g_synth[64][32];  /* cos((16 + i)(2k + 1) pi / 64) */
static float     g_imdct_long[4][36];
static float     g_imdct_short_win[12];
static float     g_cos36[18][36];
static float     g_cos12[6][12];
static float     g_cs[8], g_ca[8];
static float     g_scale12[64];    /* layer I/II scale factors 2^(1 - i/3); 63 is invalid */
static bool      g_ready;

static void tree_add(huff_node *t, int *used, uint32_t code, int len, int symbol)
{
    int node = 0;
    for (int i = len - 1; i >= 0; i--) {
        int bit = (code >> i) & 1;
        if (i == 0) {
            t[node].child[bit] = (int16_t)-symbol;
        } else {
            if (t[node].child[bit] <= 0)
                t[node].child[bit] = (int16_t)(*used)++;
            node = t[node].child[bit];
        }
    }
}

static void init_tables(void)
{
    if (g_ready)
        return;
    /* Codes count up in list order: each entry takes the next free code of its length. */
    int off = 0;
    for (int t = 0; t < 15; t++) {
        uint64_t code = 0;
        int used = 1;
        for (int i = 0; i < mp3_huff_sizes[t]; i++) {
            int len = mp3_huff_lens[off + i];
            tree_add(g_huff[t], &used, (uint32_t)(code >> (32 - len)), len, mp3_huff_syms[off + i]);
            code += 1ull << (32 - len);
        }
        off += mp3_huff_sizes[t];
    }
    for (int t = 0; t < 2; t++) {
        int used = 1;
        for (int v = 0; v < 16; v++)
            tree_add(g_quad[t], &used, mp3_quad_codes[t][v], mp3_quad_bits[t][v], v);
    }
    for (int i = 0; i < 8207; i++)
        g_pow43[i] = (float)pow(i, 4.0 / 3.0);
    for (int i = 0; i <= 256; i++) {
        double v = mp3_enwindow[i] / 65536.0;
        g_window[i] = (float)v;
        if (i)
            g_window[512 - i] = (float)((i & 63) ? -v : v);
    }
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            g_synth[i][k] = (float)cos((16 + i) * (2 * k + 1) * PI / 64);
    for (int i = 0; i < 36; i++) {
        double s36 = sin(PI / 36 * (i + 0.5));
        g_imdct_long[0][i] = (float)s36;
        g_imdct_long[1][i] = (float)(i < 18 ? s36 : i < 24 ? 1 : i < 30 ? sin(PI / 12 * (i - 18 + 0.5)) : 0);
        g_imdct_long[3][i] = (float)(i < 6 ? 0 : i < 12 ? sin(PI / 12 * (i - 6 + 0.5)) : i < 18 ? 1 : s36);
    }
    for (int i = 0; i < 12; i++)
        g_imdct_short_win[i] = (float)sin(PI / 12 * (i + 0.5));
    for (int k = 0; k < 18; k++)
        for (int i = 0; i < 36; i++)
            g_cos36[k][i] = (float)cos(PI / 72 * (2 * i + 1 + 18) * (2 * k + 1));
    for (int k = 0; k < 6; k++)
        for (int i = 0; i < 12; i++)
            g_cos12[k][i] = (float)cos(PI / 24 * (2 * i + 1 + 6) * (2 * k + 1));
    static constexpr double ci[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
    for (int i = 0; i < 8; i++) {
        double r = sqrt(1 + ci[i] * ci[i]);
        g_cs[i] = (float)(1 / r);
        g_ca[i] = (float)(ci[i] / r);
    }
    for (int i = 0; i < 63; i++)
        g_scale12[i] = (float)exp2(1.0 - i / 3.0);
    g_scale12[63] = 0;
    g_ready = true;
}

/* ---- Bits ------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t pos, end;  /* in bits */
} bits;

static uint32_t get_bits(bits *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->pos++) {
        int bit = b->pos < b->end ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1 : 0;
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}

static int huff_read(bits *b, const huff_node *t)
{
    int node = 0;
    for (;;) {
        int next = t[node].child[get_bits(b, 1)];
        if (next <= 0)
            return -next;
        node = next;
    }
}

/* ---- Header ----------------------------------------------------------------- */

typedef struct {
    int  layer;         /* 1, 2 or 3 */
    bool lsf, mpeg25, crc, free_format;
    int  sr_index;      /* 0-8: row of the band tables */
    int  sample_rate, channels, mode, mode_ext;
    int  kbps;          /* 0 for free format */
    int  padding;       /* bytes of padding in this frame */
    int  frame_bytes;   /* 0 for free format until the stream's frame length is known */
    int  side_bytes;    /* layer III side information */
} header;

/* Frame length in bytes for a bit rate, without padding. */
static int frame_length(const header *o, int kbps)
{
    if (o->layer == 1)
        return 12000 * kbps / o->sample_rate * 4;
    if (o->layer == 3 && o->lsf)
        return 72000 * kbps / o->sample_rate;
    return 144000 * kbps / o->sample_rate;
}

/* Parses a header. free_bytes is the stream's free format frame length without padding,
 * or 0 while it is not known. */
static bool parse_header(const uint8_t *h, int free_bytes, header *o)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
        return false;
    int ver = (h[1] >> 3) & 3, layer_bits = (h[1] >> 1) & 3;
    int bri = h[2] >> 4, sri = (h[2] >> 2) & 3;
    if (ver == 1 || layer_bits == 0 || bri == 15 || sri == 3)
        return false;  /* reserved values */
    static constexpr int rates[3] = {44100, 48000, 32000};
    o->layer = 4 - layer_bits;
    o->lsf = ver != 3;
    o->mpeg25 = ver == 0;
    o->crc = !(h[1] & 1);
    o->free_format = bri == 0;
    o->sr_index = sri + (o->lsf ? 3 : 0) + (o->mpeg25 ? 3 : 0);
    o->sample_rate = rates[sri] >> (o->lsf + o->mpeg25);
    o->mode = h[3] >> 6;
    o->mode_ext = (h[3] >> 4) & 3;
    o->channels = o->mode == 3 ? 1 : 2;
    o->kbps = mp3_bitrates[o->lsf][o->layer - 1][bri];
    o->padding = (h[2] >> 1) & 1 ? (o->layer == 1 ? 4 : 1) : 0;
    o->side_bytes = o->layer != 3 ? 0 : o->lsf ? (o->channels == 1 ? 9 : 17) : (o->channels == 1 ? 17 : 32);
    if (o->free_format)
        o->frame_bytes = free_bytes ? free_bytes + o->padding : 0;
    else
        o->frame_bytes = frame_length(o, o->kbps) + o->padding;
    return o->frame_bytes == 0 || o->frame_bytes > 4 + (o->crc ? 2 : 0) + o->side_bytes;
}

/* Header bits that stay the same through a stream: version, layer, sample rate and whether
 * it is free format. The channel mode may change from frame to frame. */
static uint32_t stream_bits(const uint8_t *h)
{
    return ((uint32_t)(h[1] & 0xFE) << 16) | ((uint32_t)(h[2] & 0x0C) << 8) | ((uint32_t)((h[2] >> 4) == 0) << 1);
}

/* ---- Side information ------------------------------------------------------- */

typedef struct {
    int part2_3_length, big_values, global_gain, scalefac_compress;
    bool window_switching, mixed;
    int block_type;
    int table_select[3], subblock_gain[3];
    int region0_count, region1_count;
    int preflag, scalefac_scale, count1table_select;
} granule;

typedef struct {
    int main_data_begin;
    int scfsi[2][4];
    granule gr[2][2];
} side_info;

static void read_side_info(bits *b, const header *h, side_info *si)
{
    int nch = h->channels;
    si->main_data_begin = (int)get_bits(b, h->lsf ? 8 : 9);
    get_bits(b, h->lsf ? (nch == 1 ? 1 : 2) : (nch == 1 ? 5 : 3));  /* private bits */
    if (!h->lsf)
        for (int ch = 0; ch < nch; ch++)
            for (int i = 0; i < 4; i++)
                si->scfsi[ch][i] = (int)get_bits(b, 1);
    for (int gr = 0; gr < (h->lsf ? 1 : 2); gr++) {
        for (int ch = 0; ch < nch; ch++) {
            granule *g = &si->gr[gr][ch];
            g->part2_3_length = (int)get_bits(b, 12);
            g->big_values = (int)get_bits(b, 9);
            g->global_gain = (int)get_bits(b, 8);
            g->scalefac_compress = (int)get_bits(b, h->lsf ? 9 : 4);
            g->window_switching = get_bits(b, 1);
            if (g->window_switching) {
                g->block_type = (int)get_bits(b, 2);
                g->mixed = get_bits(b, 1);
                g->table_select[0] = (int)get_bits(b, 5);
                g->table_select[1] = (int)get_bits(b, 5);
                g->table_select[2] = 0;
                for (int w = 0; w < 3; w++)
                    g->subblock_gain[w] = (int)get_bits(b, 3);
                g->region0_count = g->block_type == 2 && !g->mixed ? 8 : 7;
                g->region1_count = 36;
            } else {
                g->block_type = 0;
                g->mixed = false;
                for (int r = 0; r < 3; r++)
                    g->table_select[r] = (int)get_bits(b, 5);
                for (int w = 0; w < 3; w++)
                    g->subblock_gain[w] = 0;
                g->region0_count = (int)get_bits(b, 4);
                g->region1_count = (int)get_bits(b, 3);
            }
            g->preflag = h->lsf ? 0 : (int)get_bits(b, 1);
            g->scalefac_scale = (int)get_bits(b, 1);
            g->count1table_select = (int)get_bits(b, 1);
        }
    }
}

/* ---- Scale factors ---------------------------------------------------------- */

typedef struct {
    int l[22];          /* long blocks */
    int s[13][3];       /* short blocks, per window */
    int l_max[22];      /* largest value each long factor can take (illegal intensity position) */
    int s_max[13][3];
    int intensity_scale;
} scalefactors;

static void read_sf_mpeg1(bits *b, const granule *g, const int *scfsi, int gr, scalefactors *sf)
{
    int slen1 = mp3_slen[0][g->scalefac_compress], slen2 = mp3_slen[1][g->scalefac_compress];
    if (g->window_switching && g->block_type == 2) {
        int first_short = 0;
        if (g->mixed) {
            for (int sfb = 0; sfb < 8; sfb++)
                sf->l[sfb] = (int)get_bits(b, slen1);
            first_short = 3;
        }
        for (int sfb = first_short; sfb < 12; sfb++)
            for (int w = 0; w < 3; w++)
                sf->s[sfb][w] = (int)get_bits(b, sfb < 6 ? slen1 : slen2);
        for (int w = 0; w < 3; w++)
            sf->s[12][w] = 0;
    } else {
        static constexpr int group[5] = {0, 6, 11, 16, 21};
        for (int k = 0; k < 4; k++) {
            if (gr == 1 && scfsi[k])
                continue;  /* shared with the first granule */
            for (int sfb = group[k]; sfb < group[k + 1]; sfb++)
                sf->l[sfb] = (int)get_bits(b, k < 2 ? slen1 : slen2);
        }
        sf->l[21] = 0;
    }
    for (int i = 0; i < 22; i++)
        sf->l_max[i] = 7;
    for (int i = 0; i < 13; i++)
        for (int w = 0; w < 3; w++)
            sf->s_max[i][w] = 7;
}

static void read_sf_lsf(bits *b, granule *g, bool intensity_ch, scalefactors *sf)
{
    int sfc = g->scalefac_compress, slen[4] = {0}, table;
    g->preflag = 0;
    if (intensity_ch) {
        sf->intensity_scale = sfc & 1;
        sfc >>= 1;
        if (sfc < 180) {
            slen[0] = sfc / 36;
            slen[1] = (sfc % 36) / 6;
            slen[2] = sfc % 6;
            table = 3;
        } else if (sfc < 244) {
            sfc -= 180;
            slen[0] = (sfc & 63) >> 4;
            slen[1] = (sfc & 15) >> 2;
            slen[2] = sfc & 3;
            table = 4;
        } else {
            sfc -= 244;
            slen[0] = sfc / 3;
            slen[1] = sfc % 3;
            table = 5;
        }
    } else if (sfc < 400) {
        slen[0] = (sfc >> 4) / 5;
        slen[1] = (sfc >> 4) % 5;
        slen[2] = (sfc & 15) >> 2;
        slen[3] = sfc & 3;
        table = 0;
    } else if (sfc < 500) {
        sfc -= 400;
        slen[0] = (sfc >> 2) / 5;
        slen[1] = (sfc >> 2) % 5;
        slen[2] = sfc & 3;
        table = 1;
    } else {
        sfc -= 500;
        slen[0] = sfc / 3;
        slen[1] = sfc % 3;
        g->preflag = 1;
        table = 2;
    }
    int kind = g->window_switching && g->block_type == 2 ? (g->mixed ? 2 : 1) : 0;
    int flat[40] = {0}, flat_max[40] = {0}, n = 0;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < mp3_lsf_nsf[table][kind][k] && n < 40; i++, n++) {
            flat[n] = (int)get_bits(b, slen[k]);
            flat_max[n] = slen[k] ? (1 << slen[k]) - 1 : -1;  /* no bits: position 0, legal */
        }
    memset(sf->l, 0, sizeof sf->l);
    memset(sf->s, 0, sizeof sf->s);
    for (int i = 0; i < 22; i++)
        sf->l_max[i] = -1;
    for (int i = 0; i < 13; i++)
        for (int w = 0; w < 3; w++)
            sf->s_max[i][w] = -1;
    int i = 0;
    if (kind == 0) {
        for (int sfb = 0; sfb < 21; sfb++, i++) {
            sf->l[sfb] = flat[i];
            sf->l_max[sfb] = flat_max[i];
        }
    } else {
        int first_short = 0;
        if (kind == 2) {
            for (int sfb = 0; sfb < 6; sfb++, i++) {
                sf->l[sfb] = flat[i];
                sf->l_max[sfb] = flat_max[i];
            }
            first_short = 3;
        }
        for (int sfb = first_short; sfb < 12; sfb++)
            for (int w = 0; w < 3; w++, i++) {
                sf->s[sfb][w] = flat[i];
                sf->s_max[sfb][w] = flat_max[i];
            }
    }
}

/* ---- Granule decoding ------------------------------------------------------- */

typedef struct {
    int long_start[23];   /* line where each long band starts */
    int short_start[14];  /* frequency (per window) where each short band starts */
} bands;

static void band_starts(int sr_index, bands *bt)
{
    bt->long_start[0] = 0;
    for (int i = 0; i < 22; i++)
        bt->long_start[i + 1] = bt->long_start[i] + mp3_band_long[sr_index][i];
    bt->short_start[0] = 0;
    for (int i = 0; i < 13; i++)
        bt->short_start[i + 1] = bt->short_start[i] + mp3_band_short[sr_index][i];
}

/* Number of long bands that make up the long part (36 lines) of a mixed block. */
static int mixed_long_bands(const bands *bt)
{
    int n = 0;
    while (n < 22 && bt->long_start[n + 1] <= 36)
        n++;
    return n;
}

/* First short band of the short part of a mixed block. */
static int mixed_first_short(const bands *bt)
{
    int n = 0;
    while (n < 13 && bt->short_start[n] * 3 < 36)
        n++;
    return n;
}

/* Huffman decodes one granule of one channel into is[576]. */
static void read_huffman(bits *b, const granule *g, const bands *bt, size_t end, int *is)
{
    int big = g->big_values * 2;
    if (big > 576)
        big = 576;
    int r1, r2;
    if (g->window_switching) {
        /* region 0 covers short bands 0-2 (all windows) or long bands 0-7 */
        r1 = g->block_type == 2 ? 3 * bt->short_start[3] : bt->long_start[8];
        r2 = 576;
    } else {
        int a = g->region0_count + 1, c = g->region0_count + g->region1_count + 2;
        r1 = bt->long_start[a > 22 ? 22 : a];
        r2 = bt->long_start[c > 22 ? 22 : c];
    }
    if (r1 > big)
        r1 = big;
    if (r2 > big)
        r2 = big;

    int i = 0;
    for (int region = 0; region < 3; region++) {
        int stop = region == 0 ? r1 : region == 1 ? r2 : big;
        int tbl = mp3_huff_select[g->table_select[region]][0];
        int linbits = mp3_huff_select[g->table_select[region]][1];
        for (; i < stop; i += 2) {
            if (tbl == 0) {
                is[i] = is[i + 1] = 0;
                continue;
            }
            int sym = huff_read(b, g_huff[tbl - 1]);
            int v[2] = {sym >> 4, sym & 15};
            for (int k = 0; k < 2; k++) {
                if (linbits && v[k] == 15)
                    v[k] += (int)get_bits(b, linbits);
                if (v[k] && get_bits(b, 1))
                    v[k] = -v[k];
                is[i + k] = v[k];
            }
        }
    }

    /* count1 region: quadruples of -1, 0, 1 until the granule's bits run out */
    const huff_node *quad = g_quad[g->count1table_select];
    while (i + 4 <= 576 && b->pos < end) {
        size_t before = b->pos;
        int code = huff_read(b, quad);
        int v[4] = {(code >> 3) & 1, (code >> 2) & 1, (code >> 1) & 1, code & 1};
        for (int k = 0; k < 4; k++)
            if (v[k] && get_bits(b, 1))
                v[k] = -v[k];
        if (b->pos > end) {  /* the last quadruple ran past the end: not part of the data */
            b->pos = before;
            break;
        }
        for (int k = 0; k < 4; k++)
            is[i + k] = v[k];
        i += 4;
    }
    for (; i < 576; i++)
        is[i] = 0;
}

/* Requantizes is[] into xr[] and reorders short block parts into frequency order. */
static void requantize(const granule *g, const scalefactors *sf, const bands *bt, const int *is, float *xr)
{
    double sfm = 0.5 * (1 + g->scalefac_scale);
    double base = 0.25 * (g->global_gain - 210);
    float decoded[576];
    int long_bands = 22, first_short = 13;
    bool is_short = g->window_switching && g->block_type == 2;
    if (is_short) {
        long_bands = g->mixed ? mixed_long_bands(bt) : 0;
        first_short = g->mixed ? mixed_first_short(bt) : 0;
    }
    for (int sfb = 0; sfb < long_bands; sfb++) {
        double scale = exp2(base - sfm * (sf->l[sfb] + g->preflag * mp3_pretab[sfb]));
        for (int i = bt->long_start[sfb]; i < bt->long_start[sfb + 1]; i++) {
            int v = is[i];
            decoded[i] = (float)(v < 0 ? -g_pow43[-v] * scale : g_pow43[v] * scale);
        }
    }
    memcpy(xr, decoded, sizeof(float) * (size_t)(long_bands ? bt->long_start[long_bands] : 0));
    if (!is_short)
        return;
    for (int sfb = first_short; sfb < 13; sfb++) {
        int width = bt->short_start[sfb + 1] - bt->short_start[sfb];
        int pos = 3 * bt->short_start[sfb];
        for (int w = 0; w < 3; w++) {
            double scale = exp2(base - 2.0 * g->subblock_gain[w] - sfm * sf->s[sfb][w]);
            for (int j = 0; j < width; j++) {
                int v = is[pos + w * width + j];
                float x = (float)(v < 0 ? -g_pow43[-v] * scale : g_pow43[v] * scale);
                xr[3 * (bt->short_start[sfb] + j) + w] = x;  /* frequency order, window interleaved */
            }
        }
    }
}

/* ---- Stereo ----------------------------------------------------------------- */

/* Left and right gains for one intensity stereo band. Returns false for an illegal
 * position (MPEG-1: 7 and up; MPEG-2: the largest value its scale factor bits can hold),
 * which leaves the band to mid/side or plain stereo. */
static bool intensity_ratio(const header *h, int intensity_scale, int pos, int illegal, double *rl, double *rr)
{
    if (h->lsf ? pos == illegal : pos >= 7)
        return false;
    if (!h->lsf) {
        if (pos == 6) {
            *rl = 1;
            *rr = 0;
        } else {
            double k = tan(pos * PI / 12);
            *rl = k / (1 + k);
            *rr = 1 / (1 + k);
        }
        return true;
    }
    double io = intensity_scale ? 0.70710678118654752 : 0.84089641525371454;  /* 2^-1/2, 2^-1/4 */
    *rl = 1;
    *rr = 1;
    if (pos & 1)
        *rl = pow(io, (pos + 1) / 2);
    else if (pos)
        *rr = pow(io, pos / 2);
    return true;
}

static void stereo(const header *h, const granule *g1, const scalefactors *sf1, const bands *bt,
                   float *l, float *r)
{
    bool ms = h->mode == 1 && (h->mode_ext & 2);
    bool intensity = h->mode == 1 && (h->mode_ext & 1);
    bool use_is[576] = {false};
    float is_l[576], is_r[576];

    if (intensity) {
        /* Positions of the right channel's last non-zero line decide where intensity starts. */
        bool is_short = g1->window_switching && g1->block_type == 2;
        int long_bands = is_short ? (g1->mixed ? mixed_long_bands(bt) : 0) : 22;
        int first_short = is_short ? (g1->mixed ? mixed_first_short(bt) : 0) : 13;

        bool short_has_data = false;
        int short_bound[3] = {first_short, first_short, first_short};
        if (is_short) {
            for (int w = 0; w < 3; w++)
                for (int sfb = 12; sfb >= first_short; sfb--) {
                    bool any = false;
                    for (int f = bt->short_start[sfb]; f < bt->short_start[sfb + 1] && !any; f++)
                        any = r[3 * f + w] != 0;
                    if (any) {
                        short_bound[w] = sfb + 1;
                        short_has_data = true;
                        break;
                    }
                }
        }
        int long_bound = 0;
        if (!short_has_data)
            for (int sfb = long_bands - 1; sfb >= 0; sfb--) {
                bool any = false;
                for (int i = bt->long_start[sfb]; i < bt->long_start[sfb + 1] && !any; i++)
                    any = r[i] != 0;
                if (any) {
                    long_bound = sfb + 1;
                    break;
                }
            }
        else
            long_bound = long_bands;


        /* The top band has no scale factor: it takes the position of the band below when that
         * band is in the intensity region too, else a default (centre for MPEG-1, 0 for MPEG-2). */
        int default_pos = h->lsf ? 0 : 3;
        for (int sfb = long_bound; sfb < long_bands; sfb++) {
            int pos = sf1->l[sfb], illegal = sf1->l_max[sfb];
            if (sfb == 21) {
                pos = long_bound > 20 ? default_pos : sf1->l[20];
                illegal = long_bound > 20 ? -1 : sf1->l_max[20];
            }
            double rl = 1, rr = 1;
            if (!intensity_ratio(h, sf1->intensity_scale, pos, illegal, &rl, &rr))
                continue;
            for (int i = bt->long_start[sfb]; i < bt->long_start[sfb + 1]; i++) {
                use_is[i] = true;
                is_l[i] = (float)(l[i] * rl);
                is_r[i] = (float)(l[i] * rr);
            }
        }
        if (is_short)
            for (int w = 0; w < 3; w++)
                for (int sfb = short_bound[w]; sfb < 13; sfb++) {
                    int pos = sf1->s[sfb][w], illegal = sf1->s_max[sfb][w];
                    if (sfb == 12) {
                        pos = short_bound[w] > 11 ? default_pos : sf1->s[11][w];
                        illegal = short_bound[w] > 11 ? -1 : sf1->s_max[11][w];
                    }
                    double rl = 1, rr = 1;
                    if (!intensity_ratio(h, sf1->intensity_scale, pos, illegal, &rl, &rr))
                        continue;
                    for (int f = bt->short_start[sfb]; f < bt->short_start[sfb + 1]; f++) {
                        int i = 3 * f + w;
                        use_is[i] = true;
                        is_l[i] = (float)(l[i] * rl);
                        is_r[i] = (float)(l[i] * rr);
                    }
                }
    }

    for (int i = 0; i < 576; i++) {
        if (use_is[i]) {
            l[i] = is_l[i];
            r[i] = is_r[i];
        } else if (ms) {
            float m = l[i], s = r[i];
            l[i] = (m + s) * 0.70710678f;
            r[i] = (m - s) * 0.70710678f;
        }
    }
}

/* ---- Hybrid filter bank ----------------------------------------------------- */

static void antialias(const granule *g, float *xr)
{
    int sblimit = 32;
    if (g->window_switching && g->block_type == 2)
        sblimit = g->mixed ? 2 : 0;
    for (int sb = 1; sb < sblimit; sb++)
        for (int i = 0; i < 8; i++) {
            float lo = xr[18 * sb - 1 - i], hi = xr[18 * sb + i];
            xr[18 * sb - 1 - i] = lo * g_cs[i] - hi * g_ca[i];
            xr[18 * sb + i] = hi * g_cs[i] + lo * g_ca[i];
        }
}

/* IMDCT, windowing and overlap for every subband; out[t][sb] is the subband sample at time t. */
static void hybrid(const granule *g, const float *xr, float overlap[32][18], float out[18][32])
{
    for (int sb = 0; sb < 32; sb++) {
        const float *x = xr + 18 * sb;
        int type = g->window_switching ? g->block_type : 0;
        if (g->window_switching && g->mixed && sb < 2)
            type = 0;
        float y[36] = {0};
        if (type == 2) {
            for (int w = 0; w < 3; w++)
                for (int i = 0; i < 12; i++) {
                    float s = 0;
                    for (int k = 0; k < 6; k++)
                        s += x[3 * k + w] * g_cos12[k][i];
                    y[6 + 6 * w + i] += s * g_imdct_short_win[i];
                }
        } else {
            for (int i = 0; i < 36; i++) {
                float s = 0;
                for (int k = 0; k < 18; k++)
                    s += x[k] * g_cos36[k][i];
                y[i] = s * g_imdct_long[type][i];
            }
        }
        for (int i = 0; i < 18; i++) {
            float v = y[i] + overlap[sb][i];
            overlap[sb][i] = y[i + 18];
            out[i][sb] = (sb & 1) && (i & 1) ? -v : v;  /* frequency inversion */
        }
    }
}

/* Polyphase synthesis of the given time slots into pcm (stride = channel count). */
static void synthesis(mp3_decoder *d, int ch, int slots, float in[][32], float *pcm, int stride)
{
    float *v = d->v[ch];
    for (int t = 0; t < slots; t++) {
        int p = d->v_pos[ch] = (d->v_pos[ch] - 64) & 1023;
        for (int i = 0; i < 64; i++) {
            float s = 0;
            for (int k = 0; k < 32; k++)
                s += g_synth[i][k] * in[t][k];
            v[(p + i) & 1023] = s;
        }
        for (int j = 0; j < 32; j++) {
            float s = 0;
            for (int i = 0; i < 8; i++) {
                s += v[(p + i * 128 + j) & 1023] * g_window[i * 64 + j];
                s += v[(p + i * 128 + 96 + j) & 1023] * g_window[i * 64 + 32 + j];
            }
            pcm[(t * 32 + j) * stride] = s;
        }
    }
}

/* ---- Layers I and II -------------------------------------------------------- */

/* Dequantizes a sample code of a quantizer with an odd number of levels to -1..1. */
static inline float dequant(int code, int levels)
{
    return (float)(2 * code - (levels - 1)) / (float)levels;
}

/* Decodes a layer I frame: 384 samples per channel. */
static int decode_layer1(mp3_decoder *d, bits *b, const header *h, float *pcm)
{
    int nch = h->channels, bound = h->mode == 1 ? (h->mode_ext + 1) * 4 : 32;
    int alloc[2][32] = {0}, scf[2][32] = {0};
    for (int sb = 0; sb < 32; sb++)
        for (int ch = 0; ch < nch; ch++) {
            if (sb >= bound && ch == 1) {  /* joint stereo: one allocation for both channels */
                alloc[1][sb] = alloc[0][sb];
                continue;
            }
            alloc[ch][sb] = (int)get_bits(b, 4);
            if (alloc[ch][sb] == 15)
                alloc[ch][sb] = 0;  /* forbidden value */
        }
    for (int sb = 0; sb < 32; sb++)
        for (int ch = 0; ch < nch; ch++)
            if (alloc[ch][sb])
                scf[ch][sb] = (int)get_bits(b, 6);
    float sub[2][12][32] = {0};
    for (int s = 0; s < 12; s++)
        for (int sb = 0; sb < 32; sb++) {
            float v = 0;
            for (int ch = 0; ch < nch; ch++) {
                if (!alloc[ch][sb])
                    continue;
                if (!(sb >= bound && ch == 1)) {  /* joint stereo: channel 1 reuses the sample */
                    int nb = alloc[ch][sb] + 1;
                    v = dequant((int)get_bits(b, nb), (1 << nb) - 1);
                }
                sub[ch][s][sb] = v * g_scale12[scf[ch][sb]];
            }
        }
    for (int ch = 0; ch < nch; ch++)
        synthesis(d, ch, 12, sub[ch], pcm + ch, nch);
    return 384;
}

/* Decodes a layer II frame: 1152 samples per channel. */
static int decode_layer2(mp3_decoder *d, bits *b, const header *h, float *pcm)
{
    int nch = h->channels;
    /* Allocation table from the bit rate per channel and the sample rate (11172-3 B.2, 13818-3 B.1),
     * chosen as in FFmpeg libavcodec/mpegaudio.c (Copyright (c) 2001, 2002 Fabrice Bellard, LGPL-2.1+). */
    int kbps = h->kbps ? h->kbps : h->frame_bytes * h->sample_rate / 144000;
    int ch_kbps = kbps / nch, table;
    if (h->lsf)
        table = 4;
    else if ((h->sample_rate == 48000 && ch_kbps >= 56) || (ch_kbps >= 56 && ch_kbps <= 80))
        table = 0;
    else if (h->sample_rate != 48000 && ch_kbps >= 96)
        table = 1;
    else if (h->sample_rate != 32000 && ch_kbps <= 48)
        table = 2;
    else
        table = 3;
    static const uint8_t *const alloc_tables[5] = {mp2_alloc_1, mp2_alloc_1, mp2_alloc_3, mp2_alloc_3, mp2_alloc_4};
    const uint8_t *row = alloc_tables[table];
    int sblimit = mp2_sblimit[table];
    int bound = h->mode == 1 ? (h->mode_ext + 1) * 4 : sblimit;
    if (bound > sblimit)
        bound = sblimit;

    int cls[2][32], scfsi[2][32] = {0}, scf[2][32][3] = {0};
    for (int sb = 0; sb < sblimit; sb++) {
        int nbal = row[0];
        for (int ch = 0; ch < nch; ch++) {
            if (sb >= bound && ch == 1) {
                cls[1][sb] = cls[0][sb];
                continue;
            }
            int v = (int)get_bits(b, nbal);
            cls[ch][sb] = v ? row[v] : -1;  /* quantization class, -1 = no samples */
        }
        row += 1 << nbal;
    }
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < nch; ch++)
            if (cls[ch][sb] >= 0)
                scfsi[ch][sb] = (int)get_bits(b, 2);
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < nch; ch++) {
            if (cls[ch][sb] < 0)
                continue;
            int *s = scf[ch][sb];
            switch (scfsi[ch][sb]) {
            case 0:  /* three scale factors */
                s[0] = (int)get_bits(b, 6);
                s[1] = (int)get_bits(b, 6);
                s[2] = (int)get_bits(b, 6);
                break;
            case 1:  /* the first serves the first two parts */
                s[0] = s[1] = (int)get_bits(b, 6);
                s[2] = (int)get_bits(b, 6);
                break;
            case 2:  /* one for all three */
                s[0] = s[1] = s[2] = (int)get_bits(b, 6);
                break;
            default:  /* the second serves the last two parts */
                s[0] = (int)get_bits(b, 6);
                s[1] = s[2] = (int)get_bits(b, 6);
                break;
            }
        }

    float sub[2][36][32] = {0};
    for (int gr = 0; gr < 12; gr++) {
        int part = gr / 4;
        for (int sb = 0; sb < sblimit; sb++) {
            float v[3] = {0, 0, 0};
            for (int ch = 0; ch < nch; ch++) {
                int c = cls[ch][sb];
                if (c < 0)
                    continue;
                if (!(sb >= bound && ch == 1)) {  /* joint stereo: channel 1 reuses the samples */
                    int levels = mp2_quant_steps[c], qb = mp2_quant_bits[c], codes[3];
                    if (qb < 0) {  /* three samples grouped in one code */
                        int g = (int)get_bits(b, -qb);
                        codes[0] = g % levels;
                        g /= levels;
                        codes[1] = g % levels;
                        codes[2] = g / levels;
                    } else {
                        for (int k = 0; k < 3; k++)
                            codes[k] = (int)get_bits(b, qb);
                    }
                    for (int k = 0; k < 3; k++)
                        v[k] = dequant(codes[k], levels);
                }
                float scale = g_scale12[scf[ch][sb][part]];
                for (int k = 0; k < 3; k++)
                    sub[ch][gr * 3 + k][sb] = v[k] * scale;
            }
        }
    }
    for (int ch = 0; ch < nch; ch++)
        synthesis(d, ch, 36, sub[ch], pcm + ch, nch);
    return 1152;
}

/* ---- Frames ----------------------------------------------------------------- */

void mp3_init(mp3_decoder *d)
{
    init_tables();
    memset(d, 0, sizeof *d);
}

/* Xing/Info header of a first frame, with the encoder delay and padding of a LAME tag. */
static bool read_info(const uint8_t *fr, const header *h, mp3_frame *info)
{
    if (h->layer != 3)
        return false;
    int off = 4 + (h->crc ? 2 : 0) + h->side_bytes;
    if (off + 8 > h->frame_bytes)
        return false;
    const uint8_t *x = fr + off;
    if (memcmp(x, "Xing", 4) && memcmp(x, "Info", 4))
        return false;
    uint32_t flags = ((uint32_t)x[4] << 24) | ((uint32_t)x[5] << 16) | ((uint32_t)x[6] << 8) | x[7];
    int pos = off + 8 + ((flags & 1) ? 4 : 0) + ((flags & 2) ? 4 : 0) + ((flags & 4) ? 100 : 0) + ((flags & 8) ? 4 : 0);
    /* delay and padding are only meaningful in a tag written by LAME or FFmpeg's libavformat/libavcodec */
    if (pos + 24 <= h->frame_bytes &&
        (!memcmp(fr + pos, "LAME", 4) || !memcmp(fr + pos, "Lavf", 4) || !memcmp(fr + pos, "Lavc", 4))) {
        const uint8_t *t = fr + pos + 21;
        info->encoder_delay = (t[0] << 4) | (t[1] >> 4);
        info->encoder_padding = ((t[1] & 0x0F) << 8) | t[2];
    }
    return true;
}

/* Metadata that may follow the last frame: ID3v1, APE and Lyrics3 tags, or ID3v2. */
static bool is_tag(const uint8_t *p, size_t n)
{
    return (n >= 3 && (!memcmp(p, "TAG", 3) || !memcmp(p, "ID3", 3)))
        || (n >= 8 && !memcmp(p, "APETAGEX", 8)) || (n >= 6 && !memcmp(p, "LYRICS", 6));
}

/* True when a free format frame of length n (without padding) starting at j is followed by
 * a header of the same stream, a tag, or the end of the data. */
static bool free_frame_fits(const uint8_t *buf, size_t len, size_t j, int n, uint32_t sb)
{
    header h;
    if (j + 4 > len || !parse_header(buf + j, n, &h) || stream_bits(buf + j) != sb)
        return false;
    size_t k = j + (size_t)h.frame_bytes;
    return k + 4 > len || is_tag(buf + k, len - k) || (parse_header(buf + k, 0, &h) && stream_bits(buf + k) == sb);
}

/* Free format: the frame length is the distance to the next header of the same stream.
 * The length seen before is tried first; otherwise the nearest header that itself starts a
 * fitting frame is taken, so a stray header-like pattern in the data is not mistaken for one.
 * Returns the length without padding, or 0 if no such header follows. */
static int free_format_length(const uint8_t *buf, size_t len, size_t i, const header *h, int known)
{
    static constexpr size_t MAX_FREE = 2881;  /* largest legal frame: layer III, 32 kHz, padded */
    uint32_t sb = stream_bits(buf + i);
    if (known > 0) {
        size_t j = i + (size_t)known + (size_t)h->padding;
        if (free_frame_fits(buf, len, j, known, sb))
            return known;
    }
    for (size_t j = i + 4 + (size_t)h->side_bytes + 1; j + 4 <= len && j - i <= MAX_FREE; j++) {
        int n = (int)(j - i) - h->padding;
        if (free_frame_fits(buf, len, j, n, sb))
            return n;
    }
    return 0;
}

/* Finds the next frame: a valid header followed by another matching one, a tag, or the end. */
static bool find_frame(mp3_decoder *d, const uint8_t *buf, size_t len, size_t *at, header *h)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (!parse_header(buf + i, d->free_bytes, h))
            continue;
        uint32_t sb = stream_bits(buf + i);
        if (d->have_stream && sb != d->stream_header)
            continue;
        if (h->free_format) {
            /* Free format: each frame runs to the next header, so a stream may change its frame
             * length. Only the last frame, with no header after it, uses the length seen before. */
            int n = free_format_length(buf, len, i, h, d->free_bytes);
            if (n <= 0)
                n = d->free_bytes;
            if (n <= 0 || !parse_header(buf + i, n, h))
                continue;
            d->free_bytes = n;
        }
        size_t next = i + (size_t)h->frame_bytes;
        if (next > len)
            continue;
        header nh;
        bool ok = next + 4 > len || is_tag(buf + next, len - next)
               || (parse_header(buf + next, d->free_bytes, &nh) && stream_bits(buf + next) == sb);
        if (!ok)
            continue;
        d->have_stream = true;
        d->stream_header = sb;
        *at = i;
        return true;
    }
    return false;
}

int mp3_decode_frame(mp3_decoder *d, const uint8_t *buf, size_t len, float *pcm, mp3_frame *info)
{
    *info = (mp3_frame){.encoder_delay = -1, .encoder_padding = -1};
    header h;
    size_t at;
    bool first = !d->have_stream;
    if (!find_frame(d, buf, len, &at, &h))
        return -1;
    const uint8_t *fr = buf + at;
    if (h.channels == 2 && d->last_channels == 1) {
        /* mono to stereo: channel 1's filters hold nothing of this stream */
        memset(d->overlap[1], 0, sizeof d->overlap[1]);
        memset(d->v[1], 0, sizeof d->v[1]);
    }
    d->last_channels = h.channels;
    info->skipped = at;
    info->frame_bytes = (size_t)h.frame_bytes;
    info->sample_rate = h.sample_rate;
    info->channels = h.channels;
    if (first && read_info(fr, &h, info)) {
        info->info_frame = true;
        return 0;
    }
    int head = 4 + (h.crc ? 2 : 0);
    if (h.layer != 3) {
        bits lb = {fr + head, 0, (size_t)(h.frame_bytes - head) * 8};
        return h.layer == 1 ? decode_layer1(d, &lb, &h, pcm) : decode_layer2(d, &lb, &h, pcm);
    }

    int nch = h.channels, ngr = h.lsf ? 1 : 2;
    int samples = ngr * 576;
    side_info si = {0};
    bits b = {fr + head, 0, (size_t)h.side_bytes * 8};
    read_side_info(&b, &h, &si);

    /* Main data: the end of earlier frames' main data (bit reservoir) plus this frame's. */
    const uint8_t *md = fr + head + h.side_bytes;
    int md_len = h.frame_bytes - head - h.side_bytes;
    if (md_len < 0 || md_len > (int)sizeof d->reservoir)
        return 0;  /* cannot happen for legal frame sizes */

    /* Impossible side information: the stream is damaged here. Start afresh, as a decoder
     * picking up a stream does: drop the reservoir and the filter states, output nothing. */
    int part23 = 0;
    bool valid = true;
    for (int gr = 0; gr < ngr; gr++)
        for (int ch = 0; ch < nch; ch++) {
            const granule *g = &si.gr[gr][ch];
            if (g->big_values > 288 || (g->window_switching && g->block_type == 0))
                valid = false;
            part23 += g->part2_3_length;
        }
    if (!valid || part23 > (si.main_data_begin + md_len) * 8) {
        memset(d->overlap, 0, sizeof d->overlap);
        memset(d->v, 0, sizeof d->v);
        d->reservoir_len = 0;
        return 0;
    }
    uint8_t data[511 + sizeof d->reservoir];  /* main_data_begin is at most 511 */
    bool ok = si.main_data_begin <= d->reservoir_len && si.main_data_begin + md_len <= (int)sizeof data;
    int data_len = 0;
    if (ok) {
        memcpy(data, d->reservoir + d->reservoir_len - si.main_data_begin, (size_t)si.main_data_begin);
        memcpy(data + si.main_data_begin, md, (size_t)md_len);
        data_len = si.main_data_begin + md_len;
    }
    /* keep this frame's main data for later frames */
    if (d->reservoir_len + md_len > (int)sizeof d->reservoir) {
        int drop = d->reservoir_len + md_len - (int)sizeof d->reservoir;  /* <= reservoir_len */
        memmove(d->reservoir, d->reservoir + drop, (size_t)(d->reservoir_len - drop));
        d->reservoir_len -= drop;
    }
    memcpy(d->reservoir + d->reservoir_len, md, (size_t)md_len);
    d->reservoir_len += md_len;

    /* No output when the reservoir data is missing (the stream starts mid-way). */
    if (!ok)
        return 0;

    bands bt;
    band_starts(h.sr_index, &bt);
    bits mb = {data, 0, (size_t)data_len * 8};
    scalefactors sf[2] = {0};  /* the second granule may reuse the first's (scfsi) */
    for (int gr = 0; gr < ngr; gr++) {
        float xr[2][576] = {0};  /* lines no band covers (8 kHz mixed blocks) stay zero */
        for (int ch = 0; ch < nch; ch++) {
            granule *g = &si.gr[gr][ch];
            size_t start = mb.pos, end = start + (size_t)g->part2_3_length;
            if (end > mb.end)
                end = mb.end;
            if (h.lsf)
                read_sf_lsf(&mb, g, ch == 1 && (h.mode_ext & 1) && h.mode == 1, &sf[ch]);
            else
                read_sf_mpeg1(&mb, g, si.scfsi[ch], gr, &sf[ch]);
            int is[576];
            read_huffman(&mb, g, &bt, end, is);
            requantize(g, &sf[ch], &bt, is, xr[ch]);
            mb.pos = end;
        }
        if (nch == 2)
            stereo(&h, &si.gr[gr][1], &sf[1], &bt, xr[0], xr[1]);
        for (int ch = 0; ch < nch; ch++) {
            float sub[18][32];
            antialias(&si.gr[gr][ch], xr[ch]);
            hybrid(&si.gr[gr][ch], xr[ch], d->overlap[ch], sub);
            synthesis(d, ch, 18, sub, pcm + (size_t)gr * 576 * (size_t)nch + (size_t)ch, nch);
        }
    }
    return samples;
}
