// The recompiled game code is built on the player's PC, so a release carries none of it: xbe_recomp translates each
// xbe of GameInfo::images to C++, clang compiles that and lld links it into <exe>.dll beside the executables, all from
// the compiler folder. The first start does this behind a progress window. Finished objects are kept, so a closed window
// or a crash loses only the files in progress and the next start carries on.
// Linux: lib<exe>.so, built the same way with clang and ld.lld; the progress shows in the log for now.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <mutex>

#include "core/platform.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string_view>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "hook_exports.h"
#include "image_symbols.h"
#include "kernel/disc.h"
#include "xbe/xbe.h"

extern const char kImageSha1[];  // the xbe build.bat recompiled into this executable, written by CMake

namespace dispatch {
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp
}

namespace app {

namespace {

namespace fs = std::filesystem;

// an executable and the xbe recompiled into it, from GameInfo::images, with the config that names that xbe
struct Image {
    std::wstring exe;
    std::string xbe;
    fs::path config;
};

#ifdef _WIN32
// as CMakePresets.json and CMakeLists.txt compile it for build.bat
constexpr wchar_t kClangFlags[] =
    L"-target x86_64-pc-windows-msvc -fms-compatibility-version=19.40 -std=c++20 -O2 -march=x86-64-v2 "
    L"-fno-strict-aliasing -DNDEBUG -DX86_GAME_CODE -nostdinc -nostdinc++ -w";
constexpr wchar_t kLinkFlags[] = L"/dll /noentry /nodefaultlib /machine:x64 /opt:noicf";
constexpr wchar_t kExeSuffix[] = L".exe";
constexpr char kObject[] = ".obj";
#else
// as CMakePresets.json and sdk/cmake/XbeRecomp.cmake compile it on Linux
const char* const kClangFlags[] = {"-target", "x86_64-pc-linux-gnu", "-std=c++20", "-O2", "-march=x86-64-v2", "-fPIC",
                                   "-fno-strict-aliasing", "-DNDEBUG", "-DX86_GAME_CODE", "-nostdinc", "-nostdinc++", "-w"};
constexpr wchar_t kExeSuffix[] = L"";
constexpr char kObject[] = ".o";
#endif
constexpr uint64_t kJobMemory = 512ull << 20;  // a compiler's peak on the biggest file is about half of this
constexpr uint64_t kDiskNeeded = 1ull << 30;    // both images' sources, objects and DLLs, with room to spare

#ifdef _WIN32
// the window's colours, the Configuration app's
constexpr COLORREF kBack = RGB(5, 22, 24);
constexpr COLORREF kBack2 = RGB(9, 38, 40);
constexpr COLORREF kPanel = RGB(6, 29, 31);
constexpr COLORREF kGreen = RGB(57, 242, 138);
constexpr COLORREF kGreenDim = RGB(31, 143, 85);
constexpr COLORREF kBarEmpty = RGB(4, 16, 18);
constexpr COLORREF kRed = RGB(224, 52, 58);
constexpr COLORREF kWhite = RGB(235, 248, 240);
constexpr COLORREF kBlack = RGB(0, 0, 0);
#endif

enum class Stage { Translating, Compiling, Linking, Ready };

// written by the build thread, read by the window
struct Progress {
    std::atomic<Stage> stage{Stage::Translating};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<uint32_t> first_wave{1};  // files compiled before the time left is worth showing
    std::atomic<int64_t> compile_start{0};  // steady clock ms
    std::atomic<bool> cancel{false};
};

Progress g_progress;
std::wstring g_failure;  // the step that failed, written by the build thread before it ends
#ifdef _WIN32
HWND g_window = nullptr;
bool g_start = false;  // the build finished, so the game goes on
constexpr UINT kBuildDone = WM_APP + 1;
#endif

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

fs::path CompilerDir() { return ExeDir() / "compiler"; }
// where the build writes its work and the finished library: the compiler folder, or (Linux, a folder the program
// cannot write, as an AppImage's) gamecode/ in the data folder
fs::path BuildDir() {
#ifndef _WIN32
    if (access(ExeDir().c_str(), W_OK) != 0) return DataDir() / "gamecode";
#endif
    return CompilerDir();
}

uint32_t EntryOf(const std::vector<uint8_t>& xbe) {
    xbe::Image image;
    std::string error;
    return image.Parse(xbe, error) ? image.entry : 0;
}

// the config in compiler\config whose [xbe] path is this xbe; an included one has no path. Releases of one xbe each
// have a config, told apart by the entry point xbe_symbols wrote into it.
fs::path ConfigFor(const std::string& xbe, const std::vector<uint8_t>& data = {}) {
    uint32_t entry = data.empty() ? 0 : EntryOf(data);
    fs::path first;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(CompilerDir() / "config", ec)) {
        try {
            toml::table t = toml::parse_file(e.path().string());
            if (fs::path(t["xbe"]["path"].value_or(std::string())).filename() != xbe) continue;
            if (entry && t["xbe"]["entry"].value_or(int64_t(0)) == entry) return e.path();
            if (first.empty()) first = e.path();
        } catch (const toml::parse_error&) {
        }
    }
    return first;
}

std::vector<Image> Images() {
    std::vector<Image> images;
    for (auto& [xbe, exe] : kGame.images) {
        std::string name = exe;
        images.push_back({std::wstring(name.begin(), name.end()), xbe, ConfigFor(xbe)});
    }
    return images;
}

fs::path WorkDir(const Image& image) { return BuildDir() / "work" / image.exe; }
#ifdef _WIN32
fs::path CodeDll(const Image& image) { return ExeDir() / (image.exe + L".dll"); }
#else
fs::path CodeDll(const Image& image) {
    return (BuildDir() == CompilerDir() ? ExeDir() : BuildDir()) / (L"lib" + image.exe + L".so");
}
#endif
fs::path ExeFile(const Image& image) { return ExeDir() / (image.exe + kExeSuffix); }
fs::path StampFile(const Image& image) { return WorkDir(image) / "built.txt"; }
std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }
fs::path CompileLog() { return DataDir() / "logs" / "compile.log"; }

// keeps the failed step for the message the player sees; false, to return
bool RecordFailure(const std::wstring& what) {
    g_failure = what;
    LOGE("Game code: %ls", what.c_str());
    return false;
}

// an image to build, with what its finished DLL is stamped with
struct Target {
    const Image* image = nullptr;
    std::vector<uint8_t> xbe;
    std::string stamp;
};

