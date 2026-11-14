// SPDX-License-Identifier: AGPL-3.0-or-later
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>

#include "adpcm_enc.h"
#include "loopfit.h"
#include "mp3_input.h"
#include "resample.h"
#include "wav_writer.h"
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
    wma_encoder   *wma;
    loopfit       *fit;
    adpcm_encoder *adpcm;
    char          *err;
    int            err_size;
} pipe_ctx;

static int to_wma(const float *const *pcm, size_t samples, void *ctx)
{
    pipe_ctx *c = ctx;
    return wma_encoder_feed(c->wma, pcm, samples, c->err, c->err_size);
}

static int to_adpcm(const float *const *pcm, size_t samples, void *ctx)
{
    pipe_ctx *c = ctx;
    return adpcm_encoder_feed(c->adpcm, pcm, samples);
}

/* Reads the whole MP3 through the resampler. */
static int pump(mp3_reader *in, resampler *rs, float *buf[2], char *err, int err_size)
{
    for (;;) {
        long n = mp3_reader_read(in, buf, CHUNK, err, err_size);
        if (n < 0)
            return -1;
        if (n == 0)
            return resampler_finish(rs);
        if (resampler_feed(rs, (const float *const *)buf, (size_t)n) != 0)
            return -1;
    }
}

static int convert_xwm(mp3_reader *in, const char *out_path, int kbps, convert_result *res, float *buf[2],
                       char *err, int err_size)
{
    int channels = mp3_reader_channels(in);
    /* xWMA encoders give mono 44.1 kHz no rate above 48 kbit/s; decoders treat such files as 48. */
    if (channels == 1)
        kbps = 48;
    wma_params p = {OUT_RATE, channels, kbps * 1000, block_align_for(kbps)};
    int rc = -1;
    xwm_writer *out = xwm_open(out_path, err, err_size);
    pipe_ctx ctx = {.err = err, .err_size = err_size};
    ctx.wma = out ? wma_encoder_create(&p, xwm_packet, out, err, err_size) : nullptr;
    resampler *rs = ctx.wma ? resampler_create(mp3_reader_sample_rate(in), OUT_RATE, channels, to_wma, &ctx) : nullptr;
    if (!rs) {
        if (ctx.wma)
            snprintf(err, (size_t)err_size, "out of memory");
        goto done;
    }
    if (pump(in, rs, buf, err, err_size) != 0 || wma_encoder_finish(ctx.wma, err, err_size) != 0)
        goto done;
    size_t count;
    const uint32_t *dpds = wma_encoder_dpds(ctx.wma, &count);
    rc = xwm_close(out, &p, dpds, count, err, err_size);
    out = nullptr;  /* closed, whatever the result */
    if (rc == 0)
        res->kbps = kbps;
done:
    if (out)
        xwm_abort(out);
    resampler_free(rs);
    wma_encoder_free(ctx.wma);
    return rc;
}

/* MS ADPCM: the whole sound is held, then resampled as a loop to an exact number of blocks
 * (see loopfit.h) and encoded; the file states the rate that keeps its pitch. */
static int convert_adpcm(mp3_reader *in, const char *out_path, convert_result *res, float *buf[2],
                         char *err, int err_size)
{
    int channels = mp3_reader_channels(in);
    int rc = -1;
    wav_writer *out = wav_open(out_path, channels, OUT_RATE, err, err_size);
    pipe_ctx ctx = {.err = err, .err_size = err_size};
    ctx.adpcm = out ? adpcm_encoder_create(channels, wav_block, out) : nullptr;
    int in_rate = mp3_reader_sample_rate(in);
    ctx.fit = ctx.adpcm ? loopfit_create(channels, in_rate, OUT_RATE, ADPCM_BLOCK_SAMPLES,
                                         (size_t)in_rate * CONVERT_LOOP_SECONDS, to_adpcm, &ctx) : nullptr;
    if (!ctx.fit) {
        if (out)
            snprintf(err, (size_t)err_size, "out of memory");
        goto done;
    }
    for (;;) {
        long n = mp3_reader_read(in, buf, CHUNK, err, err_size);
        if (n < 0)
            goto done;
        if (n == 0)
            break;
        if (loopfit_feed(ctx.fit, (const float *const *)buf, (size_t)n) != 0) {
            snprintf(err, (size_t)err_size, "out of memory or error writing output file");
            goto done;
        }
    }
    loopfit_result fit;
    if (loopfit_finish(ctx.fit, &fit) != 0 || adpcm_encoder_finish(ctx.adpcm) != 0) {
        snprintf(err, (size_t)err_size, "out of memory or error writing output file");
        goto done;
    }
    res->loop_change = fit.change;
    res->loop_rate = fit.rate;
    res->loop_padded = fit.padded;
    res->loop_streamed = fit.streamed;
    wav_set_rate(out, fit.rate);
    rc = wav_close(out, err, err_size);
    out = nullptr;
done:
    if (out)
        wav_abort(out);
    loopfit_free(ctx.fit);
    adpcm_encoder_free(ctx.adpcm);
    return rc;
}

int convert_mp3(const char *in_path, const char *out_path, const convert_options *opt, convert_result *res,
                char *err, int err_size)
{
    *res = (convert_result){0};
    err[0] = '\0';
    mp3_reader *in = mp3_reader_open(in_path, err, err_size);
    if (!in)
        return -1;
    float *buf[2] = {malloc(CHUNK * sizeof(float)), malloc(CHUNK * sizeof(float))};
    int rc = -1;
    if (!buf[0] || !buf[1])
        snprintf(err, (size_t)err_size, "out of memory");
    else if (opt->format == FORMAT_ADPCM)
        rc = convert_adpcm(in, out_path, res, buf, err, err_size);
    else
        rc = convert_xwm(in, out_path, opt->kbps, res, buf, err, err_size);
    mp3_reader_close(in);
    free(buf[0]);
    free(buf[1]);
    return rc;
}
