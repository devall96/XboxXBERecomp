// mixer output through SDL3's audio (PipeWire or PulseAudio on Linux); Windows' is xaudio2.cpp
#ifndef _WIN32
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

// the console's stereo downmix in the DOLBY image: centre and back pair at -3 dB, no sub
constexpr float kCentreFold = 0.707f;
constexpr float kBackFold = 0.707f;
constexpr uint32_t kFrames = kRate / 100;  // 10 ms
constexpr uint32_t kMinQueued = 3, kMaxQueued = 10;

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<uint32_t> g_mode{kOutputAuto};
std::atomic<bool> g_mode_changed{false};

// SDL's 5.1 order is the bins' order (front left, right, centre, sub, back left, right)
int DeviceChannels() {
    SDL_AudioSpec spec{};
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr)) return 2;
    return spec.channels;
}

struct Output {
    SDL_AudioStream* stream = nullptr;
    int channels = 2;  // what the stream is fed: 6, or the 2 of the stereo fold
};

void Choose(Output& out) {
    int device = DeviceChannels();
    uint32_t mode = g_mode;
    g_surround = mode == kOutputSurround || (mode == kOutputAuto && device >= 6);
    g_mono = mode == kOutputMono;
    // a 5.1 mix on a device without 5.1 speakers is folded to stereo
    int channels = g_surround && device >= 6 ? 6 : 2;
    if (out.stream && channels != out.channels) {
        SDL_AudioSpec spec{SDL_AUDIO_F32, channels, int(kRate)};
        SDL_SetAudioStreamFormat(out.stream, &spec, nullptr);
    }
    out.channels = channels;
    LOGR("audio: %s output, %d device channels", g_mono ? "mono" : g_surround ? "5.1" : "stereo", device);
}

bool Open(Output& out) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: %s", SDL_GetError());
        return false;
    }
    Choose(out);
    SDL_AudioSpec spec{SDL_AUDIO_F32, out.channels, int(kRate)};
    out.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!out.stream) {
        LOGE("audio: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(out.stream);
    return true;
}

// L = FL + kCentreFold C + kBackFold BL and R = FR + kCentreFold C + kBackFold BR without the sub; mono hears the
// average of the two on both
void Fold(const float* in, float* out, uint32_t frames, bool mono) {
    for (uint32_t i = 0; i < frames; i++, in += kChannels, out += 2) {
        float l = in[0] + kCentreFold * in[2] + kBackFold * in[4];
        float r = in[1] + kCentreFold * in[2] + kBackFold * in[5];
        if (mono) l = r = (l + r) * 0.5f;
        out[0] = l;
        out[1] = r;
    }
}

void Pump(Output& out, const std::function<void(float*, uint32_t)>& render) {
    std::vector<float> mix(kFrames * kChannels), stereo(kFrames * 2);
    const int block = int(kFrames * sizeof(float));
    uint32_t depth = kMinQueued;
    for (bool started = false;;) {
        if (g_mode_changed.exchange(false)) Choose(out);
        int queued = SDL_GetAudioStreamQueued(out.stream);
        if (queued < 0) return;
        // a starved stream crackles: queue 10 ms more each time, up to kMaxQueued
        if (started && queued == 0 && depth < kMaxQueued) {
            depth++;
            LOGI("audio: output starved, queueing %u ms", depth * 10);
        }
        if (queued >= int(depth) * block * out.channels) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        render(mix.data(), kFrames);
        if (out.channels == 6) {
            SDL_PutAudioStreamData(out.stream, mix.data(), block * 6);
        } else {
            Fold(mix.data(), stereo.data(), kFrames, g_mono);
            SDL_PutAudioStreamData(out.stream, stereo.data(), block * 2);
        }
        started = true;
    }
}

// with no device the mix still runs at its rate, so the title's sounds play out and end
[[noreturn]] void RenderUnheard(const std::function<void(float*, uint32_t)>& render) {
    std::vector<float> buf(kFrames * kChannels);
    auto start = std::chrono::steady_clock::now();
    for (uint64_t done = 0;;) {
        auto due = std::chrono::steady_clock::now() - start;
        for (; done * std::chrono::milliseconds(10) < due; done++) render(buf.data(), kFrames);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

bool IsSurround() { return g_surround; }
bool IsMono() { return g_mono; }

void SetOutputMode(uint32_t mode) {
    if (g_mode.exchange(mode) != mode) g_mode_changed = true;
}

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::promise<void> opened;
    std::future<void> known = opened.get_future();
    std::thread([render, opened = std::move(opened)]() mutable {
        Output out;
        bool ok = Open(out);
        opened.set_value();
        if (!ok) {
            LOGE("audio: no output device, no sound");
            RenderUnheard(render);
        }
        Pump(out, render);
        LOGE("audio: the output stream failed, no sound from here");
        RenderUnheard(render);
    }).detach();
    known.wait();
}

}  // namespace audio
#endif
