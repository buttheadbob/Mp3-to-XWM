// SPDX-License-Identifier: AGPL-3.0-or-later
/* Hostile input tests for the decoder and the conversion path. Build with AddressSanitizer
 * so any out-of-bounds access stops the run:
 *   clang -std=c23 -g -O1 -fsanitize=address -Isrc -o build/test_fuzz tests/test_fuzz.c
 *         src/mp3dec.c src/mp3_input.c src/resample.c src/wma_enc.c src/mdct.c src/bitwriter.c
 *         src/xwm_writer.c src/platform.c src/convert.c src/adpcm_enc.c src/loopfit.c src/wav_writer.c
 * Each test passes when the code returns without a memory error and its checks hold. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/adpcm_enc.h"
#include "../src/convert.h"
#include "../src/loopfit.h"
#include "../src/mp3_tables.h"
#include "../src/mp3dec.h"
#include "../src/wma_enc.h"
#include "../src/xwm_writer.h"

static int failures;

static void check(bool ok, const char *what)
{
    printf("%-64s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
}

static uint32_t rng = 12345;
static uint32_t next_rand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* Bit writer for building frames by hand. */
typedef struct {
    uint8_t *p;
    size_t   bit;
} bw;

static void put(bw *w, int n, uint32_t v)
{
    for (int i = n - 1; i >= 0; i--, w->bit++)
        if ((v >> i) & 1)
            w->p[w->bit >> 3] |= (uint8_t)(0x80 >> (w->bit & 7));
}

/* Runs the decoder over buf until it finds no more frames. Returns the frames decoded. */
static int decode_all(const uint8_t *buf, size_t len, bool *sane)
{
    static mp3_decoder d;
    static float pcm[MP3_MAX_SAMPLES * 2];
    mp3_init(&d);
    int frames = 0;
    *sane = true;
    for (size_t pos = 0; pos < len;) {
        mp3_frame info;
        int n = mp3_decode_frame(&d, buf + pos, len - pos, pcm, &info);
        if (n < 0)
            break;
        if (n > MP3_MAX_SAMPLES || info.frame_bytes == 0 || info.skipped + info.frame_bytes > len - pos)
            *sane = false;
        pos += info.skipped + info.frame_bytes;
        frames++;
    }
    return frames;
}

/* Must-fix 1: free format layer III frames far over the legal size, with main_data_begin at
 * its maximum, must be rejected without touching memory outside the decoder's buffers. */
static void test_oversized_free_format(void)
{
    for (int size = 2000; size <= 9000; size += 500) {
        size_t len = (size_t)size * 6;
        uint8_t *buf = calloc(len, 1);
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)next_rand();
        for (int f = 0; f < 6; f++) {
            uint8_t *h = buf + (size_t)f * (size_t)size;
            h[0] = 0xFF;
            h[1] = 0xFB;  /* MPEG-1 layer III, no CRC */
            h[2] = 0x00;  /* free format, 44.1 kHz, no padding */
            h[3] = 0x00;  /* stereo */
            h[4] = 0xFF;  /* main_data_begin 511 */
            h[5] |= 0x80;
        }
        bool sane;
        int frames = decode_all(buf, len, &sane);
        char what[400];
        snprintf(what, sizeof what, "free format frames of %d bytes: %d decoded, no overflow", size, frames);
        check(sane && (size <= 2881 || frames == 0), what);
        free(buf);
    }
}

/* Must-fix 2: a layer III frame with the largest gain and linbits values gives samples far out
 * of range. Conversion must clamp them and finish with a valid file. */
