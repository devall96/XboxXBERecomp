#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate
inline constexpr uint32_t kRendererD3D12 = 0, kRendererVulkan = 1;
// vsync_mode: how frames meet the display
enum class VSync : uint32_t {
    Off,       // present at once, tearing where the display allows it (lowest latency)
    On,        // wait for the refresh: no tearing, a missed refresh holds the frame a whole one
    Adaptive,  // on while frames keep up with the refresh, off for a late one (a tear instead of a held frame)
    Fast,      // no wait and no tearing: the newest finished frame shows at each refresh (borderless and windowed)
    Vrr,       // for G-Sync and FreeSync: no wait, tearing allowed, the frame rate capped 3 under the refresh rate
};
// widescreen: what a wide output gets
enum class Wide : uint32_t {
    Console,  // the console reports a widescreen TV at 3:2 and wider, and the frame is shown 16:9 (anamorphic)
    Off,      // a 4:3 TV: the frame is shown 4:3 with bars at the sides
    Expand,   // the game draws a wider view (GameInfo::expands): the frame fills the output, 2D stays 4:3 centred
};

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    // the game's own resolution exactly (render scale 1), shown scaled to the window: a game's console look (Unchained's OG Graphics) sets it
    // (not in the settings file; the game's preset decides it at each start)
    bool console_resolution = false;
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;                 // vsync_mode != Off, kept for older settings files
    VSync vsync_mode = VSync::On;
    uint32_t aniso = 0;              // 0 as the game asks
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    // the 2D textures the game draws flat shapes with (fonts, frames, icons with hard edges) drawn 4x with clean
    // edges in place of their texels
    bool ui_upscale = true;
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    uint32_t audio_output = 0;         // audio::kOutputAuto, Stereo, Surround or Mono
    Wide widescreen = Wide::Console;   // Expand where the game has it (Load picks it for a new settings file)
    // HUD pieces moved out to the screen's edges, percent of the console's TV safe margin taken away: 0 where the
    // console put them, 100 at the edges with a thin margin as modern HUDs
    uint32_t hud_edge = 100;
    bool hud_visible = true;           // false hides the HUD pieces at the screen's edges (menus stay)
    uint32_t colour_filter = 0;        // colour vision: 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia
    uint32_t colour_filter_strength = 100;  // percent
    std::string colour_lut;            // a .cube file in luts\ in the data folder, empty for none
    // the renderer where there is a choice (Windows): kRendererVulkan (the default) or kRendererD3D12. Linux always
    // draws with Vulkan
    uint32_t renderer = kRendererVulkan;
    uint32_t upscaler = 1;             // 0 bilinear, 1 AMD FSR 1 (EASU) when the view renders smaller than it shows
    uint32_t sharpen = 0;              // 0 off, to 100: FSR 1's RCAS, 2 stops (100 the sharpest)
    // HDR10 output where Windows has the display in HDR: the picture's white at hdr_paper_white nits, its brightest
    // parts stretched toward hdr_peak (0 the display's own) by hdr_highlights percent
    bool hdr = false;
    uint32_t hdr_paper_white = 200, hdr_peak = 0, hdr_highlights = 50;
    // colour grading of the presented picture: exposure in hundredths of a stop, contrast and saturation in percent,
    // temperature and tint -100 to 100
    int32_t grade_exposure = 0;
    uint32_t grade_contrast = 100, grade_saturation = 100;
    int32_t grade_temperature = 0, grade_tint = 0;
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    std::string save_folder;               // the console's hard disk (saves), empty for hdd beside the exe
    uint32_t save_backups = 5;             // copies of the saves made at each start and kept, 0 for none
    std::string save_profile;              // a named set of saves (profiles\<name>), empty for the main one
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
// host pixels per console pixel: the shown view (GameInfo::view_w/h, else the back buffer) fills its place in the
// output at render_percent, the larger of the two axes' scales where the shown aspect differs from the console's
float RenderScale(uint32_t guest_width, uint32_t guest_height);
// the renderer's window picture size (its client area): the output the render scale and the shown aspect follow, so
// a resized window, a borderless desktop or a windowed size all render at the pixels they show; 0 before the first
void SetPresentSize(uint32_t width, uint32_t height);
// Wide::Console and the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9
bool Widescreen();
// the aspect the frame is shown at: the output's under Wide::Expand (4:3 up to 32:9), else 16:9 or 4:3
float FrameAspect();
// Wide::Expand: how much wider than 4:3 the view is (FrameAspect / (4/3)), which the game widens its projection by
// and 2D draws are narrowed by; 1 otherwise
float WideFactor();

