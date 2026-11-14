// SPDX-License-Identifier: AGPL-3.0-or-later
#include "resample.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static constexpr double PI     = 3.14159265358979323846;
static constexpr int    TAPS   = 32;    /* filter half length in input samples (at cutoff 1) */
static constexpr int    PHASES = 512;   /* filter table resolution between input samples */
static constexpr size_t CHUNK  = 4096;  /* input samples taken per step */
static constexpr size_t OUT_CHUNK = 1024;

struct resampler {
    int      in_rate, out_rate, channels;
    int      half, width;     /* filter half length and length in input samples */
    float   *table;           /* table[p * width + j]: weight of input sample j - half + 1 at phase p */
    float   *hist[2];         /* input samples [hist_start, hist_start + hist_len) */
    size_t   hist_len, hist_cap;
    int64_t  hist_start, total_in, next_out;
    float    out[2][OUT_CHUNK];
    resample_sink sink;
    void    *ctx;
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

resampler *resampler_create(int in_rate, int out_rate, int channels, resample_sink sink, void *ctx)
{
    resampler *r = calloc(1, sizeof *r);
    if (!r)
        return nullptr;
    *r = (resampler){.in_rate = in_rate, .out_rate = out_rate, .channels = channels, .sink = sink, .ctx = ctx};
    if (in_rate == out_rate)
        return r;  /* samples pass straight through */

    double ratio = (double)out_rate / in_rate;
    double cutoff = ratio < 1 ? ratio * 0.97 : 0.97;  /* normalised to the input Nyquist */
    r->half = (int)ceil(TAPS / cutoff);
    r->width = 2 * r->half;
    const double beta = 9.0;  /* Kaiser window */
    r->table = malloc(sizeof(float) * (size_t)(PHASES + 1) * (size_t)r->width);
    r->hist_cap = (size_t)r->width + CHUNK;
    bool ok = r->table != nullptr;
    for (int ch = 0; ch < channels; ch++)
        ok = ok && (r->hist[ch] = malloc(sizeof(float) * r->hist_cap)) != nullptr;
    if (!ok) {
        resampler_free(r);
        return nullptr;
    }
    for (int p = 0; p <= PHASES; p++) {
        double frac = (double)p / PHASES;
        for (int j = 0; j < r->width; j++) {
            double t = (j - r->half + 1) - frac;
            double x = t * cutoff;
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(PI * x) / (PI * x);
            double q = t / r->half;
            double w = fabs(q) >= 1 ? 0 : bessel_i0(beta * sqrt(1 - q * q)) / bessel_i0(beta);
            r->table[(size_t)p * (size_t)r->width + (size_t)j] = (float)(cutoff * sinc * w);
        }
    }
    return r;
}

/* Produces outputs up to (not including) limit, or while the input they need is there. */
static int produce(resampler *r, int64_t limit, bool finishing)
{
    const float *outp[2] = {r->out[0], r->out[1]};
    size_t n = 0;
    while (r->next_out < limit) {
        int64_t num = r->next_out * r->in_rate;
        int64_t base = num / r->out_rate;
        if (!finishing && base + r->half >= r->total_in)
            break;  /* needs input that has not come yet */
        double fp = (double)(num % r->out_rate) / r->out_rate * PHASES;
        int p = (int)fp;
        float mix = (float)(fp - p);
        const float *w0 = r->table + (size_t)p * (size_t)r->width;
        const float *w1 = w0 + r->width;
        for (int ch = 0; ch < r->channels; ch++) {
            double acc = 0;
            for (int j = 0; j < r->width; j++) {
                int64_t k = base + j - r->half + 1 - r->hist_start;
                if (k < 0 || k >= (int64_t)r->hist_len)
                    continue;  /* before the start or past the end: silence */
                acc += r->hist[ch][k] * (w0[j] + (w1[j] - w0[j]) * mix);
            }
            r->out[ch][n] = (float)acc;
        }
        r->next_out++;
        if (++n == OUT_CHUNK) {
            if (r->sink(outp, n, r->ctx) != 0)
                return -1;
            n = 0;
        }
    }
    if (n && r->sink(outp, n, r->ctx) != 0)
        return -1;
    /* drop input that no later output needs */
    int64_t keep = (r->next_out * r->in_rate) / r->out_rate - r->half + 1;
    if (keep > r->hist_start) {
        size_t drop = (size_t)(keep - r->hist_start);
        if (drop > r->hist_len)
            drop = r->hist_len;
        for (int ch = 0; ch < r->channels; ch++)
            memmove(r->hist[ch], r->hist[ch] + drop, (r->hist_len - drop) * sizeof(float));
        r->hist_len -= drop;
        r->hist_start += (int64_t)drop;
    }
    return 0;
}

int resampler_feed(resampler *r, const float *const *in, size_t samples)
{
    if (r->in_rate == r->out_rate)
        return samples ? r->sink(in, samples, r->ctx) : 0;
    size_t done = 0;
    while (done < samples) {
        size_t take = r->hist_cap - r->hist_len;
        if (take > samples - done)
            take = samples - done;
        for (int ch = 0; ch < r->channels; ch++)
            memcpy(r->hist[ch] + r->hist_len, in[ch] + done, take * sizeof(float));
        r->hist_len += take;
        r->total_in += (int64_t)take;
        done += take;
        if (produce(r, INT64_MAX, false) != 0)
            return -1;
    }
    return 0;
}

int resampler_finish(resampler *r)
{
    if (r->in_rate == r->out_rate)
        return 0;
    return produce(r, r->total_in * r->out_rate / r->in_rate, true);
}

void resampler_free(resampler *r)
{
    if (!r)
        return;
    free(r->table);
    free(r->hist[0]);
    free(r->hist[1]);
    free(r);
}
