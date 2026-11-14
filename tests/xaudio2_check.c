// SPDX-License-Identifier: AGPL-3.0-or-later
/* Windows test tool: plays an .xwm file through XAudio2 2.9 at zero volume and checks that
 * every sample plays without a voice error. No window; the stream is silent.
 *
 * Usage: xaudio2_check file.xwm [speed]
 * speed is the playback rate multiplier (default 1, real time). Above 1, XAudio2 ends
 * long xWMA buffers after the first packets, for the game's own files too, so a faster
 * check is only valid for files of a few packets.
 * Build: clang -std=c23 tests/xaudio2_check.c -lole32 -lxaudio2 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_errors;
static HANDLE g_stream_end;

static void STDMETHODCALLTYPE on_pass_start(IXAudio2VoiceCallback *self, UINT32 bytes) { (void)self; (void)bytes; }
static void STDMETHODCALLTYPE on_pass_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE on_stream_end(IXAudio2VoiceCallback *self)
{
    (void)self;
    SetEvent(g_stream_end);
}
static void STDMETHODCALLTYPE on_buffer_start(IXAudio2VoiceCallback *self, void *ctx) { (void)self; (void)ctx; }
static void STDMETHODCALLTYPE on_buffer_end(IXAudio2VoiceCallback *self, void *ctx) { (void)self; (void)ctx; }
static void STDMETHODCALLTYPE on_loop_end(IXAudio2VoiceCallback *self, void *ctx) { (void)self; (void)ctx; }
static void STDMETHODCALLTYPE on_voice_error(IXAudio2VoiceCallback *self, void *ctx, HRESULT hr)
{
    (void)self;
    (void)ctx;
    fprintf(stderr, "voice error 0x%08lx\n", (unsigned long)hr);
    InterlockedIncrement(&g_errors);
}

static IXAudio2VoiceCallbackVtbl g_vtbl = {
    on_pass_start, on_pass_end, on_stream_end, on_buffer_start, on_buffer_end, on_loop_end, on_voice_error,
};

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
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

    WAVEFORMATEX fmt = {0};
    uint8_t *data = nullptr;
    uint32_t data_size = 0, *dpds = nullptr, dpds_count = 0;
    for (long p = 12; p + 8 <= n;) {
        uint32_t size;
        memcpy(&size, b + p + 4, 4);
        if (size > (uint64_t)(n - p - 8)) {
            fprintf(stderr, "chunk runs past the end of the file\n");
            return 1;
        }
        if (!memcmp(b + p, "fmt ", 4))
            memcpy(&fmt, b + p + 8, size < sizeof fmt ? size : sizeof fmt);
        else if (!memcmp(b + p, "dpds", 4)) {
            /* chunks sit at unaligned file offsets; give XAudio2 aligned copies */
            dpds_count = size / 4;
            dpds = _aligned_malloc(size + 16, 16);
            memcpy(dpds, b + p + 8, size);
        } else if (!memcmp(b + p, "data", 4)) {
            data_size = size;
            data = _aligned_malloc(size + 16, 16);
            memcpy(data, b + p + 8, size);
        }
        p += 8 + size + (size & 1);
    }
    if (!data || !dpds || dpds_count == 0) {
        fprintf(stderr, "missing data or dpds chunk\n");
        return 1;
    }

    IXAudio2 *xa = nullptr;
    IXAudio2MasteringVoice *master = nullptr;
    IXAudio2SourceVoice *voice = nullptr;
    IXAudio2VoiceCallback cb = {&g_vtbl};
    g_stream_end = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
        hr = XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = IXAudio2_CreateMasteringVoice(xa, &master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE,
                                           0, nullptr, nullptr, AudioCategory_Other);
    if (SUCCEEDED(hr))
        hr = IXAudio2MasteringVoice_SetVolume(master, 0.0f, XAUDIO2_COMMIT_NOW);
    if (SUCCEEDED(hr))
        hr = IXAudio2_CreateSourceVoice(xa, &voice, &fmt, 0, XAUDIO2_MAX_FREQ_RATIO, &cb, nullptr, nullptr);
    if (FAILED(hr)) {
        fprintf(stderr, "XAudio2 setup failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    XAUDIO2_BUFFER buf = {.Flags = XAUDIO2_END_OF_STREAM, .AudioBytes = data_size, .pAudioData = data};
    XAUDIO2_BUFFER_WMA wma = {.pDecodedPacketCumulativeBytes = dpds, .PacketCount = dpds_count};
    hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buf, &wma);
    if (FAILED(hr)) {
        fprintf(stderr, "SubmitSourceBuffer rejected the file: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    float speed = argc > 2 ? (float)atof(argv[2]) : 1.0f;
    IXAudio2SourceVoice_SetFrequencyRatio(voice, speed, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);

    /* The end of stream callback comes once XAudio2 has read the whole buffer, before the
     * last samples play, so follow SamplesPlayed until it reaches the total or stops. */
    uint64_t expected = dpds[dpds_count - 1] / (2u * fmt.nChannels);
    XAUDIO2_VOICE_STATE st;
    uint64_t last = 0;
    int still = 0;
    for (;;) {
        Sleep(50);
        IXAudio2SourceVoice_GetState(voice, &st, 0);
        if (st.SamplesPlayed >= expected || g_errors)
            break;
        still = st.SamplesPlayed == last ? still + 1 : 0;
        if (still >= 60)  /* no progress for 3 seconds */
            break;
        last = st.SamplesPlayed;
    }
    bool ended = WaitForSingleObject(g_stream_end, 0) == WAIT_OBJECT_0;
    printf("XAudio2: samples played %llu of %llu, voice errors %ld, end of stream %s\n",
           (unsigned long long)st.SamplesPlayed, (unsigned long long)expected, g_errors,
           ended ? "signalled" : "not signalled");
    IXAudio2SourceVoice_DestroyVoice(voice);
    IXAudio2MasteringVoice_DestroyVoice(master);
    IXAudio2_Release(xa);
    CoUninitialize();
    return (g_errors || !ended || st.SamplesPlayed != expected) ? 1 : 0;
}
