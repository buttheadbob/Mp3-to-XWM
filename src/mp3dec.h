// SPDX-License-Identifier: AGPL-3.0-or-later
/* MPEG-1, MPEG-2 and MPEG-2.5 audio decoder: layers I, II and III, including free format. */
#ifndef MP3DEC_H
#define MP3DEC_H

#include <stddef.h>
#include <stdint.h>

#define MP3_MAX_SAMPLES 1152   /* per channel per frame */

typedef struct {
    int    sample_rate;
    int    channels;     /* 1 or 2 */
    size_t skipped;      /* bytes before the frame that were not audio */
    size_t frame_bytes;  /* bytes of the frame itself */
    bool   info_frame;   /* the frame holds a Xing/Info header instead of audio */
    int    encoder_delay, encoder_padding;  /* from a LAME tag, -1 if none */
} mp3_frame;

typedef struct {
    float    overlap[2][32][18];  /* IMDCT overlap per channel and subband */
    float    v[2][1024];          /* synthesis filter history */
    int      v_pos[2];
    uint8_t  reservoir[4096];     /* main data of earlier frames */
    int      reservoir_len;
    bool     have_stream;         /* a frame with a confirmed header was seen */
    uint32_t stream_header;       /* fixed header bits of the stream */
    int      free_bytes;          /* free format frame length without padding, once known */
    int      last_channels;       /* channel count of the previous frame */
} mp3_decoder;

void mp3_init(mp3_decoder *d);

/* Finds and decodes the next frame in buf. Writes interleaved samples (nominally -1..1;
 * hostile input can give any value) and returns the number per channel: 0 for an info
 * frame, for a frame whose bit reservoir data is missing, or for impossible side
 * information. Returns -1 when buf holds no further frame. info->skipped +
 * info->frame_bytes bytes are used. The channel count may change from frame to frame. */
int mp3_decode_frame(mp3_decoder *d, const uint8_t *buf, size_t len, float *pcm, mp3_frame *info);

#endif
