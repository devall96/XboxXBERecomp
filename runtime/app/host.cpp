// What the Windows and Linux hosts share: the program's folders, the instance number, UI lock and frame pacing
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <cwctype>
#include <thread>

#include "app/app.h"
#include "core/platform.h"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace app {

#ifdef __linux__
namespace {
// a data folder without settings or a disk yet gets the settings, disks, save backups and logs found in from (copied,
// never moved: from keeps them)
void CopyEarlierData(const std::filesystem::path& from, const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::equivalent(from, to, ec) || fs::exists(to / kGame.settings, ec) || fs::exists(to / "hdd", ec)) return;
    for (auto& e : fs::directory_iterator(from, ec)) {
        std::string name = e.path().filename().string();
        bool data = name == kGame.settings || name == "logs" || name == "profiles" || name.rfind("hdd", 0) == 0 ||
                    name.rfind("save backups", 0) == 0;
        if (!data || fs::exists(to / name, ec)) continue;
        fs::copy(e.path(), to / name, fs::copy_options::recursive, ec);
    }
}
}  // namespace
#endif

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
#ifdef __linux__
        // RecompLauncher's data folder for the game ({install}/recompdata), over a portable.txt: an unpacked AppImage's
        // folder is replaced on every update. What an earlier start wrote beside the program is copied there once.
        std::wstring launcher = platform::Env(L"RECOMPLAUNCHER_DATA");
        if (!launcher.empty()) {
            std::filesystem::path p(launcher);
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            if (p.is_absolute() && std::filesystem::is_directory(p, ec)) {
                CopyEarlierData(ExeDir(), p);
                return p;
            }
        }
#endif
        // a portable.txt beside the program, or beside the AppImage, keeps everything in that folder
        {
            std::filesystem::path home = ExeDir();
            std::wstring appimage = platform::Env(L"APPIMAGE");
            if (!appimage.empty()) home = std::filesystem::path(appimage).parent_path();
            std::error_code ec;
            if (std::filesystem::is_regular_file(home / "portable.txt", ec)) return home;
        }
#ifndef _WIN32
        // an AppImage, or an installed copy, runs from a folder it cannot write: the player's data goes to
        // $XDG_DATA_HOME/<name> (~/.local/share/<name>)
        if (!platform::Env(L"APPIMAGE").empty() || access(ExeDir().c_str(), W_OK) != 0) {
            std::wstring xdg = platform::Env(L"XDG_DATA_HOME");
            std::filesystem::path base = xdg.empty() ? std::filesystem::path(platform::Env(L"HOME")) / ".local" / "share"
                                                     : std::filesystem::path(xdg);
            std::filesystem::path p = base / kGame.name;
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            if (!ec) return p;
        }
#endif
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

namespace {
const auto g_start = std::chrono::steady_clock::now();  // the program's start, the keys' time base
}

void StartScriptedKeys(void (*press)(uint32_t vk)) {
    const char* text = std::getenv("XBE_KEYS");
    if (!text || !*text) return;
    std::vector<std::pair<double, uint32_t>> keys;
    std::string all = text;
    for (size_t pos = 0; pos < all.size();) {
        size_t comma = all.find(',', pos);
        std::string item = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            keys.push_back({std::atof(item.substr(0, colon).c_str()), uint32_t(std::atoi(item.c_str() + colon + 1))});
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    std::sort(keys.begin(), keys.end());
    std::thread([keys, press] {
        for (const auto& [at, vk] : keys) {
            std::this_thread::sleep_until(g_start + std::chrono::duration<double>(at));
            press(vk);
        }
    }).detach();
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
