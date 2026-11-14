// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adpcm_enc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The coefficient pairs and step adaptation table every MS ADPCM decoder uses. */
const int16_t adpcm_coef[7][2] = {
    {256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232},
};
static constexpr int ADAPT[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
static constexpr int MAX_DELTA = 32767;  /* the step stays within 16 bits, as every decoder stores it */

struct adpcm_encoder {
    int     channels;
    int16_t pcm[2][ADPCM_BLOCK_SAMPLES];
    int     fill;
    uint8_t block[524];
    adpcm_block_sink sink;
    void   *ctx;
};

int adpcm_block_align(int channels)
{
    return channels * (7 + (ADPCM_BLOCK_SAMPLES - 2) / 2);  /* 262 mono, 524 stereo */
}

static inline int clamp16(int v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

typedef struct {
    int s1, s2, delta;
} state;

/* Codes one sample: picks the nibble whose reconstruction is closest, keeping the step in range. */
static int code_sample(state *st, const int16_t *coef, int x)
{
    int pred = (st->s1 * coef[0] + st->s2 * coef[1]) >> 8;
    int err = x - pred;
    int n = (int)lround((double)err / st->delta);
    n = n > 7 ? 7 : n < -8 ? -8 : n;
    while (n != 0 && (ADAPT[n & 15] * st->delta) >> 8 > MAX_DELTA)
        n += n > 0 ? -1 : 1;
    int v = clamp16(pred + n * st->delta);
    st->s2 = st->s1;
    st->s1 = v;
    st->delta = (ADAPT[n & 15] * st->delta) >> 8;
    if (st->delta < 16)
        st->delta = 16;
    return n & 15;
}

/* Squared error of coding samples 2.. of x with this predictor and starting step. */
static double trial(const int16_t *x, int predictor, int delta)
{
    state st = {x[1], x[0], delta};
    double e = 0;
    for (int i = 2; i < ADPCM_BLOCK_SAMPLES; i++) {
        int before = x[i];
        code_sample(&st, adpcm_coef[predictor], before);
        double d = (double)(st.s1 - before);
        e += d * d;
    }
    return e;
}

static void put16(uint8_t *p, int v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static int encode_block(adpcm_encoder *e)
{
    int nch = e->channels;
    int best_pred[2] = {0, 0}, best_delta[2] = {16, 16};
    for (int c = 0; c < nch; c++) {
        const int16_t *x = e->pcm[c];
        /* starting step from the size of the first differences */
        int est = (abs(x[2] - x[1]) + abs(x[3] - x[2]) + abs(x[4] - x[3])) / 6;
        int seeds[3] = {est / 2, est, est * 2};
        double best = INFINITY;
        for (int p = 0; p < 7; p++)
            for (int s = 0; s < 3; s++) {
                int d = seeds[s] < 16 ? 16 : seeds[s] > MAX_DELTA ? MAX_DELTA : seeds[s];
                double err = trial(x, p, d);
                if (err < best) {
                    best = err;
                    best_pred[c] = p;
                    best_delta[c] = d;
                }
            }
    }
    /* header: predictor, step, second sample, first sample; channels interleaved per field */
    uint8_t *b = e->block;
    state st[2];
    for (int c = 0; c < nch; c++) {
        b[c] = (uint8_t)best_pred[c];
        put16(b + nch + 2 * c, best_delta[c]);
        put16(b + 3 * nch + 2 * c, e->pcm[c][1]);
        put16(b + 5 * nch + 2 * c, e->pcm[c][0]);
        st[c] = (state){e->pcm[c][1], e->pcm[c][0], best_delta[c]};
    }
    uint8_t *out = b + 7 * nch;
    int nibble = 0, high = 1;
    for (int i = 2; i < ADPCM_BLOCK_SAMPLES; i++)
        for (int c = 0; c < nch; c++) {
            int n = code_sample(&st[c], adpcm_coef[best_pred[c]], e->pcm[c][i]);
            if (high)
                nibble = n << 4;
            else
                *out++ = (uint8_t)(nibble | n);
            high = !high;
        }
    return e->sink(e->block, (size_t)adpcm_block_align(nch), e->ctx);
}

adpcm_encoder *adpcm_encoder_create(int channels, adpcm_block_sink sink, void *ctx)
{
    if (channels < 1 || channels > 2)
        return nullptr;
    adpcm_encoder *e = calloc(1, sizeof *e);
    if (!e)
        return nullptr;
    e->channels = channels;
    e->sink = sink;
    e->ctx = ctx;
    return e;
}

int adpcm_encoder_feed(adpcm_encoder *e, const float *const *pcm, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        for (int c = 0; c < e->channels; c++) {
            float v = pcm[c][i];
            if (!isfinite(v))
                v = 0.0f;
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            e->pcm[c][e->fill] = (int16_t)clamp16((int)lrintf(v * 32767.0f));
        }
        if (++e->fill == ADPCM_BLOCK_SAMPLES) {
            e->fill = 0;
            if (encode_block(e) != 0)
                return -1;
        }
    }
    return 0;
}

int adpcm_encoder_finish(adpcm_encoder *e)
{
    if (e->fill == 0)
        return 0;
    for (int c = 0; c < e->channels; c++)
        memset(e->pcm[c] + e->fill, 0, sizeof(int16_t) * (size_t)(ADPCM_BLOCK_SAMPLES - e->fill));
    e->fill = 0;
    return encode_block(e);
}

void adpcm_encoder_free(adpcm_encoder *e)
{
    free(e);
}
