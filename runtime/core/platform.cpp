#include "core/platform.h"

#include <chrono>
#include <cwchar>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <SDL3/SDL_messagebox.h>
#include <cstdio>

#include "core/log.h"
#include <pthread.h>
#include <unistd.h>

#include <cstdlib>
#endif

namespace platform {

#ifdef _WIN32

std::filesystem::path ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

std::wstring CommandLine() { return GetCommandLineW(); }
void SetArgs(int, char**) {}
const std::vector<std::string>& Args() {
    static const std::vector<std::string> none;
    return none;
}

void SpawnThread(size_t stack, std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    auto entry = [](void* p) -> DWORD {
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return 0;
    };
    HANDLE h = ::CreateThread(nullptr, stack, entry, f, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) ::CloseHandle(h);
}

void NameThread(const char* name) {
    wchar_t w[64];
    size_t i = 0;
    for (; name[i] && i < 63; i++) w[i] = wchar_t(name[i]);
    w[i] = 0;
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn set = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), w);
}

void SleepMs(uint32_t ms) { ::Sleep(ms); }
uint32_t ProcessId() { return GetCurrentProcessId(); }

void Message(const wchar_t* title, const std::wstring& text, bool error) {
    MessageBoxW(nullptr, text.c_str(), title, error ? MB_ICONERROR : MB_ICONINFORMATION);
}

bool AskYesNo(const wchar_t* title, const std::wstring& text) {
    return MessageBoxW(nullptr, text.c_str(), title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring Env(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v ? v : L"";
}

#else

namespace {
std::wstring g_command_line;
std::vector<std::string> g_args;
}  // namespace

const std::vector<std::string>& Args() { return g_args; }

std::filesystem::path ExePath() {
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
}

std::wstring CommandLine() { return g_command_line; }

void SetArgs(int argc, char** argv) {
    g_command_line.clear();
    g_args.assign(argv, argv + argc);
    for (int i = 0; i < argc; i++) {
        if (i) g_command_line += L' ';
        g_command_line += Wide(argv[i]);
    }
}

void SpawnThread(size_t stack, std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
    auto entry = [](void* p) -> void* {
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return nullptr;
    };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, entry, f) != 0) delete f;
    pthread_attr_destroy(&attr);
}

void NameThread(const char* name) {
    char n[16];  // the kernel keeps 15 characters
    size_t i = 0;
    for (; name[i] && i < 15; i++) n[i] = name[i];
    n[i] = 0;
    pthread_setname_np(pthread_self(), n);
}

void SleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint32_t ProcessId() { return uint32_t(getpid()); }

// the text goes to the log and stderr first: SDL shows its boxes through zenity or kdialog, and a desktop without
// either shows nothing
void Message(const wchar_t* title, const std::wstring& text, bool error) {
    std::string t = Utf8(text);
    if (error) LOGE("message: %s", t.c_str());
    else LOGR("message: %s", t.c_str());
    std::fprintf(stderr, "%s: %s\n", Utf8(title).c_str(), t.c_str());
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, Utf8(title).c_str(), t.c_str(),
                             nullptr);
}

std::wstring VulkanDriverHelp() {
    return L"The game draws with Vulkan 1.3, and this PC's graphics driver does not offer it.\n\n"
           L"Install your system's Vulkan driver, then start the game again:\n"
           L"  Ubuntu, Debian, Mint: sudo apt install mesa-vulkan-drivers\n"
           L"  Fedora: sudo dnf install mesa-vulkan-drivers\n"
           L"  Arch: sudo pacman -S vulkan-radeon (AMD) or vulkan-intel (Intel)\n"
           L"  NVIDIA: the NVIDIA driver from your system's driver manager\n\n"
           L"The log in the logs folder says which part failed.";
}

bool AskYesNo(const wchar_t* title, const std::wstring& text) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
        {0, 1, "Yes"},
    };
    std::string t = Utf8(title), m = Utf8(text);
    SDL_MessageBoxData data{SDL_MESSAGEBOX_WARNING, nullptr, t.c_str(), m.c_str(), 2, buttons, nullptr};
    int pressed = 0;
    return SDL_ShowMessageBox(&data, &pressed) && pressed == 1;
}

// wchar_t is UTF-32 here
std::string Utf8(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        uint32_t c = uint32_t(wc);
        if (c < 0x80) {
            s += char(c);
        } else if (c < 0x800) {
            s += char(0xC0 | (c >> 6));
            s += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += char(0xE0 | (c >> 12));
            s += char(0x80 | ((c >> 6) & 0x3F));
            s += char(0x80 | (c & 0x3F));
        } else {
            s += char(0xF0 | (c >> 18));
            s += char(0x80 | ((c >> 12) & 0x3F));
            s += char(0x80 | ((c >> 6) & 0x3F));
            s += char(0x80 | (c & 0x3F));
        }
    }
    return s;
}

std::wstring Wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t b = uint8_t(s[i]);
        uint32_t c;
        size_t n;
        if (b < 0x80) c = b, n = 1;
        else if ((b >> 5) == 6) c = b & 0x1F, n = 2;
        else if ((b >> 4) == 14) c = b & 0x0F, n = 3;
        else if ((b >> 3) == 30) c = b & 0x07, n = 4;
        else c = 0xFFFD, n = 1;
        if (i + n > s.size()) n = 1, c = 0xFFFD;
        for (size_t k = 1; k < n; k++) c = (c << 6) | (uint8_t(s[i + k]) & 0x3F);
        w += wchar_t(c);
        i += n;
    }
    return w;
}

std::wstring Env(const wchar_t* name) {
    const char* v = std::getenv(Utf8(name).c_str());
    return v ? Wide(v) : L"";
}

#endif

namespace {
// the arguments after the program's own path, whole: "--configure" in a folder's name is not the argument
std::vector<std::wstring> ArgWords() {
    std::vector<std::wstring> words;
#ifdef _WIN32
    int n = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; i++) words.emplace_back(argv[i]);
        LocalFree(argv);
    }
#else
    const std::vector<std::string>& args = Args();
    for (size_t i = 1; i < args.size(); i++) words.push_back(Wide(args[i]));
#endif
    return words;
}
}  // namespace

// "--name 3", "--name=3" or "--name3"
int ArgInt(const wchar_t* name, int def) {
    std::vector<std::wstring> words = ArgWords();
    size_t len = std::wcslen(name);
    for (size_t i = 0; i < words.size(); i++) {
        if (words[i].compare(0, len, name) != 0) continue;
        std::wstring rest = words[i].substr(len);
        if (!rest.empty() && rest[0] == L'=') rest.erase(0, 1);
        if (rest.empty() && i + 1 < words.size()) rest = words[i + 1];
        return rest.empty() ? def : int(std::wcstol(rest.c_str(), nullptr, 10));
    }
    return def;
}

std::wstring ArgString(const wchar_t* name) {
    std::vector<std::wstring> words = ArgWords();
    std::wstring eq = std::wstring(name) + L"=";
    for (size_t i = 0; i < words.size(); i++) {
        if (words[i] == name) return i + 1 < words.size() ? words[i + 1] : std::wstring();
        if (words[i].compare(0, eq.size(), eq) == 0) return words[i].substr(eq.size());
    }
    return {};
}

bool HasArg(const wchar_t* name) {
    for (const std::wstring& w : ArgWords())
        if (w == name) return true;
    return false;
}

}  // namespace platform
