// SPDX-License-Identifier: AGPL-3.0-or-later
/* Forward MDCT: 2N input samples to N coefficients, computed through an N/2 point FFT.
 * X[k] = sum_{n=0}^{2N-1} x[n] cos(pi/N (n + 1/2 + N/2)(k + 1/2)) */
#ifndef MDCT_H
#define MDCT_H

typedef struct {
    int    n;        /* number of coefficients (N) */
    float *twiddle;  /* N/2 complex FFT twiddles */
    float *pre;      /* N/2 complex pre-rotation */
    float *post;     /* N/2 complex post-rotation */
    float *buf;      /* N/2 complex work buffer + N reals */
} mdct_plan;

bool mdct_init(mdct_plan *p, int n);   /* n must be a power of two, >= 16 */
void mdct_free(mdct_plan *p);
void mdct_forward(mdct_plan *p, const float *in, float *out);

#endif
