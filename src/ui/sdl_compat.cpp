#include "emulator/sdl_compat.h"

#ifndef _WIN32

// SDL2/OpenGL implementation of the small Win32-shaped surface used by the
// shared UI code. Nothing here is compiled on Windows.

#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#include "emulator/logger.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace {

// Message ids mirrored from window.cpp (WM_APP + n). They are intentionally
// not exported by sdl_compat.h: only the translation layer needs them.
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kTrayRestoreMsg = WM_APP + 2;
constexpr UINT kDebugMsg = WM_APP + 9;
constexpr UINT kUpdateResultMsg = WM_APP + 10;

SDL_Window* g_window = nullptr;
void* g_mainWindow = nullptr;
std::atomic<bool> g_quitRequested{ false };

std::mutex g_customMutex;
std::deque<MSG> g_customMessages;

Uint32 eventForMessage(UINT message) {
    switch (message) {
        case WM_CLOSE:          return emu::sdlcompat::closeEvent();
        case kUpdateResultMsg:  return emu::sdlcompat::updateResultEvent();
        case kDebugMsg:         return emu::sdlcompat::debugEvent();
        case kTrayRestoreMsg:   return emu::sdlcompat::trayRestoreEvent();
        default:                return 0;
    }
}

std::wstring wideFromUtf8(const char* text) {
    std::wstring out;
    if (!text) return out;
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        unsigned int cp = 0;
        const unsigned char c = *p++;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && (p[0] & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (p[0] & 0x3F);
            ++p;
        } else if ((c & 0xF0) == 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((c & 0xF8) == 0xF0 && (p[0] & 0xC0) == 0x80 &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) |
                 ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            out.push_back(L'?');
            continue;
        }
        if (cp <= 0xFFFF) {
            out.push_back((wchar_t)cp);
        } else if (sizeof(wchar_t) == 4) {
            out.push_back((wchar_t)cp);
        } else {
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

std::string utf8FromWide(const wchar_t* text) {
    std::string out;
    if (!text) return out;
    for (const wchar_t* p = text; *p; ++p) {
        unsigned int cp = (unsigned int)*p;
        if (cp >= 0xD800 && cp <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned int)(p[1]) - 0xDC00);
            ++p;
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Win32 VK_* -> SDL scancode, only for the keys queried through GetKeyState.
SDL_Scancode scancodeForVk(int vk) {
    switch (vk) {
        case VK_CONTROL: return SDL_SCANCODE_LCTRL; // RCTRL checked separately
        case VK_ESCAPE:  return SDL_SCANCODE_ESCAPE;
        case VK_F11:     return SDL_SCANCODE_F11;
        case VK_SPACE:   return SDL_SCANCODE_SPACE;
        case VK_BACK:    return SDL_SCANCODE_BACKSPACE;
        case VK_TAB:     return SDL_SCANCODE_TAB;
        case VK_RETURN:  return SDL_SCANCODE_RETURN;
        case VK_LEFT:    return SDL_SCANCODE_LEFT;
        case VK_UP:      return SDL_SCANCODE_UP;
        case VK_RIGHT:   return SDL_SCANCODE_RIGHT;
        case VK_DOWN:    return SDL_SCANCODE_DOWN;
        default: break;
    }
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_0 + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    return SDL_SCANCODE_UNKNOWN;
}

void pushQuitEvent() {
    SDL_Event event{};
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

} // namespace

namespace emu {
namespace sdlcompat {

void setWindow(SDL_Window* window) { g_window = window; }
SDL_Window* window() { return g_window; }

void setMainWindow(void* mainWindow) { g_mainWindow = mainWindow; }
void* mainWindow() { return g_mainWindow; }

Uint32 updateResultEvent() {
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

Uint32 debugEvent() {
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

Uint32 trayRestoreEvent() {
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

Uint32 closeEvent() {
    static const Uint32 type = SDL_RegisterEvents(1);
    return type;
}

void enableDpiAwareness() {
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
}

bool imguiInit(HWND hwnd) {
    (void)hwnd;
    if (!g_window) return false;
    return ImGui_ImplSDL2_InitForOpenGL(g_window, SDL_GL_GetCurrentContext());
}

void imguiNewFrame() {
    ImGui_ImplSDL2_NewFrame();
}

void imguiShutdown() {
    ImGui_ImplSDL2_Shutdown();
}

bool imguiRendererInit(void* device, void* context) {
    (void)device;
    (void)context;
    // Let the backend pick from the actual GL version/profile. Hard-coding
    // #version 120 fails on macOS core-profile contexts and on new desktops.
    return ImGui_ImplOpenGL3_Init(nullptr);
}

void imguiRendererNewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
}

void imguiRendererRenderDrawData(ImDrawData* drawData) {
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

void imguiRendererShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
}

void imguiRendererCreateDeviceObjects() {
    ImGui_ImplOpenGL3_CreateDeviceObjects();
}

void processEvent(const SDL_Event& event) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    const Uint32 type = event.type;
    if (type != closeEvent() && type != updateResultEvent() &&
        type != debugEvent() && type != trayRestoreEvent()) {
        return;
    }
    MSG msg{};
    msg.hwnd = g_window;
    msg.message = (UINT)event.user.code;
    msg.wParam = (WPARAM)(uintptr_t)event.user.data1;
    msg.lParam = (LPARAM)(intptr_t)event.user.data2;
    msg.time = SDL_GetTicks();
    std::lock_guard<std::mutex> lock(g_customMutex);
    g_customMessages.push_back(msg);
}

bool nextCustomMessage(MSG& msg) {
    std::lock_guard<std::mutex> lock(g_customMutex);
    if (g_customMessages.empty()) return false;
    msg = g_customMessages.front();
    g_customMessages.pop_front();
    return true;
}

bool quitRequested() { return g_quitRequested.load(); }

void setQuitRequested(bool value) { g_quitRequested.store(value); }

int keyToVk(SDL_Keycode key) {
    switch (key) {
        case SDLK_RETURN:      return VK_RETURN;
        case SDLK_RETURN2:     return VK_RETURN;
        case SDLK_KP_ENTER:    return VK_RETURN;
        case SDLK_ESCAPE:      return VK_ESCAPE;
        case SDLK_BACKSPACE:   return VK_BACK;
        case SDLK_TAB:         return VK_TAB;
        case SDLK_LEFT:        return VK_LEFT;
        case SDLK_UP:          return VK_UP;
        case SDLK_RIGHT:       return VK_RIGHT;
        case SDLK_DOWN:        return VK_DOWN;
        case SDLK_INSERT:      return VK_INSERT;
        case SDLK_DELETE:      return VK_DELETE;
        case SDLK_HOME:        return VK_HOME;
        case SDLK_END:         return VK_END;
        case SDLK_PAGEUP:      return VK_PRIOR;
        case SDLK_PAGEDOWN:    return VK_NEXT;
        case SDLK_F1:          return VK_F1;
        case SDLK_F2:          return VK_F1 + 1;
        case SDLK_F3:          return VK_F1 + 2;
        case SDLK_F4:          return VK_F1 + 3;
        case SDLK_F5:          return VK_F1 + 4;
        case SDLK_F6:          return VK_F1 + 5;
        case SDLK_F7:          return VK_F1 + 6;
        case SDLK_F8:          return VK_F1 + 7;
        case SDLK_F9:          return VK_F1 + 8;
        case SDLK_F10:         return VK_F1 + 9;
        case SDLK_F11:         return VK_F11;
        case SDLK_F12:         return VK_F12;
        case SDLK_LCTRL:       return 0xA2; // VK_LCONTROL
        case SDLK_RCTRL:       return 0xA3; // VK_RCONTROL
        case SDLK_LSHIFT:      return 0xA0; // VK_LSHIFT
        case SDLK_RSHIFT:      return 0xA1; // VK_RSHIFT
        case SDLK_LALT:        return 0xA4; // VK_LMENU
        case SDLK_RALT:        return 0xA5; // VK_RMENU
        case SDLK_LGUI:        return 0x5B; // VK_LWIN
        case SDLK_RGUI:        return 0x5C; // VK_RWIN
        case SDLK_SPACE:       return VK_SPACE;
        case SDLK_SEMICOLON:   return VK_OEM_1;
        case SDLK_PLUS:        return 0xBB; // VK_OEM_PLUS
        case SDLK_COMMA:       return 0xBC; // VK_OEM_COMMA
        case SDLK_MINUS:       return 0xBD; // VK_OEM_MINUS
        case SDLK_PERIOD:      return 0xBE; // VK_OEM_PERIOD
        case SDLK_SLASH:       return 0xBF; // VK_OEM_2
        case SDLK_BACKQUOTE:   return VK_OEM_3;
        case SDLK_LEFTBRACKET: return VK_OEM_4;
        case SDLK_BACKSLASH:   return 0xDC; // VK_OEM_5
        case SDLK_RIGHTBRACKET:return VK_OEM_7;
        case SDLK_QUOTE:       return 0xDE; // apostrophe (not named in sdl_compat.h)
        case SDLK_LESS:        return VK_OEM_102;
        case SDLK_KP_MULTIPLY: return VK_MULTIPLY;
        case SDLK_KP_PLUS:     return VK_ADD;
        case SDLK_KP_COMMA:    return VK_SEPARATOR;
        case SDLK_KP_MINUS:    return VK_SUBTRACT;
        case SDLK_KP_PERIOD:   return VK_DECIMAL;
        case SDLK_KP_DIVIDE:   return VK_DIVIDE;
        default: break;
    }
    if (key >= SDLK_0 && key <= SDLK_9) return (int)('0' + (key - SDLK_0));
    if (key >= SDLK_a && key <= SDLK_z) return (int)('A' + (key - SDLK_a));
    if (key >= SDLK_KP_0 && key <= SDLK_KP_9) return VK_NUMPAD0 + (key - SDLK_KP_0);
    return 0;
}

std::string dropPath(HDROP drop) {
    return drop ? std::string((const char*)drop) : std::string();
}

} // namespace sdlcompat
} // namespace emu

// ---------------------------------------------------------------------------
// Win32 API shims
// ---------------------------------------------------------------------------

bool GetClientRect(HWND hwnd, RECT* rect) {
    (void)hwnd;
    if (!rect) return false;
    int w = 0, h = 0;
    SDL_GetWindowSize(emu::sdlcompat::window(), &w, &h);
    rect->left = rect->top = 0;
    rect->right = w;
    rect->bottom = h;
    return true;
}

bool GetWindowRect(HWND hwnd, RECT* rect) {
    (void)hwnd;
    if (!rect) return false;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_Window* window = emu::sdlcompat::window();
    if (!window) return false;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &w, &h);
    rect->left = x;
    rect->top = y;
    rect->right = x + w;
    rect->bottom = y + h;
    return true;
}

bool GetCursorPos(POINT* point) {
    if (!point) return false;
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    int wx = 0, wy = 0;
    SDL_Window* window = emu::sdlcompat::window();
    if (window) SDL_GetWindowPosition(window, &wx, &wy);
    point->x = wx + x;
    point->y = wy + y;
    return true;
}

bool SetCursorPos(int x, int y) {
    SDL_WarpMouseGlobal(x, y);
    return true;
}

bool IsWindow(HWND hwnd) {
    return hwnd != nullptr && hwnd == emu::sdlcompat::window();
}

bool IsWindowVisible(HWND hwnd) {
    SDL_Window* window = emu::sdlcompat::window();
    if (!window || (hwnd && hwnd != window)) return false;
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) != 0;
}

bool IsIconic(HWND hwnd) {
    SDL_Window* window = emu::sdlcompat::window();
    if (!window || (hwnd && hwnd != window)) return false;
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
}

bool IsZoomed(HWND hwnd) {
    SDL_Window* window = emu::sdlcompat::window();
    if (!window || (hwnd && hwnd != window)) return false;
    return (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool ShowWindow(HWND hwnd, int command) {
    SDL_Window* window = emu::sdlcompat::window();
    (void)hwnd;
    if (!window) return false;
    switch (command) {
        case SW_HIDE:         SDL_HideWindow(window); break;
        case SW_MINIMIZE:     SDL_MinimizeWindow(window); break;
        case SW_MAXIMIZE:     SDL_MaximizeWindow(window); break;
        case SW_RESTORE:      SDL_RestoreWindow(window); SDL_ShowWindow(window); break;
        case SW_SHOWNOACTIVATE:
        case SW_SHOW:
        default:              SDL_ShowWindow(window); SDL_RaiseWindow(window); break;
    }
    return true;
}

bool UpdateWindow(HWND hwnd) {
    (void)hwnd;
    return true;
}

bool SetForegroundWindow(HWND hwnd) {
    (void)hwnd;
    SDL_Window* window = emu::sdlcompat::window();
    if (!window) return false;
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    return true;
}

bool SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    (void)hwnd;
    SDL_Window* window = emu::sdlcompat::window();
    if (!window) return false;
    if (after == HWND_TOP) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        return true;
    }
    SDL_SetWindowFullscreen(window, 0);
    if (!(flags & SWP_NOMOVE)) SDL_SetWindowPosition(window, x, y);
    if (!(flags & SWP_NOSIZE)) SDL_SetWindowSize(window, std::max(1, cx), std::max(1, cy));
    return true;
}

bool SetCapture(HWND hwnd) {
    (void)hwnd;
    return true;
}

void ReleaseCapture() {}

bool PostMessageW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)hwnd;
    if (message == WM_QUIT) {
        emu::sdlcompat::setQuitRequested(true);
        pushQuitEvent();
        return true;
    }
    const Uint32 type = eventForMessage(message);
    if (type == 0 || type == (Uint32)-1) return false;
    SDL_Event event{};
    event.user.type = type;
    event.user.timestamp = SDL_GetTicks();
    SDL_Window* window = emu::sdlcompat::window();
    event.user.windowID = window ? SDL_GetWindowID(window) : 0;
    event.user.code = (int)message;
    event.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(wParam));
    // Pointer payloads (e.g. UpdateInfo*) stay valid until handle() consumes it.
    event.user.data2 = reinterpret_cast<void*>(static_cast<intptr_t>(lParam));
    return SDL_PushEvent(&event) == 1;
}

