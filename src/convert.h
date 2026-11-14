// SPDX-License-Identifier: AGPL-3.0-or-later
/* Converts one MP3 file to a sound file the game plays: xWMA, or MS ADPCM for gapless loops. */
#ifndef CONVERT_H
#define CONVERT_H

typedef enum {
    FORMAT_XWM,    /* WMA 2 in xWMA: small; a loop has a short gap at its end */
    FORMAT_ADPCM,  /* MS ADPCM: loops without a gap; about 7-8 times larger than 48 kbit/s xWMA */
} output_format;

/* Bit rates offered for stereo xWMA output, in kbit/s. Mono xWMA is always 48 kbit/s. */
static constexpr int CONVERT_RATES[3] = {48, 96, 192};

/* ADPCM sounds up to this long are held and fitted as loops; longer ones are converted as they
 * come (holding 10 minutes of 48 kHz stereo takes about 230 MB). */
static constexpr int CONVERT_LOOP_SECONDS = 600;

typedef struct {
    output_format format;
    int           kbps;           /* xWMA only: one of CONVERT_RATES */
} convert_options;

typedef struct {
    int    kbps;                  /* xWMA: the rate written */
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
