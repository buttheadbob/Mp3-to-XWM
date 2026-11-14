// SPDX-License-Identifier: AGPL-3.0-or-later
/* Converts one MP3 file to an xWMA (.xwm) file. */
#ifndef CONVERT_H
#define CONVERT_H

/* Bit rates offered for stereo output, in kbit/s. Mono output is always 48 kbit/s. */
static constexpr int CONVERT_RATES[3] = {48, 96, 192};

/* Converts at the requested bit rate (one of CONVERT_RATES). Returns 0 on success and sets
 * *used_kbps to the rate written. On failure writes a reason into err. */
int convert_mp3_to_xwm(const char *in_path, const char *out_path, int kbps, int *used_kbps,
                       char *err, int err_size);

#endif
