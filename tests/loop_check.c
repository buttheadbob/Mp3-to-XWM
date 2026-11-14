// SPDX-License-Identifier: AGPL-3.0-or-later
/* Checks gapless ADPCM loops. Builds 10 s loops of a 441 Hz tone and of noise with a 4410-sample
 * period (at 44.1 kHz; at 48 kHz the same tone and a 4800-sample period), so both loop seamlessly
 * as sources and their length is not a whole number of 512-sample blocks. Each goes through the
 * loop fit (whole-loop resampling to whole blocks, with the header rate that keeps the pitch) and
 * the ADPCM encoder into out_dir, is decoded again by the MS ADPCM decoder below, and is measured:
 *   length    decoded samples: a whole number of blocks, no padding
 *   seam      SNR across the loop point (+-2048 samples) against the source resampled exactly
 *   mid       SNR in the middle of the loop, the codec's own level
 *   step      the sample step at the loop point against the largest step elsewhere
 *   pitch     the header rate plays the samples in the source's own time (within 0.1 cent)
 * Usage: loop_check out_dir */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/adpcm_enc.h"
#include "../src/loopfit.h"
#include "../src/wav_writer.h"

static constexpr double PI = 3.14159265358979323846;

static double source(int kind, int rate, double i)  /* continuous in i: the exact reference */
{
    if (kind == 0)
        return 0.5 * sin(2 * PI * 441.0 * i / rate);
    /* noise: 200 sines on the 10 Hz grid, so it repeats every 0.1 s */
    double v = 0;
    for (int k = 1; k <= 200; k++)
        v += sin(2 * PI * 10.0 * (k * 7 % 1500 + 1) * i / rate + k * 1.234);
    return v * 0.1 / sqrt(100.0);  /* rms 0.1: peaks stay well inside full scale */
}

typedef struct {
    loopfit       *fit;
    adpcm_encoder *enc;
} chain;

static int to_enc(const float *const *pcm, size_t n, void *ctx)
{
    return adpcm_encoder_feed(((chain *)ctx)->enc, pcm, n);
}

/* MS ADPCM decoder, written from the format description, to check the files independently. */
static long decode(const char *path, float **out)
{
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    static const int coef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        *out = nullptr;
        return 0;
    }
    fclose(f);
    long data = 12, size = 0;
    for (long p = 12; p + 8 <= n;) {
        long s = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (long)b[p + 7] << 24;
        if (!memcmp(b + p, "data", 4)) {
            data = p + 8;
            size = s;
            break;
        }
        p += 8 + s + (s & 1);
    }
    int align = b[32] | b[33] << 8, ch = b[22];
    long blocks = size / align, total = blocks * 512;
    *out = malloc(sizeof(float) * (size_t)total);
    for (long k = 0; k < blocks; k++) {
        const unsigned char *bl = b + data + k * align;
        int pred[2], delta[2], s1[2], s2[2];
        for (int c = 0; c < ch; c++) {
            pred[c] = bl[c];
            delta[c] = (short)(bl[ch + 2 * c] | bl[ch + 2 * c + 1] << 8);
            s1[c] = (short)(bl[3 * ch + 2 * c] | bl[3 * ch + 2 * c + 1] << 8);
            s2[c] = (short)(bl[5 * ch + 2 * c] | bl[5 * ch + 2 * c + 1] << 8);
        }
        float *o = *out + k * 512;
        o[0] = s2[0] / 32768.0f;  /* channel 0 only is measured */
        o[1] = s1[0] / 32768.0f;
        const unsigned char *nib = bl + 7 * ch;
        int idx = 0;
        for (int i = 2; i < 512; i++)
            for (int c = 0; c < ch; c++, idx++) {
                int nb = idx & 1 ? nib[idx >> 1] & 15 : nib[idx >> 1] >> 4;
                int sn = nb >= 8 ? nb - 16 : nb;
                int p = (s1[c] * coef[pred[c]][0] + s2[c] * coef[pred[c]][1]) >> 8;
                int v = p + sn * delta[c];
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                s2[c] = s1[c];
                s1[c] = v;
                delta[c] = adapt[nb] * delta[c] >> 8;
                if (delta[c] < 16)
                    delta[c] = 16;
                if (c == 0)
                    o[i] = v / 32768.0f;
            }
    }
    free(b);
    return total;
}

/* The sample rate in the file's header. */
static long header_rate(const char *path)
{
    unsigned char h[28];
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    return got == sizeof h ? h[24] | h[25] << 8 | h[26] << 16 | (long)h[27] << 24 : -1;
}

