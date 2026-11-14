// SPDX-License-Identifier: AGPL-3.0-or-later
/* Decodes an MP3 with mp3dec and with minimp3 and reports how closely they agree. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp3dec.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_SIMD
#include "minimp3.h"

static float *grow(float *p, size_t *cap, size_t need)
{
    if (need <= *cap)
        return p;
    while (*cap < need)
        *cap = *cap ? *cap * 2 : 1 << 20;
    return realloc(p, *cap * sizeof(float));
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        return 2;
    fclose(f);
    size_t skip = 0;  /* ID3v2 */
    if (n > 10 && !memcmp(buf, "ID3", 3))
        skip = 10 + (((size_t)buf[6] & 0x7F) << 21 | ((size_t)buf[7] & 0x7F) << 14 | ((size_t)buf[8] & 0x7F) << 7 | (buf[9] & 0x7F));

    static float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    float *a = nullptr, *b = nullptr;
    size_t acap = 0, bcap = 0, an = 0, bn = 0;
    int ch = 0, rate = 0, frames = 0;

    static mp3_decoder d;
    mp3_init(&d);
    for (size_t pos = skip; pos < (size_t)n;) {
        mp3_frame fi;
        int s = mp3_decode_frame(&d, buf + pos, (size_t)n - pos, pcm, &fi);
        if (s < 0)
            break;
        pos += fi.skipped + fi.frame_bytes;
        if (fi.info_frame)
            continue;
        ch = fi.channels;
        rate = fi.sample_rate;
        frames++;
        a = grow(a, &acap, an + (size_t)s * (size_t)ch);
        memcpy(a + an, pcm, sizeof(float) * (size_t)s * (size_t)ch);
        an += (size_t)s * (size_t)ch;
    }

    static mp3dec_t md;
    mp3dec_init(&md);
    for (size_t pos = skip; pos < (size_t)n;) {
        mp3dec_frame_info_t info;
        int s = mp3dec_decode_frame(&md, buf + pos, (int)((size_t)n - pos), pcm, &info);
        if (!info.frame_bytes)
            break;
        pos += (size_t)info.frame_bytes;
        b = grow(b, &bcap, bn + (size_t)s * (size_t)info.channels);
        memcpy(b + bn, pcm, sizeof(float) * (size_t)s * (size_t)info.channels);
        bn += (size_t)s * (size_t)info.channels;
    }

    /* minimp3 decodes an info frame as silence; find the offset between the two. */
    long best = 0;
    double best_err = INFINITY;
    for (long off = 0; off <= 1152 * ch && (size_t)off < bn; off += ch) {
        double e = 0;
        size_t m = an < bn - (size_t)off ? an : bn - (size_t)off;
        for (size_t i = 0; i < m && i < 200000; i++) {
            double t = a[i] - b[i + (size_t)off];
            e += t * t;
        }
        if (e < best_err) {
            best_err = e;
            best = off;
        }
    }
    size_t m = an < bn - (size_t)best ? an : bn - (size_t)best;
    double sig = 0, err = 0, peak = 0;
    for (size_t i = 0; i < m; i++) {
        double t = a[i] - b[i + (size_t)best];
        sig += (double)b[i + (size_t)best] * b[i + (size_t)best];
        err += t * t;
        if (fabs(t) > peak)
            peak = fabs(t);
    }
    if (getenv("FRAMES")) {  /* per granule error, to find where the decoders part */
        size_t g = 576 * (size_t)ch;
        for (size_t fr = 0; fr * g < m; fr++) {
            double fe = 0, fs = 0;
            for (size_t i = fr * g; i < (fr + 1) * g && i < m; i++) {
                double t = a[i] - b[i + (size_t)best];
                fe += t * t;
                fs += (double)b[i + (size_t)best] * b[i + (size_t)best];
            }
            if (fe > 1e-9)
                printf("granule %zu: error %.3g signal %.3g\n", fr, fe, fs);
        }
    }
    (void)argc;
    printf("%-28s %d Hz x%d, %d frames, samples ours %zu minimp3 %zu (offset %ld): SNR %.1f dB, max diff %.2g\n",
           argv[1], rate, ch, frames, an / (size_t)(ch ? ch : 1), bn / (size_t)(ch ? ch : 1), best / (ch ? ch : 1),
           err > 0 ? 10 * log10(sig / err) : 999.0, peak);
    return 0;
}
