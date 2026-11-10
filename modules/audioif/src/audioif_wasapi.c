// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// WASAPI for the audio pump on Windows. See audioif_wasapi.h.
//
// Shared mode, event driven, signed 16-bit at the pump's own rate: Windows
// converts to and from the engine's mix format (AUTOCONVERTPCM), so the pump
// never learns the device runs float at 48 kHz.
//
// The speaker paces the pump. A write waits on the device's event until
// there is room, so a pump writing here needs no wall-clock pace of its own;
// pacing twice would let two clocks drift apart under it.
//
// The microphone is read the same way: a read waits until a block has
// arrived. A pump whose graph starts at the microphone is therefore paced by
// the microphone, and the speaker's queue holds what `prime_ms` put there
// plus the jitter between the two.
//
// ole32 is loaded at run time rather than linked. The windows port links
// neither ole32 nor uuid, and the four entry points and five GUIDs this file
// uses are not worth a change to every build's link line.

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#define COBJMACROS
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "audioif_wasapi.h"

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const GUID audioif_clsid_enumerator = {
    0xBCDE0395, 0xE52F, 0x467C,
    { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID audioif_iid_enumerator = {
    0xA95664D2, 0x9614, 0x4F35,
    { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID audioif_iid_client = {
    0x1CB9AD4C, 0xDBFA, 0x4C32,
    { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID audioif_iid_render = {
    0xF294ACFC, 0x3146, 0x4483,
    { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
static const GUID audioif_iid_capture = {
    0xC8ADBD64, 0xE71E, 0x48A0,
    { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

// PKEY_Device_FriendlyName and PKEY_Device_ContainerId, spelled out so the
// build needs neither functiondiscoverykeys_devpkey.h nor a link to uuid.
static const PROPERTYKEY audioif_pkey_name = {
    { 0xA45C254E, 0xDF1C, 0x4EFD,
      { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
static const PROPERTYKEY audioif_pkey_container = {
    { 0x8C7ED206, 0x3F8A, 0x4827,
      { 0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C } }, 2 };

typedef HRESULT (WINAPI *audioif_co_init_t)(LPVOID, DWORD);
typedef HRESULT (WINAPI *audioif_co_create_t)(REFCLSID, LPUNKNOWN, DWORD,
    REFIID, LPVOID *);
typedef void (WINAPI *audioif_co_uninit_t)(void);
typedef void (WINAPI *audioif_co_free_t)(LPVOID);
typedef HRESULT (WINAPI *audioif_pv_clear_t)(PROPVARIANT *);

static audioif_co_init_t audioif_co_init;
static audioif_co_create_t audioif_co_create;
static audioif_co_uninit_t audioif_co_uninit;
static audioif_co_free_t audioif_co_free;
static audioif_pv_clear_t audioif_pv_clear;

static bool audioif_ole_load(void) {
    if (audioif_co_create != NULL) {
        return true;
    }
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    if (ole == NULL) {
        return false;
    }
    audioif_co_init = (audioif_co_init_t)(void *)GetProcAddress(ole,
        "CoInitializeEx");
    audioif_co_uninit = (audioif_co_uninit_t)(void *)GetProcAddress(ole,
        "CoUninitialize");
    audioif_co_create = (audioif_co_create_t)(void *)GetProcAddress(ole,
        "CoCreateInstance");
    audioif_co_free = (audioif_co_free_t)(void *)GetProcAddress(ole,
        "CoTaskMemFree");
    audioif_pv_clear = (audioif_pv_clear_t)(void *)GetProcAddress(ole,
        "PropVariantClear");
    return audioif_co_init != NULL && audioif_co_uninit != NULL
           && audioif_co_create != NULL && audioif_co_free != NULL
           && audioif_pv_clear != NULL;
}

struct audioif_wasapi {
    audioif_wasapi_cfg_t cfg;
    bool tried;                 // the open has been attempted, on some thread
    bool started;
    HANDLE event;
    IAudioClient *client;
    IAudioRenderClient *render;
    IAudioCaptureClient *capture;
    uint32_t frame_bytes;
    // Microphone: what arrived beyond the last read, oldest first.
    uint8_t *fifo;
    uint32_t fifo_cap;          // bytes
    uint32_t fifo_len;          // bytes waiting, from fifo[0]
    audioif_wasapi_stats_t stats;
};

// An endpoint by ID, or the default one for the direction. The caller has
// COM up on this thread.
static HRESULT audioif_device(const char *id, bool capture, IMMDevice **out) {
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr = audioif_co_create(&audioif_clsid_enumerator, NULL,
        CLSCTX_ALL, &audioif_iid_enumerator, (void **)&en);
    if (FAILED(hr)) {
        return hr;
    }
    if (id != NULL && id[0]) {
        WCHAR wide[128];
        if (MultiByteToWideChar(CP_UTF8, 0, id, -1, wide, 128) == 0) {
            IMMDeviceEnumerator_Release(en);
            return E_INVALIDARG;
        }
        hr = IMMDeviceEnumerator_GetDevice(en, wide, out);
    } else {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en,
            capture ? eCapture : eRender, eConsole, out);
    }
    IMMDeviceEnumerator_Release(en);
    return hr;
}

// COM on this thread, multithreaded. A thread that already chose a
// single-threaded apartment keeps it, and everything here still works from
// that thread; only a thread this call initialised needs the matching
// uninitialise, which is what the return says.
static bool audioif_com_up(HRESULT *hr) {
    if (!audioif_ole_load()) {
        *hr = E_NOINTERFACE;
        return false;
    }
    HRESULT r = audioif_co_init(NULL, COINIT_MULTITHREADED);
    *hr = S_OK;
    return r == S_OK || r == S_FALSE;
}

bool audioif_wasapi_probe(const char *device, bool capture, int32_t *hr_out) {
    HRESULT hr;
    const bool mine = audioif_com_up(&hr);
    if (FAILED(hr)) {
        *hr_out = (int32_t)hr;
        return false;
    }
    IMMDevice *dev = NULL;
    hr = audioif_device(device, capture, &dev);
    if (dev != NULL) {
        IMMDevice_Release(dev);
    }
    if (mine) {
        audioif_co_uninit();
    }
    *hr_out = (int32_t)hr;
    return SUCCEEDED(hr);
}

static void audioif_utf8(const WCHAR *src, char *dst, size_t len) {
    if (len == 0) {
        return;
    }
    dst[0] = 0;
    if (src != NULL
        && WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)len, NULL, NULL) == 0) {
        dst[0] = 0;
    }
    dst[len - 1] = 0;
}

bool audioif_wasapi_container_is_host(const char *container) {
    // {00000000-0000-0000-FFFF-FFFFFFFFFFFF}, in whatever case it came.
    static const char host[] = "{00000000-0000-0000-ffff-ffffffffffff}";
    for (size_t i = 0; i < sizeof(host) - 1; i++) {
        char ch = container[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (ch != host[i]) {
            return false;
        }
    }
    return container[sizeof(host) - 1] == 0;
}

bool audioif_wasapi_describe(const char *device, bool capture, char *id,
    size_t id_len, char *name, size_t name_len, char *container,
    size_t container_len, int32_t *hr_out) {
    id[0] = name[0] = container[0] = 0;
    HRESULT hr;
    const bool mine = audioif_com_up(&hr);
    if (FAILED(hr)) {
        *hr_out = (int32_t)hr;
        return false;
    }
    IMMDevice *dev = NULL;
    hr = audioif_device(device, capture, &dev);
    if (SUCCEEDED(hr)) {
        LPWSTR wid = NULL;
        if (SUCCEEDED(IMMDevice_GetId(dev, &wid))) {
            audioif_utf8(wid, id, id_len);
            audioif_co_free(wid);
        }
        IPropertyStore *props = NULL;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(props, &audioif_pkey_name, &v))
                && v.vt == VT_LPWSTR) {
                audioif_utf8(v.pwszVal, name, name_len);
            }
            audioif_pv_clear(&v);
            PropVariantInit(&v);
            if (SUCCEEDED(IPropertyStore_GetValue(props,
                &audioif_pkey_container, &v)) && v.vt == VT_CLSID
                && v.puuid != NULL) {
                WCHAR text[40];
                const GUID *g = v.puuid;
                _snwprintf(text, 40,
                    L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                    (unsigned long)g->Data1, g->Data2, g->Data3,
                    g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
                    g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
                text[39] = 0;
                audioif_utf8(text, container, container_len);
            }
            audioif_pv_clear(&v);
            IPropertyStore_Release(props);
        }
        IMMDevice_Release(dev);
    }
    if (mine) {
        audioif_co_uninit();
    }
    *hr_out = (int32_t)hr;
    return SUCCEEDED(hr);
}

audioif_wasapi_t *audioif_wasapi_new(const audioif_wasapi_cfg_t *cfg) {
    audioif_wasapi_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    w->cfg = *cfg;
    w->cfg.device[sizeof(w->cfg.device) - 1] = 0;
    w->frame_bytes = cfg->channels * 2;
    return w;
}

static void audioif_wasapi_close(audioif_wasapi_t *w) {
    if (w->client != NULL && w->started) {
        IAudioClient_Stop(w->client);
    }
    w->started = false;
    if (w->render != NULL) {
        IAudioRenderClient_Release(w->render);
        w->render = NULL;
    }
    if (w->capture != NULL) {
        IAudioCaptureClient_Release(w->capture);
        w->capture = NULL;
    }
    if (w->client != NULL) {
        IAudioClient_Release(w->client);
        w->client = NULL;
    }
    if (w->event != NULL) {
        CloseHandle(w->event);
        w->event = NULL;
    }
}

void audioif_wasapi_free(audioif_wasapi_t *w) {
    if (w == NULL) {
        return;
    }
    audioif_wasapi_close(w);
    free(w->fifo);
    free(w);
}

// The open, on the thread that will use the stream. Once: a device that
// failed to open stays failed, and the pump hears silence from it (or writes
// into nothing) with `error` saying why, rather than retrying every block.
static bool audioif_wasapi_open(audioif_wasapi_t *w) {
    if (w->tried) {
        return w->client != NULL;
    }
    w->tried = true;
    HRESULT hr;
    (void)audioif_com_up(&hr);    // the pump thread keeps COM until it exits
    IMMDevice *dev = NULL;
    if (SUCCEEDED(hr)) {
        hr = audioif_device(w->cfg.device, w->cfg.capture, &dev);
    }
    if (SUCCEEDED(hr)) {
        hr = IMMDevice_Activate(dev, &audioif_iid_client, CLSCTX_ALL, NULL,
            (void **)&w->client);
        IMMDevice_Release(dev);
    }
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX wf;
        memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = (WORD)w->cfg.channels;
        wf.nSamplesPerSec = w->cfg.rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)w->frame_bytes;
        wf.nAvgBytesPerSec = w->cfg.rate * w->frame_bytes;
        const REFERENCE_TIME duration = (REFERENCE_TIME)w->cfg.buffer_ms * 10000;
        hr = IAudioClient_Initialize(w->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            duration, 0, &wf, NULL);
    }
    if (SUCCEEDED(hr)) {
        w->event = CreateEventW(NULL, FALSE, FALSE, NULL);
        hr = w->event != NULL ? IAudioClient_SetEventHandle(w->client, w->event)
            : HRESULT_FROM_WIN32(GetLastError());
    }
    UINT32 frames = 0;
    if (SUCCEEDED(hr)) {
        hr = IAudioClient_GetBufferSize(w->client, &frames);
    }
    if (SUCCEEDED(hr)) {
        w->stats.buffer_frames = frames;
        // What the engine adds on its own side, for anyone adding up the
        // latency: its default period and the stream latency it reports.
        // Neither is fatal to have missed.
        REFERENCE_TIME period = 0, latency = 0;
        if (SUCCEEDED(IAudioClient_GetDevicePeriod(w->client, &period, NULL))) {
            w->stats.period_frames = (uint32_t)(period * w->cfg.rate / 10000000);
        }
        if (SUCCEEDED(IAudioClient_GetStreamLatency(w->client, &latency))) {
            w->stats.latency_frames = (uint32_t)(latency * w->cfg.rate / 10000000);
        }
        if (w->cfg.capture) {
            hr = IAudioClient_GetService(w->client, &audioif_iid_capture,
                (void **)&w->capture);
            if (SUCCEEDED(hr)) {
                // Room for everything the device can hold plus one more
                // buffer, so draining it never has to stop halfway.
                w->fifo_cap = frames * 2 * w->frame_bytes;
                w->fifo = malloc(w->fifo_cap);
                if (w->fifo == NULL) {
                    hr = E_OUTOFMEMORY;
                }
            }
        } else {
            hr = IAudioClient_GetService(w->client, &audioif_iid_render,
                (void **)&w->render);
        }
    }
    if (SUCCEEDED(hr) && w->render != NULL) {
        // The cushion: silence queued ahead of the first block, so the
        // engine's first periods find something while the pump catches up.
        UINT32 prime = w->cfg.rate * w->cfg.prime_ms / 1000;
        if (prime > frames) {
            prime = frames;
        }
        if (prime) {
            BYTE *data = NULL;
            hr = IAudioRenderClient_GetBuffer(w->render, prime, &data);
            if (SUCCEEDED(hr)) {
                hr = IAudioRenderClient_ReleaseBuffer(w->render, prime,
                    AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
    }
    if (SUCCEEDED(hr)) {
        hr = IAudioClient_Start(w->client);
        w->started = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
        w->stats.error = (int32_t)hr;
        audioif_wasapi_close(w);
        return false;
    }
    return true;
}

uint32_t audioif_wasapi_write(audioif_wasapi_t *w, const uint8_t *buffer,
    uint32_t length, uint32_t timeout_ms, bool *timed_out) {
    *timed_out = false;
    if (w == NULL || !audioif_wasapi_open(w) || w->render == NULL) {
        return 0;
    }
    const uint32_t fb = w->frame_bytes;
    const uint32_t total = length / fb;
    uint32_t done = 0;
    bool first = true;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (done < total) {
        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(w->client, &padding))) {
            break;
        }
        if (first) {
            // An empty queue at the moment a block arrives is a block that
            // came too late: the engine has already played silence, and the
            // cushion went with it. Put the cushion back in front of this
            // block, or a pump paced by a microphone runs on with nothing
            // queued and starves again every few periods.
            if (padding == 0) {
                w->stats.starved++;
                UINT32 prime = w->cfg.rate * w->cfg.prime_ms / 1000;
                if (prime > w->stats.buffer_frames / 2) {
                    prime = w->stats.buffer_frames / 2;
                }
                BYTE *data = NULL;
                if (prime && SUCCEEDED(IAudioRenderClient_GetBuffer(w->render,
                    prime, &data))) {
                    IAudioRenderClient_ReleaseBuffer(w->render, prime,
                        AUDCLNT_BUFFERFLAGS_SILENT);
                    padding = prime;
                }
            }
            first = false;
        }
        uint32_t room = w->stats.buffer_frames > padding
            ? w->stats.buffer_frames - padding : 0;
        if (room == 0) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                *timed_out = true;
                w->stats.timeouts++;
                break;
            }
            WaitForSingleObject(w->event, (DWORD)(deadline - now));
            continue;
        }
        uint32_t n = total - done;
        if (n > room) {
            n = room;
        }
        BYTE *data = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(w->render, n, &data))) {
            break;
        }
        memcpy(data, buffer + done * fb, n * fb);
        IAudioRenderClient_ReleaseBuffer(w->render, n, 0);
        done += n;
        w->stats.queued_frames = padding + n;
    }
    w->stats.frames += done;
    return done * fb;
}

// Everything the device has captured, into the fifo. Packets that do not fit
// are dropped whole and counted, which only happens when nobody has read for
// longer than the device buffer.
static void audioif_wasapi_drain(audioif_wasapi_t *w) {
    const uint32_t fb = w->frame_bytes;
    for (;;) {
        UINT32 packet = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(w->capture, &packet))
            || packet == 0) {
            return;
        }
        BYTE *data = NULL;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(IAudioCaptureClient_GetBuffer(w->capture, &data, &frames,
            &flags, NULL, NULL))) {
            return;
        }
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            w->stats.glitches++;
        }
        const uint32_t bytes = frames * fb;
        if (w->fifo_len + bytes <= w->fifo_cap) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                memset(w->fifo + w->fifo_len, 0, bytes);
            } else {
                memcpy(w->fifo + w->fifo_len, data, bytes);
            }
            w->fifo_len += bytes;
        } else {
            w->stats.dropped += frames;
        }
        IAudioCaptureClient_ReleaseBuffer(w->capture, frames);
    }
}