static void test_huge_samples(const char *dir)
{
    uint8_t frame[417] = {0};  /* 128 kbit/s at 44.1 kHz */
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x90;  /* 128 kbit/s, 44.1 kHz */
    frame[3] = 0xC0;  /* mono */
    bw w = {frame + 4, 0};
    put(&w, 9, 0);      /* main_data_begin */
    put(&w, 5, 0);      /* private bits */
    put(&w, 4, 0);      /* scfsi */
    for (int gr = 0; gr < 2; gr++) {
        put(&w, 12, 1580);  /* part2_3_length: half the main data each (396 bytes in all) */
        put(&w, 9, 288);    /* big_values: the maximum */
        put(&w, 8, 255);    /* global_gain: the maximum */
        put(&w, 4, 0);      /* scalefac_compress */
        put(&w, 1, 0);      /* no window switching */
        put(&w, 5, 31);     /* table_select: linbits 13, for every region */
        put(&w, 5, 31);
        put(&w, 5, 31);
        put(&w, 4, 15);     /* region0_count */
        put(&w, 3, 7);      /* region1_count */
        put(&w, 1, 0);      /* preflag */
        put(&w, 1, 0);      /* scalefac_scale */
        put(&w, 1, 0);      /* count1table_select */
    }
    /* Main data: in each granule, pairs of (15, 15) from table 24 with all linbits set, which
     * is the largest value a line can hold (15 + 8191). Codes are rebuilt the way the decoder
     * assigns them: counting up in list order. */
    uint32_t code = 0;
    int code_len = 0;
    {
        int off = 0;
        for (int t = 0; t < 14; t++)
            off += mp3_huff_sizes[t];  /* table 24 is the 15th list */
        uint64_t c = 0;
        for (int i = 0; i < mp3_huff_sizes[14]; i++) {
            int len = mp3_huff_lens[off + i];
            if (mp3_huff_syms[off + i] == 0xFF) {
                code = (uint32_t)(c >> (32 - len));
                code_len = len;
            }
            c += 1ull << (32 - len);
        }
    }
    bw md = {frame + 4 + 17, 0};
    for (int gr = 0; gr < 2; gr++) {
        size_t end = md.bit + 1580;
        while (md.bit + (size_t)code_len + 28 <= end) {
            put(&md, code_len, code);
            put(&md, 13, 0x1FFF);  /* x linbits */
            put(&md, 1, 0);        /* x sign */
            put(&md, 13, 0x1FFF);  /* y linbits */
            put(&md, 1, 1);        /* y sign */
        }
        md.bit = end;
    }

    char in_path[512], out_path[512];
    snprintf(in_path, sizeof in_path, "%s/fuzz_huge.mp3", dir);
    snprintf(out_path, sizeof out_path, "%s/fuzz_huge.xwm", dir);
    FILE *f = fopen(in_path, "wb");
    for (int i = 0; i < 40; i++)
        fwrite(frame, 1, sizeof frame, f);
    fclose(f);

    bool sane;
    static mp3_decoder d;
    static float pcm[MP3_MAX_SAMPLES * 2];
    mp3_init(&d);
    mp3_frame info;
    int n = mp3_decode_frame(&d, frame, sizeof frame, pcm, &info);
    /* the first frame of a stream is mostly filter delay; the second shows the full level */
    n = mp3_decode_frame(&d, frame, sizeof frame, pcm, &info);
    float peak = 0;
    int nonfinite = 0;
    for (int i = 0; i < n; i++) {
        if (!isfinite(pcm[i]))
            nonfinite++;
        else if (fabsf(pcm[i]) > peak)
            peak = fabsf(pcm[i]);
    }
    char what[400];
    snprintf(what, sizeof what, "huge gain frame: %d samples, peak %.3g, %d not finite", n, peak, nonfinite);
    check(n > 0 && (peak > 100 || nonfinite > 0), what);
    (void)decode_all;
    (void)sane;

    {
        char err[256] = "";
        convert_options opt = {FORMAT_XWM};
        convert_result res;
        int rc = convert_mp3(in_path, out_path, &opt, &res, err, sizeof err);
        snprintf(what, sizeof what, "huge samples convert to XWM (%s)", rc ? err : "written");
        check(rc == 0, what);
    }
    {
        char err[256] = "";
        convert_options opt = {FORMAT_ADPCM};
        convert_result res;
        int rc = convert_mp3(in_path, out_path, &opt, &res, err, sizeof err);
        snprintf(what, sizeof what, "huge samples convert to MS ADPCM (%s)", rc ? err : "written");
        check(rc == 0, what);
    }
    remove(in_path);
    remove(out_path);
}

