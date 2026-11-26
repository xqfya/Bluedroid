#pragma once

// Win32-shaped compatibility layer used only on Linux/macOS. It lets the
// cross-platform UI code keep the same message/handle vocabulary while SDL2
// and OpenGL provide the real window, input, drag/drop and rendering path.

#ifndef _WIN32

#include <SDL.h>
#include <cstdint>
#include <string>

struct ImDrawData;

using HWND = void*;
using HINSTANCE = void*;
using HICON = void*;
using HCURSOR = void*;
using HMENU = void*;
using HMONITOR = void*;
using HDROP = void*;
using HRGN = void*;
using HDC = void*;
using HANDLE = void*;
using LONG = int32_t;
using LONG_PTR = int64_t;
using ULONG_PTR = uint64_t;
using DWORD_PTR = uint64_t;
using UINT_PTR = uint64_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using BOOL = int;
using UINT = uint32_t;
using ULONGLONG = uint64_t;
using WPARAM = uint64_t;
using LPARAM = int64_t;
using LRESULT = int64_t;
using LPWSTR = wchar_t*;
#define CALLBACK

struct POINT {
    LONG x = 0;
    LONG y = 0;
};

struct RECT {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

struct MSG {
    HWND hwnd = nullptr;
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    DWORD time = 0;
    POINT pt{};
};

struct MONITORINFO {
    DWORD cbSize = sizeof(MONITORINFO);
    RECT rcMonitor{};
    RECT rcWork{};
    DWORD dwFlags = 0;
};

#ifndef HIWORD
#define HIWORD(x) ((WORD)((((DWORD_PTR)(x)) >> 16) & 0xFFFF))
#endif
#ifndef LOWORD
#define LOWORD(x) ((WORD)((DWORD_PTR)(x) & 0xFFFF))
#endif

// Window messages used by the shared UI code.
static constexpr UINT WM_APP = 0x8000;
static constexpr UINT WM_CREATE = 0x0001;
static constexpr UINT WM_NCHITTEST = 0x0084;
static constexpr UINT WM_SIZE = 0x0005;
static constexpr UINT WM_DROPFILES = 0x0233;
static constexpr UINT WM_PAINT = 0x000F;
static constexpr UINT WM_KEYDOWN = 0x0100;
static constexpr UINT WM_KEYUP = 0x0101;
static constexpr UINT WM_CHAR = 0x0102;
static constexpr UINT WM_LBUTTONDOWN = 0x0201;
static constexpr UINT WM_LBUTTONUP = 0x0202;
static constexpr UINT WM_LBUTTONDBLCLK = 0x0203;
static constexpr UINT WM_RBUTTONUP = 0x0205;
static constexpr UINT WM_MOUSEMOVE = 0x0200;
static constexpr UINT WM_CAPTURECHANGED = 0x021D;
static constexpr UINT WM_CLOSE = 0x0010;
static constexpr UINT WM_DESTROY = 0x0002;
static constexpr UINT WM_QUIT = 0x0012;
static constexpr UINT PM_REMOVE = 0x0001;

// Hit test codes.
static constexpr LRESULT HTCLIENT = 1;
static constexpr LRESULT HTCAPTION = 2;
static constexpr LRESULT HTLEFT = 10;
static constexpr LRESULT HTRIGHT = 11;
static constexpr LRESULT HTTOP = 12;
static constexpr LRESULT HTTOPLEFT = 13;
static constexpr LRESULT HTTOPRIGHT = 14;
static constexpr LRESULT HTBOTTOM = 15;
static constexpr LRESULT HTBOTTOMLEFT = 16;
static constexpr LRESULT HTBOTTOMRIGHT = 17;

// Virtual keys mirrored from WinUser.h for the shared input mapping.
static constexpr int VK_RETURN = 0x0D;
static constexpr int VK_BACK = 0x08;
static constexpr int VK_TAB = 0x09;
static constexpr int VK_CONTROL = 0x11;
static constexpr int VK_ESCAPE = 0x1B;
static constexpr int VK_SPACE = 0x20;
static constexpr int VK_LEFT = 0x25;
static constexpr int VK_UP = 0x26;
static constexpr int VK_RIGHT = 0x27;
static constexpr int VK_DOWN = 0x28;
static constexpr int VK_INSERT = 0x2D;
static constexpr int VK_DELETE = 0x2E;
static constexpr int VK_HOME = 0x24;
static constexpr int VK_END = 0x23;
static constexpr int VK_PRIOR = 0x21;
static constexpr int VK_NEXT = 0x22;
static constexpr int VK_F1 = 0x70;
static constexpr int VK_F2 = 0x71;
static constexpr int VK_F3 = 0x72;
static constexpr int VK_F4 = 0x73;
static constexpr int VK_F5 = 0x74;
static constexpr int VK_F6 = 0x75;
static constexpr int VK_F7 = 0x76;
static constexpr int VK_F8 = 0x77;
static constexpr int VK_F9 = 0x78;
static constexpr int VK_F10 = 0x79;
static constexpr int VK_F11 = 0x7A;
static constexpr int VK_F12 = 0x7B;
static constexpr int VK_OEM_1 = 0xBA;
static constexpr int VK_OEM_3 = 0xC0;
static constexpr int VK_OEM_4 = 0xDB;
static constexpr int VK_OEM_7 = 0xDD;
static constexpr int VK_OEM_102 = 0xE2;
static constexpr int VK_NUMPAD0 = 0x60;
static constexpr int VK_NUMPAD9 = 0x69;
static constexpr int VK_MULTIPLY = 0x6A;
static constexpr int VK_ADD = 0x6B;
static constexpr int VK_SEPARATOR = 0x6C;
static constexpr int VK_SUBTRACT = 0x6D;
static constexpr int VK_DECIMAL = 0x6E;
static constexpr int VK_DIVIDE = 0x6F;

// Window styles/flags (values are only used as tokens by the shim).
static constexpr LONG_PTR WS_POPUP = 0x80000000u;
static constexpr LONG_PTR WS_SYSMENU = 0x00080000u;
static constexpr LONG_PTR WS_MINIMIZEBOX = 0x00020000u;
static constexpr LONG_PTR WS_MAXIMIZEBOX = 0x00010000u;
static constexpr LONG_PTR WS_CLIPCHILDREN = 0x02000000u;
static constexpr LONG_PTR WS_CLIPSIBLINGS = 0x04000000u;
static constexpr LONG_PTR WS_VISIBLE = 0x10000000u;
static constexpr LONG_PTR WS_EX_APPWINDOW = 0x00040000u;
static constexpr LONG_PTR WS_EX_TOOLWINDOW = 0x00000080u;
static constexpr UINT SW_HIDE = 0;
static constexpr UINT SW_SHOW = 5;
static constexpr UINT SW_SHOWNOACTIVATE = 4;
static constexpr UINT SW_MINIMIZE = 6;
static constexpr UINT SW_RESTORE = 9;
static constexpr UINT SW_MAXIMIZE = 3;
static constexpr UINT SWP_NOSIZE = 0x0001;
static constexpr UINT SWP_NOMOVE = 0x0002;
static constexpr UINT SWP_NOZORDER = 0x0004;
static constexpr UINT SWP_NOACTIVATE = 0x0010;
static constexpr UINT SWP_FRAMECHANGED = 0x0020;
static inline HWND HWND_TOP = reinterpret_cast<HWND>(static_cast<uintptr_t>(-1));
static constexpr UINT MONITOR_DEFAULTTONEAREST = 0;
static constexpr UINT DWMWA_WINDOW_CORNER_PREFERENCE = 33;
static constexpr DWORD DWMWCP_DONOTROUND = 1;
static constexpr UINT MF_STRING = 0;
static constexpr UINT MF_SEPARATOR = 0x0800;
static constexpr UINT TPM_RETURNCMD = 0x0100;
static constexpr UINT TPM_NONOTIFY = 0x0080;
static constexpr UINT MB_OK = 0;
static constexpr UINT MB_ICONERROR = 0x10;

namespace emu {
namespace sdlcompat {

void setWindow(SDL_Window* window);
SDL_Window* window();
void setMainWindow(void* mainWindow);
void* mainWindow();

Uint32 updateResultEvent();
Uint32 debugEvent();
Uint32 trayRestoreEvent();
Uint32 closeEvent();

// ImGui backend shims with the Win32/DX11 names used by the shared UI code.
void enableDpiAwareness();
bool imguiInit(HWND hwnd);
void imguiNewFrame();
void imguiShutdown();
bool imguiRendererInit(void* device, void* context);
void imguiRendererNewFrame();
void imguiRendererRenderDrawData(ImDrawData* drawData);
void imguiRendererShutdown();
void imguiRendererCreateDeviceObjects();

// SDL event helpers used by MainWindow::runMessageLoop().
void processEvent(const SDL_Event& event);
bool nextCustomMessage(MSG& msg);
bool quitRequested();
void setQuitRequested(bool value);
int keyToVk(SDL_Keycode key);
std::string dropPath(HDROP drop);

} // namespace sdlcompat
} // namespace emu

// ImGui backend names kept so window.cpp needs no Windows-specific branches.
inline void ImGui_ImplWin32_EnableDpiAwareness() { emu::sdlcompat::enableDpiAwareness(); }
inline bool ImGui_ImplWin32_Init(HWND hwnd) { return emu::sdlcompat::imguiInit(hwnd); }
inline void ImGui_ImplWin32_NewFrame() { emu::sdlcompat::imguiNewFrame(); }
inline void ImGui_ImplWin32_Shutdown() { emu::sdlcompat::imguiShutdown(); }
inline bool ImGui_ImplDX11_Init(void* device, void* context) {
    (void)device; (void)context;
    return emu::sdlcompat::imguiRendererInit(device, context);
}
inline void ImGui_ImplDX11_NewFrame() { emu::sdlcompat::imguiRendererNewFrame(); }
inline void ImGui_ImplDX11_RenderDrawData(ImDrawData* drawData) {
    emu::sdlcompat::imguiRendererRenderDrawData(drawData);
}
inline void ImGui_ImplDX11_Shutdown() { emu::sdlcompat::imguiRendererShutdown(); }
inline void ImGui_ImplDX11_CreateDeviceObjects() { emu::sdlcompat::imguiRendererCreateDeviceObjects(); }

// Small Win32 API surface used by the shared UI code.
bool GetClientRect(HWND hwnd, RECT* rect);
bool GetWindowRect(HWND hwnd, RECT* rect);
bool GetCursorPos(POINT* point);
bool SetCursorPos(int x, int y);
bool IsWindow(HWND hwnd);
bool IsWindowVisible(HWND hwnd);
bool IsIconic(HWND hwnd);
bool IsZoomed(HWND hwnd);
bool ShowWindow(HWND hwnd, int command);
bool UpdateWindow(HWND hwnd);
bool SetForegroundWindow(HWND hwnd);
bool SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags);
bool SetCapture(HWND hwnd);
void ReleaseCapture();
bool PostMessageW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
void PostQuitMessage(int exitCode);
int MessageBoxW(HWND hwnd, const wchar_t* text, const wchar_t* caption, UINT type);
short GetKeyState(int key);
UINT DragQueryFileW(HDROP drop, UINT index, wchar_t* buffer, UINT capacity);
void DragFinish(HDROP drop);
HMENU CreatePopupMenu();
bool AppendMenuW(HMENU menu, UINT flags, UINT_PTR itemId, const wchar_t* text);
UINT TrackPopupMenu(HMENU menu, UINT flags, int x, int y, int reserved,
                    HWND hwnd, const RECT* rect);
bool DestroyMenu(HMENU menu);
HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
bool GetMonitorInfo(HMONITOR monitor, MONITORINFO* info);
bool DwmSetWindowAttribute(HWND hwnd, DWORD attribute, const void* value, DWORD size);
bool SetWindowRgn(HWND hwnd, HRGN region, bool redraw);
HRGN CreateRoundRectRgn(int left, int top, int right, int bottom, int w, int h);

#endif // !_WIN32
