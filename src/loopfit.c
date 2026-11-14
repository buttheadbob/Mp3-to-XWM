// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loopfit.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "resample.h"

static constexpr double PI      = 3.14159265358979323846;
static constexpr int    TAPS    = 16;   /* filter half length in input samples at cutoff 1 */
static constexpr int    PHASES  = 256;
static constexpr size_t OUT_CHUNK = 1024;

struct loopfit {
    int        channels, in_rate, out_rate;
    size_t     block, max_hold;
    float     *buf[2];          /* the whole sound at the input rate */
    size_t     len, cap;
    int        half, width;     /* filter half length and length in input samples */
    float     *table;           /* table[p * width + j]: weight of input sample j - half + 1 at phase p */
    float      out[2][OUT_CHUNK];
    resampler *stream;          /* set once the sound is longer than max_hold */
    size_t     sent;            /* output samples sent while streaming */
    loopfit_sink sink;
    void      *ctx;
};

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

loopfit *loopfit_create(int channels, int in_rate, int out_rate, size_t block, size_t max_hold,
                        loopfit_sink sink, void *ctx)
{
    loopfit *f = calloc(1, sizeof *f);
    if (!f)
        return nullptr;
    *f = (loopfit){.channels = channels, .in_rate = in_rate, .out_rate = out_rate, .block = block,
                   .max_hold = max_hold, .sink = sink, .ctx = ctx};
    return f;
}

/* Kaiser windowed sinc for a conversion that makes ratio output samples of each input sample;
 * when that shrinks the sound the cutoff follows the output Nyquist. */
static int make_table(loopfit *f, double ratio)
{
    double cutoff = (ratio < 1 ? ratio : 1.0) * 0.97;
    f->half = (int)ceil(TAPS / cutoff);
    f->width = 2 * f->half;
    free(f->table);
    f->table = malloc(sizeof(float) * (size_t)(PHASES + 1) * (size_t)f->width);
    if (!f->table)
        return -1;
    for (int p = 0; p <= PHASES; p++)
        for (int j = 0; j < f->width; j++) {
            double t = (j - f->half + 1) - (double)p / PHASES, x = cutoff * t;
            double sinc = fabs(x) < 1e-9 ? 1 : sin(PI * x) / (PI * x);
            double q = t / f->half;
            double w = fabs(q) >= 1 ? 0 : bessel_i0(8.0 * sqrt(1 - q * q)) / bessel_i0(8.0);
            f->table[(size_t)p * (size_t)f->width + (size_t)j] = (float)(cutoff * sinc * w);
        }
    return 0;
}

static int count_sink(const float *const *pcm, size_t samples, void *ctx)
{
    loopfit *f = ctx;
    f->sent += samples;
    return f->sink(pcm, samples, f->ctx);
}

/* The sound is too long to hold: send what is held through a streaming resampler and free it. */
static int start_stream(loopfit *f)
{
    f->stream = resampler_create(f->in_rate, f->out_rate, f->channels, count_sink, f);
    if (!f->stream)
        return -1;
    const float *held[2] = {f->buf[0], f->buf[1]};
    int rc = f->len ? resampler_feed(f->stream, held, f->len) : 0;
    free(f->buf[0]);
    free(f->buf[1]);
    f->buf[0] = f->buf[1] = nullptr;
    f->len = f->cap = 0;
    return rc;
}

int loopfit_feed(loopfit *f, const float *const *pcm, size_t samples)
{
    if (!f->stream && f->len + samples > f->max_hold && start_stream(f) != 0)
        return -1;
    if (f->stream)
        return resampler_feed(f->stream, pcm, samples);
    if (f->len + samples > f->cap) {
        size_t cap = f->cap ? f->cap : 1 << 16;
        while (cap < f->len + samples)
            cap *= 2;
        if (cap > f->max_hold)
            cap = f->max_hold;  /* at least len + samples, which is within max_hold here */
        for (int c = 0; c < f->channels; c++) {
            float *p = realloc(f->buf[c], cap * sizeof(float));
            if (!p)
                return -1;
            f->buf[c] = p;
        }
        f->cap = cap;
    }
    for (int c = 0; c < f->channels; c++)
        memcpy(f->buf[c] + f->len, pcm[c], samples * sizeof(float));
    f->len += samples;
    return 0;
}

