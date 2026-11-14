// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mp3_input.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp3dec.h"
#include "platform.h"

static constexpr int    DECODER_DELAY = 529;     /* samples added by the MP3 synthesis filter */
static constexpr size_t BUF_SIZE      = 1 << 16;
static constexpr size_t LOOKAHEAD     = 1 << 14; /* > two largest frames and the free format search */
static constexpr int    MAX_PADDING   = 4095;    /* largest padding a LAME tag can state */

struct mp3_reader {
    FILE       *f;
    uint8_t     buf[BUF_SIZE];
    size_t      pos, len;     /* unread bytes are buf[pos .. len) */
    bool        eof;
    bool        read_error;
    bool        id3v1;        /* the file ends with a 128 byte ID3v1 tag */
    mp3_decoder dec;
    float       frame[MP3_MAX_SAMPLES * 2];
    int         rate, channels;
    size_t      head_skip;    /* samples still to drop at the start (gapless) */
    size_t      tail;         /* samples to drop at the end (gapless) */
    float      *fifo[2];      /* decoded samples held back until more than `tail` are waiting */
    size_t      fifo_cap, fifo_start, fifo_count;
    bool        done;
};

/* Keeps at least LOOKAHEAD bytes ahead of the decoder unless the file ends. */
static void refill(mp3_reader *r)
{
    if (r->eof || r->len - r->pos >= LOOKAHEAD)
        return;
    memmove(r->buf, r->buf + r->pos, r->len - r->pos);
    r->len -= r->pos;
    r->pos = 0;
    r->len += fread(r->buf + r->len, 1, BUF_SIZE - r->len, r->f);
    if (ferror(r->f)) {
        r->read_error = true;
        r->eof = true;
        return;
    }
    if (r->len < BUF_SIZE) {
        r->eof = true;
        if (r->id3v1 && r->len - r->pos >= 128 && !memcmp(r->buf + r->len - 128, "TAG", 3))
            r->len -= 128;
    }
}

