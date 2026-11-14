// SPDX-License-Identifier: AGPL-3.0-or-later
/* Writes an xWMA (.xwm) RIFF file (fmt, dpds and data chunks) from packets as they come. */
#ifndef XWM_WRITER_H
#define XWM_WRITER_H

#include "wma_enc.h"

typedef struct xwm_writer xwm_writer;

/* Starts the output. Nothing appears under path until xwm_close succeeds. */
xwm_writer *xwm_open(const char *path, char *err, int err_size);
/* Packet sink for the encoder; ctx is the xwm_writer. */
int  xwm_packet(const uint8_t *packet, size_t size, void *ctx);
/* Writes the finished file and frees w. Returns 0 on success; on failure nothing is left behind. */
int  xwm_close(xwm_writer *w, const wma_params *p, const uint32_t *dpds, size_t count, char *err, int err_size);
/* Discards the output and frees w. */
void xwm_abort(xwm_writer *w);

#endif
