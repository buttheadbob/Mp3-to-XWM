// SPDX-License-Identifier: AGPL-3.0-or-later
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>

#include "mp3_input.h"
#include "resample.h"
#include "wma_enc.h"
#include "xwm_writer.h"

static constexpr int    OUT_RATE = 44100;
static constexpr size_t CHUNK    = 4096;  /* samples per channel read at a time */

/* Packet size for each bit rate at 44.1 kHz: the bytes of 16384 samples (8 frames of 2048),
 * rounded up. 2230 is what the game's own music uses; 4459 and 8917 are the matching sizes
 * in XAudio2's list of xWMA block alignments. */
static int block_align_for(int kbps)
{
    return kbps == 192 ? 8917 : kbps == 96 ? 4459 : 2230;
}

typedef struct {
    wma_encoder *enc;
    char        *err;
    int          err_size;
} pipe_ctx;

static int to_encoder(const float *const *pcm, size_t samples, void *ctx)
{
    pipe_ctx *c = ctx;
    return wma_encoder_feed(c->enc, pcm, samples, c->err, c->err_size);
}

int convert_mp3_to_xwm(const char *in_path, const char *out_path, int kbps, int *used_kbps,
                       char *err, int err_size)
{
    mp3_reader *in = mp3_reader_open(in_path, err, err_size);
    if (!in)
        return -1;
    int channels = mp3_reader_channels(in);
    /* xWMA encoders give mono 44.1 kHz no rate above 48 kbit/s; decoders treat such files as 48. */
    if (channels == 1)
        kbps = 48;
    wma_params p = {
        .sample_rate = OUT_RATE,
        .channels    = channels,
        .bitrate     = kbps * 1000,
        .block_align = block_align_for(kbps),
    };

    int rc = -1;
    float *buf[2] = {malloc(CHUNK * sizeof(float)), malloc(CHUNK * sizeof(float))};
    xwm_writer *out = xwm_open(out_path, err, err_size);
    wma_encoder *enc = out ? wma_encoder_create(&p, xwm_packet, out, err, err_size) : nullptr;
    pipe_ctx ctx = {enc, err, err_size};
    resampler *rs = enc ? resampler_create(mp3_reader_sample_rate(in), OUT_RATE, channels, to_encoder, &ctx) : nullptr;
    if (!buf[0] || !buf[1] || !rs) {
        if (out && enc)
            snprintf(err, (size_t)err_size, "out of memory");
        goto done;
    }

    for (;;) {
        long n = mp3_reader_read(in, buf, CHUNK, err, err_size);
        if (n < 0)
            goto done;
        if (n == 0)
            break;
        if (resampler_feed(rs, (const float *const *)buf, (size_t)n) != 0)
            goto done;
    }
    if (resampler_finish(rs) != 0 || wma_encoder_finish(enc, err, err_size) != 0)
        goto done;
    size_t count;
    const uint32_t *dpds = wma_encoder_dpds(enc, &count);
    rc = xwm_close(out, &p, dpds, count, err, err_size);
    out = nullptr;  /* closed, whatever the result */
    if (rc == 0)
        *used_kbps = kbps;

done:
    if (out)
        xwm_abort(out);
    resampler_free(rs);
    wma_encoder_free(enc);
    mp3_reader_close(in);
    free(buf[0]);
    free(buf[1]);
    return rc;
}
