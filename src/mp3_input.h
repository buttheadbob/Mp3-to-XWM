// SPDX-License-Identifier: AGPL-3.0-or-later
/* Reads an MP3 file piece by piece as planar float samples, with gapless trimming. */
#ifndef MP3_INPUT_H
#define MP3_INPUT_H

#include <stddef.h>

typedef struct mp3_reader mp3_reader;

/* Opens the file and decodes up to its first audio frame to learn the format. */
mp3_reader *mp3_reader_open(const char *path, char *err, int err_size);
int  mp3_reader_sample_rate(const mp3_reader *r);
int  mp3_reader_channels(const mp3_reader *r);   /* 1 or 2 */
/* Writes up to max samples per channel into out[0..channels-1]. Returns the count,
 * 0 at the end of the audio, or -1 on error. */
long mp3_reader_read(mp3_reader *r, float *const out[2], size_t max, char *err, int err_size);
void mp3_reader_close(mp3_reader *r);

#endif
