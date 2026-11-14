// SPDX-License-Identifier: AGPL-3.0-or-later
/* Windows test tool: decodes an .xwm file with Microsoft's WMA decoder (the Media
 * Foundation transform, the decoder family XAudio2 uses) and writes a 16-bit WAV.
 * Runs offline: no audio device, no window.
 *
 * Usage: mf_decode in.xwm out.wav [encode_options] [samples_per_block]
 * Build: clang -std=c23 tests/mf_decode.c -lole32 -lmfplat -lmfuuid -lwmcodecdspuuid */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmreg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wmcodecdsp.h>

typedef struct {
    WAVEFORMATEX fmt;
    uint8_t     *data;
    uint32_t     data_size;
    uint32_t    *dpds;
    uint32_t     dpds_count;
} xwm_file;

static int load_xwm(const char *path, xwm_file *x)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "XWMA", 4))
        return -1;
    *x = (xwm_file){0};
    for (long p = 12; p + 8 <= n;) {
        uint32_t size;
        memcpy(&size, b + p + 4, 4);
        if (size > (uint64_t)(n - p - 8))
            return -1;  /* chunk runs past the end of the file */
        const uint8_t *c = b + p + 8;
        if (!memcmp(b + p, "fmt ", 4))
            memcpy(&x->fmt, c, sizeof x->fmt < size ? sizeof x->fmt : size);
        else if (!memcmp(b + p, "dpds", 4)) {
            x->dpds = (uint32_t *)c;
            x->dpds_count = size / 4;
        } else if (!memcmp(b + p, "data", 4)) {
            x->data = (uint8_t *)c;
            x->data_size = size;
        }
        p += 8 + size + (size & 1);
    }
    return x->data && x->fmt.nBlockAlign ? 0 : -1;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