/* Converts the held sound to out_len samples, reading input position j * step for output j.
 * wrap: the sound is a loop, so positions before 0 and past the end read its other end. */
static int render(loopfit *f, size_t out_len, double step, bool wrap)
{
    if (make_table(f, 1.0 / step) != 0)
        return -1;
    const float *outp[2] = {f->out[0], f->out[1]};
    long m = (long)f->len;
    size_t n = 0;
    for (size_t j = 0; j < out_len; j++) {
        double u = (double)j * step;
        long base = (long)floor(u);
        double fp = (u - (double)base) * PHASES;
        int p = (int)fp;
        float mix = (float)(fp - p);
        const float *w0 = f->table + (size_t)p * (size_t)f->width, *w1 = w0 + f->width;
        for (int c = 0; c < f->channels; c++) {
            double acc = 0;
            for (int k = 0; k < f->width; k++) {
                long i = base + k - f->half + 1;
                if (wrap)
                    i = ((i % m) + m) % m;
                else if (i < 0 || i >= m)
                    continue;
                acc += f->buf[c][i] * (w0[k] + (w1[k] - w0[k]) * mix);
            }
            f->out[c][n] = (float)acc;
        }
        if (++n == OUT_CHUNK) {
            if (f->sink(outp, n, f->ctx) != 0)
                return -1;
            n = 0;
        }
    }
    return n ? f->sink(outp, n, f->ctx) : 0;
}

/* Sends count samples of silence. */
static int silence(loopfit *f, size_t count)
{
    const float *outp[2] = {f->out[0], f->out[1]};
    memset(f->out, 0, sizeof f->out);
    while (count > 0) {
        size_t k = count < OUT_CHUNK ? count : OUT_CHUNK;
        if (f->sink(outp, k, f->ctx) != 0)
            return -1;
        count -= k;
    }
    return 0;
}

static size_t round_up(size_t n, size_t block)
{
    return (n + block - 1) / block * block;
}

int loopfit_finish(loopfit *f, loopfit_result *res)
{
    *res = (loopfit_result){.rate = f->out_rate};
    if (f->stream) {
        res->streamed = true;
        if (resampler_finish(f->stream) != 0)
            return -1;
        res->change = (long)(round_up(f->sent, f->block) - f->sent);
        return silence(f, (size_t)res->change);
    }
    if (f->len == 0)
        return 0;
    /* plain conversion length, and the nearest whole number of blocks */
    double nominal = (double)f->len * f->out_rate / f->in_rate;
    size_t plain = (size_t)llround(nominal);
    size_t blocks = (size_t)llround(nominal / (double)f->block);
    if (blocks == 0)
        blocks = 1;
    size_t fitted = blocks * f->block;
    /* the rate that plays fitted samples in the sound's own length */
    double rate = (double)f->in_rate * (double)fitted / (double)f->len;
    if (rate >= LOOPFIT_MIN_RATE && rate <= LOOPFIT_MAX_RATE) {
        res->change = (long)fitted - (long)plain;
        res->rate = (int)lround(rate);
        return render(f, fitted, (double)f->len / (double)fitted, true);
    }

    /* Too short for any rate XAudio2 plays: convert it unchanged and pad with silence. */
    size_t whole = round_up(plain, f->block);
    res->change = (long)(whole - plain);
    res->padded = true;
    if (render(f, plain, (double)f->in_rate / f->out_rate, false) != 0)
        return -1;
    return silence(f, whole - plain);
}

void loopfit_free(loopfit *f)
{
    if (!f)
        return;
    resampler_free(f->stream);
    free(f->buf[0]);
    free(f->buf[1]);
    free(f->table);
    free(f);
}
