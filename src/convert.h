// SPDX-License-Identifier: AGPL-3.0-or-later
/* Converts one MP3 file to a sound file the game plays: xWMA, or MS ADPCM for gapless loops. */
#ifndef CONVERT_H
#define CONVERT_H

typedef enum {
    FORMAT_XWM,    /* WMA 2 in xWMA: small; a loop has a short gap at its end */
    FORMAT_ADPCM,  /* MS ADPCM: loops without a gap; about 7-8 times larger than 48 kbit/s xWMA */
} output_format;

/* xWMA output: 48 kbit/s with 2230 byte packets, the format of the game's own music. Stereo
 * files at 96 and 192 kbit/s (4459 and 8917 byte packets) did not play properly in the game. */
static constexpr int CONVERT_XWM_KBPS = 48;

/* ADPCM sounds up to this long are held and fitted as loops; longer ones are converted as they
 * come (holding 10 minutes of 48 kHz stereo takes about 230 MB). */
static constexpr int CONVERT_LOOP_SECONDS = 600;

typedef struct {
    output_format format;
} convert_options;

typedef struct {
    long   loop_change;           /* ADPCM: samples added (negative: removed) to fit whole blocks */
    int    loop_rate;             /* ADPCM: sample rate in the file, which keeps pitch and speed */
    bool   loop_padded;           /* ADPCM: too short to fit; loop_change samples of silence were added */
    bool   loop_streamed;         /* ADPCM: longer than CONVERT_LOOP_SECONDS; not fitted, loop_change
                                   * samples of silence were added */
} convert_result;

/* Returns 0 on success. On failure writes a reason into err. */
int convert_mp3(const char *in_path, const char *out_path, const convert_options *opt, convert_result *res,
                char *err, int err_size);

#endif