// a DLL serves the executable it was linked against and the xbe it was translated from
#ifdef _WIN32
bool StampFor(const Image& image, const std::vector<uint8_t>& xbe, std::string& stamp) {
    WIN32_FILE_ATTRIBUTE_DATA exe{};
    fs::path path = ExeDir() / (image.exe + L".exe");
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &exe)) return false;
    uint64_t size = (uint64_t(exe.nFileSizeHigh) << 32) | exe.nFileSizeLow;
    uint64_t time = (uint64_t(exe.ftLastWriteTime.dwHighDateTime) << 32) | exe.ftLastWriteTime.dwLowDateTime;
    stamp = std::to_string(size) + " " + std::to_string(time) + " " + Sha1(xbe);
    return true;
}
#else
bool StampFor(const Image& image, const std::vector<uint8_t>& xbe, std::string& stamp) {
    std::error_code ec;
    uint64_t size = fs::file_size(ExeFile(image), ec);
    if (ec) return false;
    auto time = fs::last_write_time(ExeFile(image), ec).time_since_epoch().count();
    if (ec) return false;
    stamp = std::to_string(size) + " " + std::to_string(time) + " " + Sha1(xbe);
    return true;
}
#endif

// build.bat's DLL has no stamp: it is current when newer than the executable it was linked against and the game is
// the release it was built from
bool Current(const Target& target) {
    std::error_code ec;
    fs::file_time_type built = fs::last_write_time(CodeDll(*target.image), ec);
    if (ec) return false;
    std::ifstream f(StampFile(*target.image));
    bool built_release = target.image->xbe != IMAGE_XBE || Sha1(target.xbe) == kImageSha1;
    if (!f) return built_release && built > fs::last_write_time(ExeFile(*target.image), ec);
    std::string line;
    return std::getline(f, line) && line == target.stamp;
}

// an image's xbe from the game in assets: the extracted file, else the mounted disc image
bool ReadXbe(const GameFiles& game, const char* name, std::vector<uint8_t>& out) {
    if (std::string_view(name) == IMAGE_XBE) {
        out = game.xbe;
        return true;
    }
    std::ifstream f(game.dir / name, std::ios::binary);
    if (f) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    const kernel::Disc* disc = kernel::MountedDisc();
    return disc && disc->Load(name, out);
}

std::vector<Target> StaleTargets(const GameFiles& game, std::vector<Image>& images) {
    std::vector<Target> stale;
    for (Image& image : images) {
        Target target{&image};
        if (!ReadXbe(game, image.xbe.c_str(), target.xbe)) {
            LOGR("Game code: no %s in the game files, %ls is not built", image.xbe.c_str(), image.exe.c_str());
            continue;
        }
        image.config = ConfigFor(image.xbe, target.xbe);
        if (!StampFor(image, target.xbe, target.stamp) || Current(target)) continue;
        stale.push_back(std::move(target));
    }
    return stale;
}

// a source to compile and the object it makes
struct Job {
    fs::path source;
    fs::path object;
    uint64_t bytes = 0;
};

uint32_t PartitionCount(const Image& image) {
    try {
        toml::table t = toml::parse_file(image.config.string());
        return uint32_t(t["xbe"]["partitions"].value_or(int64_t(0)));
    } catch (const toml::parse_error&) {
        return 0;
    }
}

#ifdef _WIN32
fs::path Tool(const wchar_t* name) { return CompilerDir() / (std::wstring(name) + L".exe"); }
#else
// the compiler folder's own, else (a development build) the system's
fs::path Tool(const wchar_t* name) {
    std::error_code ec;
    fs::path own = CompilerDir() / name;
    if (fs::exists(own, ec)) return own;
    std::string path = platform::Utf8(platform::Env(L"PATH"));
    for (size_t at = 0; at <= path.size();) {
        size_t end = path.find(':', at);
        if (end == std::string::npos) end = path.size();
        fs::path p = fs::path(path.substr(at, end - at)) / name;
        if (end > at && fs::exists(p, ec)) return p;
        at = end + 1;
    }
    return own;
}
#endif

// the newest of what every object of an image is built from besides its own source
fs::file_time_type SharedInputsTime(const Image& image) {
    std::error_code ec;
    fs::file_time_type newest = fs::last_write_time(WorkDir(image) / "x86_recomp_shared.h", ec);
    newest = std::max(newest, fs::last_write_time(Tool(L"clang"), ec));
    for (auto& e : fs::recursive_directory_iterator(CompilerDir() / "include", ec))
        newest = std::max(newest, e.last_write_time(ec));
    return newest;
}

// the image's sources whose object is missing or older than what it is built from
void AddJobs(const Image& image, std::vector<Job>& jobs) {
    fs::path work = WorkDir(image);
    std::vector<std::string> names{"x86_func_mapping"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) names.push_back("x86_recomp." + std::to_string(i));
    fs::file_time_type shared = SharedInputsTime(image);
    for (const std::string& name : names) {
        Job job{work / (name + ".cpp"), work / (name + kObject)};
        std::error_code ec;
        job.bytes = fs::file_size(job.source, ec);
        fs::file_time_type built = fs::last_write_time(job.object, ec);
        if (!ec && built > shared && built > fs::last_write_time(job.source, ec)) continue;
        jobs.push_back(std::move(job));
    }
}

#ifdef _WIN32
// compilers that run at once: one a logical processor, as many as the free memory holds
size_t Slots() {
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    size_t by_memory = size_t(mem.ullAvailPhys / kJobMemory);
    size_t by_cpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, MAXIMUM_WAIT_OBJECTS);
}

// starts and waits on the build's processes; closing the job, or this process ending, ends them all
class Processes {
public:
    Processes() {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        log_ = CreateFileW(CompileLog().c_str(), FILE_APPEND_DATA, share, &inherit, CREATE_ALWAYS, attributes, nullptr);
    }
    ~Processes() {
        CloseHandle(job_);
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
    }

    // null when it did not start; its output goes to logs\compile.log
    HANDLE Start(std::wstring command, const fs::path& dir) {
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log_;
        si.hStdError = log_;
        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, dir.c_str(), &si, &pi)) {
            LOGE("game code: could not start %ls (%lu)", command.c_str(), GetLastError());
            return nullptr;
        }
        AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // closes the process; 0 when it exited cleanly
    static DWORD ExitCode(HANDLE process) {
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        return code;
    }
    static bool Succeeded(HANDLE process) { return ExitCode(process) == 0; }

    // false when cancelled, which ends every process of the build
    bool WaitAll(std::vector<HANDLE>& processes) {
        while (WaitForMultipleObjects(DWORD(processes.size()), processes.data(), TRUE, 250) == WAIT_TIMEOUT)
            if (g_progress.cancel) return Cancel();
        return true;
    }

    bool Cancel() {
        TerminateJobObject(job_, 1);
        return false;
    }

