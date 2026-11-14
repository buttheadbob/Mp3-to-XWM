// SPDX-License-Identifier: AGPL-3.0-or-later
/* MSB-first bit writer over a growable byte buffer. */
#ifndef BITWRITER_H
#define BITWRITER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t   cap;    /* bytes allocated */
    size_t   bits;   /* bits written */
    bool     failed; /* set when an allocation fails */
} bitwriter;

void bw_init(bitwriter *bw);
void bw_free(bitwriter *bw);
void bw_put(bitwriter *bw, int nbits, uint32_t value);   /* nbits 0..32 */
/* Removes the first n bytes; the bits after them move to the front. */
void bw_drop_front(bitwriter *bw, size_t n);
/* Appends nbits bits of src starting at bit offset src_bit. */
void bw_copy(bitwriter *bw, const uint8_t *src, size_t src_bit, size_t nbits);

#endif