#define CHECK(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    fprintf(stderr, "%s failed: 0x%08lx\n", #expr, (unsigned long)hr_); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mf_decode in.xwm out.wav [encode_options] [samples_per_block]\n");
        return 2;
    }
    xwm_file x;
    if (load_xwm(argv[1], &x)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    uint16_t options = argc > 3 ? (uint16_t)strtoul(argv[3], nullptr, 0) : 0x1F;
    uint32_t spb = argc > 4 ? (uint32_t)strtoul(argv[4], nullptr, 0) : 0;

    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));

    IMFTransform *t = nullptr;
    CHECK(CoCreateInstance(&CLSID_CWMADecMediaObject, nullptr, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&t));

    /* WMA 2 wave format with its 10 bytes of codec data:
     * samples per block (4), encode options (2), super block align (4). */
    struct {
        WAVEFORMATEX w;
        uint8_t      extra[10];
    } wf = {0};
    wf.w = x.fmt;
    wf.w.cbSize = 10;
    memcpy(wf.extra, &spb, 4);
    memcpy(wf.extra + 4, &options, 2);
    IMFMediaType *in_type = nullptr;
    CHECK(MFCreateMediaType(&in_type));
    CHECK(MFInitMediaTypeFromWaveFormatEx(in_type, &wf.w, sizeof wf));
    CHECK(IMFTransform_SetInputType(t, 0, in_type, 0));

    IMFMediaType *out_type = nullptr;
    bool found = false;
    for (DWORD i = 0; !found && SUCCEEDED(IMFTransform_GetOutputAvailableType(t, 0, i, &out_type)); i++) {
        GUID sub;
        UINT32 bits = 0;
        IMFMediaType_GetGUID(out_type, &MF_MT_SUBTYPE, &sub);
        IMFMediaType_GetUINT32(out_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 16)
            found = SUCCEEDED(IMFTransform_SetOutputType(t, 0, out_type, 0));
        if (!found)
            IMFMediaType_Release(out_type);
    }
    if (!found) {
        fprintf(stderr, "decoder offers no 16-bit PCM output\n");
        return 1;
    }
    MFT_OUTPUT_STREAM_INFO osi;
    CHECK(IMFTransform_GetOutputStreamInfo(t, 0, &osi));
    DWORD out_size = osi.cbSize ? osi.cbSize : 1 << 20;
    CHECK(IMFTransform_ProcessMessage(t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));

    FILE *wav = fopen(argv[2], "wb");
    if (!wav)
        return 1;
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wav);
    put32(wav, 16);
    put16(wav, 1);
    put16(wav, x.fmt.nChannels);
    put32(wav, x.fmt.nSamplesPerSec);
    put32(wav, x.fmt.nSamplesPerSec * x.fmt.nChannels * 2);
    put16(wav, (uint16_t)(x.fmt.nChannels * 2));
    put16(wav, 16);
    fwrite("data\0\0\0\0", 1, 8, wav);
    uint32_t pcm_bytes = 0;

    uint32_t packets = x.data_size / x.fmt.nBlockAlign;
    int errors = 0;
    for (uint32_t k = 0; k <= packets; k++) {
        if (k < packets) {
            IMFMediaBuffer *buf;
            IMFSample *s;
            BYTE *p;
            CHECK(MFCreateMemoryBuffer(x.fmt.nBlockAlign, &buf));
            IMFMediaBuffer_Lock(buf, &p, nullptr, nullptr);
            memcpy(p, x.data + (size_t)k * x.fmt.nBlockAlign, x.fmt.nBlockAlign);
            IMFMediaBuffer_Unlock(buf);
            IMFMediaBuffer_SetCurrentLength(buf, x.fmt.nBlockAlign);
            CHECK(MFCreateSample(&s));
            IMFSample_AddBuffer(s, buf);
            HRESULT hr = IMFTransform_ProcessInput(t, 0, s, 0);
            IMFSample_Release(s);
            IMFMediaBuffer_Release(buf);
            if (FAILED(hr)) {
                fprintf(stderr, "packet %u: ProcessInput 0x%08lx\n", k, (unsigned long)hr);
                errors++;
                continue;
            }
        } else {
            IMFTransform_ProcessMessage(t, MFT_MESSAGE_COMMAND_DRAIN, 0);
        }
        for (;;) {
            IMFMediaBuffer *buf = nullptr;
            IMFSample *s = nullptr;
            MFT_OUTPUT_DATA_BUFFER od = {0};
            if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                CHECK(MFCreateMemoryBuffer(out_size, &buf));
                CHECK(MFCreateSample(&s));
                IMFSample_AddBuffer(s, buf);
                od.pSample = s;
            }
            DWORD status = 0;
            HRESULT hr = IMFTransform_ProcessOutput(t, 0, 1, &od, &status);
            if (SUCCEEDED(hr) && od.pSample) {
                IMFMediaBuffer *cb;
                IMFSample_ConvertToContiguousBuffer(od.pSample, &cb);
                BYTE *p;
                DWORD len;
                IMFMediaBuffer_Lock(cb, &p, nullptr, &len);
                fwrite(p, 1, len, wav);
                pcm_bytes += len;
                IMFMediaBuffer_Unlock(cb);
                IMFMediaBuffer_Release(cb);
            }
            if (od.pEvents)
                IMFCollection_Release(od.pEvents);
            if (od.pSample && od.pSample != s)
                IMFSample_Release(od.pSample);
            if (s)
                IMFSample_Release(s);
            if (buf)
                IMFMediaBuffer_Release(buf);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                break;
            if (FAILED(hr)) {
                fprintf(stderr, "packet %u: ProcessOutput 0x%08lx\n", k, (unsigned long)hr);
                errors++;
                break;
            }
        }
    }
    fseek(wav, 4, SEEK_SET);
    put32(wav, 36 + pcm_bytes);
    fseek(wav, 40, SEEK_SET);
    put32(wav, pcm_bytes);
    fclose(wav);

    uint32_t expect = x.dpds_count ? x.dpds[x.dpds_count - 1] : 0;
    printf("packets %u, decoded %u bytes (dpds total %u), errors %d\n", packets, pcm_bytes, expect, errors);
    IMFTransform_Release(t);
    MFShutdown();
    CoUninitialize();
    return errors ? 1 : 0;
}