void PostQuitMessage(int exitCode) {
    (void)exitCode;
    emu::sdlcompat::setQuitRequested(true);
    pushQuitEvent();
}

LRESULT DefWindowProcW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    (void)hwnd; (void)message; (void)wParam; (void)lParam;
    return 0;
}

bool DestroyWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;
    emu::sdlcompat::setQuitRequested(true);
    pushQuitEvent();
    return true;
}

int MessageBoxW(HWND hwnd, const wchar_t* text, const wchar_t* caption, UINT type) {
    SDL_Window* window = emu::sdlcompat::window();
    if (hwnd && hwnd != window) window = nullptr;
    const std::string message = utf8FromWide(text);
    const std::string title = utf8FromWide(caption);
    SDL_ShowSimpleMessageBox((type & MB_ICONERROR) ? SDL_MESSAGEBOX_ERROR
                                                   : SDL_MESSAGEBOX_INFORMATION,
                             title.c_str(), message.c_str(), window);
    return 1; // IDOK
}

short GetKeyState(int key) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) return 0;
    auto pressed = [&](SDL_Scancode sc) -> short {
        return sc != SDL_SCANCODE_UNKNOWN && keys[sc] ? (short)0x8000 : (short)0;
    };
    if (key == VK_CONTROL) {
        return (short)(pressed(SDL_SCANCODE_LCTRL) | pressed(SDL_SCANCODE_RCTRL));
    }
    return pressed(scancodeForVk(key));
}