/* The encoder takes NaN, infinities and huge values without undefined behaviour. */
static int count_packets(const uint8_t *p, size_t n, void *ctx)
{
    (void)p;
    (void)n;
    ++*(int *)ctx;
    return 0;
}

static void test_encoder_bad_floats(void)
{
    size_t n = 44100;
    float *a = malloc(n * sizeof(float)), *b = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        uint32_t r = next_rand() % 6;
        a[i] = r == 0 ? NAN : r == 1 ? INFINITY : r == 2 ? -INFINITY : r == 3 ? 1e30f : r == 4 ? -1e30f : 0.5f;
        b[i] = -a[i];
    }
    for (int kbps = 48; kbps <= 192; kbps *= 2) {
        wma_params p = {44100, 2, kbps * 1000, kbps == 192 ? 8917 : kbps == 96 ? 4459 : 2230};
        char err[256] = "";
        int packets = 0;
        wma_encoder *e = wma_encoder_create(&p, count_packets, &packets, err, sizeof err);
        const float *ch[2] = {a, b};
        bool ok = e && wma_encoder_feed(e, ch, n, err, sizeof err) == 0 && wma_encoder_finish(e, err, sizeof err) == 0;
        char what[400];
        snprintf(what, sizeof what, "encoder with NaN/inf/1e30 input at %d kbit/s: %d packets", kbps, packets);
        check(ok && packets > 0, what);
        wma_encoder_free(e);
    }
    free(a);
    free(b);
}

/* The ADPCM encoder and the length fit take NaN, infinities and huge values, in pieces of any
 * size, and always give whole blocks; sounds past the hold limit (20000 samples here) stream. */
typedef struct {
    loopfit       *fit;
    adpcm_encoder *enc;
    int            blocks;
    size_t         bytes;
} adpcm_chain;

static int count_blocks(const uint8_t *p, size_t n, void *ctx)
{
    (void)p;
    adpcm_chain *c = ctx;
    c->blocks++;
    c->bytes += n;
    return 0;
}

static int fit_to_enc(const float *const *pcm, size_t n, void *ctx)
{
    return adpcm_encoder_feed(((adpcm_chain *)ctx)->enc, pcm, n);
}

static void test_adpcm_bad_floats(void)
{
    for (int channels = 1; channels <= 2; channels++)
        for (size_t total = 1; total < 300000; total = total * 7 + 13) {
            float *a = malloc(total * sizeof(float)), *b = malloc(total * sizeof(float));
            for (size_t i = 0; i < total; i++) {
                uint32_t r = next_rand() % 8;
                a[i] = r == 0 ? NAN : r == 1 ? INFINITY : r == 2 ? -INFINITY : r == 3 ? 1e30f
                     : r == 4 ? -1e30f : (float)((int)(next_rand() % 65536) - 32768) / 16384.0f;
                b[i] = -a[i];
            }
            adpcm_chain c = {0};
            c.enc = adpcm_encoder_create(channels, count_blocks, &c);
            c.fit = loopfit_create(channels, 44100, 44100, ADPCM_BLOCK_SAMPLES, 20000, fit_to_enc, &c);
            bool ok = c.enc && c.fit;
            for (size_t i = 0; ok && i < total;) {
                size_t n = 1 + next_rand() % 5000;
                if (n > total - i)
                    n = total - i;
                const float *p[2] = {a + i, b + i};
                ok = loopfit_feed(c.fit, p, n) == 0;
                i += n;
            }
            loopfit_result fit = {0};
            ok = ok && loopfit_finish(c.fit, &fit) == 0 && adpcm_encoder_finish(c.enc) == 0;
            size_t samples = total + (size_t)fit.change;
            char what[400];
            snprintf(what, sizeof what, "ADPCM %d ch, %zu samples of NaN/inf/1e30/noise: %d whole blocks%s, %d Hz",
                     channels, total, c.blocks, fit.streamed ? " (streamed)" : fit.padded ? " (padded)" : "",
                     fit.rate);
            check(ok && samples % ADPCM_BLOCK_SAMPLES == 0 && (size_t)c.blocks * ADPCM_BLOCK_SAMPLES == samples
                      && c.bytes == (size_t)c.blocks * (size_t)adpcm_block_align(channels),
                  what);
            loopfit_free(c.fit);
            adpcm_encoder_free(c.enc);
            free(a);
            free(b);
        }
}