uint32_t audioif_wasapi_read(audioif_wasapi_t *w, uint8_t *buffer,
    uint32_t length, uint32_t timeout_ms) {
    if (w == NULL || !audioif_wasapi_open(w) || w->capture == NULL) {
        return 0;
    }
    const uint32_t fb = w->frame_bytes;
    length = (length / fb) * fb;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        audioif_wasapi_drain(w);
        // Hold the backlog to one read plus `keep_ms`. A pump that fell
        // behind once (a slow block, a page fault) otherwise carries that
        // delay for as long as the stream runs, because the microphone and
        // the speaker run at the same rate and nothing else drains it.
        if (w->cfg.keep_ms) {
            const uint32_t keep = length
                + (w->cfg.rate * w->cfg.keep_ms / 1000) * fb;
            if (w->fifo_len > keep) {
                const uint32_t cut = w->fifo_len - keep;
                memmove(w->fifo, w->fifo + cut, keep);
                w->fifo_len = keep;
                w->stats.dropped += cut / fb;
            }
        }
        if (w->fifo_len >= length) {
            break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            w->stats.timeouts++;
            break;
        }
        WaitForSingleObject(w->event, (DWORD)(deadline - now));
    }
    const uint32_t n = w->fifo_len < length ? w->fifo_len : length;
    memcpy(buffer, w->fifo, n);
    memmove(w->fifo, w->fifo + n, w->fifo_len - n);
    w->fifo_len -= n;
    w->stats.queued_frames = w->fifo_len / fb;
    w->stats.frames += n / fb;
    return n;
}

void audioif_wasapi_stats(const audioif_wasapi_t *w,
    audioif_wasapi_stats_t *out) {
    if (w == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = w->stats;
}

#else
// Not Windows: nothing to build, and a translation unit may not be empty.
typedef int audioif_wasapi_unused_t;
#endif  // _WIN32
