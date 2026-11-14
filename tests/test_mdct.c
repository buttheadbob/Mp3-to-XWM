// SPDX-License-Identifier: AGPL-3.0-or-later
/* Compares the fast MDCT against the direct formula for every block size WMA uses. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/mdct.h"

int main(void)
{
    int fails = 0;
    for (int n = 128; n <= 2048; n *= 2) {
        mdct_plan p;
        float *in = malloc(sizeof(float) * 2 * (size_t)n), *out = malloc(sizeof(float) * (size_t)n);
        if (!in || !out || !mdct_init(&p, n))
            return 2;
        srand(n);
        for (int i = 0; i < 2 * n; i++)
            in[i] = (float)rand() / RAND_MAX - 0.5f;
        mdct_forward(&p, in, out);
        double maxerr = 0, maxref = 0;
        for (int k = 0; k < n; k++) {
            double s = 0;
            for (int i = 0; i < 2 * n; i++)
                s += in[i] * cos(3.14159265358979323846 / n * (i + 0.5 + n / 2.0) * (k + 0.5));
            maxerr = fmax(maxerr, fabs(s - out[k]));
            maxref = fmax(maxref, fabs(s));
        }
        int ok = maxerr <= 1e-4 * maxref;
        printf("N=%4d max error %.3g (max value %.3g) %s\n", n, maxerr, maxref, ok ? "ok" : "FAIL");
        fails += !ok;
        mdct_free(&p);
        free(in);
        free(out);
    }
    return fails != 0;
}