private:
    HANDLE job_ = nullptr;
    HANDLE log_ = INVALID_HANDLE_VALUE;
};

// xbe_recomp writes each image's C++ into its work folder, the images side by side
bool Translate(std::vector<Target>& targets, Processes& run) {
    std::vector<HANDLE> processes;
    for (Target& target : targets) {
        fs::path work = WorkDir(*target.image);
        fs::path xbe = work / target.image->xbe;
        std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(target.xbe.data()), target.xbe.size());
        std::wstring command = Quote(CompilerDir() / "xbe_recomp.exe") + L" " + Quote(target.image->config);
        command += L" " + Quote(xbe) + L" " + Quote(work) + L" --xdk " + Quote(CompilerDir() / "xdk");
        HANDLE process = run.Start(command, work);
        if (!process) return RecordFailure(L"xbe_recomp.exe could not be started.");
        processes.push_back(process);
    }
    if (!run.WaitAll(processes)) return false;
    bool ok = true;
    constexpr DWORD kDllNotFound = 0xC0000135;  // STATUS_DLL_NOT_FOUND, the process never started
    const wchar_t* missing_runtime = L"xbe_recomp.exe needs msvcp140.dll and vcruntime140.dll beside it in the compiler folder. Extract the whole release again, then start the game.";
    for (size_t i = 0; i < processes.size(); i++) {
        DWORD code = Processes::ExitCode(processes[i]);
        const std::string& xbe = targets[i].image->xbe;
        if (code == kDllNotFound) ok = RecordFailure(missing_runtime);
        else if (code) ok = RecordFailure(L"Translating " + std::wstring(xbe.begin(), xbe.end()) + L" failed.");
    }
    return ok;
}

std::wstring CompileCommand(const Job& job) {
    fs::path include = CompilerDir() / "include";
    std::wstring command = Quote(CompilerDir() / "clang.exe") + L" " + kClangFlags;
    command += L" -isystem " + Quote(include / "freestanding") + L" -I" + Quote(include) + L" -c ";
    return command + job.source.filename().wstring() + L" -o " + job.object.filename().wstring() + L".tmp";
}

// an object is written beside its final name and moved there once whole
bool FinishJob(const Job& job, HANDLE process) {
    std::wstring name = job.source.parent_path().filename().wstring() + L"\\" + job.source.filename().wstring();
    if (!Processes::Succeeded(process)) return RecordFailure(L"Compiling " + name + L" failed.");
    fs::path temp = job.object;
    temp += ".tmp";
    std::error_code ec;
    fs::rename(temp, job.object, ec);
    if (ec) return RecordFailure(L"Saving the compiled " + name + L" failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

// the biggest files first, as many at once as Slots allows
bool Compile(std::vector<Job>& jobs, Processes& run) {
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    size_t slots = Slots();
    LOGR("Game code: compiling %zu files, %zu at a time", jobs.size(), slots);
    for (const Job& job : jobs) g_progress.bytes_total += job.bytes;
    g_progress.files_total = uint32_t(jobs.size());
    g_progress.first_wave = uint32_t(std::min(slots, jobs.size()));
    g_progress.compile_start = NowMs();
    g_progress.stage = Stage::Compiling;

    std::vector<HANDLE> running;
    std::vector<const Job*> running_jobs;
    size_t next = 0;
    while (next < jobs.size() || !running.empty()) {
        for (; running.size() < slots && next < jobs.size(); next++) {
            HANDLE process = run.Start(CompileCommand(jobs[next]), jobs[next].source.parent_path());
            if (!process) {
                RecordFailure(L"clang.exe could not be started.");
                return run.Cancel();
            }
            running.push_back(process);
            running_jobs.push_back(&jobs[next]);
        }
        DWORD wait = WaitForMultipleObjects(DWORD(running.size()), running.data(), FALSE, 250);
        if (g_progress.cancel) return run.Cancel();
        if (wait == WAIT_TIMEOUT) continue;
        size_t done = wait - WAIT_OBJECT_0;
        if (done >= running.size()) {
            RecordFailure(L"Waiting on the compilers failed.");
            return run.Cancel();
        }
        if (!FinishJob(*running_jobs[done], running[done])) return run.Cancel();
        running.erase(running.begin() + done);
        running_jobs.erase(running_jobs.begin() + done);
    }
    return true;
}

// lld links the objects against the executable's exports into the DLL, then the stamp says what it was built from
bool Link(const Target& target, Processes& run) {
    const Image& image = *target.image;
    fs::path work = WorkDir(image);
    std::wstring command = Quote(CompilerDir() / "lld-link.exe") + L" " + kLinkFlags + L" /def:exports.def";
    command += L" /implib:code.lib /out:" + Quote(CodeDll(image));
    command += L" " + Quote(CompilerDir() / (image.exe + L".lib")) + L" x86_func_mapping.obj";
    for (uint32_t i = 0; i < PartitionCount(image); i++) command += L" x86_recomp." + std::to_wstring(i) + L".obj";
    std::wstring dll = CodeDll(image).filename().wstring();
    std::vector<HANDLE> process{run.Start(command, work)};
    if (!process[0]) return RecordFailure(L"lld-link.exe could not be started.");
    if (!run.WaitAll(process)) return false;
    if (!Processes::Succeeded(process[0])) {
        std::wstring hint = L" If the game is already running, close it, then start it again.";
        return RecordFailure(L"Linking " + dll + L" failed." + hint);
    }
    std::ofstream(StampFile(image)) << target.stamp << "\n";
    return true;
}
#else
// compilers that run at once: one a logical processor, as many as the available memory holds
size_t Slots() {
    uint64_t available = 0;
    std::ifstream meminfo("/proc/meminfo");
    for (std::string key; meminfo >> key;) {
        uint64_t kb = 0;
        meminfo >> kb;
        if (key == "MemAvailable:") available = kb << 10;
        meminfo.ignore(64, '\n');
    }
    size_t by_memory = available ? size_t(available / kJobMemory) : 4;
    size_t by_cpu = size_t(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN)));
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, 64);
}

