// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wav_writer.h"

#include <stdio.h>
#include <stdlib.h>

#include "adpcm_enc.h"
#include "platform.h"

static constexpr uint32_t HEADER_BYTES = 12 + 8 + 50 + 8;  /* RIFF/WAVE, fmt chunk, data chunk header */

struct wav_writer {
    FILE    *f;
    uint64_t data_size;
    int      channels, sample_rate;
    bool     failed;
    char     path[PATH_MAX_LEN];
    char     tmp_path[PATH_MAX_LEN];
};

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

static void put32(FILE *f, uint32_t v)
{
    put16(f, v & 0xFFFF);
    put16(f, v >> 16);
}

/* RIFF header with the given data size. */
static void write_header(FILE *f, int channels, int sample_rate, uint32_t data_size)
{
    int align = adpcm_block_align(channels);
    fwrite("RIFF", 1, 4, f);
    put32(f, HEADER_BYTES - 8 + data_size + (data_size & 1));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put32(f, 50);
    put16(f, 2);                                       /* WAVE_FORMAT_ADPCM */
    put16(f, (unsigned)channels);
    put32(f, (uint32_t)sample_rate);
    put32(f, (uint32_t)((uint64_t)sample_rate * (uint64_t)align / ADPCM_BLOCK_SAMPLES));
    put16(f, (unsigned)align);
    put16(f, 4);                                       /* bits per sample */
    put16(f, 32);                                      /* extra format bytes */
    put16(f, ADPCM_BLOCK_SAMPLES);                     /* samples per block */
    put16(f, 7);                                       /* coefficient pairs */
    for (int i = 0; i < 7; i++) {
        put16(f, (unsigned)(uint16_t)adpcm_coef[i][0]);
        put16(f, (unsigned)(uint16_t)adpcm_coef[i][1]);
    }
    fwrite("data", 1, 4, f);
    put32(f, data_size);
}

wav_writer *wav_open(const char *path, int channels, int sample_rate, char *err, int err_size)
{
    wav_writer *w = calloc(1, sizeof *w);
    if (!w) {
        snprintf(err, (size_t)err_size, "out of memory");
        return nullptr;
    }
    int a = snprintf(w->path, sizeof w->path, "%s", path);
    int b = snprintf(w->tmp_path, sizeof w->tmp_path, "%s.part", path);
    if (a < 0 || b < 0 || (size_t)b >= sizeof w->tmp_path) {
        snprintf(err, (size_t)err_size, "output path too long");
        free(w);
        return nullptr;
    }
    w->f = plat_fopen(w->tmp_path, "wb");
    if (!w->f) {
        snprintf(err, (size_t)err_size, "cannot create output file");
        free(w);
        return nullptr;
    }
    w->channels = channels;
    w->sample_rate = sample_rate;
    write_header(w->f, channels, sample_rate, 0);  /* sizes are filled in when the file is complete */
    return w;
}

int wav_block(const uint8_t *block, size_t size, void *ctx)
{
    wav_writer *w = ctx;
    if (fwrite(block, 1, size, w->f) != size) {
        w->failed = true;
        return -1;
    }
    w->data_size += size;
    return 0;
}

void wav_set_rate(wav_writer *w, int sample_rate)
{
    w->sample_rate = sample_rate;
}

void wav_abort(wav_writer *w)
{
    if (!w)
        return;
    if (w->f)
        fclose(w->f);
    plat_remove(w->tmp_path);
    free(w);
}

int wav_close(wav_writer *w, char *err, int err_size)
{
    if (w->data_size + HEADER_BYTES > 0xFFFFFFFFu) {
        snprintf(err, (size_t)err_size, "output is larger than 4 GB");
        wav_abort(w);
        return -1;
    }
    uint32_t data_size = (uint32_t)w->data_size;
    if (data_size & 1)
        fputc(0, w->f);
    /* fill in the sizes and the rate: RIFF size at offset 4, sample rate and bytes per second
     * at 24, data size at the end of the header */
    bool bad = w->failed || fseek(w->f, 4, SEEK_SET) != 0;
    if (!bad) {
        put32(w->f, HEADER_BYTES - 8 + data_size + (data_size & 1));
        bad = fseek(w->f, 24, SEEK_SET) != 0;
    }
    if (!bad) {
        put32(w->f, (uint32_t)w->sample_rate);
        put32(w->f, (uint32_t)((uint64_t)w->sample_rate * (uint64_t)adpcm_block_align(w->channels)
                               / ADPCM_BLOCK_SAMPLES));
        bad = fseek(w->f, HEADER_BYTES - 4, SEEK_SET) != 0;
    }
    if (!bad)
        put32(w->f, data_size);
    bad |= ferror(w->f) != 0;
    bad |= fclose(w->f) != 0;
    w->f = nullptr;
    if (bad || plat_rename(w->tmp_path, w->path) != 0) {
        snprintf(err, (size_t)err_size, bad ? "error writing output file" : "cannot replace the output file");
        wav_abort(w);
        return -1;
    }
    free(w);
    return 0;
}
