// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mdct.h"

#include <math.h>
#include <stdlib.h>

static constexpr double PI = 3.14159265358979323846;

bool mdct_init(mdct_plan *p, int n)
{
    int m = n / 2;
    *p = (mdct_plan){.n = n};
    p->twiddle = malloc(sizeof(float) * (size_t)m);       /* m/2 complex */
    p->pre     = malloc(sizeof(float) * 2 * (size_t)m);
    p->post    = malloc(sizeof(float) * 2 * (size_t)m);
    p->buf     = malloc(sizeof(float) * (2 * (size_t)m + (size_t)n));
    if (!p->twiddle || !p->pre || !p->post || !p->buf) {
        mdct_free(p);
        return false;
    }
    for (int i = 0; i < m / 2; i++) {
        double a = -2.0 * PI * i / m;
        p->twiddle[2 * i]     = (float)cos(a);
        p->twiddle[2 * i + 1] = (float)sin(a);
    }
    for (int i = 0; i < m; i++) {
        double a = -PI * i / n;
        double b = -PI * (i + 0.25) / n;
        p->pre[2 * i]      = (float)cos(a);
        p->pre[2 * i + 1]  = (float)sin(a);
        p->post[2 * i]     = (float)cos(b);
        p->post[2 * i + 1] = (float)sin(b);
    }
    return true;
}

void mdct_free(mdct_plan *p)
{
    free(p->twiddle);
    free(p->pre);
    free(p->post);
    free(p->buf);
    *p = (mdct_plan){0};
}

/* In-place radix-2 complex FFT of m points (interleaved re/im). */
static void fft(float *z, int m, const float *tw)
{
    for (int i = 1, j = 0; i < m; i++) {
        int bit = m >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j) {
            float tr = z[2 * i], ti = z[2 * i + 1];
            z[2 * i] = z[2 * j];
            z[2 * i + 1] = z[2 * j + 1];
            z[2 * j] = tr;
            z[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= m; len <<= 1) {
        int half = len >> 1, step = m / len;
        for (int i = 0; i < m; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = tw[2 * k * step], wi = tw[2 * k * step + 1];
                float *a = z + 2 * (i + k), *b = z + 2 * (i + k + half);
                float br = b[0] * wr - b[1] * wi;
                float bi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - br;
                b[1] = a[1] - bi;
                a[0] += br;
                a[1] += bi;
            }
        }
    }
}

void mdct_forward(mdct_plan *p, const float *in, float *out)
{
    int n = p->n, m = n / 2, h = n / 2;
    float *z = p->buf;          /* m complex values */
    float *v = p->buf + 2 * m;  /* n reals: the folded DCT-IV input */

    /* Fold the four quarters (a, b, c, d) of the input into (-c_r - d, a - b_r). */
    const float *a = in, *b = in + h, *c = in + 2 * h, *d = in + 3 * h;
    for (int i = 0; i < h; i++) {
        v[i]     = -c[h - 1 - i] - d[i];
        v[h + i] = a[i] - b[h - 1 - i];
    }

    /* DCT-IV of v through an m point FFT. */
    for (int i = 0; i < m; i++) {
        float re = v[2 * i], im = v[n - 1 - 2 * i];
        z[2 * i]     = re * p->pre[2 * i] - im * p->pre[2 * i + 1];
        z[2 * i + 1] = re * p->pre[2 * i + 1] + im * p->pre[2 * i];
    }
    fft(z, m, p->twiddle);
    for (int k = 0; k < m; k++) {
        float re = z[2 * k] * p->post[2 * k] - z[2 * k + 1] * p->post[2 * k + 1];
        float im = z[2 * k] * p->post[2 * k + 1] + z[2 * k + 1] * p->post[2 * k];
        out[2 * k]         = re;
        out[n - 1 - 2 * k] = -im;
    }
}
