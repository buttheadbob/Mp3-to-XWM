// SPDX-License-Identifier: AGPL-3.0-or-later
/* Resamples a whole sound as a loop so its length becomes an exact multiple of a block size:
 * the sound is held, then resampled to the nearest whole number of blocks. Samples before its
 * start and after its end are its own end and start, so the loop point is resampled like any
 * other point of the sound. The file then states the sample rate that plays those samples in
 * the sound's own time (loopfit_result.rate), so pitch and speed stay as they were.
 *
 * A sound longer than the hold limit is not held: it is converted at the plain output rate as
 * it comes and padded with silence to a whole block, so it will not loop seamlessly. */
#ifndef LOOPFIT_H
#define LOOPFIT_H

#include <stddef.h>

typedef int (*loopfit_sink)(const float *const *pcm, size_t samples, void *ctx);

/* Sample rates XAudio2 accepts for a source voice; a fit whose rate falls outside is padded. */
static constexpr int LOOPFIT_MIN_RATE = 1000;
static constexpr int LOOPFIT_MAX_RATE = 200000;

typedef struct {
    long change;     /* output samples added (negative: removed) against a plain conversion */
    int  rate;       /* sample rate to write into the file */
    bool padded;     /* too short to fit: converted unchanged, change samples of silence added */
    bool streamed;   /* longer than the hold limit: converted unchanged, change samples of silence added */
} loopfit_result;

typedef struct loopfit loopfit;

/* max_hold: the longest sound, in input samples per channel, that is held and fitted. */
loopfit *loopfit_create(int channels, int in_rate, int out_rate, size_t block, size_t max_hold,
                        loopfit_sink sink, void *ctx);
int  loopfit_feed(loopfit *f, const float *const *pcm, size_t samples);
/* Converts what is held (or the rest of a streamed sound) and sends it. Returns 0 on success. */
int  loopfit_finish(loopfit *f, loopfit_result *res);
void loopfit_free(loopfit *f);

#endif