// starts and waits on the build's processes, each at a lower priority and ended with this one; output to
// logs/compile.log
class Processes {
public:
    using Handle = pid_t;
    Processes() {
        log_ = open(CompileLog().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    ~Processes() {
        if (log_ >= 0) close(log_);
    }

    // 0 when it did not start
    Handle Start(const std::vector<std::string>& args, const fs::path& dir) {
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        std::string where = dir.string();
        pid_t parent = getpid();
        pid_t pid = fork();
        if (pid == 0) {
            // only async-signal-safe calls until exec
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(127);
            setpriority(PRIO_PROCESS, 0, 10);
            if (log_ >= 0) {
                dup2(log_, 1);
                dup2(log_, 2);
            }
            if (chdir(where.c_str()) != 0) _exit(127);
            execv(argv[0], argv.data());
            _exit(127);
        }
        if (pid < 0) {
            LOGE("game code: could not start %s", args[0].c_str());
            return 0;
        }
        std::lock_guard lock(m_);
        running_.push_back(pid);
        return pid;
    }

    // reaps the process; 0 when it exited cleanly
    int ExitCode(Handle pid) {
        {
            std::lock_guard lock(m_);
            if (auto it = done_.find(pid); it != done_.end()) return it->second;
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid) return 1;
        return Finished(pid, status);
    }
    bool Succeeded(Handle pid) { return ExitCode(pid) == 0; }

    // the index of a process that has ended, or -1 after ms without one (or once cancelled)
    int WaitAny(const std::vector<Handle>& processes, int ms) {
        for (int waited = 0;; waited += 10) {
            for (size_t i = 0; i < processes.size(); i++) {
                {
                    std::lock_guard lock(m_);
                    if (done_.count(processes[i])) return int(i);
                }
                int status = 0;
                if (waitpid(processes[i], &status, WNOHANG) == processes[i]) {
                    Finished(processes[i], status);
                    return int(i);
                }
            }
            if (waited >= ms || g_progress.cancel) return -1;
            usleep(10000);
        }
    }

    // false when cancelled, which ends every process of the build
    bool WaitAll(std::vector<Handle>& processes) {
        for (Handle p : processes) {
            for (;;) {
                {
                    std::lock_guard lock(m_);
                    if (done_.count(p)) break;
                }
                int status = 0;
                if (waitpid(p, &status, WNOHANG) == p) {
                    Finished(p, status);
                    break;
                }
                if (g_progress.cancel) return Cancel();
                usleep(20000);
            }
        }
        return true;
    }

    bool Cancel() {
        std::lock_guard lock(m_);
        for (pid_t p : running_)
            if (!done_.count(p)) kill(p, SIGKILL);
        return false;
    }

private:
    int Finished(pid_t pid, int status) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        std::lock_guard lock(m_);
        done_[pid] = code;
        return code;
    }

    int log_ = -1;
    std::mutex m_;
    std::vector<pid_t> running_;
    std::map<pid_t, int> done_;
};

std::string Arg(const fs::path& p) { return p.string(); }

// xbe_recomp writes each image's C++ into its work folder, the images side by side
bool Translate(std::vector<Target>& targets, Processes& run) {
    std::vector<Processes::Handle> processes;
    for (Target& target : targets) {
        fs::path work = WorkDir(*target.image);
        fs::path xbe = work / target.image->xbe;
        std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(target.xbe.data()), target.xbe.size());
        Processes::Handle process = run.Start({Arg(Tool(L"xbe_recomp")), Arg(target.image->config), Arg(xbe), Arg(work),
                                               "--xdk", Arg(CompilerDir() / "xdk")},
                                              work);
        if (!process) return RecordFailure(L"xbe_recomp could not be started.");
        processes.push_back(process);
    }
    if (!run.WaitAll(processes)) return false;
    bool ok = true;
    for (size_t i = 0; i < processes.size(); i++) {
        const std::string& xbe = targets[i].image->xbe;
        if (run.ExitCode(processes[i])) ok = RecordFailure(L"Translating " + std::wstring(xbe.begin(), xbe.end()) + L" failed.");
    }
    return ok;
}

std::vector<std::string> CompileCommand(const Job& job) {
    fs::path include = CompilerDir() / "include";
    std::vector<std::string> args{Arg(Tool(L"clang"))};
    for (const char* flag : kClangFlags) args.push_back(flag);
    args.insert(args.end(), {"-isystem", Arg(include / "freestanding"), "-I" + Arg(include), "-c",
                             job.source.filename().string(), "-o", job.object.filename().string() + ".tmp"});
    return args;
}

