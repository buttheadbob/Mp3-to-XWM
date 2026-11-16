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
static constexpr size_t CHUNK    = 4096;  /* samples read at a time */

/* Output is always mono. The game mixes a 3D sound (a Sound Block, for one) as a single channel:
 * a stereo file plays from the left speaker only, while a mono one is placed between the
 * speakers. The game's own 3D sounds are mono, and a mono 2D sound plays from both speakers. */
static constexpr int CHANNELS = 1;

/* Packet size at 48 kbit/s and 44.1 kHz: the bytes of 16384 samples (8 frames of 2048), rounded
 * up, as in the game's own music. */
static constexpr int XWM_BLOCK_ALIGN = 2230;

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
static int pump(mp3_reader *in, resampler *rs, float *buf, char *err, int err_size)
{
    for (;;) {
        long n = mp3_reader_read(in, buf, CHUNK, err, err_size);
        if (n < 0)
            return -1;
        if (n == 0)
            return resampler_finish(rs);
        if (resampler_feed(rs, (const float *const *)&buf, (size_t)n) != 0)
            return -1;
    }
}

static int convert_xwm(mp3_reader *in, const char *out_path, float *buf, char *err, int err_size)
{
    wma_params p = {OUT_RATE, CHANNELS, CONVERT_XWM_KBPS * 1000, XWM_BLOCK_ALIGN};
    int rc = -1;
    xwm_writer *out = xwm_open(out_path, err, err_size);
    pipe_ctx ctx = {.err = err, .err_size = err_size};
    ctx.wma = out ? wma_encoder_create(&p, xwm_packet, out, err, err_size) : nullptr;
    resampler *rs = ctx.wma ? resampler_create(mp3_reader_sample_rate(in), OUT_RATE, CHANNELS, to_wma, &ctx) : nullptr;
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
done:
    if (out)
        xwm_abort(out);
    resampler_free(rs);
    wma_encoder_free(ctx.wma);
    return rc;
}

/* MS ADPCM: the whole sound is held, then resampled as a loop to an exact number of blocks
 * (see loopfit.h) and encoded; the file states the rate that keeps its pitch. */
static int convert_adpcm(mp3_reader *in, const char *out_path, convert_result *res, float *buf,
                         char *err, int err_size)
{
    int rc = -1;
    wav_writer *out = wav_open(out_path, CHANNELS, OUT_RATE, err, err_size);
    pipe_ctx ctx = {.err = err, .err_size = err_size};
    ctx.adpcm = out ? adpcm_encoder_create(CHANNELS, wav_block, out) : nullptr;
    int in_rate = mp3_reader_sample_rate(in);
    ctx.fit = ctx.adpcm ? loopfit_create(CHANNELS, in_rate, OUT_RATE, ADPCM_BLOCK_SAMPLES,
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
        if (loopfit_feed(ctx.fit, (const float *const *)&buf, (size_t)n) != 0) {
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
    float *buf = malloc(CHUNK * sizeof(float));
    int rc = -1;
    if (!buf)
        snprintf(err, (size_t)err_size, "out of memory");
    else if (opt->format == FORMAT_ADPCM)
        rc = convert_adpcm(in, out_path, res, buf, err, err_size);
    else
        rc = convert_xwm(in, out_path, buf, err, err_size);
    mp3_reader_close(in);
    free(buf);
    return rc;
}