std::filesystem::path ExeDir();
// where the player's data lives: the game's files (assets), saves (hdd), settings, logs, caches and texture packs.
// <NAME>_DATA_ROOT (GameInfo::name upper case, e.g. UNCHAINED_DATA_ROOT) moves it, else the exe's folder. The program
// (the exe, the built game code, the compiler) stays beside the exe.
std::filesystem::path DataDir();
// the console's hard disk: save_folder, else hdd (hddN for --instance N) beside the exe (saves.cpp)
std::filesystem::path HardDisk();
// at start, before boot: the saves copied to "save backups\<date time>" beside the disk, save_backups kept
void BackupSaves();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
// --game <disc image or folder> plays that copy where it is, instead of the one in assets
std::filesystem::path GameArg();
// what the program exits with when it cannot start, so a launcher can tell the player why (0: the player stopped it)
inline constexpr int kExitNoGame = 3;        // no copy of the game found, or it could not be read
inline constexpr int kExitOtherRelease = 4;  // another release of the game than this build runs
inline constexpr int kExitGameCode = 5;      // the game's code could not be built or loaded
int GameError();                             // why the last FindGame failed: kExitNoGame or kExitOtherRelease
std::wstring GameErrorText();                // and what it told the player
void QuietGameErrors();                      // FindGame shows no dialogs (a setup step without a window)
int GameCodeError();                         // after PrepareGameCode failed: kExitGameCode, or 0 when cancelled
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
// the same build without its window, for a launcher's setup screen that shows its own progress: false when the game
// is not found or its code is current. The status is read while it runs.
bool StartGameCodeBuild();
struct BuildStatus {
    bool running = false, done = false, ok = false;
    std::wstring stage;      // what it is doing, as the build window says it
    std::wstring time_left;  // while compiling
    float fraction = 0;      // 0..1
    std::wstring failure;    // when it failed
};
BuildStatus GameCodeBuildStatus();
void CancelGameCodeBuild();
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();
// XBE_KEYS="<seconds>:<virtual key>,...": each key pressed and let go in the game window at that time after the
// start, for tests that open menus or take screenshots; press sends one key to the window (any thread)
void StartScriptedKeys(void (*press)(uint32_t vk));

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // xbe file name, SHA-1: other releases the executable runs, each from its own [xbe] named config
    std::vector<std::pair<const char*, const char*>> releases;
    // the part of the back buffer a TV shows (the top left view_w x view_h), 0 for all of it: a game that draws
    // 640x448 of a 640x480 buffer leaves its last lines unused, and its 4:3 is the 448 lines' aspect
    uint32_t view_w = 0, view_h = 0;
    // the game widens its own 3D view by WideFactor() (a hook on its projection), which allows Wide::Expand
    bool expands = false;
    // a render target's host pixels per console pixel in place of the render scale (a shadow map drawn finer), 0 for
    // the render scale; data is the surface's virtual address, a depth target shares its colour target's size
    float (*target_scale)(uint32_t data, uint32_t gw, uint32_t gh, bool depth, float render_scale) = nullptr;
    // only the release this executable was made from (and `releases`) starts: another is refused with release_name,
    // the expected SHA-1 and the one found, instead of offered at the player's risk
    bool one_release = false;
    // a file on the game's disc served from the host instead (a settings file the game reads, rewritten): path is
    // the disc path ("Game.ini", "map\\hub.mxb"); true with host set takes it; kernel::ReadOriginalGameFile reads
    // the disc's own
    bool (*disc_file)(const std::string& path, std::filesystem::path& host) = nullptr;
    // a draw sampling this texture (its virtual address) is left out: an effect the player turned off
    bool (*skip_texture)(uint32_t data) = nullptr;
    // the game's own launcher or Configuration app, before the game is looked for (not on a relaunch): false ends the
    // run (the player closed it), true starts the game; it reads the command line itself
    bool (*launcher)() = nullptr;
    const wchar_t* release_name = nullptr;
    // a texture's picture edited by the game (button prompts for the device in use): texture_edits says which
    // textures (the texture pack's keys, with their size) it may edit; texture_image, on a worker thread, gets level 0
    // as BGRA rows packed and may change its size; false draws the console's picture
    bool (*texture_edits)(uint64_t key, uint32_t w, uint32_t h) = nullptr;
    bool (*texture_image)(uint64_t key, uint32_t& w, uint32_t& h, std::vector<uint8_t>& bgra) = nullptr;
};
extern const GameInfo kGame;

// any thread: every GameInfo::texture_image made again at the next frame (the device in use changed)
void RefreshTextureImages();

// sleeps out the rest of the frame for the fps cap
void Pace();
// the game window's monitor's refresh rate in Hz, 0 when unreadable (read again each second)
uint32_t RefreshRate();
// the monitor a window is on (nullptr: the primary): whether Windows drives it in HDR, and its peak brightness
struct HdrInfo {
    bool hdr = false;
    float max_nits = 0;
};
HdrInfo HdrDisplay(void* hwnd);

}  // namespace app
