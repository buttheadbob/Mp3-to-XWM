// SPDX-License-Identifier: AGPL-3.0-or-later
#include "xwm_writer.h"

#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

/* Packets are written to a temporary data file as they come. At the end the header and the
 * dpds table (which must come before the data chunk) go into a second temporary file, the
 * data is copied after them, and that file is renamed to the final name. An interrupted
 * run never leaves a truncated .xwm or destroys an older good one. */
struct xwm_writer {
    FILE    *data;
    uint64_t data_size;
    bool     failed;
    char     path[PATH_MAX_LEN];
    char     data_path[PATH_MAX_LEN];
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

xwm_writer *xwm_open(const char *path, char *err, int err_size)
{
    xwm_writer *w = calloc(1, sizeof *w);
    if (!w) {
        snprintf(err, (size_t)err_size, "out of memory");
        return nullptr;
    }
    int a = snprintf(w->path, sizeof w->path, "%s", path);
    int b = snprintf(w->data_path, sizeof w->data_path, "%s.data.part", path);
    int c = snprintf(w->tmp_path, sizeof w->tmp_path, "%s.part", path);
    if (a < 0 || b < 0 || c < 0 || (size_t)b >= sizeof w->data_path || (size_t)c >= sizeof w->tmp_path) {
        snprintf(err, (size_t)err_size, "output path too long");
        free(w);
        return nullptr;
    }
    w->data = plat_fopen(w->data_path, "wb");
    if (!w->data) {
        snprintf(err, (size_t)err_size, "cannot create output file");
        free(w);
        return nullptr;
    }
    return w;
}

int xwm_packet(const uint8_t *packet, size_t size, void *ctx)
{
    xwm_writer *w = ctx;
    if (fwrite(packet, 1, size, w->data) != size) {
        w->failed = true;
        return -1;
    }
    w->data_size += size;
    return 0;
}

void xwm_abort(xwm_writer *w)
{
    if (!w)
        return;
    if (w->data)
        fclose(w->data);
    plat_remove(w->data_path);
    plat_remove(w->tmp_path);
    free(w);
}

int xwm_close(xwm_writer *w, const wma_params *p, const uint32_t *dpds, size_t count, char *err, int err_size)
{
    uint64_t data_size = w->data_size;
    uint64_t dpds_size = (uint64_t)count * 4u;
    uint64_t riff_size = 4 + (8 + 18) + (8 + dpds_size) + (8 + data_size + (data_size & 1));
    if (riff_size > 0xFFFFFFFFu) {
        snprintf(err, (size_t)err_size, "output is larger than 4 GB");
        xwm_abort(w);
        return -1;
    }
    bool bad = w->failed || ferror(w->data);
    bad |= fclose(w->data) != 0;
    w->data = nullptr;
    FILE *in = bad ? nullptr : plat_fopen(w->data_path, "rb");
    FILE *f = in ? plat_fopen(w->tmp_path, "wb") : nullptr;
    if (!in || !f) {
        snprintf(err, (size_t)err_size, bad ? "error writing output file" : "cannot create output file");
        if (in)
            fclose(in);
        xwm_abort(w);
        return -1;
    }

    fwrite("RIFF", 1, 4, f);
    put32(f, (uint32_t)riff_size);
    fwrite("XWMA", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    put32(f, 18);
    put16(f, 0x0161);                        /* WAVE_FORMAT_WMAUDIO2 */
    put16(f, (unsigned)p->channels);
    put32(f, (uint32_t)p->sample_rate);
    put32(f, (uint32_t)(p->bitrate / 8));    /* average bytes per second */
    put16(f, (unsigned)p->block_align);
    put16(f, 16);                            /* bits per sample of the decoded audio */
    put16(f, 0);                             /* no extra format data */

    fwrite("dpds", 1, 4, f);
    put32(f, (uint32_t)dpds_size);
    for (size_t i = 0; i < count; i++)
        put32(f, dpds[i]);

    fwrite("data", 1, 4, f);
    put32(f, (uint32_t)data_size);
    static thread_local uint8_t buf[1 << 16];
    uint64_t copied = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        fwrite(buf, 1, n, f);
        copied += n;
    }
    if (data_size & 1)
        fputc(0, f);

    bad = ferror(in) || copied != data_size;
    fclose(in);
    bad |= ferror(f) != 0;
    bad |= fclose(f) != 0;
    if (bad || plat_rename(w->tmp_path, w->path) != 0) {
        snprintf(err, (size_t)err_size, bad ? "error writing output file" : "cannot replace the output file");
        xwm_abort(w);
        return -1;
    }
    plat_remove(w->data_path);
    free(w);
    return 0;
}
