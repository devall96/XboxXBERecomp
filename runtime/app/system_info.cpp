// the log's opening lines: what the game runs on and with which settings
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <SDL3/SDL_video.h>
#include <cpuid.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <fstream>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

#ifdef _WIN32
#pragma comment(lib, "advapi32.lib")
#endif

namespace app {

namespace {

#ifdef _WIN32
std::string RegString(HKEY key, const char* name) {
    char text[256] = {};
    DWORD size = sizeof(text) - 1;
    if (RegQueryValueExA(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(text), &size) != ERROR_SUCCESS) return {};
    return text;
}

std::string OsVersion() {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS)
        return "unknown";
    std::string name = RegString(key, "ProductName"), display = RegString(key, "DisplayVersion"),
                build = RegString(key, "CurrentBuildNumber");
    DWORD ubr = 0, size = sizeof(ubr);
    RegQueryValueExA(key, "UBR", nullptr, nullptr, reinterpret_cast<LPBYTE>(&ubr), &size);
    RegCloseKey(key);
    // Windows 11 still names itself Windows 10 here, its build number tells
    if (auto ten = name.find("Windows 10"); ten != std::string::npos && std::atoi(build.c_str()) >= 22000)
        name.replace(ten, 10, "Windows 11");
    if (!display.empty()) name += " " + display;
    if (!build.empty()) name += " (build " + build + "." + std::to_string(ubr) + ")";
    using WineVersion = const char*(__cdecl*)();
    if (auto wine = reinterpret_cast<WineVersion>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))))
        name += std::string(" on Wine ") + wine();
    return name;
}
#else
// PRETTY_NAME from os-release, and the kernel
std::string OsVersion() {
    std::string name = "Linux";
    std::ifstream f("/etc/os-release");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        name = line.substr(12);
        if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
    }
    utsname u{};
    if (uname(&u) == 0) name += std::string(" (kernel ") + u.release + ")";
    if (const char* session = std::getenv("XDG_SESSION_TYPE")) name += std::string(", ") + session;
    return name;
}

#undef __cpuid  // cpuid.h's macro; MSVC's function form below
void __cpuid(int r[4], int leaf) {
    unsigned a, b, c, d;
    __cpuid_count(unsigned(leaf), 0, a, b, c, d);
    r[0] = int(a), r[1] = int(b), r[2] = int(c), r[3] = int(d);
}
#endif

std::string CpuName() {
    int r[4] = {};
    __cpuid(r, int(0x80000000));
    if (uint32_t(r[0]) < 0x80000004) return "unknown";
    char brand[49] = {};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, int(0x80000002 + i));
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

}  // namespace

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s", kGame.title, IMAGE_XBE, date);
    LOGR("Path: %ls", ExeDir().wstring().c_str());
    if (DataDir() != ExeDir()) LOGR("Data: %ls", DataDir().wstring().c_str());
    LOGR("OS: %s", OsVersion().c_str());
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    LOGR("CPU: %s (%lu threads)", CpuName().c_str(), si.dwNumberOfProcessors);
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    LOGR("RAM: %.1f GB", mem.ullTotalPhys / 1073741824.0);
    // one line per adapter, each monitor reports its own
    std::vector<std::string> seen;
    for (DWORD i = 0;; i++) {
        DISPLAY_DEVICEA dd{sizeof(dd)};
        if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
        if (std::find(seen.begin(), seen.end(), dd.DeviceString) != seen.end()) continue;
        seen.emplace_back(dd.DeviceString);
        LOGR("GPU %zu: %s%s", seen.size(), dd.DeviceString, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
    }
#else
    LOGR("CPU: %s (%ld threads)", CpuName().c_str(), sysconf(_SC_NPROCESSORS_ONLN));
    LOGR("RAM: %.1f GB", double(sysconf(_SC_PHYS_PAGES)) * double(sysconf(_SC_PAGE_SIZE)) / 1073741824.0);
    LOGR("Video: %s", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");  // the GPU: the renderer's log
#endif
    Mode desk = Desktop();
    LOGR("Desktop: %ux%u", desk.width, desk.height);
    LogSettings();
}

}  // namespace app