static double snr(double sig, double err)
{
    return err > 0 ? 10 * log10(sig / err) : 999;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int failures = 0;
    for (int rate = 44100; rate <= 48000; rate += 3900)
        for (int kind = 0; kind < 2; kind++) {
            long len = rate * 10L;  /* source samples: 10 s */
            char path[512];
            snprintf(path, sizeof path, "%s/loop_%s_%d_adpcm.xwm", dir, kind ? "noise" : "tone", rate);
            char err[256] = "";
            wav_writer *w = wav_open(path, 2, 44100, err, sizeof err);
            if (!w) {
                printf("%s: %s\n", path, err);
                return 1;
            }
            chain c = {nullptr, adpcm_encoder_create(2, wav_block, w)};
            c.fit = loopfit_create(2, rate, 44100, ADPCM_BLOCK_SAMPLES, (size_t)len, to_enc, &c);
            static float a[4096], bb[4096];
            for (long i = 0; i < len; i += 4096) {
                long n = len - i < 4096 ? len - i : 4096;
                for (long k = 0; k < n; k++)
                    a[k] = bb[k] = (float)source(kind, rate, (double)(i + k));
                const float *p[2] = {a, bb};
                loopfit_feed(c.fit, p, (size_t)n);
            }
            loopfit_result fit;
            loopfit_finish(c.fit, &fit);
            adpcm_encoder_finish(c.enc);
            wav_set_rate(w, fit.rate);
            wav_close(w, err, sizeof err);
            loopfit_free(c.fit);
            adpcm_encoder_free(c.enc);

            float *y;
            long n = decode(path, &y);
            long lp = n, W = 2048;
            double step_in = (double)len / (double)lp;  /* source samples per output sample */
            /* seam: the loop played twice in a row against the source at the same positions */
            double ss = 0, se = 0, ms = 0, me = 0;
            for (long k = -W; k < W; k++) {
                long j = (k + lp) % lp;
                double ideal = source(kind, rate, (double)j * step_in);
                ss += ideal * ideal;
                se += (y[j] - ideal) * (y[j] - ideal);
            }
            for (long j = lp / 2 - W; j < lp / 2 + W; j++) {
                double ideal = source(kind, rate, (double)j * step_in);
                ms += ideal * ideal;
                me += (y[j] - ideal) * (y[j] - ideal);
            }
            double step = fabs(y[0] - y[lp - 1]), maxstep = 0;
            for (long k = 1; k < lp; k++)
                if (fabs(y[k] - y[k - 1]) > maxstep)
                    maxstep = fabs(y[k] - y[k - 1]);
            bool ok = n > 0 && n % 512 == 0 && step <= maxstep && snr(ss, se) > snr(ms, me) - 3;
            /* the header rate plays the fitted samples in the source's own time: pitch unchanged */
            long hdr = header_rate(path);
            double cents = hdr > 0 ? 1200.0 * log2(((double)len / rate) / ((double)n / (double)hdr)) : 999;
            ok = ok && hdr == fit.rate && !fit.padded && !fit.streamed && fabs(cents) < 0.1;
            printf("%-5s %d Hz: length %ld = %ld blocks (change %+ld), header %ld Hz, pitch %+.3f cents  seam %.1f dB"
                   "  mid %.1f dB  step %.4f (max %.4f)  %s\n",
                   kind ? "noise" : "tone", rate, n, n / 512, fit.change, hdr, cents, snr(ss, se), snr(ms, me), step,
                   maxstep, ok ? "ok" : "FAIL");
            failures += !ok;
            free(y);
        }

    /* past the hold limit: a 48 kHz tone streamed at the plain rate, padded to whole blocks */
    {
        long len = 480000;
        char path[512], err[256] = "";
        snprintf(path, sizeof path, "%s/loop_streamed_adpcm.xwm", dir);
        wav_writer *w = wav_open(path, 2, 44100, err, sizeof err);
        if (!w) {
            printf("%s: %s\n", path, err);
            return 1;
        }
        chain c = {nullptr, adpcm_encoder_create(2, wav_block, w)};
        c.fit = loopfit_create(2, 48000, 44100, ADPCM_BLOCK_SAMPLES, (size_t)len / 2, to_enc, &c);
        static float a[4096];
        for (long i = 0; i < len; i += 4096) {
            long n = len - i < 4096 ? len - i : 4096;
            for (long k = 0; k < n; k++)
                a[k] = (float)source(0, 48000, (double)(i + k));
            const float *p[2] = {a, a};
            loopfit_feed(c.fit, p, (size_t)n);
        }
        loopfit_result fit;
        loopfit_finish(c.fit, &fit);
        adpcm_encoder_finish(c.enc);
        wav_set_rate(w, fit.rate);
        wav_close(w, err, sizeof err);
        loopfit_free(c.fit);
        adpcm_encoder_free(c.enc);
        float *y;
        long n = decode(path, &y);
        /* the middle against the source at the plain rate */
        double ms = 0, me = 0, step_in = 48000.0 / 44100.0;
        for (long j = n / 2 - 2048; j < n / 2 + 2048; j++) {
            double ideal = source(0, 48000, (double)j * step_in);
            ms += ideal * ideal;
            me += (y[j] - ideal) * (y[j] - ideal);
        }
        long hdr = header_rate(path);
        bool ok = fit.streamed && n % 512 == 0 && n == 441000 + fit.change && hdr == 44100 && snr(ms, me) > 50;
        printf("streamed 48000 Hz: length %ld = %ld blocks (silence %ld), header %ld Hz  mid %.1f dB  %s\n", n,
               n / 512, fit.change, hdr, snr(ms, me), ok ? "ok" : "FAIL");
        failures += !ok;
        free(y);
    }
    return failures != 0;
}
