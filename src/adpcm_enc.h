// SPDX-License-Identifier: AGPL-3.0-or-later
/* Microsoft ADPCM (WAVE_FORMAT_ADPCM, tag 2) encoder, fed in pieces. Blocks hold 512 samples per
 * channel: 256 bytes per channel per block, plus a 7-byte header per channel (262 mono, 524 stereo),
 * the layout of the game's own ADPCM sounds. */
#ifndef ADPCM_ENC_H
#define ADPCM_ENC_H

#include <stddef.h>
#include <stdint.h>

static constexpr int ADPCM_BLOCK_SAMPLES = 512;

/* Receives each finished block. Returns 0 on success. */
typedef int (*adpcm_block_sink)(const uint8_t *block, size_t size, void *ctx);

typedef struct adpcm_encoder adpcm_encoder;

adpcm_encoder *adpcm_encoder_create(int channels, adpcm_block_sink sink, void *ctx);
/* Encodes planar float samples (range -1..1). Any number per call. */
int  adpcm_encoder_feed(adpcm_encoder *e, const float *const *pcm, size_t samples);
/* Encodes the last partial block, padded with silence. */
int  adpcm_encoder_finish(adpcm_encoder *e);
int  adpcm_block_align(int channels);
void adpcm_encoder_free(adpcm_encoder *e);

/* The standard coefficient pairs, written into the format header. */
extern const int16_t adpcm_coef[7][2];

#endif
