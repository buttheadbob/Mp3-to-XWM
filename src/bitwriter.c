// SPDX-License-Identifier: AGPL-3.0-or-later
#include "bitwriter.h"

#include <stdlib.h>
#include <string.h>

void bw_init(bitwriter *bw)
{
    *bw = (bitwriter){0};
}

void bw_free(bitwriter *bw)
{
    free(bw->data);
    *bw = (bitwriter){0};
}

static bool bw_reserve(bitwriter *bw, size_t extra_bits)
{
    size_t need = (bw->bits + extra_bits + 7) / 8;
    if (need <= bw->cap)
        return true;
    size_t cap = bw->cap ? bw->cap : 4096;
    while (cap < need)
        cap *= 2;
    uint8_t *p = realloc(bw->data, cap);
    if (!p) {
        bw->failed = true;
        return false;
    }
    memset(p + bw->cap, 0, cap - bw->cap);
    bw->data = p;
    bw->cap = cap;
    return true;
}

void bw_put(bitwriter *bw, int nbits, uint32_t value)
{
    if (nbits <= 0 || !bw_reserve(bw, (size_t)nbits))
        return;
    for (int i = nbits - 1; i >= 0; i--) {
        if ((value >> i) & 1u)
            bw->data[bw->bits >> 3] |= (uint8_t)(0x80u >> (bw->bits & 7));
        bw->bits++;
    }
}

void bw_drop_front(bitwriter *bw, size_t n)
{
    if (n > bw->bits / 8)
        n = bw->bits / 8;  /* only whole bytes that are fully written */
    if (n == 0)
        return;
    size_t used = (bw->bits + 7) / 8;
    memmove(bw->data, bw->data + n, used - n);
    memset(bw->data + used - n, 0, n);
    bw->bits -= n * 8;
}

void bw_copy(bitwriter *bw, const uint8_t *src, size_t src_bit, size_t nbits)
{
    if (!bw_reserve(bw, nbits))
        return;
    for (size_t i = 0; i < nbits; i++) {
        size_t s = src_bit + i;
        if ((src[s >> 3] >> (7 - (s & 7))) & 1u)
            bw->data[bw->bits >> 3] |= (uint8_t)(0x80u >> (bw->bits & 7));
        bw->bits++;
    }
}