UINT DragQueryFileW(HDROP drop, UINT index, wchar_t* buffer, UINT capacity) {
    if (!drop) return 0;
    if (index == 0xFFFFFFFF) return 1; // exactly one dropped file is forwarded
    if (index != 0) return 0;
    const std::wstring path = wideFromUtf8((const char*)drop);
    if (!buffer || capacity == 0) return (UINT)path.size();
    const UINT count = std::min(capacity - 1, (UINT)path.size());
    for (UINT i = 0; i < count; ++i) buffer[i] = path[i];
    buffer[count] = L'\0';
    return (UINT)path.size();
}

void DragFinish(HDROP drop) {
    SDL_free(drop);
}

HMENU CreatePopupMenu() {
    return reinterpret_cast<HMENU>(static_cast<uintptr_t>(1));
}

bool AppendMenuW(HMENU menu, UINT flags, UINT_PTR itemId, const wchar_t* text) {
    (void)menu; (void)flags; (void)itemId; (void)text;
    return true;
}

UINT TrackPopupMenu(HMENU menu, UINT flags, int x, int y, int reserved,
                    HWND hwnd, const RECT* rect) {
    (void)menu; (void)flags; (void)x; (void)y; (void)reserved; (void)hwnd; (void)rect;
    return 1; // TRAY_MENU_RESTORE
}

