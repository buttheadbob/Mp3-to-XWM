// SPDX-License-Identifier: AGPL-3.0-or-later
/* Encodes a generated signal straight to .xwm, bypassing MP3 decoding.
 * Usage: synth_encode impulse|noise|sine out.xwm [channels] [bitrate] [block_align] [piece] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/wma_enc.h"
#include "../src/xwm_writer.h"

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    int ch = argc > 3 ? atoi(argv[3]) : 1;
    size_t n = 44100 * 3;
    float *pcm[2] = {calloc(n, sizeof(float)), calloc(n, sizeof(float))};
    unsigned seed = 1;
    for (size_t i = 0; i < n; i++) {
        float v = 0;
        if (!strcmp(argv[1], "impulse"))
            v = (i == 10000) ? 0.8f : 0.0f;
        else if (!strcmp(argv[1], "sine"))
            v = 0.5f * (float)sin(2 * 3.14159265358979 * 1000.0 * i / 44100.0);
        else {
            seed = seed * 1664525u + 1013904223u;
            v = ((int)(seed >> 8) / 8388608.0f - 1.0f) * 0.3f;
        }
        pcm[0][i] = v;
        pcm[1][i] = v * 0.5f;
    }
    int br = argc > 4 ? atoi(argv[4]) : 48000, ba = argc > 5 ? atoi(argv[5]) : 2230;
    wma_params p = {44100, ch, br, ba};
    char err[200] = "";
    /* feed in uneven pieces to exercise the streaming paths */
    size_t piece = argc > 6 ? (size_t)atol(argv[6]) : 777;
    xwm_writer *out = xwm_open(argv[2], err, sizeof err);
    wma_encoder *e = out ? wma_encoder_create(&p, xwm_packet, out, err, sizeof err) : nullptr;
    bool ok = e != nullptr;
    for (size_t i = 0; ok && i < n; i += piece) {
        const float *at[2] = {pcm[0] + i, pcm[1] + i};
        ok = wma_encoder_feed(e, at, n - i < piece ? n - i : piece, err, sizeof err) == 0;
    }
    ok = ok && wma_encoder_finish(e, err, sizeof err) == 0;
    size_t count = 0;
    const uint32_t *dpds = ok ? wma_encoder_dpds(e, &count) : nullptr;
    if (!ok || xwm_close(out, &p, dpds, count, err, sizeof err) != 0) {
        if (!ok)
            xwm_abort(out);
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    wma_encoder_free(e);
    /* raw reference as 16-bit WAV for comparison */
    char ref[512];
    snprintf(ref, sizeof ref, "%s.ref.raw", argv[2]);
    FILE *f = fopen(ref, "wb");
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < ch; c++) {
            short s = (short)lrintf(pcm[c][i] * 32767);
            fwrite(&s, 2, 1, f);
        }
    fclose(f);
    return 0;
}