// an object is written beside its final name and moved there once whole
bool FinishJob(const Job& job, Processes::Handle process, Processes& run) {
    std::wstring name = job.source.parent_path().filename().wstring() + L"/" + job.source.filename().wstring();
    if (!run.Succeeded(process)) return RecordFailure(L"Compiling " + name + L" failed.");
    fs::path temp = job.object;
    temp += ".tmp";
    std::error_code ec;
    fs::rename(temp, job.object, ec);
    if (ec) return RecordFailure(L"Saving the compiled " + name + L" failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

// the biggest files first, as many at once as Slots allows
bool Compile(std::vector<Job>& jobs, Processes& run) {
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    size_t slots = Slots();
    LOGR("Game code: compiling %zu files, %zu at a time", jobs.size(), slots);
    for (const Job& job : jobs) g_progress.bytes_total += job.bytes;
    g_progress.files_total = uint32_t(jobs.size());
    g_progress.first_wave = uint32_t(std::min(slots, jobs.size()));
    g_progress.compile_start = NowMs();
    g_progress.stage = Stage::Compiling;

    std::vector<Processes::Handle> running;
    std::vector<const Job*> running_jobs;
    size_t next = 0;
    uint32_t logged = 0;
    while (next < jobs.size() || !running.empty()) {
        for (; running.size() < slots && next < jobs.size(); next++) {
            Processes::Handle process = run.Start(CompileCommand(jobs[next]), jobs[next].source.parent_path());
            if (!process) {
                RecordFailure(L"clang could not be started.");
                return run.Cancel();
            }
            running.push_back(process);
            running_jobs.push_back(&jobs[next]);
        }
        int done = run.WaitAny(running, 250);
        if (g_progress.cancel) return run.Cancel();
        if (done < 0) continue;
        if (!FinishJob(*running_jobs[size_t(done)], running[size_t(done)], run)) return run.Cancel();
        running.erase(running.begin() + done);
        running_jobs.erase(running_jobs.begin() + done);
        // no window yet on Linux: the progress in the log, every tenth
        uint32_t tenth = g_progress.files_total ? g_progress.files_done * 10 / g_progress.files_total : 10;
        if (tenth != logged) {
            logged = tenth;
            LOGR("Game code: %u of %u files compiled", g_progress.files_done.load(), g_progress.files_total.load());
        }
    }
    return true;
}

// ld.lld links the objects into lib<exe>.so, whose calls into the runtime resolve against the executable when it is
// loaded; then the stamp says what it was built from
bool Link(const Target& target, Processes& run) {
    const Image& image = *target.image;
    fs::path work = WorkDir(image);
    fs::path so = CodeDll(image);
    std::vector<std::string> args{Arg(Tool(L"ld.lld")), "-shared", "-z", "undefs", "--icf=none", "-soname",
                                  so.filename().string(), "-o", Arg(so), "x86_func_mapping.o"};
    for (uint32_t i = 0; i < PartitionCount(image); i++) args.push_back("x86_recomp." + std::to_string(i) + ".o");
    std::vector<Processes::Handle> process{run.Start(args, work)};
    if (!process[0]) return RecordFailure(L"ld.lld could not be started.");
    if (!run.WaitAll(process)) return false;
    if (!run.Succeeded(process[0])) return RecordFailure(L"Linking " + so.filename().wstring() + L" failed.");
    std::ofstream(StampFile(image)) << target.stamp << "\n";
    return true;
}
#endif

bool Build(std::vector<Target>& targets) {
    Processes run;
    if (!Translate(targets, run)) return false;
    std::vector<Job> jobs;
    for (const Target& target : targets) AddJobs(*target.image, jobs);
    if (!Compile(jobs, run)) return false;
    g_progress.stage = Stage::Linking;
    for (const Target& target : targets)
        if (!Link(target, run)) return false;
    return true;
}

// what stops a build before it starts, for the player to fix; empty when nothing does
std::wstring Blocker(const std::vector<Target>& targets) {
#ifdef _WIN32
    const wchar_t* tools[] = {L"clang", L"lld-link", L"xbe_recomp"};
#else
    const wchar_t* tools[] = {L"clang", L"ld.lld", L"xbe_recomp"};
#endif
    for (const wchar_t* tool : tools) {
        std::error_code ec;
        if (fs::exists(Tool(tool), ec)) continue;
#ifndef _WIN32
        // the release carries xbe_recomp; clang and ld.lld are the system's where it has them
        if (std::wstring(tool) != L"xbe_recomp")
            return L"Preparing the game needs clang and ld.lld. Install them (Ubuntu and Debian: sudo apt install "
                   L"clang lld; Fedora: sudo dnf install clang lld; Arch: sudo pacman -S clang lld), then start the "
                   L"game again.";
#endif
        return L"The compiler folder is missing files. Extract the whole release again, then start the game.";
    }
    for (const Target& target : targets) {
        if (target.image->config.empty())
            return L"The compiler folder is missing files. Extract the whole release again, then start the game.";
        std::error_code ec;
        fs::create_directories(WorkDir(*target.image), ec);
        if (ec)
            return L"The game's folder cannot be written to. Move it somewhere you can write to, such as Documents.";
    }
#ifdef _WIN32
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(CompilerDir().c_str(), &free, nullptr, nullptr) && free.QuadPart < kDiskNeeded)
        return L"Preparing the game needs 1 GB of free space on the drive it is on.";
#else
    struct statvfs fsinfo{};
    if (statvfs(BuildDir().c_str(), &fsinfo) == 0 && uint64_t(fsinfo.f_bavail) * fsinfo.f_frsize < kDiskNeeded)
        return L"Preparing the game needs 1 GB of free space on the drive it is on.";
#endif
    return {};
}

std::wstring StageText() {
    switch (g_progress.stage.load()) {
        case Stage::Translating:
            return L"Translating the game's code";
        case Stage::Compiling:
            return L"Compiling the game's code, file " +
                   std::to_wstring(std::min(g_progress.files_done + 1, g_progress.files_total.load())) + L" of " +
                   std::to_wstring(g_progress.files_total);
        case Stage::Linking:
            return L"Linking the game's code";
        default:
            return L"Starting the game";
    }
}

// from the rate the files compiled so far went at, once every compiler has finished one
std::wstring TimeLeftText() {
    if (g_progress.stage != Stage::Compiling) return {};
    uint64_t done = g_progress.bytes_done;
    if (g_progress.files_done < g_progress.first_wave || !done) return L"Working out the time left";
    double elapsed = double(NowMs() - g_progress.compile_start);
    double left = elapsed * double(g_progress.bytes_total - done) / double(done);
    int minutes = int(left / 60000.0 + 0.5);
    if (minutes < 1) return L"Less than a minute left";
    return L"About " + std::to_wstring(minutes) + (minutes == 1 ? L" minute left" : L" minutes left");
}

float Fraction() {
    switch (g_progress.stage.load()) {
        case Stage::Translating:
            return 0.0f;
        case Stage::Compiling:
            return g_progress.bytes_total ? float(g_progress.bytes_done) / float(g_progress.bytes_total) : 0.0f;
        default:
            return 1.0f;
    }
}

#ifdef _WIN32
// ---- the window, in the Configuration app's look: a teal room with a faint grid and scanlines, the title as a big
// slanted word, and a green terminal frame with a tab and a cut corner. It has no choices: the game starts by itself
// once the build is done, and Escape or closing it stops the build, keeping what is done for the next start.

COLORREF Mix(COLORREF a, COLORREF b, int alpha) {  // b over a, alpha of 255
    auto c = [&](int x, int y) { return (x * (255 - alpha) + y * alpha) / 255; };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}

struct Fonts {
    HFONT word = nullptr;     // the title
    HFONT heading = nullptr;  // the frame's heading
    HFONT body = nullptr;     // the note under it
    HFONT term = nullptr;     // the tab, the stage and the numbers
};
Fonts g_fonts;

HFONT MakeFont(const wchar_t* face, int px, int weight, bool italic = false) {
    LOGFONTW lf{};
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    lf.lfItalic = italic;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

void MakeFonts(UINT dpi) {
    auto s = [&](int v) { return MulDiv(v, int(dpi), 96); };
    g_fonts.word = MakeFont(L"Bahnschrift", s(44), FW_BOLD, true);
    g_fonts.heading = MakeFont(L"Bahnschrift", s(22), FW_SEMIBOLD);
    g_fonts.body = MakeFont(L"Bahnschrift", s(15), FW_NORMAL);
    g_fonts.term = MakeFont(L"Consolas", s(15), FW_NORMAL);
}

void DeleteFonts() {
    for (HFONT f : {g_fonts.word, g_fonts.heading, g_fonts.body, g_fonts.term})
        if (f) DeleteObject(f);
    g_fonts = {};
}

void DrawLine(HDC dc, RECT row, const std::wstring& text, COLORREF color, UINT align) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), int(text.size()), &row, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void FillBox(HDC dc, const RECT& box, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &box, brush);
    DeleteObject(brush);
}