bool DestroyMenu(HMENU menu) {
    (void)menu;
    return true;
}

HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    (void)hwnd; (void)flags;
    int index = SDL_GetWindowDisplayIndex(emu::sdlcompat::window());
    if (index < 0) index = 0;
    return reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(index + 1));
}

bool GetMonitorInfo(HMONITOR monitor, MONITORINFO* info) {
    if (!info) return false;
    int index = (int)reinterpret_cast<uintptr_t>(monitor) - 1;
    if (index < 0) return false;
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(index, &bounds) != 0) return false;
    info->rcMonitor = { bounds.x, bounds.y, bounds.x + bounds.w, bounds.y + bounds.h };
    info->rcWork = info->rcMonitor;
    info->dwFlags = 0;
    return true;
}

bool DwmSetWindowAttribute(HWND hwnd, DWORD attribute, const void* value, DWORD size) {
    (void)hwnd; (void)attribute; (void)value; (void)size;
    return true;
}

bool SetWindowRgn(HWND hwnd, HRGN region, bool redraw) {
    (void)hwnd; (void)region; (void)redraw;
    return true;
}

HRGN CreateRoundRectRgn(int left, int top, int right, int bottom, int w, int h) {
    (void)left; (void)top; (void)right; (void)bottom; (void)w; (void)h;
    return reinterpret_cast<HRGN>(static_cast<uintptr_t>(1));
}

#endif // !_WIN32
