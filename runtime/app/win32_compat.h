#pragma once
// Linux: what a game's Win32 window code uses, from SDL. GameInfo::message gets the same messages as on Windows (the
// SDL host translates its events: key presses as WM_KEYDOWN with virtual-key codes, mouse buttons, focus), and key
// state, the cursor and the window's client area come from SDL. Include <windows.h> on Windows, this on Linux.
#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>

using HWND = void*;
using DWORD = uint32_t;
using HCURSOR = void*;
using BOOL = int;
using UINT = uint32_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;

struct RECT {
    int32_t left, top, right, bottom;
};
struct POINT {
    int32_t x, y;
};
struct WINDOWPOS {
    HWND hwnd, hwndInsertAfter;
    int x, y, cx, cy;
    UINT flags;
};

constexpr BOOL TRUE = 1, FALSE = 0;
constexpr int MAX_PATH = 260;

#define LOWORD(l) (uint16_t(uintptr_t(l) & 0xFFFF))
#define HIWORD(l) (uint16_t((uintptr_t(l) >> 16) & 0xFFFF))

// messages the SDL host sends
constexpr UINT WM_ACTIVATE = 0x0006, WM_KILLFOCUS = 0x0008, WM_SETCURSOR = 0x0020, WM_WINDOWPOSCHANGING = 0x0046,
               WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105,
               WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204,
               WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_XBUTTONDOWN = 0x020B,
               WM_XBUTTONUP = 0x020C;
constexpr UINT WA_INACTIVE = 0, WA_ACTIVE = 1;
constexpr uint16_t HTCLIENT = 1;
constexpr UINT SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010;

// virtual-key codes
enum : int {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2D,
    VK_DELETE = 0x2E, VK_NUMPAD0 = 0x60, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E,
    VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
    VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5,
    VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE,
    VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
};

// the key or button's state now: bit 15 while held
int16_t GetAsyncKeyState(int vk);
// the game window while it has the keyboard focus, else null
HWND GetForegroundWindow();
BOOL GetClientRect(HWND hwnd, RECT* rc);
BOOL GetCursorPos(POINT* p);  // in the client area: ClientToScreen and ScreenToClient leave points as they are
BOOL SetCursorPos(int x, int y);
inline BOOL ClientToScreen(HWND, POINT*) { return 1; }
inline BOOL ScreenToClient(HWND, POINT*) { return 1; }
HCURSOR SetCursor(HCURSOR cursor);  // null hides it until the next WM_SETCURSOR the game leaves to the host
inline BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return 1; }
DWORD GetCurrentProcessId();
// the game window is this process's
DWORD GetWindowThreadProcessId(HWND hwnd, DWORD* pid);
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
// a path's c_str() is narrow here
inline FILE* _wfopen(const std::filesystem::path& path, const wchar_t* mode) {
    std::string m;
    for (const wchar_t* c = mode; *c; c++) m += char(*c);
    return std::fopen(path.c_str(), m.c_str());
}
// no Windows folder: fonts looked for under it are not found
inline UINT GetWindowsDirectoryW(wchar_t* out, UINT size) {
    if (size) out[0] = 0;
    return 0;
}

namespace win32_compat {
int VirtualKey(int sdl_scancode);  // 0 for a key without one
bool CursorHidden();               // SetCursor(null) since the last WM_SETCURSOR
void ShowCursorAgain();               // and leaves relative mode
void HideCursor();                    // SetCursor(null): hidden, in relative mode for mouse look
void MouseMoved(float x, float y, float dx, float dy);  // each SDL motion event, for GetCursorPos
}  // namespace win32_compat
#endif
