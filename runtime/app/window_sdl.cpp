// The Linux host: an SDL3 window and event loop, crash logging; the game is described by app::kGame. GameInfo::message
// gets the Win32 messages a game's window code expects (app/win32_compat.h). Windows' host is main.cpp.
#ifndef _WIN32
#include <SDL3/SDL.h>

#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#include "app/app.h"
#include "app/win32_compat.h"
#include "core/input.h"
#include "core/log.h"
#include "core/platform.h"
#include "gpu/backend.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
Uint32 g_event_apply = 0, g_event_open = 0;
std::mutex g_open_lock;
std::condition_variable g_opened;
uint64_t g_open_requests = 0, g_open_done = 0;

// what the console's last guest thread was doing when a fault ends the process; the default action then writes the
// core dump (or Ubuntu's crash report)
void OnFault(int sig, siginfo_t* info, void*) {
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        uint64_t va = uint64_t(reinterpret_cast<uintptr_t>(info->si_addr));
        uint64_t g = uint64_t(reinterpret_cast<uintptr_t>(kernel::g_base));
        LOGE("signal %d (%s) at %llX on guest thread %X, guest %08llX", sig, strsignal(sig), (unsigned long long)va,
             t->id, (unsigned long long)(va - g));
        kernel::LogGuestStack(*t->ctx, 256);
    } else {
        LOGE("signal %d (%s) at %p", sig, strsignal(sig), info->si_addr);
    }
    logx::Flush();
    signal(sig, SIG_DFL);
    raise(sig);
}

void CatchFaults() {
    struct sigaction sa{};
    sa.sa_sigaction = OnFault;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// event thread only. Fullscreen and borderless take the desktop (SDL's fullscreen without a mode change, which a
// Wayland compositor gives anyway); a set resolution in Fullscreen asks for the closest mode.
void ApplyNow() {
    const Settings& s = Get();
    Mode out = Output();
    if (s.display_mode == DisplayMode::Windowed) {
        SDL_SetWindowFullscreen(g_window, false);
        SDL_SetWindowBordered(g_window, true);
        SDL_SetWindowResizable(g_window, true);
        // the setting is in pixels, the window's size in the desktop's units
        float k = SDL_GetWindowDisplayScale(g_window);
        if (k <= 0) k = 1;
        SDL_SetWindowSize(g_window, int(out.width / k + 0.5f), int(out.height / k + 0.5f));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        const SDL_DisplayMode* mode = nullptr;
        SDL_DisplayMode closest{};
        if (s.display_mode == DisplayMode::Fullscreen && s.width && s.height) {
            SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
            if (SDL_GetClosestFullscreenDisplayMode(d ? d : SDL_GetPrimaryDisplay(), int(s.width), int(s.height), 0,
                                                    true, &closest))
                mode = &closest;
        }
        SDL_SetWindowFullscreenMode(g_window, mode);
        SDL_SetWindowFullscreen(g_window, true);
    }
}

void Show() {
    CloseSplash();
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
}

// a game's window code sees the event as the Win32 message it would have had
bool Message(UINT msg, WPARAM wp, LPARAM lp) {
    intptr_t result = 0;
    return kGame.message && kGame.message(g_window, msg, wp, lp, &result);
}

void OnEvent(const SDL_Event& e) {
    if (e.type == g_event_apply) {
        ApplyNow();
        return;
    }
    if (e.type == g_event_open) {
        if (e.user.code) ApplyNow();
        Show();
        std::lock_guard lock(g_open_lock);
        g_open_done = uint64_t(reinterpret_cast<uintptr_t>(e.user.data1));
        g_opened.notify_all();
        return;
    }
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int vk = win32_compat::VirtualKey(e.key.scancode);
            if (!vk) break;
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            bool alt = (e.key.mod & SDL_KMOD_ALT) || vk == VK_LMENU || vk == VK_RMENU || vk == VK_F10;
            // Windows reports Alt as VK_MENU in its messages and F10 as a system key
            WPARAM wp = (vk == VK_LMENU || vk == VK_RMENU) ? VK_MENU : WPARAM(vk);
            LPARAM lp = 1 | (LPARAM(e.key.scancode & 0xFF) << 16) | (e.key.repeat ? LPARAM(1) << 30 : 0);
            UINT msg = alt ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP);
            if (Message(msg, wp, lp) || !down) break;
            if (vk == VK_F3) DumpThreads();
            if (vk == VK_F12) gpu::g_screenshot = true;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            LPARAM lp = (LPARAM(int(e.button.y)) << 16) | (LPARAM(int(e.button.x)) & 0xFFFF);
            switch (e.button.button) {
                case SDL_BUTTON_LEFT: Message(down ? WM_LBUTTONDOWN : WM_LBUTTONUP, 0, lp); break;
                case SDL_BUTTON_RIGHT: Message(down ? WM_RBUTTONDOWN : WM_RBUTTONUP, 0, lp); break;
                case SDL_BUTTON_MIDDLE: Message(down ? WM_MBUTTONDOWN : WM_MBUTTONUP, 0, lp); break;
                default: Message(down ? WM_XBUTTONDOWN : WM_XBUTTONUP, WPARAM(e.button.button - SDL_BUTTON_X1 + 1) << 16, lp);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            // Windows asks for the cursor on each move inside the client area; left to the host, it shows
            if (!Message(WM_SETCURSOR, WPARAM(g_window), LPARAM(HTCLIENT) | (LPARAM(WM_MOUSEMOVE) << 16)))
                win32_compat::ShowCursorAgain();
            // after: the move that captures the mouse already counts as relative, so the camera does not jump
            win32_compat::MouseMoved(e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            Message(WM_KILLFOCUS, 0, 0);
            Message(WM_ACTIVATE, WA_INACTIVE, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            Message(WM_ACTIVATE, WA_ACTIVE, 0);
            break;
        default:
            break;
    }
}

}  // namespace

void* Window() { return g_window; }

void ApplyDisplay() {
    SDL_Event e{};
    e.type = g_event_apply;
    SDL_PushEvent(&e);
}

void OpenGameWindow(bool apply) {
    std::unique_lock lock(g_open_lock);
    uint64_t n = ++g_open_requests;
    SDL_Event e{};
    e.type = g_event_open;
    e.user.code = apply ? 1 : 0;
    e.user.data1 = reinterpret_cast<void*>(uintptr_t(n));
    SDL_PushEvent(&e);
    g_opened.wait(lock, [&] { return g_open_done >= n; });
}

uint32_t RefreshRate() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d ? d : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? uint32_t(m->refresh_rate + 0.5f) : 0;
}

