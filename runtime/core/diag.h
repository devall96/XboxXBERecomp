#pragma once
#include <cstdlib>
#include <filesystem>
#include <vector>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

// XBE_FRAMETIMES: a CSV written per present (its index, the time since start and the interval, in ms), for frame
// time suites (1% lows, hitches); empty: none
inline const std::filesystem::path& FrameTimesFile() {
    static const std::filesystem::path file = [] {
        const wchar_t* v = _wgetenv(L"XBE_FRAMETIMES");
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return file;
}

// XBE_CAPTURE_AT: seconds after start, comma separated, at which the renderer captures a frame as F3 does (with
// XBE_CAPTURE_DIR), for unattended tests
inline const std::vector<double>& CaptureAt() {
    static const std::vector<double> at = [] {
        std::vector<double> out;
        const char* v = std::getenv("XBE_CAPTURE_AT");
        while (v && *v) {
            char* end = nullptr;
            double t = std::strtod(v, &end);
            if (end == v) break;
            out.push_back(t);
            v = *end == ',' ? end + 1 : end;
        }
        return out;
    }();
    return at;
}

// XBE_SKIP_TEX: textures whose draws the renderer leaves out (hex addresses, comma separated), for finding what a pass
// does to the picture
inline const std::vector<uint32_t>& SkipTextures() {
    static const std::vector<uint32_t> list = [] {
        std::vector<uint32_t> out;
        const char* v = std::getenv("XBE_SKIP_TEX");
        while (v && *v) {
            char* end = nullptr;
            unsigned long t = std::strtoul(v, &end, 16);
            if (end == v) break;
            out.push_back(uint32_t(t));
            v = *end == ',' ? end + 1 : end;
        }
        return out;
    }();
    return list;
}

}  // namespace diag
