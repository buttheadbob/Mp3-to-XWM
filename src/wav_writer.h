// SPDX-License-Identifier: AGPL-3.0-or-later
/* Writes an MS ADPCM RIFF WAVE file (fmt and data chunks, the layout of the game's own ADPCM
 * sounds) from blocks as they come. */
#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stddef.h>
#include <stdint.h>

typedef struct wav_writer wav_writer;

/* Starts the output. Nothing appears under path until wav_close succeeds. */
wav_writer *wav_open(const char *path, int channels, int sample_rate, char *err, int err_size);
/* Block sink for the ADPCM encoder; ctx is the wav_writer. */
int  wav_block(const uint8_t *block, size_t size, void *ctx);
/* Sets the sample rate written into the header when the file is finished. */
void wav_set_rate(wav_writer *w, int sample_rate);
/* Finishes the file and frees w. Returns 0 on success; on failure nothing is left behind. */
int  wav_close(wav_writer *w, char *err, int err_size);
/* Discards the output and frees w. */
void wav_abort(wav_writer *w);

#endif
