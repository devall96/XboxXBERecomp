// The console's hard disk on the PC: where it lives (save_folder) and a copy of its saves at each start (save_backups)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <system_error>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace fs = std::filesystem;

// save_profile: a name keeps its own disk in "profiles\<name>" (its saves and their backups); empty is the main one
fs::path HardDisk() {
    std::wstring hdd = Instance() > 1 ? L"hdd" + std::to_wstring(Instance()) : L"hdd";
    const std::string& custom = Get().save_folder;
    fs::path base = custom.empty() ? ExeDir() : fs::u8path(custom);
    if (base.is_relative()) base = ExeDir() / base;
    std::string profile;
    for (char c : Get().save_profile)  // a folder name: letters, digits, space, - and _
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') profile += c;
    if (!profile.empty()) base = base / "profiles" / fs::u8path(profile);
    // the main disk is hdd beside the exe; a custom folder or a profile is the disk itself, a second instance's
    // beside it
    if (custom.empty() && profile.empty()) return base / hdd;
    return Instance() > 1 ? base / hdd : base;
}

namespace {

constexpr const wchar_t* kSaves = L"E\\UDATA";  // every title's saves on the console's E: partition
constexpr uint32_t kMaxBackups = 50;

uintmax_t FileCount(const fs::path& dir) {
    std::error_code ec;
    uintmax_t n = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) n++;
    return n;
}

}  // namespace

// before boot: E:\UDATA copied to "save backups\<date time>" beside the disk, the newest save_backups kept
void BackupSaves() {
    uint32_t keep = std::min(Get().save_backups, kMaxBackups);
    fs::path saves = HardDisk() / kSaves;
    std::error_code ec;
    if (!keep || !fs::exists(saves, ec) || !FileCount(saves)) return;
    // beside the disk; a profile's or a custom folder's own, named after it
    fs::path disk = HardDisk();
    std::wstring name = disk.filename().wstring();
    fs::path root = disk.parent_path() / (name == L"hdd" ? std::wstring(L"save backups") : L"save backups " + name);
    if (Instance() > 1) root += std::to_wstring(Instance());
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t stamp[32];
    wcsftime(stamp, 32, L"%Y-%m-%d %H%M%S", &tm);
    fs::path dest = root / stamp;
    fs::create_directories(dest, ec);
    fs::copy(saves, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        LOGE("saves: backup to %s failed: %s", dest.string().c_str(), ec.message().c_str());
        return;
    }
    LOGI("saves: backed up to %s", dest.string().c_str());
    // names sort by time: the oldest go
    std::vector<fs::path> all;
    for (auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec)) all.push_back(e.path());
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + keep < all.size(); i++) fs::remove_all(all[i], ec);
}

}  // namespace app
