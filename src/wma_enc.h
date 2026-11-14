// SPDX-License-Identifier: AGPL-3.0-or-later
/* WMA version 2 encoder producing xWMA packets (bit reservoir superframes), fed in pieces. */
#ifndef WMA_ENC_H
#define WMA_ENC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int sample_rate;   /* 44100 or 48000 */
    int channels;      /* 1 or 2 */
    int bitrate;       /* bits per second, as stored in the file header */
    int block_align;   /* packet size in bytes */
} wma_params;

/* Receives each finished packet of block_align bytes. Returns 0 on success. */
typedef int (*wma_packet_sink)(const uint8_t *packet, size_t size, void *ctx);

typedef struct wma_encoder wma_encoder;

wma_encoder *wma_encoder_create(const wma_params *p, wma_packet_sink sink, void *ctx, char *err, int err_size);
/* Encodes planar float samples (range -1..1). Any number of samples per call. */
int  wma_encoder_feed(wma_encoder *e, const float *const *pcm, size_t samples, char *err, int err_size);
/* Encodes the rest and sends the last packets. */
int  wma_encoder_finish(wma_encoder *e, char *err, int err_size);
/* Decoded byte count (16-bit PCM) after each packet sent, for the dpds chunk. */
const uint32_t *wma_encoder_dpds(const wma_encoder *e, size_t *count);
void wma_encoder_free(wma_encoder *e);

#endif
