// SPDX-License-Identifier: AGPL-3.0-or-later
/* Sample rate conversion with a windowed sinc filter, fed in pieces. */
#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <stddef.h>

/* Receives converted planar samples. Returns 0 to go on. */
typedef int (*resample_sink)(const float *const *pcm, size_t samples, void *ctx);

typedef struct resampler resampler;

resampler *resampler_create(int in_rate, int out_rate, int channels, resample_sink sink, void *ctx);
/* Converts planar input; output goes to the sink as it becomes ready. Returns 0 on success. */
int  resampler_feed(resampler *r, const float *const *in, size_t samples);
/* Converts the rest (reading past the end as silence). */
int  resampler_finish(resampler *r);
void resampler_free(resampler *r);

#endif
