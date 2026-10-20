/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"

/* XAudio2 on Windows; SDL2 elsewhere (below). */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL2 audio */

/* The same contract on SDL2: a queue-based device at 48 kHz stereo 16-bit,
 * fed from the APU monitor frame. Like the XAudio2 path, a submission is
 * dropped rather than queued when three buffers are already waiting, so a
 * frame thread that runs ahead cannot build up latency.
 *
 * RECOMP_AUDIO=0 keeps it closed (headless runs, CI); RECOMP_AUDIO_WAV=<file>
 * also records every submitted sample, which is how output is checked
 * without speakers. */
#include <SDL.h>
#include <stdlib.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024
#define XA2_NUM_BUFS      3

static SDL_AudioDeviceID g_dev;
static int   g_xa2_initialized;
static long  g_xa2_frames_written;
static FILE *g_wav;
static uint32_t g_wav_bytes;

static void wav_header(FILE *f, uint32_t data_bytes)
{
    uint32_t v;
    uint16_t h;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + data_bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    h = 1; fwrite(&h, 2, 1, f); h = XA2_CHANNELS; fwrite(&h, 2, 1, f);
    v = XA2_SAMPLE_RATE; fwrite(&v, 4, 1, f);
    v = XA2_SAMPLE_RATE * XA2_CHANNELS * 2; fwrite(&v, 4, 1, f);
    h = XA2_CHANNELS * 2; fwrite(&h, 2, 1, f); h = 16; fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *off = getenv("RECOMP_AUDIO"), *wav = getenv("RECOMP_AUDIO_WAV");

    if (g_xa2_initialized) return 1;
    if (wav && *wav) {
        g_wav = fopen(wav, "wb");
        if (g_wav) wav_header(g_wav, 0);
    }
    if (off && !strcmp(off, "0")) {
        if (!g_wav) return 0;
        g_xa2_initialized = 1;            /* recording only */
        fprintf(stderr, "[XA2] SDL audio off; recording to %s\n", wav);
        return 1;
    }
    /* SDL would otherwise turn SIGINT/SIGTERM into an SDL_QUIT event that only
     * the window thread reads: with no window, the process stopped answering
     * kill and Ctrl-C. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio init failed: %s\n", SDL_GetError());
        if (!g_wav) return 0;
    } else {
        SDL_zero(want);
        want.freq = XA2_SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = XA2_CHANNELS;
        want.samples = XA2_BUF_SAMPLES;
        want.callback = NULL;              /* SDL_QueueAudio */
        g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!g_dev) {
            fprintf(stderr, "[XA2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            if (!g_wav) return 0;
        } else {
            SDL_PauseAudioDevice(g_dev, 0);
            fprintf(stderr, "[XA2] SDL2 audio output: %d Hz, %d channels (%s)\n",
                    have.freq, have.channels, SDL_GetCurrentAudioDriver());
        }
    }
    g_xa2_initialized = 1;
    return 1;
}

void xa2_shutdown(void)
{
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_wav) {
        wav_header(g_wav, g_wav_bytes);
        fclose(g_wav);
        g_wav = NULL;
    }
    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%ld buffers written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* RECOMP_PERF: every 5 s, how the output queue fared -- blocks sent, blocks
 * dropped because it was full (the mixer ahead of real time), and sends
 * that found it empty (behind: the device ran dry, a click), with the least
 * it held. Crackle is one or the other. */
static void xa2_stats(uint32_t queued, int dropped)
{
    static int on = -1;
    static uint32_t sent, drops, dry, low = 0xFFFFFFFFu, t0;
    uint32_t now;
    if (on < 0)
        on = getenv("RECOMP_PERF") != NULL;
    if (!on)
        return;
    now = SDL_GetTicks();
    if (!t0)
        t0 = now;
    if (dropped)
        drops++;
    else
        sent++;
    if (queued == 0)
        dry++;
    if (queued < low)
        low = queued;
    if (now - t0 >= 5000) {
        fprintf(stderr, "[XA2] %.1f s: %u blocks sent, %u dropped (queue full), %u found it"
                " empty; at least %.1f ms queued\n", (now - t0) / 1000.0, sent, drops, dry,
                low / (double)(XA2_CHANNELS * sizeof(int16_t)) * 1000.0 / XA2_SAMPLE_RATE);
        sent = drops = dry = 0;
        low = 0xFFFFFFFFu;
        t0 = now;
    }
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    uint32_t bytes, queued = 0;
    if (!g_xa2_initialized) return 0;
    if (num_samples > XA2_BUF_SAMPLES) num_samples = XA2_BUF_SAMPLES;
    bytes = (uint32_t)num_samples * XA2_CHANNELS * sizeof(int16_t);
    if (g_dev)
        queued = SDL_GetQueuedAudioSize(g_dev);
    if (g_dev && queued >= XA2_NUM_BUFS * XA2_BUF_SAMPLES
                                                 * XA2_CHANNELS * sizeof(int16_t)) {
        xa2_stats(queued, 1);
        return 0;
    }
    xa2_stats(queued, 0);
    if (g_dev && SDL_QueueAudio(g_dev, samples, bytes) != 0)
        return 0;
    if (g_wav) {
        fwrite(samples, 1, bytes, g_wav);
        g_wav_bytes += bytes;
        if ((g_xa2_frames_written & 63) == 0)
            wav_header(g_wav, g_wav_bytes);   /* playable even if killed */
    }
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#endif /* _WIN32 */
