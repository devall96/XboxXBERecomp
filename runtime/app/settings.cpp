// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
const char* kWide[] = {"console", "off", "expand"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    if (kGame.expands) s.widescreen = Wide::Expand;
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.audio_output = std::min(t["audio_output"].value_or(s.audio_output), audio::kOutputMono);
    std::string wide = t["widescreen"].value_or(std::string(kWide[uint32_t(s.widescreen)]));
    for (uint32_t i = 0; i < 3; i++)
        if (wide == kWide[i]) s.widescreen = Wide(i);
    if (s.widescreen == Wide::Expand && !kGame.expands) s.widescreen = Wide::Console;
    s.upscaler = std::min(t["upscaler"].value_or(s.upscaler), 1u);
    s.sharpen = std::min(t["sharpen"].value_or(s.sharpen), 100u);
    s.grade_exposure = std::clamp(t["grade_exposure"].value_or(s.grade_exposure), -300, 300);
    s.grade_contrast = std::clamp(t["grade_contrast"].value_or(s.grade_contrast), 50u, 200u);
    s.grade_saturation = std::min(t["grade_saturation"].value_or(s.grade_saturation), 200u);
    s.grade_temperature = std::clamp(t["grade_temperature"].value_or(s.grade_temperature), -100, 100);
    s.grade_tint = std::clamp(t["grade_tint"].value_or(s.grade_tint), -100, 100);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    audio::SetOutputMode(s.audio_output);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip},
                  {"audio_output", int64_t(s.audio_output)},
                  {"widescreen", kWide[uint32_t(s.widescreen)]},
                  {"upscaler", int64_t(s.upscaler)},
                  {"sharpen", int64_t(s.sharpen)},
                  {"grade_exposure", int64_t(s.grade_exposure)},
                  {"grade_contrast", int64_t(s.grade_contrast)},
                  {"grade_saturation", int64_t(s.grade_saturation)},
                  {"grade_temperature", int64_t(s.grade_temperature)},
                  {"grade_tint", int64_t(s.grade_tint)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// written beside the real file and renamed over it, so a crash mid save keeps the old settings
void Save() {
    std::filesystem::path tmp = File();
    tmp += ".tmp";
    std::ofstream f(tmp);
    f << Table() << "\n";
    f.close();
    if (f) MoveFileExW(tmp.c_str(), File().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
        HMONITOR monitor;
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
}

namespace {
std::atomic<uint64_t> g_present{0};  // width << 32 | height
}

void SetPresentSize(uint32_t width, uint32_t height) {
    constexpr uint32_t kMinimized = 16;  // a minimised window's client area: keep the last real size
    if (width < kMinimized || height < kMinimized) return;
    g_present.store(uint64_t(width) << 32 | height);
}

// what the frame is shown on: the window's picture once the renderer has one, else the resolution setting
Mode Shown() {
    uint64_t p = g_present.load();
    if (p) return {uint32_t(p >> 32), uint32_t(p)};
    return CachedOutput();
}

// asked per frame and per HUD quad: CachedOutput costs a monitor lookup, the registry only on a change
bool Widescreen() {
    if (Get().widescreen != Wide::Console) return false;
    Mode m = Shown();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float FrameAspect() {
    constexpr float k43 = 4.0f / 3.0f;
    constexpr float kWidest = 32.0f / 9.0f;
    if (Get().widescreen == Wide::Expand) {
        Mode m = Shown();
        if (!m.height) return k43;
        return std::clamp(float(m.width) / float(m.height), k43, kWidest);
    }
    return Widescreen() ? 16.0f / 9.0f : k43;
}

float WideFactor() {
    constexpr float kMin = 1.01f;  // below this the view stays the console's
    if (Get().widescreen != Wide::Expand) return 1.0f;
    float k = FrameAspect() * 3.0f / 4.0f;
    return k < kMin ? 1.0f : k;
}

float RenderScale(uint32_t guest_width, uint32_t guest_height) {
    if (!guest_width || !guest_height) return 1.0f;
    Mode m = Shown();
    float vw = float(kGame.view_w ? kGame.view_w : guest_width);
    float vh = float(kGame.view_h ? kGame.view_h : guest_height);
    // the view's place in the output: as tall as fits at the shown aspect
    float aspect = FrameAspect();
    float h = std::min(float(m.height), float(m.width) / aspect);
    float w = h * aspect;
    float pct = std::max(Get().render_percent, 10u) / 100.0f;  // 200 and up supersample
    // never under the console's own resolution unless render_percent asks for less
    return std::max(std::max(h / vh, w / vw) * pct, std::min(pct, 1.0f));
}

}  // namespace app
