// SPDX-License-Identifier: AGPL-3.0-or-later
/* Windows test tool: decodes an MS ADPCM WAVE file (any name, .xwm included) with Windows' own
 * MS ADPCM codec through the Audio Compression Manager, writes 16-bit PCM and reports the sample
 * count, then compares Windows' PCM sample by sample with the reference decoder below. Fails if
 * the sample count is wrong or any sample differs from the reference with the prediction rounded
 * down (an arithmetic shift), which is what Windows does. Offline: no audio device, no window.
 *
 * Usage: acm_decode in.xwm out.wav
 * Build: clang -std=c23 tests/acm_decode.c -lmsacm32 -lwinmm */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MS ADPCM decoder written from the format description, to compare with Windows' output sample by
 * sample. trunc: the prediction is divided by 256 rounding toward zero; otherwise it is shifted,
 * rounding down. Writes interleaved samples to out. */
static void decode_ref(const uint8_t *data, uint32_t size, int align, int ch, bool trunc, int16_t *out)
{
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    static const int coef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
    long blocks = (long)(size / (uint32_t)align);
    for (long k = 0; k < blocks; k++) {
        const uint8_t *b = data + k * align;
        int pred[2], delta[2], s1[2], s2[2];
        for (int c = 0; c < ch; c++) {
            pred[c] = b[c] < 7 ? b[c] : 0;
            delta[c] = (int16_t)(b[ch + 2 * c] | b[ch + 2 * c + 1] << 8);
            s1[c] = (int16_t)(b[3 * ch + 2 * c] | b[3 * ch + 2 * c + 1] << 8);
            s2[c] = (int16_t)(b[5 * ch + 2 * c] | b[5 * ch + 2 * c + 1] << 8);
        }
        int16_t *o = out + k * 512 * ch;
        for (int c = 0; c < ch; c++) {
            o[c] = (int16_t)s2[c];
            o[ch + c] = (int16_t)s1[c];
        }
        const uint8_t *nib = b + 7 * ch;
        long idx = 0;
        for (int i = 2; i < 512; i++)
            for (int c = 0; c < ch; c++, idx++) {
                int nb = idx & 1 ? nib[idx >> 1] & 15 : nib[idx >> 1] >> 4;
                int sn = nb >= 8 ? nb - 16 : nb;
                int sum = s1[c] * coef[pred[c]][0] + s2[c] * coef[pred[c]][1];
                int p = trunc ? sum / 256 : sum >> 8;
                int v = p + sn * delta[c];
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                s2[c] = s1[c];
                s1[c] = v;
                delta[c] = adapt[nb] * delta[c] / 256;
                if (delta[c] < 16)
                    delta[c] = 16;
                o[i * ch + c] = (int16_t)v;
            }
    }
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: acm_decode in.xwm out.wav\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n)
        return 1;
    fclose(f);
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        fprintf(stderr, "not a RIFF WAVE file\n");
        return 1;
    }
    const WAVEFORMATEX *fmt = nullptr;
    const uint8_t *data = nullptr;
    uint32_t data_size = 0, fmt_size = 0;
    for (long p = 12; p + 8 <= n;) {
        uint32_t size;
        memcpy(&size, b + p + 4, 4);
        if (size > (uint64_t)(n - p - 8)) {
            fprintf(stderr, "chunk runs past the end of the file\n");
            return 1;
        }
        if (!memcmp(b + p, "fmt ", 4)) {
            fmt = (const WAVEFORMATEX *)(b + p + 8);
            fmt_size = size;
        } else if (!memcmp(b + p, "data", 4)) {
            data = b + p + 8;
            data_size = size;
        }
        p += 8 + size + (size & 1);
    }
    if (!fmt || !data || fmt_size < sizeof(WAVEFORMATEX) || fmt->wFormatTag != WAVE_FORMAT_ADPCM) {
        fprintf(stderr, "no MS ADPCM format or data chunk\n");
        return 1;
    }

    WAVEFORMATEX pcm = {WAVE_FORMAT_PCM, fmt->nChannels, fmt->nSamplesPerSec,
                        fmt->nSamplesPerSec * fmt->nChannels * 2, (WORD)(fmt->nChannels * 2), 16, 0};
    HACMSTREAM hs;
    MMRESULT mr = acmStreamOpen(&hs, nullptr, (LPWAVEFORMATEX)fmt, &pcm, nullptr, 0, 0, ACM_STREAMOPENF_NONREALTIME);
    if (mr) {
        fprintf(stderr, "acmStreamOpen failed: %u\n", mr);
        return 1;
    }
    /* whole blocks at a time */
    DWORD in_size = (DWORD)fmt->nBlockAlign * 64, out_size = 0;
    acmStreamSize(hs, in_size, &out_size, ACM_STREAMSIZEF_SOURCE);
    uint8_t *out = malloc(out_size);
    FILE *wav = fopen(argv[2], "wb");
    if (!out || !wav)
        return 1;
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wav);
    put32(wav, 16);
    put16(wav, 1);
    put16(wav, pcm.nChannels);
    put32(wav, pcm.nSamplesPerSec);
    put32(wav, pcm.nAvgBytesPerSec);
    put16(wav, pcm.nBlockAlign);
    put16(wav, 16);
    fwrite("data\0\0\0\0", 1, 8, wav);
    uint64_t pcm_bytes = 0;
    int errors = 0;
    uint32_t blocks = data_size / fmt->nBlockAlign;
    size_t total = (size_t)blocks * 512 * fmt->nChannels;
    int16_t *acm_pcm = malloc(sizeof(int16_t) * (total ? total : 1));
    int16_t *ref = malloc(sizeof(int16_t) * (total ? total : 1));
    if (!acm_pcm || !ref)
        return 1;
    for (uint32_t pos = 0; pos < data_size;) {
        DWORD take = data_size - pos < in_size ? data_size - pos : in_size;
        ACMSTREAMHEADER h = {.cbStruct = sizeof h};
        h.pbSrc = (LPBYTE)(data + pos);
        h.cbSrcLength = take;
        h.pbDst = out;
        h.cbDstLength = out_size;
        if (acmStreamPrepareHeader(hs, &h, 0) || acmStreamConvert(hs, &h, ACM_STREAMCONVERTF_BLOCKALIGN)) {
            errors++;
            break;
        }
        fwrite(out, 1, h.cbDstLengthUsed, wav);
        if (pcm_bytes + h.cbDstLengthUsed <= total * 2)
            memcpy((uint8_t *)acm_pcm + pcm_bytes, out, h.cbDstLengthUsed);
        pcm_bytes += h.cbDstLengthUsed;
        pos += h.cbSrcLengthUsed ? h.cbSrcLengthUsed : take;
        acmStreamUnprepareHeader(hs, &h, 0);
        if (!h.cbSrcLengthUsed)
            break;
    }
    acmStreamClose(hs, 0);
    fseek(wav, 4, SEEK_SET);
    put32(wav, (uint32_t)(36 + pcm_bytes));
    fseek(wav, 40, SEEK_SET);
    put32(wav, (uint32_t)pcm_bytes);
    fclose(wav);
    /* sample by sample against the reference decoder, with both roundings of the prediction */
    long diff_trunc = 0, diff_floor = 0;
    size_t got = (size_t)(pcm_bytes / 2) < total ? (size_t)(pcm_bytes / 2) : total;
    decode_ref(data, data_size, fmt->nBlockAlign, fmt->nChannels, true, ref);
    for (size_t i = 0; i < got; i++)
        diff_trunc += acm_pcm[i] != ref[i];
    decode_ref(data, data_size, fmt->nBlockAlign, fmt->nChannels, false, ref);
    for (size_t i = 0; i < got; i++)
        diff_floor += acm_pcm[i] != ref[i];
    printf("blocks %u (%u bytes each), decoded %llu samples per channel, expected %u, errors %d;"
           " samples differing from the reference decoder: rounding toward zero %ld, rounding down %ld\n",
           blocks, fmt->nBlockAlign, (unsigned long long)(pcm_bytes / pcm.nBlockAlign), blocks * 512u, errors,
           diff_trunc, diff_floor);
    /* Windows' codec shifts (rounds down); the encoder predicts the same way */
    return errors || pcm_bytes / pcm.nBlockAlign != blocks * 512u || diff_floor != 0;
}
