#ifndef _WIN32
#include "app/win32_compat.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <unistd.h>

#include <atomic>

#include "app/app.h"

namespace win32_compat {

namespace {
std::atomic<bool> g_hidden{false};
}

int VirtualKey(int sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_PAUSE: return VK_PAUSE;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
        case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
        case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
        case SDL_SCANCODE_LALT: return VK_LMENU;
        case SDL_SCANCODE_RALT: return VK_RMENU;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_PLUS: return VK_ADD;
        case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
        case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        default: return 0;
    }
}

bool CursorHidden() { return g_hidden; }
void ShowCursorAgain() {
    if (g_hidden.exchange(false)) SDL_ShowCursor();
}

}  // namespace win32_compat

namespace {

SDL_Window* GameWindow() { return static_cast<SDL_Window*>(app::Window()); }

// the scancodes a virtual key stands for, either side for the generic shift, control and alt
bool ScancodeDown(const bool* keys, int vk) {
    switch (vk) {
        case VK_SHIFT: return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        case VK_CONTROL: return keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
        case VK_MENU: return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
        default: break;
    }
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (keys[sc] && win32_compat::VirtualKey(sc) == vk) return true;
    return false;
}

}  // namespace

int16_t GetAsyncKeyState(int vk) {
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
    bool down = false;
    switch (vk) {
        case VK_LBUTTON: down = buttons & SDL_BUTTON_LMASK; break;
        case VK_RBUTTON: down = buttons & SDL_BUTTON_RMASK; break;
        case VK_MBUTTON: down = buttons & SDL_BUTTON_MMASK; break;
        case VK_XBUTTON1: down = buttons & SDL_BUTTON_X1MASK; break;
        case VK_XBUTTON2: down = buttons & SDL_BUTTON_X2MASK; break;
        default: {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            down = keys && ScancodeDown(keys, vk);
        }
    }
    return down ? int16_t(0x8000) : 0;
}

HWND GetForegroundWindow() {
    SDL_Window* w = GameWindow();
    return w && SDL_GetKeyboardFocus() == w ? w : nullptr;
}

BOOL GetClientRect(HWND hwnd, RECT* rc) {
    int w = 0, h = 0;
    if (!hwnd || !SDL_GetWindowSize(static_cast<SDL_Window*>(hwnd), &w, &h)) return 0;
    *rc = {0, 0, w, h};
    return 1;
}

BOOL GetCursorPos(POINT* p) {
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    *p = {int32_t(x), int32_t(y)};
    return 1;
}

BOOL SetCursorPos(int x, int y) {
    if (SDL_Window* w = GameWindow()) SDL_WarpMouseInWindow(w, float(x), float(y));
    return 1;
}

DWORD GetCurrentProcessId() { return DWORD(getpid()); }

DWORD GetWindowThreadProcessId(HWND hwnd, DWORD* pid) {
    if (!hwnd) return 0;
    if (pid) *pid = GetCurrentProcessId();
    return 1;
}

HCURSOR SetCursor(HCURSOR cursor) {
    if (!cursor && !win32_compat::g_hidden.exchange(true)) SDL_HideCursor();
    return nullptr;
}
#endif