// HDR output on Linux comes with the Vulkan swap chain's colour spaces: none yet
HdrInfo HdrDisplay(void*) { return {}; }


}  // namespace app

int main(int argc, char** argv) {
    platform::SetArgs(argc, argv);
    app::CatchFaults();
    bool relaunched = platform::HasArg(L"--relaunched");
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    logx::Init((app::DataDir() / "logs").string(), log, relaunched);
    SDL_SetHint(SDL_HINT_APP_ID, platform::Utf8(app::kGame.name).c_str());
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOGE("SDL: %s", SDL_GetError());
        return 1;
    }
    app::g_event_apply = SDL_RegisterEvents(2);
    app::g_event_open = app::g_event_apply + 1;
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    if (app::kGame.launcher && !relaunched && !app::kGame.launcher()) return 0;

    app::GameFiles game;
    if (!app::FindGame(game)) return app::GameError();
    if (!app::PrepareGameCode(game)) return app::GameCodeError();

    bool splash = app::OpenSplash(), handover = app::StartHandover();
    std::string title = platform::Utf8(app::kGame.title);
    if (app::Instance()) title += " [" + std::to_string(app::Instance()) + "]";
    app::g_window = SDL_CreateWindow(title.c_str(), 1280, 720,
                                     SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                         SDL_WINDOW_VULKAN);
    if (!app::g_window) {
        std::string why = SDL_GetError();
        LOGE("SDL: no window: %s", why.c_str());
        // a window for Vulkan fails first where the PC has no Vulkan driver: the same help as the renderer's
        bool vulkan = why.find("Vulkan") != std::string::npos || why.find("vulkan") != std::string::npos;
        platform::Message(app::kGame.short_title,
                          vulkan ? platform::VulkanDriverHelp() : std::wstring(L"The game window could not be opened."),
                          true);
        return 1;
    }
    app::ApplyNow();
    if (!handover && !splash) app::Show();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_InitForVulkan(app::g_window);

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_window);
    app::BackupSaves();
    app::StartScriptedKeys([](uint32_t vk) {
        for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
            if (win32_compat::VirtualKey(sc) != int(vk)) continue;
            for (bool down : {true, false}) {
                SDL_Event e{};
                e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.windowID = SDL_GetWindowID(app::g_window);
                e.key.scancode = SDL_Scancode(sc);
                e.key.key = SDL_GetKeyFromScancode(SDL_Scancode(sc), SDL_KMOD_NONE, false);
                e.key.down = down;
                SDL_PushEvent(&e);
            }
            return;
        }
    });
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), app::HardDisk().string())) {
        platform::Message(app::kGame.short_title, L"The game failed to start, see the log in the logs folder.", true);
        return 1;
    }

    for (bool running = true; running;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        do {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
            {
                std::lock_guard lock(app::UiLock());
                if (ImGui::GetCurrentContext()) ImGui_ImplSDL3_ProcessEvent(&e);
            }
            app::OnEvent(e);
        } while (running && SDL_PollEvent(&e));
    }
    logx::Flush();
    kernel::Shutdown(0);
}
#endif