/* Skips ID3v2 tags at the start of the file. */
static void skip_id3v2(mp3_reader *r)
{
    for (;;) {
        refill(r);
        const uint8_t *h = r->buf + r->pos;
        if (r->len - r->pos < 10 || memcmp(h, "ID3", 3) || h[3] == 0xFF)
            return;
        size_t size = ((size_t)(h[6] & 0x7F) << 21) | ((size_t)(h[7] & 0x7F) << 14)
                    | ((size_t)(h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        size += 10 + ((h[5] & 0x10) ? 10 : 0);
        while (size > 0 && r->len > r->pos) {
            size_t step = r->len - r->pos < size ? r->len - r->pos : size;
            r->pos += step;
            size -= step;
            refill(r);
        }
    }
}

/* Limits a decoded sample to -1..1; anything not finite becomes silence. */
static inline float clean(float v)
{
    if (!isfinite(v))
        return 0.0f;
    return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

/* Adds a frame of in_ch interleaved channels to the FIFO, converted to the stream's channel
 * count (mono frames are doubled, stereo frames are mixed down). */
static bool fifo_push(mp3_reader *r, const float *pcm, int n, int in_ch, size_t skip)
{
    for (size_t i = skip; i < (size_t)n; i++) {
        if (r->fifo_count == r->fifo_cap)
            return false;
        size_t at = (r->fifo_start + r->fifo_count) % r->fifo_cap;
        const float *s = pcm + i * (size_t)in_ch;
        if (r->channels == in_ch) {
            for (int ch = 0; ch < in_ch; ch++)
                r->fifo[ch][at] = clean(s[ch]);
        } else if (r->channels == 2) {
            r->fifo[0][at] = r->fifo[1][at] = clean(s[0]);
        } else {
            r->fifo[0][at] = clean(0.5f * (s[0] + s[1]));
        }
        r->fifo_count++;
    }
    return true;
}

/* Decodes the next frame into the FIFO. Returns 1 on success, 0 at the end, -1 on error. */
static int decode_next(mp3_reader *r, char *err, int err_size)
{
    for (;;) {
        refill(r);
        if (r->read_error) {
            snprintf(err, (size_t)err_size, "error reading file");
            return -1;
        }
        if (r->pos >= r->len)
            return 0;
        mp3_frame info;
        int n = mp3_decode_frame(&r->dec, r->buf + r->pos, r->len - r->pos, r->frame, &info);
        if (n < 0) {
            if (r->eof)
                return 0;
            /* no frame in this window: skip it, but keep the end, where a frame may start */
            size_t keep = 8192;
            r->pos = r->len - r->pos > keep ? r->len - keep : r->len;
            continue;
        }
        r->pos += info.skipped + info.frame_bytes;
        if (info.info_frame) {
            if (info.encoder_delay >= 0) {
                r->head_skip = (size_t)info.encoder_delay + DECODER_DELAY;
                r->tail = info.encoder_padding > DECODER_DELAY ? (size_t)(info.encoder_padding - DECODER_DELAY) : 0;
            }
            continue;
        }
        if (n == 0)
            continue;
        if (r->channels == 0) {
            r->rate = info.sample_rate;
            r->channels = info.channels;
        } else if (info.sample_rate != r->rate) {
            snprintf(err, (size_t)err_size, "sample rate changes inside the file");
            return -1;
        }
        size_t skip = r->head_skip < (size_t)n ? r->head_skip : (size_t)n;
        r->head_skip -= skip;
        if (!fifo_push(r, r->frame, n, info.channels, skip)) {
            snprintf(err, (size_t)err_size, "internal buffer overflow");
            return -1;
        }
        return 1;
    }
}

mp3_reader *mp3_reader_open(const char *path, char *err, int err_size)
{
    mp3_reader *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, (size_t)err_size, "out of memory");
        return nullptr;
    }
    r->fifo_cap = (size_t)MAX_PADDING + MP3_MAX_SAMPLES + 1;
    r->fifo[0] = malloc(r->fifo_cap * sizeof(float));
    r->fifo[1] = malloc(r->fifo_cap * sizeof(float));
    r->f = plat_fopen(path, "rb");
    if (!r->fifo[0] || !r->fifo[1] || !r->f) {
        snprintf(err, (size_t)err_size, r->f ? "out of memory" : "cannot read file");
        mp3_reader_close(r);
        return nullptr;
    }
    char tag[3];
    r->id3v1 = fseek(r->f, -128, SEEK_END) == 0 && fread(tag, 1, 3, r->f) == 3 && !memcmp(tag, "TAG", 3);
    rewind(r->f);
    mp3_init(&r->dec);
    skip_id3v2(r);
    /* decode until the first audio, which sets the format */
    int got;
    while ((got = decode_next(r, err, err_size)) > 0 && r->channels == 0)
        ;
    if (got < 0 || r->channels == 0) {
        if (got >= 0)
            snprintf(err, (size_t)err_size, "no MPEG audio found");
        mp3_reader_close(r);
        return nullptr;
    }
    return r;
}

int mp3_reader_sample_rate(const mp3_reader *r)
{
    return r->rate;
}

int mp3_reader_channels(const mp3_reader *r)
{
    return r->channels;
}

long mp3_reader_read(mp3_reader *r, float *const out[2], size_t max, char *err, int err_size)
{
    size_t produced = 0;
    while (produced < max) {
        /* samples beyond the held-back tail can go out */
        size_t ready = r->fifo_count > r->tail ? r->fifo_count - r->tail : 0;
        if (ready) {
            size_t take = ready < max - produced ? ready : max - produced;
            for (size_t i = 0; i < take; i++) {
                size_t at = (r->fifo_start + i) % r->fifo_cap;
                for (int ch = 0; ch < r->channels; ch++)
                    out[ch][produced + i] = r->fifo[ch][at];
            }
            r->fifo_start = (r->fifo_start + take) % r->fifo_cap;
            r->fifo_count -= take;
            produced += take;
            continue;
        }
        if (r->done)
            break;
        int got = decode_next(r, err, err_size);
        if (got < 0)
            return -1;
        if (got == 0)
            r->done = true;  /* the samples still held are the gapless tail: dropped */
    }
    return (long)produced;
}

void mp3_reader_close(mp3_reader *r)
{
    if (!r)
        return;
    if (r->f)
        fclose(r->f);
    free(r->fifo[0]);
    free(r->fifo[1]);
    free(r);
}
