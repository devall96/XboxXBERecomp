// What the Windows and Linux hosts share: the program's folders, the instance number, UI lock and frame pacing
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <thread>

#include "app/app.h"
#include "core/platform.h"

namespace app {

std::filesystem::path DataDir() {
    static const std::filesystem::path dir = [] {
        std::wstring var;
        for (const wchar_t* c = kGame.name; *c; c++) var += wchar_t(std::towupper(*c));
        var += L"_DATA_ROOT";
        std::wstring v = platform::Env(var.c_str());
        if (!v.empty()) {
            std::filesystem::path p(v);
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            if (p.is_absolute() && std::filesystem::is_directory(p, ec)) return p;
        }
        return ExeDir();
    }();
    return dir;
}

std::filesystem::path ExeDir() { return platform::ExePath().parent_path(); }

int Instance() {
    static const int n = std::max(0, std::min(8, platform::ArgInt(L"--instance", 0)));
    return n;
}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    if (cap == kFpsCapRefresh) cap = RefreshRate();
    // VRR: under the refresh rate, so frames stay inside the display's variable range instead of meeting its ceiling
    constexpr uint32_t kVrrMargin = 3;
    if (Get().vsync_mode == VSync::Vrr) {
        uint32_t hz = RefreshRate();
        if (hz > kVrrMargin * 10 && (!cap || cap > hz - kVrrMargin)) cap = hz - kVrrMargin;
    }
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) platform::SleepMs(1);
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app