// a filled shape with its outline
void Shape(HDC dc, const POINT* points, int count, COLORREF line, int width, COLORREF fill) {
    HPEN pen = CreatePen(PS_SOLID, width, line);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, brush);
    Polygon(dc, points, count);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

// the room: teal darkening downwards, a faint grid, the terminal's scanlines, and a thin green edge
void PaintRoom(uint32_t* pixels, int w, int h, int grid) {
    for (int y = 0; y < h; y++) {
        int t = h > 1 ? y * 255 / (h - 1) : 0;
        COLORREF row = Mix(kBack2, kBack, t);
        bool scan = y % 3 == 0;
        for (int x = 0; x < w; x++) {
            COLORREF c = row;
            if (x % grid == 0 || y % grid == 0) c = Mix(c, kGreen, 22);
            if (scan) c = Mix(c, RGB(0, 0, 0), 40);
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1) c = kGreenDim;
            pixels[size_t(y) * w + x] = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
        }
    }
}

// the title as the game writes its big words: white, a red edge offset down-right, a black outline
void PaintWord(HDC dc, int x, int y, int s3, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, g_fonts.word);
    RECT r{};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    auto at = [&](int dx, int dy, COLORREF c) {
        RECT box{x + dx, y + dy, x + dx + r.right + s3 * 4, y + dy + r.bottom};
        SetTextColor(dc, c);
        DrawTextW(dc, text.c_str(), -1, &box, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    };
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) at(dx * s3, dy * s3, kBlack);
    at(s3 + s3 / 3, s3 + s3 / 3, kRed);
    at(0, 0, kWhite);
    SelectObject(dc, old);
}

// the menu frame: a tab with a slanted right edge on top, the box's top right corner cut off
void PaintFrame(HDC dc, RECT box, const wchar_t* tab, int (*s)(int)) {
    int cut = s(18), th = s(26), line = std::max(1, s(2));
    HGDIOBJ old = SelectObject(dc, g_fonts.term);
    SIZE ts{};
    GetTextExtentPoint32W(dc, tab, int(wcslen(tab)), &ts);
    int tw = ts.cx + s(40);
    POINT outline[] = {{box.left, box.top + th},           {box.right - cut, box.top + th}, {box.right, box.top + th + cut},
                       {box.right, box.bottom},            {box.left, box.bottom}};
    Shape(dc, outline, 5, kGreen, line, kPanel);
    POINT t[] = {{box.left + s(10), box.top + th},       {box.left + s(10), box.top},
                 {box.left + s(10) + tw - s(14), box.top}, {box.left + s(10) + tw, box.top + s(14)},
                 {box.left + s(10) + tw, box.top + th}};
    Shape(dc, t, 5, kGreen, line, Mix(kBack2, kGreen, 60));
    RECT label{box.left + s(22), box.top, box.left + s(10) + tw, box.top + th};
    DrawLine(dc, label, tab, kGreen, DT_LEFT);
    SelectObject(dc, old);
}

UINT g_dpi = 96;
bool g_dpi_fixed = false;  // XBE_TEST_DPI: a test run's scale, kept when the window moves between monitors
int Scaled(int v) { return MulDiv(v, int(g_dpi), 96); }

// a failed build stays up in the same window: what failed, and the end of the compiler's log
bool g_failed = false;
std::vector<std::wstring> g_log_lines;

// the last lines of logs\compile.log, where a compiler's error is
std::wstring CompileLogTail() {
    constexpr size_t kTailBytes = 1500;
    std::ifstream f(CompileLog(), std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(kTailBytes) ? size - std::streamoff(kTailBytes) : 0;
    std::string tail(size_t(size - start), '\0');
    f.seekg(start);
    f.read(tail.data(), std::streamsize(tail.size()));
    if (start) tail.erase(0, tail.find('\n') + 1);  // from a whole line
    int length = MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), nullptr, 0);
    std::wstring text(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), text.data(), length);
    return text;
}

std::vector<std::wstring> Lines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

// the window's size: the progress page, or the failure page, taller when it has the log to show
SIZE WindowSize() {
    if (g_failed && !g_log_lines.empty()) return {Scaled(820), Scaled(560)};
    if (g_failed) return {Scaled(640), Scaled(300)};
    return {Scaled(640), Scaled(292)};
}