/* Random data with frame headers of every layer, version and rate sprinkled in. */
static void test_random_streams(void)
{
    bool all_sane = true;
    int total = 0;
    for (int iter = 0; iter < 3000; iter++) {
        size_t len = 2000 + next_rand() % 30000;
        uint8_t *buf = malloc(len);
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)next_rand();
        int headers = 1 + (int)(next_rand() % 40);
        for (int k = 0; k < headers; k++) {
            size_t at = next_rand() % (len - 4);
            buf[at] = 0xFF;
            buf[at + 1] = (uint8_t)(0xE0 | (next_rand() & 0x1F));
        }
        bool sane;
        total += decode_all(buf, len, &sane);
        all_sane &= sane;
        free(buf);
    }
    char what[400];
    snprintf(what, sizeof what, "3000 random streams: %d frames decoded, results in bounds", total);
    check(all_sane, what);
}

/* Streams of well formed headers with correct frame lengths and random contents, for every
 * version, layer, sample rate, bit rate and channel mode: the decoder sees garbage side
 * information, scale factors and Huffman data in otherwise valid frames. */
static void test_random_frames(void)
{
    static const int rates[3] = {44100, 48000, 32000};
    bool all_sane = true;
    int total = 0;
    for (int iter = 0; iter < 2000; iter++) {
        int ver_bits = (int[]){0, 2, 3}[next_rand() % 3];  /* MPEG-2.5, 2, 1 */
        int layer_bits = 1 + (int)(next_rand() % 3);       /* layer III, II, I */
        int layer = 4 - layer_bits, lsf = ver_bits != 3, m25 = ver_bits == 0;
        int sri = (int)(next_rand() % 3), sr = rates[sri] >> (lsf + m25);
        size_t cap = 64 * 2900, len = 0;
        uint8_t *buf = malloc(cap);
        for (int f = 0; f < 40; f++) {
            int bri = 1 + (int)(next_rand() % 14), pad = (int)(next_rand() & 1), crc = (int)(next_rand() & 1);
            int kbps = mp3_bitrates[lsf][layer - 1][bri];
            int bytes = layer == 1 ? (12000 * kbps / sr + pad) * 4
                      : (layer == 3 && lsf ? 72000 : 144000) * kbps / sr + pad;
            if (len + (size_t)bytes > cap)
                break;
            uint8_t *h = buf + len;
            for (int i = 0; i < bytes; i++)
                h[i] = (uint8_t)next_rand();
            h[0] = 0xFF;
            h[1] = (uint8_t)(0xE0 | ver_bits << 3 | layer_bits << 1 | !crc);
            h[2] = (uint8_t)(bri << 4 | sri << 2 | pad << 1);
            h[3] = (uint8_t)(next_rand() & 0xF0);  /* any channel mode and extension */
            len += (size_t)bytes;
        }
        bool sane;
        total += decode_all(buf, len, &sane);
        all_sane &= sane;
        free(buf);
    }
    char what[400];
    snprintf(what, sizeof what, "2000 streams of random frames: %d frames decoded, in bounds", total);
    check(all_sane && total > 10000, what);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    test_oversized_free_format();
    test_huge_samples(dir);
    test_encoder_bad_floats();
    test_adpcm_bad_floats();
    test_random_streams();
    test_random_frames();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
