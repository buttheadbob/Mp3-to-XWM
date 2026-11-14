// SPDX-License-Identifier: AGPL-3.0-or-later
/* Decodes an MPEG audio test vector (.bit) with mp3dec and compares it with the reference
 * output (.pcm, 16-bit little endian, interleaved). Reports SNR and the largest difference
 * in 16-bit steps. Usage: vectors_check file.bit file.pcm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mp3dec.h"

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? (size_t)len : 1);
    *n = b && len > 0 ? fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    size_t bn = 0, rn = 0;
    uint8_t *bit = load(argv[1], &bn), *ref = load(argv[2], &rn);
    if (!bit || !ref)
        return 2;
    size_t refs = rn / 2, outs = 0, cap = 1 << 20;
    int16_t *out = malloc(cap * sizeof *out);
    static float pcm[MP3_MAX_SAMPLES * 2];
    static mp3_decoder d;
    mp3_init(&d);
    int ch = 0;
    for (size_t pos = 0; pos < bn;) {
        mp3_frame fi;
        int s = mp3_decode_frame(&d, bit + pos, bn - pos, pcm, &fi);
        if (s < 0)
            break;
        pos += fi.skipped + fi.frame_bytes;
        ch = fi.channels;
        for (int i = 0; i < s * ch; i++) {
            if (outs == cap)
                out = realloc(out, (cap *= 2) * sizeof *out);
            double v = pcm[i] * 32768.0;
            out[outs++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
        }
    }
    /* offset of our output against the reference: up to 2 frames either way */
    long best = 0;
    double best_e = INFINITY;
    int step = ch ? ch : 1;
    for (long off = -2304L * step; off <= 2304L * step; off += step) {
        double e = 0;
        size_t cnt = 0;
        for (size_t i = 0; i < 100000 && i < refs; i++) {
            long j = (long)i + off;
            if (j < 0 || (size_t)j >= outs)
                continue;
            int r = (int16_t)(ref[2 * i] | ref[2 * i + 1] << 8);
            e += (double)(out[j] - r) * (out[j] - r);
            cnt++;
        }
        if (cnt > 1000 && e / (double)cnt < best_e) {
            best_e = e / (double)cnt;
            best = off;
        }
    }
    size_t m = 0;
    double sig = 0, err = 0;
    int maxd = 0;
    for (size_t i = 0; i < refs; i++) {
        long j = (long)i + best;
        if (j < 0 || (size_t)j >= outs)
            continue;
        m++;
        int r = (int16_t)(ref[2 * i] | ref[2 * i + 1] << 8);
        int dlt = abs(out[j] - r);
        sig += (double)r * r;
        err += (double)dlt * dlt;
        if (dlt > maxd)
            maxd = dlt;
    }
    printf("%-44s ch %d  ours %zu ref %zu offset %ld compared %zu  SNR %6.1f dB  max diff %d LSB\n", argv[1], ch,
           outs, refs, best / step, m, err > 0 ? 10 * log10(sig / err) : 999.0, maxd);
    return 0;
}
