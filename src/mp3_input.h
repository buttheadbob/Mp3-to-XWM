// SPDX-License-Identifier: AGPL-3.0-or-later
/* Reads an MP3 file piece by piece as mono float samples (stereo is mixed down to the mean of
 * left and right), with gapless trimming. */
#ifndef MP3_INPUT_H
#define MP3_INPUT_H

#include <stddef.h>

typedef struct mp3_reader mp3_reader;

/* Opens the file and decodes up to its first audio frame to learn the format. */
mp3_reader *mp3_reader_open(const char *path, char *err, int err_size);
int  mp3_reader_sample_rate(const mp3_reader *r);
/* Writes up to max samples into out. Returns the count, 0 at the end of the audio, or -1 on error. */
long mp3_reader_read(mp3_reader *r, float *out, size_t max, char *err, int err_size);
void mp3_reader_close(mp3_reader *r);

#endif