// centred on the monitor the window is on, at its size
void PlaceWindow(HWND hwnd) {
    SIZE size = WindowSize();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT work = mi.rcWork;
    // never bigger than the screen: a small one at a high scale gets fewer of the log's lines
    size.cx = std::min<LONG>(size.cx, work.right - work.left - Scaled(16));
    size.cy = std::min<LONG>(size.cy, work.bottom - work.top - Scaled(16));
    int x = work.left + (work.right - work.left - size.cx) / 2;
    int y = work.top + (work.bottom - work.top - size.cy) / 2;
    SetWindowPos(hwnd, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PaintHeader(HDC dc, int w) {
    auto s = Scaled;
    std::wstring title = kGame.title;
    for (wchar_t& c : title) c = wchar_t(towupper(c));
    PaintWord(dc, s(22), s(10), std::max(1, s(2)), title);
    HGDIOBJ old = SelectObject(dc, g_fonts.term);
    DrawLine(dc, {s(24), s(10), w - s(24), s(36)}, L"FIRST START", kGreenDim, DT_RIGHT);
    DrawLine(dc, {s(24), s(36), w - s(24), s(62)}, L"PREPARING THE GAME FOR THIS PC", kGreen, DT_RIGHT);
    HPEN rule = CreatePen(PS_SOLID, 1, kGreenDim);
    HGDIOBJ old_pen = SelectObject(dc, rule);
    MoveToEx(dc, s(22), s(72), nullptr);
    LineTo(dc, w - s(22), s(72));
    SelectObject(dc, old_pen);
    DeleteObject(rule);
    SelectObject(dc, old);
}

void FrameBox(HDC dc, RECT box, COLORREF fill, COLORREF edge) {
    FillBox(dc, box, fill);
    HBRUSH brush = CreateSolidBrush(edge);
    FrameRect(dc, &box, brush);
    DeleteObject(brush);
}

// what failed, in the player's words, then the end of the compiler's log in a terminal box
void PaintFailure(HDC dc, const RECT& frame) {
    auto s = Scaled;
    int left = frame.left + s(22), right = frame.right - s(22), y = frame.top + s(38), bottom = frame.bottom - s(16);
    HGDIOBJ old = SelectObject(dc, g_fonts.heading);
    DrawLine(dc, {left, y, right, y + s(28)}, L"THE BUILD FAILED", kRed, DT_LEFT);
    y += s(34);

    SelectObject(dc, g_fonts.body);
    SetTextColor(dc, kWhite);
    std::wstring what = g_failure.empty() ? std::wstring(L"Preparing the game failed.") : g_failure;
    RECT text{left, y, right, y + s(44)};
    DrawTextW(dc, what.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    y += s(50);

    SelectObject(dc, g_fonts.term);
    RECT foot{left, bottom - s(20), right, bottom};
    DrawLine(dc, foot, g_log_lines.empty() ? L"" : L"Full details: logs\\compile.log", kGreenDim, DT_LEFT);
    DrawLine(dc, foot, L"ESC OR ENTER CLOSES", kGreenDim, DT_RIGHT);
    if (g_log_lines.empty()) {
        SelectObject(dc, old);
        return;
    }

    DrawLine(dc, {left, y, right, y + s(20)}, L"> The last lines of the log", kGreen, DT_LEFT);
    y += s(24);
    RECT box{left, y, right, foot.top - s(10)};
    FrameBox(dc, box, kBarEmpty, kGreenDim);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int line_h = std::max(1, int(tm.tmHeight));
    int fit = std::max(0, int((box.bottom - box.top - s(16)) / line_h));
    size_t first = g_log_lines.size() > size_t(fit) ? g_log_lines.size() - size_t(fit) : 0;
    int ly = box.top + s(8);
    for (size_t i = first; i < g_log_lines.size(); i++, ly += line_h) {
        RECT row{box.left + s(10), ly, box.right - s(10), ly + line_h};
        SetTextColor(dc, kWhite);
        DrawTextW(dc, g_log_lines[i].c_str(), int(g_log_lines[i].size()), &row,
                  DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
}

void Paint(HDC dc, const RECT& client) {
    auto s = Scaled;
    SetBkMode(dc, TRANSPARENT);
    int w = client.right, h = client.bottom;
    PaintHeader(dc, w);

    // the frame and what is in it
    bool stopping = g_progress.cancel;
    RECT frame{s(22), s(86), w - s(22), h - s(20)};
    PaintFrame(dc, frame, g_failed ? L"STOPPED" : stopping ? L"STOPPING" : L"PREPARING", s);
    if (g_failed) {
        PaintFailure(dc, frame);
        return;
    }
    int left = frame.left + s(22), right = frame.right - s(22), y = frame.top + s(38);

    HGDIOBJ old = SelectObject(dc, g_fonts.heading);
    DrawLine(dc, {left, y, right, y + s(28)}, stopping ? L"STOPPING" : L"BUILDING THE GAME'S CODE", stopping ? kRed : kGreen,
             DT_LEFT);
    y += s(30);
    SelectObject(dc, g_fonts.body);
    DrawLine(dc, {left, y, right, y + s(22)},
             L"Once, on the first start of a new build. The game starts by itself when it is done.", kWhite, DT_LEFT);
    y += s(32);

    SelectObject(dc, g_fonts.term);
    std::wstring stage = stopping ? L"Stopping, what is done so far is kept" : StageText();
    DrawLine(dc, {left, y, right, y + s(20)}, L"> " + stage, kGreen, DT_LEFT);
    DrawLine(dc, {left, y, right, y + s(20)}, std::to_wstring(int(Fraction() * 100.0f)) + L"%", kWhite, DT_RIGHT);
    y += s(24);

    RECT bar{left, y, right, y + s(18)};
    FrameBox(dc, bar, kBarEmpty, kGreenDim);
    RECT filled = bar;
    filled.right = bar.left + LONG(float(bar.right - bar.left) * Fraction());
    FillBox(dc, filled, kGreen);
    y += s(26);

    DrawLine(dc, {left, y, right, y + s(20)}, TimeLeftText(), kGreenDim, DT_LEFT);
    DrawLine(dc, {left, y, right, y + s(20)}, L"ESC STOPS", kGreenDim, DT_RIGHT);
    SelectObject(dc, old);
}

// painted off screen, then copied, so the bar does not flicker
void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        PaintRoom(static_cast<uint32_t*>(bits), client.right, client.bottom, std::max(8, Scaled(40)));
        Paint(memory, client);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

void CloseWindowWith(HWND hwnd, bool start) {
    g_start = start;
    DestroyWindow(hwnd);
    DeleteFonts();
}

// the failure page in place of a message box: it stays up until the player closes it
void ShowFailed(HWND hwnd, bool with_log) {
    g_failed = true;
    if (with_log) g_log_lines = Lines(CompileLogTail());
    SetClassLongPtrW(hwnd, GCLP_HCURSOR, LONG_PTR(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))));  // IDC_ARROW
    PlaceWindow(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    FlashWindow(hwnd, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (!g_failed) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_NCHITTEST:
            return HTCAPTION;  // no title bar: the whole window drags it
        case WM_NCLBUTTONDBLCLK:
            return 0;  // and a double click does not maximise it
        case WM_DPICHANGED: {
            // moved to a monitor at another scale: the fonts and sizes follow it
            if (!g_dpi_fixed) g_dpi = HIWORD(wp);
            DeleteFonts();
            MakeFonts(g_dpi);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SIZE size = WindowSize();
            SetWindowPos(hwnd, nullptr, r->left, r->top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (g_failed && (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE)) CloseWindowWith(hwnd, false);
            else if (wp == VK_ESCAPE) g_progress.cancel = true;
            return 0;
        case WM_CLOSE:
            if (g_failed) CloseWindowWith(hwnd, false);
            else g_progress.cancel = true;  // the build ends at its next step, then the window closes
            return 0;
        case kBuildDone:
            if (wp) CloseWindowWith(hwnd, true);  // built: straight into the game
            else if (g_progress.cancel) CloseWindowWith(hwnd, false);  // stopped
            else ShowFailed(hwnd, true);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// centred on the primary monitor, sized for its scale (XBE_TEST_DPI sets one for tests), borderless with its own edge
HWND OpenWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32650));  // IDC_APPSTARTING
    std::wstring class_name = std::wstring(kGame.name) + L"GameCode";
    wc.lpszClassName = class_name.c_str();
    RegisterClassExW(&wc);
    POINT origin{0, 0};
    HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    g_dpi = GetDpiForSystem();
    if (const char* test = std::getenv("XBE_TEST_DPI"); test && std::atoi(test) >= 48) {
        g_dpi = UINT(std::atoi(test));
        g_dpi_fixed = true;
    }
    MakeFonts(g_dpi);
    SIZE size = WindowSize();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(primary, &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - size.cx) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - size.cy) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, kGame.title, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                                x, y, size.cx, size.cy, nullptr, nullptr, inst, nullptr);
    if (!g_dpi_fixed) {
        // the monitor's own scale, which can differ from the one Windows started with
        if (UINT dpi = GetDpiForWindow(hwnd); dpi && dpi != g_dpi) {
            g_dpi = dpi;
            DeleteFonts();
            MakeFonts(g_dpi);
            PlaceWindow(hwnd);
        }
    }
    SetTimer(hwnd, 1, 100, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return hwnd;
}

// the build on its own thread, the window on this one; false when it failed or the player stopped it. A failure,
// or what keeps the build from starting, shows in the window until the player closes it.
bool BuildWithWindow(std::vector<Target>& targets) {
    g_failed = false;
    g_log_lines.clear();
    std::wstring blocker = Blocker(targets);
    g_window = OpenWindow();
    bool ok = false;
    std::thread worker;
    if (!blocker.empty()) {
        g_failure = blocker;
        LOGE("Game code: %ls", blocker.c_str());
        ShowFailed(g_window, false);
    } else {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // no sleep while it works
        worker = std::thread([&] {
            ok = Build(targets);
            if (ok) g_progress.stage = Stage::Ready;
            PostMessageW(g_window, kBuildDone, ok, 0);
        });
    }
    MSG msg;
    while (IsWindow(g_window) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (worker.joinable()) {
        worker.join();
        SetThreadExecutionState(ES_CONTINUOUS);
    }
    return ok && g_start;
}
#else
// the last lines of logs/compile.log, where a compiler's error is
std::wstring CompileLogTail() {
    constexpr size_t kTailBytes = 1500;
    std::ifstream f(CompileLog(), std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(kTailBytes) ? size - std::streamoff(kTailBytes) : 0;
    std::string tail(size_t(size - start), '\0');
    f.seekg(start);
    f.read(tail.data(), std::streamsize(tail.size()));
    if (start) tail.erase(0, tail.find('\n') + 1);
    return platform::Wide(tail);
}

// no window yet: the build runs before the game window opens, its progress in the log
bool BuildWithWindow(std::vector<Target>& targets) {
    if (std::wstring blocker = Blocker(targets); !blocker.empty()) {
        platform::Message(kGame.title, blocker, true);
        return false;
    }
    bool ok = Build(targets);
    if (!ok && !g_progress.cancel) {
        std::wstring text = L"Preparing the game failed. " + g_failure;
        if (std::wstring tail = CompileLogTail(); !tail.empty()) text += L"\n\nThe last lines of the log:\n\n" + tail;
        text += L"\n\nThe full details are in logs/compile.log.";
        platform::Message(kGame.title, text, true);
    }
    return ok;
}
#endif

// one build at a time on this PC: a second copy of the game started meanwhile waits for it
class BuildLock {
public:
#ifdef _WIN32
    BuildLock() {
        lock_ = CreateMutexW(nullptr, FALSE, (std::wstring(kGame.name) + L"-GameCode").c_str());
        WaitForSingleObject(lock_, INFINITE);
    }
    ~BuildLock() {
        ReleaseMutex(lock_);
        CloseHandle(lock_);
    }

private:
    HANDLE lock_ = nullptr;
#else
    BuildLock() {
        std::error_code ec;
        fs::create_directories(BuildDir(), ec);
        fd_ = open((BuildDir() / "build.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0) flock(fd_, LOCK_EX);
    }
    ~BuildLock() {
        if (fd_ >= 0) close(fd_);
    }

private:
    int fd_ = -1;
#endif
};

// no sleep while the build works
void KeepAwake(bool on) {
#ifdef _WIN32
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
#else
    (void)on;
#endif
}

}  // namespace

namespace {
std::atomic<bool> g_headless_running{false}, g_headless_done{false}, g_headless_ok{false};
}  // namespace

bool StartGameCodeBuild() {
    if (g_headless_running) return true;
    GameFiles game;
    if (!FindGame(game)) return false;
    static std::vector<Image> images;
    images = Images();
    static std::vector<Target> stale;
    stale = StaleTargets(game, images);
    if (stale.empty()) {
        g_headless_done = g_headless_ok = true;
        return false;
    }
    if (!Blocker(stale).empty()) {
        g_failure = Blocker(stale);
        g_headless_done = true;
        g_headless_ok = false;
        return false;
    }
    for (const Target& target : stale) LOGR("Game code: building %ls (setup)", CodeDll(*target.image).filename().wstring().c_str());
    g_headless_running = true;
    g_headless_done = g_headless_ok = false;
    g_progress.cancel = false;
    std::thread([] {
        bool ok;
        {
            BuildLock lock;
            KeepAwake(true);
            ok = Build(stale);
            if (ok) g_progress.stage = Stage::Ready;
            KeepAwake(false);
        }
        g_headless_ok = ok;
        g_headless_done = true;
        g_headless_running = false;
    }).detach();
    return true;
}

BuildStatus GameCodeBuildStatus() {
    BuildStatus st;
    st.running = g_headless_running;
    st.done = g_headless_done;
    st.ok = g_headless_ok;
    st.stage = st.done && st.ok ? std::wstring(L"The game's code is built.") : StageText();
    st.time_left = TimeLeftText();
    st.fraction = st.done && st.ok ? 1.0f : Fraction();
    if (st.done && !st.ok) st.failure = g_failure.empty() ? std::wstring(L"Stopped.") : g_failure;
    return st;
}

void CancelGameCodeBuild() { g_progress.cancel = true; }

bool PrepareGameCode(const GameFiles& game) {
    // one build at a time: a second copy of the game started meanwhile waits for it, then finds the DLL current
    std::vector<Image> images = Images();
    bool built;
    {
        BuildLock lock;
        std::vector<Target> stale = StaleTargets(game, images);
        for (const Target& target : stale) LOGR("Game code: building %ls", CodeDll(*target.image).filename().wstring().c_str());
        built = stale.empty() || BuildWithWindow(stale);
    }
    if (!built) return false;
    for (const Image& image : images)
        if (image.xbe == IMAGE_XBE && dispatch::Load(CodeDll(image))) return true;
#ifdef _WIN32
    MessageBoxW(nullptr, L"The game's code could not be loaded, see the log in the logs folder.", kGame.title,
                MB_ICONERROR);
#else
    platform::Message(kGame.title, L"The game's code could not be loaded, see the log in the logs folder.", true);
#endif
    return false;
}

}  // namespace app
