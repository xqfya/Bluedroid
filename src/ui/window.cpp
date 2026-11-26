#include "emulator/ui_window.h"
#include "emulator/app_setup.h"
#include "emulator/android_vm.h"
#include "emulator/mock_vm.h"
#include "emulator/config.h"
#include "emulator/logger.h"
#include "emulator/ppm_writer.h"
#include "emulator/network_stack.h"
#include "emulator/audio_engine.h"
#include "emulator/updater.h"
#include "emulator/platform.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "ui.h"

#ifdef _WIN32
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <commdlg.h>
#else
#include "emulator/sdl_compat.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#ifndef _WIN32
// The SDL compatibility header intentionally mirrors only the API surface the
// input manager needs; window.cpp still uses a couple of Win32 BOOL tokens.
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif

#ifndef HIWORD
#define HIWORD(x) ((WORD)((((DWORD_PTR)(x)) >> 16) & 0xFFFF))
#endif
#ifndef LOWORD
#define LOWORD(x) ((WORD)((DWORD_PTR)(x) & 0xFFFF))
#endif

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                           WPARAM wParam, LPARAM lParam);
#else
// Extra Win32-shaped shims used by the shared UI code. sdl_compat.h deliberately
// only exposes the subset needed by the input manager; these live in
// sdl_compat.cpp so window.cpp keeps one shared implementation of the UI.
extern LRESULT DefWindowProcW(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
extern bool DestroyWindow(HWND hwnd);
#endif

namespace emu {

static const wchar_t* CLASS_NAME = L"BlueDroidEmulatorWindow";
static const UINT TRAY_MSG = WM_APP + 1;
static const UINT TRAY_RESTORE_MSG = WM_APP + 2;
static const UINT DEBUG_MSG = WM_APP + 9;
static const UINT UPDATE_RESULT_MSG = WM_APP + 10;
static const UINT TRAY_ID = 1;
static const UINT TRAY_MENU_RESTORE = 1;
static const UINT TRAY_MENU_STOP_ALL = 2;
static const UINT TRAY_MENU_EXIT = 3;

// Fully borderless. Edge resizing is provided by WM_NCHITTEST instead of
// WS_THICKFRAME, because WS_THICKFRAME leaves a thin non-client strip that can
// show the desktop through the top edge.
static const LONG_PTR kWindowStyle =
    WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
    WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

// Older MinGW headers do not expose the Windows 11 corner preference values.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWCP_ROUNDSMALL
#define DWMWCP_ROUNDSMALL 3
#endif

// Single built-in palette for the release. Preset themes were removed; this is
// the only starting look, and users can fine-tune every colour in Appearance.
static hdr::Theme builtInTheme() {
    hdr::Theme t;
    t.windowBg  = IM_COL32(0, 0, 0, 255);
    t.titleBar  = IM_COL32(31, 37, 47, 255);
    t.panel     = IM_COL32(0, 0, 0, 255);
    t.navBar    = IM_COL32(41, 48, 62, 255);
    t.button    = IM_COL32(57, 30, 55, 255);
    t.border    = IM_COL32(76, 87, 108, 255);
    t.text      = IM_COL32(235, 238, 245, 255);
    t.textMuted = IM_COL32(150, 159, 177, 255);
    t.accent    = IM_COL32(244, 150, 255, 255);
    t.success   = IM_COL32(52, 211, 120, 255);
    t.danger    = IM_COL32(226, 62, 78, 255);
    return t;
}

static hdr::Theme g_uiTheme = builtInTheme();
static ImFont* g_fontBold = nullptr;
// Pristine ImGui style captured before any theme/scale is applied. Restoring
// this first keeps repeated applyThemeFromSettings() calls from compounding.
static ImGuiStyle g_baseImGuiStyle;
static bool g_baseImGuiStyleCaptured = false;

namespace ui {
    inline ImVec4 Accent = hdr::V4(g_uiTheme.accent);
    inline ImVec4 Bg = hdr::V4(g_uiTheme.windowBg);
    inline ImVec4 Card = hdr::V4(g_uiTheme.panel);
    inline ImVec4 Widget = hdr::V4(g_uiTheme.button);
    inline ImVec4 WidgetHi = hdr::V4(hdr::Mix(g_uiTheme.button, g_uiTheme.border, 0.35f));
    inline ImVec4 TextDim = hdr::V4(g_uiTheme.textMuted);
    inline ImVec4 TextMain = hdr::V4(g_uiTheme.text);

    inline ImU32 U32(ImVec4 c) { return ImGui::GetColorU32(c); }

    inline void RefreshTheme() {
        Accent = hdr::V4(g_uiTheme.accent);
        Bg = hdr::V4(g_uiTheme.windowBg);
        Card = hdr::V4(g_uiTheme.panel);
        Widget = hdr::V4(g_uiTheme.button);
        WidgetHi = hdr::V4(hdr::Mix(g_uiTheme.button, g_uiTheme.border, 0.35f));
        TextDim = hdr::V4(g_uiTheme.textMuted);
        TextMain = hdr::V4(g_uiTheme.text);
        hdr::ApplyStyle(g_uiTheme);
    }

    inline void ApplyStyle() { RefreshTheme(); }

    inline void BeginCard(const char* id) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
        ImGui::BeginChild(id, ImVec2(0, 0),
            ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
    }
    inline void EndCard() { ImGui::EndChild(); }

    inline bool Button(const char* label, ImVec2 size = ImVec2(-1, 32)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
        bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        return clicked;
    }

    inline bool ButtonAccent(const char* label, ImVec2 size = ImVec2(-1, 32)) {
        ImGui::PushStyleColor(ImGuiCol_Button, U32(Accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            U32(ImVec4(Accent.x * 1.1f, Accent.y * 1.1f, Accent.z, 1.f)));
        bool clicked = Button(label, size);
        ImGui::PopStyleColor(2);
        return clicked;
    }

    inline bool Toggle(const char* label, bool* v) {
        const float h = 22.f, w = 40.f;
        float rowW = ImGui::GetContentRegionAvail().x;
        ImVec2 p = ImGui::GetCursorScreenPos();

        bool clicked = ImGui::InvisibleButton(label, ImVec2(rowW, h));
        if (clicked) *v = !*v;

        ImGuiID id = ImGui::GetItemID();
        float t = ImGui::GetStateStorage()->GetFloat(id, *v ? 1.f : 0.f);
        t = ImLerp(t, *v ? 1.f : 0.f, ImSaturate(ImGui::GetIO().DeltaTime * 14.f));
        ImGui::GetStateStorage()->SetFloat(id, t);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* end = ImGui::FindRenderedTextEnd(label);
        dl->AddText(ImVec2(p.x, p.y + (h - ImGui::GetFontSize()) * 0.5f), U32(TextMain), label, end);

        ImVec2 a(p.x + rowW - w, p.y), b(p.x + rowW, p.y + h);
        dl->AddRectFilled(a, b, U32(ImLerp(Widget, Accent, t)), h * 0.5f);
        float r = h * 0.5f - 3.f;
        float cx = ImLerp(a.x + h * 0.5f, b.x - h * 0.5f, t);
        dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, IM_COL32_WHITE, 24);
        return clicked;
    }
} // namespace ui

// Ease-out cubic used for short popup/card entrances. Time-based so it works
// even when a popup is opened from a Win32 message outside an ImGui frame.
static float uiEntrance(std::chrono::steady_clock::time_point openedAt, int durationMs = 190) {
    if (openedAt == std::chrono::steady_clock::time_point{}) return 1.0f;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - openedAt).count();
    float t = (float)ms / (float)durationMs;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

// Popups use ImGuiCol_PopupBg by default, which is darker than the manager
// cards. Push the active theme colours instead so they read as the same UI.
struct PopupTheme {
    float eased = 1.0f;

    explicit PopupTheme(float entrance, bool centered, ImVec2 anchor = {}) : eased(entrance) {
        if (centered) {
            ImVec2 c = ImGui::GetMainViewport()->GetCenter();
            ImGui::SetNextWindowPos(ImVec2(c.x, c.y + (1.0f - eased) * 10.0f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        } else {
            ImGui::SetNextWindowPos(ImVec2(anchor.x, anchor.y + (1.0f - eased) * 6.0f),
                                    ImGuiCond_Always);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.02f, eased));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12.0f * (0.65f + 0.35f * eased));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ui::Card);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Card);
        ImGui::PushStyleColor(ImGuiCol_Border, hdr::V4(hdr::Alpha(g_uiTheme.accent, 0.45f)));
        ImGui::PushStyleColor(ImGuiCol_Separator, hdr::V4(hdr::Alpha(g_uiTheme.border, 0.55f)));
    }

    ~PopupTheme() {
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    }
};

static unsigned int rgbPacked(ImU32 color) {
    ImVec4 c = hdr::V4(color);
    unsigned int r = (unsigned int)std::lround(c.x * 255.0f);
    unsigned int g = (unsigned int)std::lround(c.y * 255.0f);
    unsigned int b = (unsigned int)std::lround(c.z * 255.0f);
    return (r << 16) | (g << 8) | b;
}

static ImU32 imColor(unsigned int packedRgb) {
    return IM_COL32((packedRgb >> 16) & 0xFF, (packedRgb >> 8) & 0xFF,
                    packedRgb & 0xFF, 255);
}

static unsigned int packedFromFloat3(const float col[3]) {
    auto byte = [](float v) -> unsigned int {
        return (unsigned int)std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f);
    };
    return (byte(col[0]) << 16) | (byte(col[1]) << 8) | byte(col[2]);
}

static bool colorEditPacked(const char* label, unsigned int& packed) {
    ImVec4 c = hdr::V4(imColor(packed));
    float col[3] = { c.x, c.y, c.z };
    if (!ImGui::ColorEdit3(label, col, ImGuiColorEditFlags_NoInputs)) return false;
    packed = packedFromFloat3(col);
    return true;
}

// Reset the palette to the release default (the user's chosen colours).
static void seedPaletteDefaults(AppSettings& s) {
    hdr::Theme t = builtInTheme();
    s.accentColor = rgbPacked(t.accent);
    s.windowBgColor = rgbPacked(t.windowBg);
    s.titleBarColor = rgbPacked(t.titleBar);
    s.panelColor = rgbPacked(t.panel);
    s.navBarColor = rgbPacked(t.navBar);
    s.buttonColor = rgbPacked(t.button);
    s.borderColor = rgbPacked(t.border);
    s.textColor = rgbPacked(t.text);
    s.textMutedColor = rgbPacked(t.textMuted);
    s.successColor = rgbPacked(t.success);
    s.dangerColor = rgbPacked(t.danger);
}

static std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool isApkPath(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".apk";
}

static std::string formatBytes(size_t bytes) {
    char buf[48];
    if (bytes >= 1024ULL * 1024ULL)
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%d B", (int)bytes);
    return buf;
}

#ifdef _WIN32
static ULONGLONG fileTimeValue(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | (ULONGLONG)ft.dwLowDateTime;
}
#endif

static void loadUiFonts(float sizePx, int family) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    namespace fs = std::filesystem;
    ImFont* regularFont = nullptr;
    g_fontBold = nullptr;
#ifdef _WIN32
    struct FontPair { const wchar_t* regular; const wchar_t* bold; };
    const FontPair fonts[] = {
        { L"C:\\Windows\\Fonts\\calibri.ttf", L"C:\\Windows\\Fonts\\calibrib.ttf" },
        { L"C:\\Windows\\Fonts\\segoeui.ttf", L"C:\\Windows\\Fonts\\segoeuib.ttf" },
    };
    family = (family == 1) ? 1 : 0;
    const int order[2] = { family, 1 - family };

    for (int i : order) {
        const auto& f = fonts[i];
        std::error_code ec;
        if (!fs::exists(f.regular, ec)) continue;
        regularFont = io.Fonts->AddFontFromFileTTF(fs::path(f.regular).string().c_str(), sizePx);
        if (fs::exists(f.bold, ec))
            g_fontBold = io.Fonts->AddFontFromFileTTF(fs::path(f.bold).string().c_str(), sizePx);
        break;
    }
#else
    struct FontPair { const char* regular; const char* bold; };
    const FontPair fonts[] = {
        { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" },
        { "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
          "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf" },
        { "/Library/Fonts/Arial Unicode.ttf", "/Library/Fonts/Arial Bold.ttf" },
    };
    for (const auto& f : fonts) {
        std::error_code ec;
        if (!fs::exists(f.regular, ec)) continue;
        regularFont = io.Fonts->AddFontFromFileTTF(f.regular, sizePx);
        if (fs::exists(f.bold, ec))
            g_fontBold = io.Fonts->AddFontFromFileTTF(f.bold, sizePx);
        break;
    }
#endif
    if (!regularFont) io.Fonts->AddFontDefault();
    io.FontDefault = regularFont ? regularFont : io.Fonts->Fonts[0];
    // This ImGui version uses dynamic font textures. Both backends build and
    // upload the atlas during NewFrame/Render; calling Build() here trips the
    // RendererHasTextures assertion.
}

struct MainWindow::Renderer {
#ifdef _WIN32
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    // HD post-processing: ImGui renders into an offscreen target, then a small
    // HLSL pass sharpens/tunes it on the way to the swap chain. Ultra HD uses
    // a 4x MSAA offscreen target before the same post pass.
    ID3D11Texture2D* sceneTex = nullptr;
    ID3D11RenderTargetView* sceneRtv = nullptr;
    ID3D11ShaderResourceView* sceneSrv = nullptr;
    ID3D11Texture2D* msaaTex = nullptr;
    ID3D11RenderTargetView* msaaRtv = nullptr;
    ID3D11VertexShader* postVS = nullptr;
    ID3D11PixelShader* postPS = nullptr;
    ID3D11Buffer* postConstants = nullptr;
    ID3D11SamplerState* postSampler = nullptr;
#else
    SDL_GLContext glContext = nullptr;
    // Alias used by the shared releaseTexture(context) calls.
    void* context = nullptr;
#endif
    UINT sceneW = 0;
    UINT sceneH = 0;
#ifdef _WIN32
    UINT sampleCount = 0;
#endif
};

struct MainWindow::PhoneTexture {
#ifdef _WIN32
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
#else
    GLuint texture = 0;
#endif
    // ImGui 1.93 uses a 64-bit ImTextureID; keep the backend handle in a
    // single shared field so the drawing code needs no platform branch.
    ImTextureID view = 0;
    int w = 0, h = 0;
};

#ifdef _WIN32
struct PostConstants {
    float sharpness;
    float contrast;
    float saturation;
    float vignette;
    float glow;
    float texelX;
    float texelY;
    float reserved;
};

template <class T> static void releaseCom(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
#else
// sdl_compat's drop shim hands us UTF-8 decoded into wchar_t code points.
// A small round-trip is enough for file paths on POSIX.
static std::wstring toWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back((wchar_t)c);
    return w;
}

static std::string toUtf8(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (size_t i = 0; i < w.size(); ++i) {
        unsigned int c = (unsigned int)w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + ((unsigned int)w[++i] - 0xDC00);
        }
        if (c < 0x80) {
            s.push_back((char)c);
        } else if (c < 0x800) {
            s.push_back((char)(0xC0 | (c >> 6)));
            s.push_back((char)(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            if (c >= 0xD800 && c <= 0xDFFF) {
                s.push_back('?'); // lone surrogate
                continue;
            }
            s.push_back((char)(0xE0 | (c >> 12)));
            s.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (c >> 18)));
            s.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return s;
}
#endif

// Keys that produce printable text. These are delivered by WM_CHAR only;
// sending them as Android key events as well doubles characters and uses the
// wrong Windows->Android keycode (e.g. 'f' became '=' and '3' became 'c').
static bool isTextKey(int vk) {
    if (vk == VK_SPACE) return true;
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk >= VK_OEM_1 && vk <= VK_OEM_3) return true;
    if (vk >= VK_OEM_4 && vk <= VK_OEM_7) return true;
    // On POSIX sdl_compat.h exposes VK_OEM_7 as 0xDD (']'), while apostrophe
    // is the undocumented 0xDE. It still arrives as WM_CHAR text input.
    if (vk == 0xDE) return true;
    if (vk == VK_OEM_102) return true;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return true;
    if (vk == VK_MULTIPLY || vk == VK_ADD || vk == VK_SEPARATOR ||
        vk == VK_SUBTRACT || vk == VK_DECIMAL || vk == VK_DIVIDE) return true;
    return false;
}

static void stopVm(IVirtualMachine* vm) {
    if (!vm) return;
    if (auto* android = dynamic_cast<AndroidVirtualMachine*>(vm)) android->terminate();
    else vm->stop();
}

// ---------------------------------------------------------------------------
// Snapshot previews (small stills, not a live mirror)
// ---------------------------------------------------------------------------

static bool loadPpmRgba(const std::string& path, PhonePreview& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string magic;
    int maxv = 0;
    f >> magic >> out.w >> out.h >> maxv;
    if (!f || magic != "P6" || out.w <= 0 || out.h <= 0 || maxv != 255) return false;
    f.get();
    std::vector<uint8_t> rgb((size_t)out.w * out.h * 3);
    f.read((char*)rgb.data(), (std::streamsize)rgb.size());
    if (f.gcount() != (std::streamsize)rgb.size()) return false;
    out.rgba.assign((size_t)out.w * out.h * 4, 255);
    for (size_t i = 0; i < (size_t)out.w * out.h; ++i) {
        out.rgba[i * 4 + 0] = rgb[i * 3 + 0];
        out.rgba[i * 4 + 1] = rgb[i * 3 + 1];
        out.rgba[i * 4 + 2] = rgb[i * 3 + 2];
    }
    return out.valid();
}

static void makePhonePreview(const uint8_t* src, int sw, int sh, PhonePreview& out) {
    if (!src || sw <= 0 || sh <= 0) return;
    int tw = std::min(160, sw);
    int th = (int)std::lround((double)tw * (double)sh / (double)sw);
    if (th < 1) th = 1;
    out.w = tw;
    out.h = th;
    out.rgba.assign((size_t)tw * th * 4, 255);
    for (int y = 0; y < th; ++y) {
        int y0 = (int)((long long)y * sh / th);
        int y1 = std::max(y0 + 1, (int)((long long)(y + 1) * sh / th));
        for (int x = 0; x < tw; ++x) {
            int x0 = (int)((long long)x * sw / tw);
            int x1 = std::max(x0 + 1, (int)((long long)(x + 1) * sw / tw));
            long long r = 0, g = 0, b = 0;
            int n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* px = src + ((size_t)sy * sw + sx) * 4;
                    r += px[0]; g += px[1]; b += px[2];
                    ++n;
                }
            }
            uint8_t* dst = out.rgba.data() + ((size_t)y * tw + x) * 4;
            dst[0] = (uint8_t)(r / n);
            dst[1] = (uint8_t)(g / n);
            dst[2] = (uint8_t)(b / n);
            dst[3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------

MainWindow::MainWindow() {
    createdAt_ = std::chrono::steady_clock::now();
    lastFpsSample_ = createdAt_;
    lastInputAt_ = createdAt_;
    lastAfkAt_ = createdAt_;
    lastAwakeAt_ = createdAt_;
    statePath_ = AppSetup::profilesPath();
}

MainWindow::~MainWindow() {
    cancelAllApkJobs();
#ifndef _WIN32
    // Capture these before shutdownRenderer() clears renderer_->glContext.
    SDL_Window* sdlWindow = (SDL_Window*)hwnd_;
    SDL_GLContext glContext = renderer_ ? renderer_->glContext : nullptr;
    if (!glContext && sdlWindow) glContext = SDL_GL_GetCurrentContext();
#endif
    shutdownRenderer();
#ifndef _WIN32
    if (glContext) SDL_GL_DeleteContext(glContext);
    if (sdlWindow) SDL_DestroyWindow(sdlWindow);
    emu::sdlcompat::setWindow(nullptr);
    emu::sdlcompat::setMainWindow(nullptr);
    SDL_Quit();
#endif
}

bool MainWindow::initRenderer() {
    renderer_ = std::make_unique<Renderer>();

    ImGui::CreateContext();
    g_baseImGuiStyle = ImGui::GetStyle();
    g_baseImGuiStyleCaptured = true;
    ImGui_ImplWin32_EnableDpiAwareness();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // Real UI font sized from the saved appearance settings rather than ImGui's
    // tiny bitmap default (Calibri/Segoe on Windows, DejaVu elsewhere).
    loadUiFonts((float)state_.settings.fontSize, state_.settings.fontFamily);
    applyThemeFromSettings();

#ifdef _WIN32
    if (!ImGui_ImplWin32_Init(hwnd_)) {
        LOG_ERROR("ImGui Win32 backend failed");
        shutdownRenderer();
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = (UINT)std::max(1, (int)rc.right);
    UINT h = (UINT)std::max(1, (int)rc.bottom);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &level, 1, D3D11_SDK_VERSION, &sd, &renderer_->swapChain,
        &renderer_->device, nullptr, &renderer_->context);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11 init failed hr=0x%08X", (unsigned int)hr);
        shutdownRenderer();
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    if (FAILED(renderer_->swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        LOG_ERROR("SwapChain GetBuffer failed");
        shutdownRenderer();
        return false;
    }
    hr = renderer_->device->CreateRenderTargetView(back, nullptr, &renderer_->rtv);
    back->Release();
    if (FAILED(hr)) {
        LOG_ERROR("CreateRenderTargetView failed hr=0x%08X", (unsigned int)hr);
        shutdownRenderer();
        return false;
    }

    if (!ImGui_ImplDX11_Init(renderer_->device, renderer_->context)) {
        LOG_ERROR("ImGui DX11 backend failed");
        shutdownRenderer();
        return false;
    }
    if (!createPostShaders())
        LOG_ERROR("HD shaders unavailable; falling back to standard rendering");
#else
    if (!ImGui_ImplWin32_Init(hwnd_)) {
        LOG_ERROR("ImGui SDL2 backend failed");
        shutdownRenderer();
        return false;
    }
    renderer_->glContext = SDL_GL_GetCurrentContext();
    if (!renderer_->glContext) {
        LOG_ERROR("SDL OpenGL context is missing");
        shutdownRenderer();
        return false;
    }
    renderer_->context = renderer_->glContext;
    if (!ImGui_ImplDX11_Init(nullptr, nullptr)) {
        LOG_ERROR("ImGui OpenGL3 backend failed");
        shutdownRenderer();
        return false;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    createPostShaders(); // SDL path logs once that HD post effects are unavailable
#endif
    return true;
}

bool MainWindow::createPostShaders() {
#ifndef _WIN32
    // HD post-processing is a DX11-only feature. Keep the setting plumbed
    // through the shared UI but do not build a second GL effect pipeline.
    static bool logged = false;
    if (!logged) {
        LOG_INFO("HD post-processing is unavailable with the SDL/OpenGL backend");
        logged = true;
    }
    return false;
#else
    if (!renderer_ || !renderer_->device) return false;

    static const char kPostVs[] = R"hd(
struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUT main(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    VS_OUT o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = p;
    return o;
}
)hd";

    static const char kPostPs[] = R"hd(
Texture2D Scene : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer PostParams : register(b0) {
    float4 P0; // sharpness, contrast, saturation, vignette
    float4 P1; // glow, texel x, texel y, reserved
};

struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 bright(float3 c) {
    return max(c - float3(0.72, 0.72, 0.72), float3(0, 0, 0));
}

float4 main(VS_OUT input) : SV_TARGET {
    float2 uv = input.uv;
    float2 t = P1.yz;
    float3 c = Scene.Sample(LinearSampler, uv).rgb;

    if (P0.x > 0.001) {
        float3 n = Scene.Sample(LinearSampler, uv + float2(0.0, -t.y)).rgb;
        float3 s = Scene.Sample(LinearSampler, uv + float2(0.0,  t.y)).rgb;
        float3 l = Scene.Sample(LinearSampler, uv + float2(-t.x, 0.0)).rgb;
        float3 r = Scene.Sample(LinearSampler, uv + float2( t.x, 0.0)).rgb;
        float3 sharpened = c * (1.0 + 4.0 * P0.x) - (n + s + l + r) * P0.x;
        c = lerp(c, max(sharpened, float3(0, 0, 0)), 0.85);
    }

    if (P1.x > 0.001) {
        float2 g = t * 2.5;
        float3 b = Scene.Sample(LinearSampler, uv + float2(0.0, -g.y)).rgb;
        b += Scene.Sample(LinearSampler, uv + float2(0.0,  g.y)).rgb;
        b += Scene.Sample(LinearSampler, uv + float2(-g.x, 0.0)).rgb;
        b += Scene.Sample(LinearSampler, uv + float2( g.x, 0.0)).rgb;
        c += bright(b * 0.25) * P1.x * 0.55;
    }

    c = saturate((c - 0.5) * (1.0 + P0.y * 0.55) + 0.5);
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(lum, lum, lum), c, 1.0 + P0.z * 0.85);

    float2 d = uv * 2.0 - 1.0;
    float vig = 1.0 - saturate(dot(d, d)) * P0.w * 0.28;
    return float4(saturate(c * vig), 1.0);
}
)hd";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(kPostVs, strlen(kPostVs), nullptr, nullptr, nullptr,
        "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            LOG_ERROR("HD vertex shader: %s", (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        releaseCom(vsBlob);
        return false;
    }
    releaseCom(errors);

    hr = D3DCompile(kPostPs, strlen(kPostPs), nullptr, nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            LOG_ERROR("HD pixel shader: %s", (const char*)errors->GetBufferPointer());
            errors->Release();
        }
        releaseCom(vsBlob);
        releaseCom(psBlob);
        return false;
    }
    releaseCom(errors);

    hr = renderer_->device->CreateVertexShader(vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(), nullptr, &renderer_->postVS);
    if (SUCCEEDED(hr)) {
        hr = renderer_->device->CreatePixelShader(psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(), nullptr, &renderer_->postPS);
    }
    releaseCom(vsBlob);
    releaseCom(psBlob);
    if (FAILED(hr)) {
        LOG_ERROR("HD shader create failed hr=0x%08X", (unsigned int)hr);
        return false;
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(PostConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(renderer_->device->CreateBuffer(&cb, nullptr, &renderer_->postConstants))) {
        LOG_ERROR("HD constant buffer create failed");
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(renderer_->device->CreateSamplerState(&sampler, &renderer_->postSampler))) {
        LOG_ERROR("HD sampler create failed");
        return false;
    }
    return true;
#endif
}

void MainWindow::releasePostTargets() {
    if (!renderer_) return;
#ifdef _WIN32
    releaseCom(renderer_->sceneRtv);
    releaseCom(renderer_->sceneSrv);
    releaseCom(renderer_->sceneTex);
    releaseCom(renderer_->msaaRtv);
    releaseCom(renderer_->msaaTex);
    renderer_->sampleCount = 0;
#endif
    renderer_->sceneW = renderer_->sceneH = 0;
}

bool MainWindow::ensurePostTargets(UINT width, UINT height) {
#ifndef _WIN32
    (void)width; (void)height;
    return false;
#else
    if (!renderer_ || !renderer_->device || width == 0 || height == 0) return false;
    if (state_.settings.shaderMode <= 0) {
        releasePostTargets();
        return false;
    }

    UINT samples = state_.settings.shaderMode == 2 ? 4 : 1;
    if (samples > 1) {
        UINT levels = 0;
        HRESULT hr = renderer_->device->CheckMultisampleQualityLevels(
            DXGI_FORMAT_R8G8B8A8_UNORM, samples, &levels);
        if (FAILED(hr) || levels == 0) samples = 1;
    }

    if (renderer_->sceneTex && renderer_->sceneRtv && renderer_->sceneSrv &&
        renderer_->sceneW == width && renderer_->sceneH == height &&
        renderer_->sampleCount == samples) {
        updatePostConstants(width, height);
        return true;
    }

    releasePostTargets();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(renderer_->device->CreateTexture2D(&td, nullptr, &renderer_->sceneTex))) {
        LOG_ERROR("HD scene texture create failed");
        releasePostTargets();
        return false;
    }
    if (FAILED(renderer_->device->CreateRenderTargetView(renderer_->sceneTex, nullptr,
            &renderer_->sceneRtv)) ||
        FAILED(renderer_->device->CreateShaderResourceView(renderer_->sceneTex, nullptr,
            &renderer_->sceneSrv))) {
        LOG_ERROR("HD scene view create failed");
        releasePostTargets();
        return false;
    }

    if (samples > 1) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        td.SampleDesc.Count = samples;
        if (SUCCEEDED(renderer_->device->CreateTexture2D(&td, nullptr, &renderer_->msaaTex))) {
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = td.Format;
            rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
            if (FAILED(renderer_->device->CreateRenderTargetView(renderer_->msaaTex,
                    &rtvDesc, &renderer_->msaaRtv))) {
                releaseCom(renderer_->msaaTex);
            }
        }
    }

    renderer_->sceneW = width;
    renderer_->sceneH = height;
    renderer_->sampleCount = samples;
    updatePostConstants(width, height);
    return true;
#endif
}

void MainWindow::updatePostConstants(UINT width, UINT height) {
#ifndef _WIN32
    (void)width; (void)height;
    return;
#else
    if (!renderer_ || !renderer_->context || !renderer_->postConstants) return;
    PostConstants pc{};
    pc.sharpness = (float)state_.settings.sharpness / 100.0f * 0.55f;
    pc.contrast = (float)state_.settings.contrast / 100.0f;
    pc.saturation = (float)state_.settings.saturation / 100.0f;
    pc.vignette = (float)state_.settings.vignette / 100.0f;
    pc.glow = (float)state_.settings.glow / 100.0f;
    pc.texelX = width ? 1.0f / (float)width : 0.0f;
    pc.texelY = height ? 1.0f / (float)height : 0.0f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(renderer_->context->Map(renderer_->postConstants, 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &pc, sizeof(pc));
    renderer_->context->Unmap(renderer_->postConstants, 0);
#endif
}

void MainWindow::renderPostPass() {
#ifndef _WIN32
    return;
#else
    auto* r = renderer_.get();
    if (!r || !r->context || !r->rtv || !r->sceneSrv || !r->postVS ||
        !r->postPS || !r->postConstants || !r->postSampler) return;

    ID3D11DeviceContext* ctx = r->context;
    if (r->msaaTex && r->msaaRtv) {
        ctx->ResolveSubresource(r->sceneTex, 0, r->msaaTex, 0,
            DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    ID3D11RenderTargetView* target = r->rtv;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)r->sceneW;
    vp.Height = (float)r->sceneH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    D3D11_RECT scissor{ 0, 0, (LONG)r->sceneW, (LONG)r->sceneH };
    ctx->RSSetScissorRects(1, &scissor);

    ID3D11BlendState* noBlend = nullptr;
    ID3D11Buffer* noVertexBuffer = nullptr;
    UINT stride = 0, offset = 0;
    ctx->OMSetBlendState(noBlend, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetVertexBuffers(0, 1, &noVertexBuffer, &stride, &offset);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(r->postVS, nullptr, 0);
    ctx->PSSetShader(r->postPS, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &r->postConstants);
    ctx->PSSetSamplers(0, 1, &r->postSampler);
    ctx->PSSetShaderResources(0, 1, &r->sceneSrv);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* noSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &noSrv);
#endif
}

#ifdef _WIN32
void MainWindow::releaseTexture(PhoneTexture* tex, ID3D11DeviceContext* ctx) {
    if (!tex) return;
    (void)ctx;
    if (tex->srv) { tex->srv->Release(); tex->srv = nullptr; }
    if (tex->texture) { tex->texture->Release(); tex->texture = nullptr; }
    tex->view = 0;
    tex->w = tex->h = 0;
}
#else
void MainWindow::releaseTexture(PhoneTexture* tex, void* ctx) {
    if (!tex) return;
    (void)ctx;
    if (tex->texture) {
        glDeleteTextures(1, &tex->texture);
        tex->texture = 0;
    }
    tex->view = 0;
    tex->w = tex->h = 0;
}
#endif

void MainWindow::shutdownRenderer() {
    for (auto& kv : textures_) releaseTexture(kv.second.get(), renderer_ ? renderer_->context : nullptr);
    textures_.clear();
    if (!renderer_) return;

    releasePostTargets();
#ifdef _WIN32
    releaseCom(renderer_->postVS);
    releaseCom(renderer_->postPS);
    releaseCom(renderer_->postConstants);
    releaseCom(renderer_->postSampler);
#endif

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_fontBold = nullptr;

#ifdef _WIN32
    if (renderer_->rtv) { renderer_->rtv->Release(); renderer_->rtv = nullptr; }
    if (renderer_->swapChain) { renderer_->swapChain->Release(); renderer_->swapChain = nullptr; }
    if (renderer_->context) { renderer_->context->Release(); renderer_->context = nullptr; }
    if (renderer_->device) { renderer_->device->Release(); renderer_->device = nullptr; }
#else
    // The SDL window/GL context outlives the ImGui context; it is released when
    // the SDL event loop exits so WM_DESTROY-style teardown stays order-safe.
    renderer_->glContext = nullptr;
    renderer_->context = nullptr;
#endif
    renderer_.reset();
}

void MainWindow::resizeRenderer() {
#ifdef _WIN32
    if (!renderer_ || !renderer_->swapChain || !renderer_->device || !renderer_->context) return;
#else
    if (!renderer_) return;
#endif
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = (UINT)std::max(1, (int)rc.right);
    UINT h = (UINT)std::max(1, (int)rc.bottom);
#ifdef _WIN32
    if (renderer_->rtv) { renderer_->rtv->Release(); renderer_->rtv = nullptr; }
    if (FAILED(renderer_->swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(renderer_->swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    renderer_->device->CreateRenderTargetView(back, nullptr, &renderer_->rtv);
    back->Release();
#else
    // SDL keeps the GL drawable in sync with the window; only the cached size
    // used for post targets needs refreshing.
    renderer_->sceneW = w;
    renderer_->sceneH = h;
#endif
}

bool MainWindow::create(HINSTANCE hInst, int nCmdShow) {
    hInst_ = hInst;
#ifndef _WIN32
    // SDL's quit state and custom-event queue live in the compat layer, not in
    // this instance. Clear both so a recreated manager starts from a clean slate.
    emu::sdlcompat::setQuitRequested(false);
    MSG pending{};
    while (emu::sdlcompat::nextCustomMessage(pending)) {}
#endif
    createdAt_ = std::chrono::steady_clock::now();
    lastFpsSample_ = createdAt_;
    lastInputAt_ = createdAt_;
    lastAfkAt_ = createdAt_;
    lastAwakeAt_ = createdAt_;

    state_ = loadManagerState(statePath_);
    loadGlobalSettingsFromConfig();
    snprintf(searchBuffer_, sizeof(searchBuffer_), "%s", state_.settings.searchText.c_str());
    lastResourceSample_ = createdAt_;
    loadPreviews();

    std::string title = ConfigManager::instance().config().instanceName + " Manager";
    winW_ = 1280;
    winH_ = 800;
#ifdef _WIN32
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("RegisterClassEx failed");
        return false;
    }

    std::wstring wtitle(title.begin(), title.end());
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, CLASS_NAME, wtitle.c_str(), kWindowStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, winW_, winH_, nullptr, nullptr, hInst, this);
    if (!hwnd_) { LOG_ERROR("CreateWindowEx failed"); return false; }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    DragAcceptFiles(hwnd_, TRUE);
    // Recalculate the non-client area now that WndProc can answer WM_NCCALCSIZE.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    applyWindowCorners();
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __APPLE__
    // macOS only exposes modern GL through a core profile. ImGui selects
    // #version 150 there, so request a context that supports it.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#else
    // A 3.0 compatibility context is universally available on desktop Linux
    // and accepts the backend's default #version 130 shaders.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
    hwnd_ = (HWND)SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, winW_, winH_,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL |
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!hwnd_) {
        LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BlueDroid",
            "Could not create the BlueDroid window.", nullptr);
        SDL_Quit();
        return false;
    }
    SDL_StartTextInput();
    SDL_GLContext gl = SDL_GL_CreateContext((SDL_Window*)hwnd_);
    if (!gl) {
        LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BlueDroid",
            "OpenGL is required to run BlueDroid.", (SDL_Window*)hwnd_);
        SDL_DestroyWindow((SDL_Window*)hwnd_);
        hwnd_ = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent((SDL_Window*)hwnd_, gl);
    SDL_GL_SetSwapInterval(1); // match DX11's Present(1, 0)
    emu::sdlcompat::setWindow((SDL_Window*)hwnd_);
    emu::sdlcompat::setMainWindow(this);

    // The custom ImGui cursor replaces the native one.
    static const Uint8 emptyBits[1] = { 0 };
    static const Uint8 emptyMask[1] = { 0 };
    static SDL_Cursor* emptyCursor = nullptr;
    if (!emptyCursor) emptyCursor = SDL_CreateCursor(emptyBits, emptyMask, 1, 1, 0, 0);
    if (emptyCursor) SDL_SetCursor(emptyCursor);

    applyWindowCorners();
#endif

    if (!initRenderer()) {
#ifdef _WIN32
        MessageBoxW(nullptr, L"DirectX 11 is required to run BlueDroid.", L"BlueDroid", MB_OK | MB_ICONERROR);
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BlueDroid",
            "OpenGL is required to run BlueDroid.", (SDL_Window*)hwnd_);
#endif
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    applyThemeFromSettings();

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    // Apply after the window is visible; DWM behaves differently if the corner
    // preference is set before the first composition pass.
    applyWindowCorners();

    // No emulator starts automatically. If the window was only closed
    // temporarily, phones from that session are still running - reattach to
    // them here so they keep their apps/state.
    reattachRunningPhones();
    applyParkedAudio();

    NetworkStack::instance().init();
    AudioEngine::instance().init();
    LOG_INFO("Manager opened (%d phones)", (int)state_.phones.size());
    return true;
}

void MainWindow::setVm(IVirtualMachine* vm) {
    vm_ = vm;
    input_ = vm ? std::make_unique<InputManager>(vm) : nullptr;
}

void MainWindow::debugToggleAntiAfk() {
    state_.settings.antiAfk = !state_.settings.antiAfk;
    persist();
}

void MainWindow::debugToggleKeepAwake() {
    state_.settings.keepAwake = !state_.settings.keepAwake;
    if (state_.settings.keepAwake) lastAwakeAt_ = createdAt_;
    persist();
}

void MainWindow::debugOpenSettings() { openSettings(); }
void MainWindow::debugCloseSettings() { closeSettings(); }
void MainWindow::debugOpenFirstPhone() {
    auto ids = orderedProfileIds();
    if (!ids.empty()) openEmulator(ids.front());
}
void MainWindow::debugLeaveEmulator() { leaveEmulator(); }
void MainWindow::debugStopAll() { closeAllPhones(); }
void MainWindow::debugExitAll() { exitEverything(); }
void MainWindow::debugToggleListView() {
    state_.settings.listView = !state_.settings.listView;
    persist();
}
void MainWindow::debugCycleFilter() {
    state_.settings.statusFilter = (state_.settings.statusFilter + 1) % 5;
    persist();
}
void MainWindow::debugOpenSettingsTab(int tab) {
    state_.settings.settingsTab = std::min(2, std::max(0, tab));
    persist();
    openSettings();
}
void MainWindow::debugToggleFontSize() {
    state_.settings.fontSize = state_.settings.fontSize >= 22 ? 12 : state_.settings.fontSize + 1;
    fontRebuildPending_ = true;
}
void MainWindow::debugOpenApkPicker(const std::string& path) { openApkPicker(path); }
void MainWindow::debugInstallTestApk(const std::string& path) {
    openApkPicker(path);
    auto ids = orderedProfileIds();
    if (ids.empty()) return;
    apkAllPhones_ = false;
    apkSelectedIds_ = { ids.front() };
    apkPickerOpen_ = false;
    startApkInstall();
}

void MainWindow::runMessageLoop() {
#ifdef _WIN32
    MSG msg{};
    for (;;) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        renderFrame();
    }
#else
    auto makePoint = [](int x, int y) -> LPARAM {
        return (LPARAM)(((DWORD)(x)&0xFFFF) | (((DWORD)(y)&0xFFFF) << 16));
    };
    bool quit = false;
    bool resizingWindow = false;
    LRESULT resizeHit = HTCLIENT;
    POINT resizeCursorOrigin{};
    RECT resizeWindowOrigin{};
    SDL_Event event{};
    MSG msg{};
    for (;;) {
        while (SDL_PollEvent(&event)) {
            emu::sdlcompat::processEvent(event);
            switch (event.type) {
                case SDL_QUIT:
                    // A quit already in flight came from exitEverything();
                    // don't run WM_CLOSE a second time for the same shutdown.
                    if (emu::sdlcompat::quitRequested()) {
                        quit = true;
                        break;
                    }
                    // Route the window-manager close through the same WM_CLOSE
                    // flow as Windows so "minimize to tray" is still honored.
                    handle(hwnd_, WM_CLOSE, 0, 0);
                    if (emu::sdlcompat::quitRequested()) quit = true;
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        handle(hwnd_, WM_SIZE, (WPARAM)event.window.data1,
                               makePoint((int)event.window.data1, (int)event.window.data2));
                    } else if (event.window.event == SDL_WINDOWEVENT_CLOSE &&
                               !emu::sdlcompat::quitRequested()) {
                        handle(hwnd_, WM_CLOSE, 0, 0);
                    }
                    break;
                case SDL_DROPFILE:
                    // DragFinish() in handle() frees event.drop.file via SDL_free.
                    handle(hwnd_, WM_DROPFILES, (WPARAM)event.drop.file, 0);
                    break;
                case SDL_KEYDOWN:
                    handle(hwnd_, WM_KEYDOWN, (WPARAM)emu::sdlcompat::keyToVk(event.key.keysym.sym), 0);
                    break;
                case SDL_KEYUP:
                    handle(hwnd_, WM_KEYUP, (WPARAM)emu::sdlcompat::keyToVk(event.key.keysym.sym), 0);
                    break;
                case SDL_TEXTINPUT: {
                    for (const unsigned char* p = (const unsigned char*)event.text.text; *p; ++p)
                        handle(hwnd_, WM_CHAR, (WPARAM)*p, 0);
                    break;
                }
                case SDL_MOUSEMOTION:
                    if (resizingWindow) {
                        // SDL has no DefWindowProc to act on WM_NCHITTEST, so
                        // apply the shared hit result ourselves.
                        POINT pt{};
                        GetCursorPos(&pt);
                        const int dx = pt.x - resizeCursorOrigin.x;
                        const int dy = pt.y - resizeCursorOrigin.y;
                        int x = resizeWindowOrigin.left;
                        int y = resizeWindowOrigin.top;
                        int w = resizeWindowOrigin.right - resizeWindowOrigin.left;
                        int h = resizeWindowOrigin.bottom - resizeWindowOrigin.top;
                        constexpr int minW = 480, minH = 360;
                        switch (resizeHit) {
                            case HTLEFT:       x += dx; w -= dx; break;
                            case HTRIGHT:      w += dx; break;
                            case HTTOP:        y += dy; h -= dy; break;
                            case HTBOTTOM:     h += dy; break;
                            case HTTOPLEFT:    x += dx; y += dy; w -= dx; h -= dy; break;
                            case HTTOPRIGHT:   y += dy; w += dx; h -= dy; break;
                            case HTBOTTOMLEFT: x += dx; w -= dx; h += dy; break;
                            case HTBOTTOMRIGHT:w += dx; h += dy; break;
                            default: break;
                        }
                        if (w < minW) { if (resizeHit == HTLEFT || resizeHit == HTTOPLEFT ||
                                            resizeHit == HTBOTTOMLEFT) x -= minW - w;
                                        w = minW; }
                        if (h < minH) { if (resizeHit == HTTOP || resizeHit == HTTOPLEFT ||
                                            resizeHit == HTTOPRIGHT) y -= minH - h;
                                        h = minH; }
                        SDL_SetWindowPosition((SDL_Window*)hwnd_, x, y);
                        SDL_SetWindowSize((SDL_Window*)hwnd_, w, h);
                    } else {
                        handle(hwnd_, WM_MOUSEMOVE, 1, makePoint(event.motion.x, event.motion.y));
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        // WM_NCHITTEST expects screen coordinates. SDL only
                        // gives us client coords, so ask SDL for the global one.
                        POINT cursor{};
                        GetCursorPos(&cursor);
                        const LRESULT hit = handle(hwnd_, WM_NCHITTEST, 0,
                            makePoint(cursor.x, cursor.y));
                        if (hit == HTCAPTION) {
                            RECT window{};
                            if (GetWindowRect(hwnd_, &window)) {
                                dragCursorOrigin_ = cursor;
                                dragWindowOrigin_ = { window.left, window.top };
                                draggingWindow_ = true;
                                SetCapture(hwnd_);
                                break;
                            }
                        } else if (hit != HTCLIENT) {
                            RECT window{};
                            if (GetWindowRect(hwnd_, &window)) {
                                resizeCursorOrigin = cursor;
                                resizeWindowOrigin = window;
                                resizeHit = hit;
                                resizingWindow = true;
                                SetCapture(hwnd_);
                                break;
                            }
                        }
                        handle(hwnd_, WM_LBUTTONDOWN, 1,
                               makePoint(event.button.x, event.button.y));
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        if (resizingWindow) {
                            resizingWindow = false;
                            ReleaseCapture();
                            break;
                        }
                        if (draggingWindow_) {
                            draggingWindow_ = false;
                            ReleaseCapture();
                        }
                        handle(hwnd_, WM_LBUTTONUP, 1,
                               makePoint(event.button.x, event.button.y));
                    }
                    break;
                default:
                    break;
            }
        }
        while (emu::sdlcompat::nextCustomMessage(msg)) {
            handle(msg.hwnd ? msg.hwnd : hwnd_, msg.message, msg.wParam, msg.lParam);
        }
        if (quit || emu::sdlcompat::quitRequested()) break;
        renderFrame();
    }
#endif
}

#ifdef _WIN32
LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self && self->renderer_) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    }
    if (self) return self->handle(hwnd, msg, wParam, lParam);
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#else
LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = (MainWindow*)emu::sdlcompat::mainWindow();
    if (self) return self->handle(hwnd, msg, wParam, lParam);
    return 0;
}
#endif

PhoneProfile* MainWindow::findProfile(int id) {
    for (auto& p : state_.phones) if (p.id == id) return &p;
    return nullptr;
}

std::vector<int> MainWindow::orderedProfileIds() const {
    std::vector<int> ids;
    ids.reserve(state_.phones.size());
    for (const auto& p : state_.phones) ids.push_back(p.id);
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) {
        const PhoneProfile* pa = nullptr;
        const PhoneProfile* pb = nullptr;
        for (const auto& p : state_.phones) {
            if (p.id == a) pa = &p;
            if (p.id == b) pb = &p;
        }
        if (!pa || !pb) return a < b;
        if (pa->starred != pb->starred) return pa->starred > pb->starred;
        return pa->id < pb->id;
    });
    return ids;
}

MainWindow::PhoneStatus MainWindow::phoneStatus(int id) const {
    const PhoneProfile* p = nullptr;
    for (const auto& q : state_.phones) {
        if (q.id == id) { p = &q; break; }
    }
    if (p && p->paused) return PhoneStatus::Paused;

    auto it = vms_.find(id);
    if (it == vms_.end()) return PhoneStatus::Stopped;
    switch (it->second->state()) {
        case VmState::Starting: return PhoneStatus::Starting;
        case VmState::Running:
            return (mode_ == UiMode::Emulator && currentId_ == id)
                ? PhoneStatus::Running : PhoneStatus::Parked;
        case VmState::Paused: return PhoneStatus::Paused;
        case VmState::Error: return PhoneStatus::Error;
        case VmState::Stopping: return PhoneStatus::Starting;
        case VmState::Stopped: return PhoneStatus::Stopped;
    }
    return PhoneStatus::Stopped;
}

bool MainWindow::phoneMatchesFilters(int id) const {
    const PhoneProfile* p = nullptr;
    for (const auto& q : state_.phones) {
        if (q.id == id) { p = &q; break; }
    }
    if (!p) return false;

    if (!state_.settings.searchText.empty()) {
        std::string needle = state_.settings.searchText;
        std::transform(needle.begin(), needle.end(), needle.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        std::string hay = p->name + " " + p->avd + " " + p->templateName + " " +
                          std::to_string(p->port);
        std::transform(hay.begin(), hay.end(), hay.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        if (hay.find(needle) == std::string::npos) return false;
    }

    switch (state_.settings.statusFilter) {
        case 1: {
            auto s = phoneStatus(id);
            return s == PhoneStatus::Running || s == PhoneStatus::Parked ||
                   s == PhoneStatus::Starting;
        }
        case 2: return phoneStatus(id) == PhoneStatus::Stopped;
        case 3: return phoneStatus(id) == PhoneStatus::Paused;
        case 4: return p->starred;
        default: return true;
    }
}

void MainWindow::sampleResources() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastResourceSample_);
    if (elapsed < std::chrono::milliseconds(700)) return;
    const double elapsed100ns = (double)elapsed.count() * 10000.0;
    lastResourceSample_ = now;
    if (elapsed100ns <= 0) return;

    unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
    for (auto& kv : vms_) {
        IVirtualMachine* vm = kv.second.get();
        ResourceSample& sample = resourceSamples_[kv.first];
        sample.takenAt = now;
        if (!vm || (vm->state() != VmState::Running && vm->state() != VmState::Paused)) {
            sample.cpuPercent = 0.0;
            sample.ramBytes = 0;
            sample.cpuTime = 0;
            continue;
        }

        uint64_t total = 0;
        size_t rssBytes = 0;
        bool haveStats = false;
        if (auto* android = dynamic_cast<AndroidVirtualMachine*>(vm)) {
            haveStats = emu::platform::processStats(android->processId(), &total, &rssBytes);
        }
        if (!haveStats) {
            sample.cpuPercent = 0.0;
            sample.ramBytes = 0;
            sample.cpuTime = 0;
            continue;
        }

        if (sample.cpuTime && total >= sample.cpuTime) {
            double pct = (double)(total - sample.cpuTime) * 100.0 / elapsed100ns / (double)cores;
            sample.cpuPercent = std::min(100.0, std::max(0.0, pct));
        } else {
            sample.cpuPercent = 0.0;
        }
        sample.cpuTime = total;
        sample.ramBytes = rssBytes;
    }
}

void MainWindow::applyThemeFromSettings() {
    state_.settings.uiScalePercent = std::min(150, std::max(100, state_.settings.uiScalePercent));
    g_uiTheme = builtInTheme();
    g_uiTheme.cornerScale = (float)state_.settings.cornerScale10 / 10.0f;
    if (state_.settings.accentColor != 0)
        g_uiTheme.accent = imColor(state_.settings.accentColor);
    if (state_.settings.useCustomColors) {
        g_uiTheme.windowBg = imColor(state_.settings.windowBgColor);
        g_uiTheme.titleBar = imColor(state_.settings.titleBarColor);
        g_uiTheme.panel = imColor(state_.settings.panelColor);
        g_uiTheme.navBar = imColor(state_.settings.navBarColor);
        g_uiTheme.button = imColor(state_.settings.buttonColor);
        g_uiTheme.border = imColor(state_.settings.borderColor);
        g_uiTheme.text = imColor(state_.settings.textColor);
        g_uiTheme.textMuted = imColor(state_.settings.textMutedColor);
        g_uiTheme.success = imColor(state_.settings.successColor);
        g_uiTheme.danger = imColor(state_.settings.dangerColor);
    }

    // ScaleAllSizes() multiplies every ImGui size. Applying it twice would turn
    // 150% into 225%, so always restore the captured style before scaling again.
    // ui::RefreshTheme() then re-applies the selected theme colours.
    if (!g_baseImGuiStyleCaptured) {
        g_baseImGuiStyle = ImGui::GetStyle();
        g_baseImGuiStyleCaptured = true;
    }
    ImGui::GetStyle() = g_baseImGuiStyle;
    ui::RefreshTheme();

    const float scale = (float)state_.settings.uiScalePercent / 100.0f;
    ImGui::GetIO().FontGlobalScale = scale;
    if (scale > 1.0f) ImGui::GetStyle().ScaleAllSizes(scale);
}

void MainWindow::rebuildFonts() {
    state_.settings.fontSize = std::min(22, std::max(12, state_.settings.fontSize));
    if (ImGui::GetCurrentContext())
        loadUiFonts((float)state_.settings.fontSize, state_.settings.fontFamily);
}

void MainWindow::loadGlobalSettingsFromConfig() {
    const auto& cfg = ConfigManager::instance().config();
    state_.settings.uiScalePercent = std::min(150, std::max(100, cfg.uiScalePercent));
    state_.settings.fpsCap = (cfg.fpsCap == 30 || cfg.fpsCap == 120) ? cfg.fpsCap : 60;
    state_.settings.gpuMode = std::min(1, std::max(0, cfg.gpuMode));
    state_.settings.minimizeToTray = cfg.minimizeToTray;
    state_.settings.muteParkedAudio = cfg.muteParkedAudio;
    if (!cfg.performancePreset.empty())
        state_.settings.performancePreset = cfg.performancePreset;
}

void MainWindow::syncGlobalSettingsToConfig() const {
    auto& cfg = ConfigManager::instance().config();
    cfg.theme = "Custom";
    cfg.uiScalePercent = std::min(150, std::max(100, state_.settings.uiScalePercent));
    cfg.fpsCap = (state_.settings.fpsCap == 30 || state_.settings.fpsCap == 120)
        ? state_.settings.fpsCap : 60;
    cfg.gpuMode = std::min(1, std::max(0, state_.settings.gpuMode));
    cfg.minimizeToTray = state_.settings.minimizeToTray;
    cfg.muteParkedAudio = state_.settings.muteParkedAudio;
    if (!state_.settings.performancePreset.empty())
        cfg.performancePreset = state_.settings.performancePreset;
    ConfigManager::instance().save(AppSetup::configPath());
}

void MainWindow::applyPerformancePreset(const std::string& name) {
    auto& cfg = ConfigManager::instance().config();
    if (name == "Compact") {
        cfg.cpuCores = 2; cfg.ramMB = 2048;
        cfg.screenWidth = 720; cfg.screenHeight = 1280; cfg.dpi = 320;
    } else if (name == "Balanced") {
        cfg.cpuCores = 4; cfg.ramMB = 4096;
        cfg.screenWidth = 1080; cfg.screenHeight = 1920; cfg.dpi = 320;
    } else if (name == "Performance") {
        cfg.cpuCores = 8; cfg.ramMB = 8192;
        cfg.screenWidth = 1080; cfg.screenHeight = 1920; cfg.dpi = 480;
    } else {
        return;
    }
    state_.settings.performancePreset = name;
    LOG_INFO("Performance preset %s applied to emulator.ini", name.c_str());
    persist();
}

void MainWindow::applyParkedAudio() const {
    for (const auto& kv : vms_) {
        if (kv.second->state() != VmState::Running) continue;
        const bool active = mode_ == UiMode::Emulator && currentId_ == kv.first;
        kv.second->setAudioMuted(state_.settings.muteParkedAudio && !active);
    }
}

void MainWindow::startPhone(int id) {
    PhoneProfile* p = findProfile(id);
    if (!p || vms_.count(id)) return;
    if (p->paused) {
        p->paused = false;
        persist();
    }
    IVirtualMachine* raw = makeVmForProfile(p, false);
    if (raw) vms_[id] = std::unique_ptr<IVirtualMachine>(raw);
}

void MainWindow::deletePhones(const std::vector<int>& ids) {
    namespace fs = std::filesystem;
    for (int id : ids) {
        PhoneProfile* p = findProfile(id);
        if (!p) continue;
        std::string avd = p->avd;
        fs::path phoneData = AppSetup::phoneDataDir(p->port);
        closePhone(id);
        removePreview(id);
        resourceSamples_.erase(id);
        selectedIds_.erase(std::remove(selectedIds_.begin(), selectedIds_.end(), id),
                           selectedIds_.end());
        try {
            fs::remove_all(fs::path(AppSetup::avdDir()) / (avd + ".avd"));
            fs::remove(fs::path(AppSetup::avdDir()) / (avd + ".ini"));
            fs::remove_all(phoneData);
        } catch (...) { /* best effort */ }
        state_.phones.erase(std::remove_if(state_.phones.begin(), state_.phones.end(),
            [id](const PhoneProfile& q) { return q.id == id; }), state_.phones.end());
    }
    deleteIds_.clear();
    deleteId_ = 0;
    confirmDelete_ = false;
    persist();
}

void MainWindow::persist() const {
    saveManagerState(statePath_, state_);
    syncGlobalSettingsToConfig();
}

std::string MainWindow::previewPath(const PhoneProfile& p) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(AppSetup::thumbsDir(), ec);
    return (fs::path(AppSetup::thumbsDir()) / (p.avd + ".ppm")).string();
}

void MainWindow::loadPreviews() {
    previews_.clear();
    auto now = std::chrono::steady_clock::now();
    int loaded = 0;
    for (const auto& p : state_.phones) {
        PhonePreview pv;
        if (loadPpmRgba(previewPath(p), pv)) {
            pv.takenAt = now;
            previews_[p.id] = std::move(pv);
            ++loaded;
        }
    }
    if (loaded) LOG_INFO("Loaded %d phone preview(s)", loaded);
}

bool MainWindow::capturePreview(int id, bool saveToDisk) {
    auto now = std::chrono::steady_clock::now();
    PhonePreview& slot = previews_[id];
    IVirtualMachine* vm = nullptr;
    auto it = vms_.find(id);
    if (it != vms_.end()) vm = it->second.get();
    else if (mode_ == UiMode::Emulator && currentId_ == id) vm = vm_;

    if (!vm || vm->state() != VmState::Running) {
        slot.takenAt = now;
        return false;
    }

    PhonePreview fresh;
    vm->readFrame([&](const uint8_t* fb, int w, int h) {
        makePhonePreview(fb, w, h, fresh);
    });
    if (!fresh.valid()) {
        slot.takenAt = now;
        return false;
    }
    fresh.takenAt = now;
    slot = std::move(fresh);

    if (saveToDisk) {
        if (auto* p = findProfile(id)) {
            ppm_write(previewPath(*p).c_str(), slot.w, slot.h, slot.rgba.data());
        }
    }
    return true;
}

void MainWindow::refreshPreviews(bool force) {
    if (vms_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    int refreshed = 0;
    for (const auto& kv : vms_) {
        auto it = previews_.find(kv.first);
        if (!force && it != previews_.end() &&
            now - it->second.takenAt < std::chrono::minutes(state_.settings.previewMinutes)) {
            continue;
        }
        if (capturePreview(kv.first, true)) ++refreshed;
        if (!force) break;
    }
    if (refreshed) LOG_INFO("refreshed %d phone preview(s)", refreshed);
}

void MainWindow::removePreview(int id) {
    previews_.erase(id);
    if (auto* p = findProfile(id)) {
        std::error_code ec;
        std::filesystem::remove(previewPath(*p), ec);
    }
}

void MainWindow::addPhone(const std::string& name, const std::string& templateName) {
    int id = 1;
    while (findProfile(id)) ++id;

    int port = 5554;
    bool used = true;
    while (used) {
        used = false;
        for (const auto& p : state_.phones) {
            if (p.port == port) { used = true; break; }
        }
        if (used) port += 2;
    }

    PhoneProfile p;
    p.id = id;
    p.name = name;
    // AVD name is the physical boundary for Android userdata. Two phones with
    // the same display name must still get separate AVDs and separate data.
    const std::string baseAvd = "BD_" + sanitizeAvdName(p.name, id);
    p.avd = baseAvd;
    for (int suffix = 2; ; ++suffix) {
        const std::string wanted = lowerAscii(p.avd);
        bool collision = false;
        for (const auto& existing : state_.phones) {
            if (lowerAscii(existing.avd) == wanted) { collision = true; break; }
        }
        if (!collision) break;
        p.avd = baseAvd + "_" + std::to_string(suffix);
    }
    p.port = port;
    p.color = (unsigned int)(0x3B78DE + ((id * 0x1837) % 0x40));
    p.templateName = templateName;

    auto& cfg = ConfigManager::instance().config();
    if (templateName == "Compact") {
        p.cpuCores = 2; p.ramMB = 2048; p.screenWidth = 720; p.screenHeight = 1280; p.dpi = 320;
    } else if (templateName == "Tablet") {
        p.cpuCores = 6; p.ramMB = 6144; p.screenWidth = 1600; p.screenHeight = 2560; p.dpi = 320;
    } else if (templateName == "Performance") {
        p.cpuCores = 8; p.ramMB = 8192; p.screenWidth = 1080; p.screenHeight = 1920; p.dpi = 480;
    } else if (templateName == "Balanced") {
        p.cpuCores = cfg.cpuCores; p.ramMB = cfg.ramMB;
        p.screenWidth = cfg.screenWidth; p.screenHeight = cfg.screenHeight; p.dpi = cfg.dpi;
        p.templateName = "Balanced";
    }
    // "Global" leaves every override at 0 so emulator.ini decides.

    state_.phones.push_back(p);
    persist();
}

IVirtualMachine* MainWindow::makeVmForProfile(PhoneProfile* profile, bool attachExisting) {
    if (!profile) return nullptr;
    auto& cfg = ConfigManager::instance().config();
    VmSpec spec;
    spec.cpuCores = profile->cpuCores > 0 ? profile->cpuCores : cfg.cpuCores;
    spec.ramMB = profile->ramMB > 0 ? profile->ramMB : cfg.ramMB;
    spec.screenWidth = profile->screenWidth > 0 ? profile->screenWidth : cfg.screenWidth;
    spec.screenHeight = profile->screenHeight > 0 ? profile->screenHeight : cfg.screenHeight;
    spec.dpi = profile->dpi > 0 ? profile->dpi : cfg.dpi;
    spec.systemImagePath = AppSetup::imagesDir() + "/" + cfg.systemImage;
    // The Android backend keeps data in the AVD's own userdata-qemu.img. Give
    // the spec a per-phone path as well so no backend ever falls back to one
    // shared data image for every profile.
    spec.dataImagePath = AppSetup::phoneDataDir(profile->port) + "/userdata-qemu.img";
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(spec.dataImagePath).parent_path(), ec);
    }
    spec.enableGpu = cfg.gpuMode == 0;
    spec.enableNetwork = true;
    spec.fpsCap = cfg.fpsCap;
    spec.gpuMode = cfg.gpuMode;

    IVirtualMachine* raw = createVirtualMachine(cfg.vmBackend);
    if (!raw) return nullptr;
    if (auto* android = dynamic_cast<AndroidVirtualMachine*>(raw)) {
        android->configure(profile->avd, profile->port, attachExisting);
        if (attachExisting && !android->isDeviceOnline()) {
            delete raw;
            return nullptr;
        }
    }
    if (!raw->create(spec) || !raw->start()) {
        LOG_ERROR("VM %s failed for %s: %s", attachExisting ? "attach" : "create/start",
            profile->name.c_str(), raw->lastError().c_str());
        delete raw;
        return nullptr;
    }
    return raw;
}

void MainWindow::reattachRunningPhones() {
    if (ConfigManager::instance().config().vmBackend != "android") return;
    int attached = 0;
    for (auto& p : state_.phones) {
        if (p.paused) continue;
        IVirtualMachine* raw = makeVmForProfile(&p, true);
        if (!raw) continue;
        vms_[p.id] = std::unique_ptr<IVirtualMachine>(raw);
        ++attached;
        LOG_INFO("Reattached phone %s (port %d)", p.name.c_str(), p.port);
    }
    if (attached) LOG_INFO("Reattached %d running phone(s)", attached);
}

void MainWindow::openEmulator(int id) {
    PhoneProfile* profile = findProfile(id);
    if (!profile) return;
    LOG_INFO("open phone id=%d name=%s port=%d avd=%s", id, profile->name.c_str(), profile->port, profile->avd.c_str());
    if (mode_ == UiMode::Emulator && currentId_ == id) return;
    if (mode_ == UiMode::Emulator) leaveEmulator();

    auto it = vms_.find(id);
    if (it == vms_.end()) {
        IVirtualMachine* raw = makeVmForProfile(profile, false);
        if (!raw) {
            showMessageModal_ = true;
            modalTitle_ = "BlueDroid";
            modalMessage_ = "Could not start this phone. Check that Android setup has finished.";
            return;
        }
        vms_[id] = std::unique_ptr<IVirtualMachine>(raw);
    }
    setVm(vms_[id].get());

    if (profile->paused) vm_->pause();
    else if (vm_->state() == VmState::Paused) vm_->resume();

    currentId_ = id;
    mode_ = UiMode::Emulator;
    applyParkedAudio();
    lastInputAt_ = std::chrono::steady_clock::now();
    lastFrame_ = 0;
}

void MainWindow::closePhone(int id) {
    capturePreview(id, true);
    auto it = vms_.find(id);
    if (it != vms_.end()) {
        stopVm(it->second.get());
        vms_.erase(it);
    }
    auto tex = textures_.find(id);
    if (tex != textures_.end()) {
        releaseTexture(tex->second.get(), renderer_ ? renderer_->context : nullptr);
        textures_.erase(tex);
    }
    resourceSamples_.erase(id);
    selectedIds_.erase(std::remove(selectedIds_.begin(), selectedIds_.end(), id),
                       selectedIds_.end());
    if (currentId_ == id) {
        currentId_ = 0;
        vm_ = nullptr;
        input_.reset();
    }
}

void MainWindow::leaveEmulator() {
    if (mode_ != UiMode::Emulator) return;
    int id = currentId_;
    LOG_INFO("park phone id=%d (kept running)", id);
    capturePreview(id, true);
    phoneHovered_ = false;
    touchActive_ = false;
    vm_ = nullptr;
    input_.reset();
    currentId_ = 0;
    mode_ = UiMode::Manager;
    applyParkedAudio();
}

void MainWindow::closeAllPhones() {
    for (const auto& kv : vms_) capturePreview(kv.first, true);
    for (auto& kv : vms_) stopVm(kv.second.get());
    vms_.clear();
    for (auto& kv : textures_) releaseTexture(kv.second.get(), renderer_ ? renderer_->context : nullptr);
    textures_.clear();
    resourceSamples_.clear();
    selectedIds_.clear();
    vm_ = nullptr;
    input_.reset();
    currentId_ = 0;
    phoneHovered_ = false;
    touchActive_ = false;
    if (mode_ == UiMode::Emulator || mode_ == UiMode::Settings) {
        mode_ = UiMode::Manager;
        settingsReturnMode_ = UiMode::Manager;
    }
}

void MainWindow::detachAllPhones() {
    for (const auto& kv : vms_) capturePreview(kv.first, true);
    for (auto& kv : vms_) kv.second.release();
    vms_.clear();
    vm_ = nullptr;
    input_.reset();
    currentId_ = 0;
}

void MainWindow::exitEverything() {
    removeTrayIcon();
    cancelAllApkJobs();
    closeAllPhones();
    NetworkStack::instance().shutdown();
    AudioEngine::instance().shutdown();
    DestroyWindow(hwnd_);
}

void MainWindow::installTrayIcon() {
#ifdef _WIN32
    if (trayIcon_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = TRAY_MSG;
    nid.hIcon = LoadIcon(hInst_, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"BlueDroid - phones still running");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) trayIcon_ = true;
#else
    // SDL2 has no system tray API. hideToTray() minimizes instead, so phones
    // keep running and the window remains reachable from the dock/taskbar.
    (void)TRAY_ID;
    (void)TRAY_MSG;
    LOG_DEBUG("System tray icons are unavailable with the SDL backend");
#endif
}

void MainWindow::removeTrayIcon() {
#ifdef _WIN32
    if (!trayIcon_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayIcon_ = false;
#else
    trayIcon_ = false;
#endif
}

void MainWindow::hideToTray() {
#ifdef _WIN32
    installTrayIcon();
    ShowWindow(hwnd_, SW_HIDE);
#else
    // SDL2 has no system tray. Minimize instead of hiding so the manager can
    // still be restored from the dock/taskbar while phones keep running.
    ShowWindow(hwnd_, SW_MINIMIZE);
#endif
}

void MainWindow::restoreFromTray() {
    removeTrayIcon();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void MainWindow::openSettings() {
    if (mode_ == UiMode::Settings) return;
    settingsReturnMode_ = mode_;
    naming_ = false;
    nameBuffer_[0] = 0;
    mode_ = UiMode::Settings;
}

void MainWindow::closeSettings() {
    if (mode_ != UiMode::Settings) return;
    mode_ = settingsReturnMode_;
    if (mode_ == UiMode::Emulator && (!vm_ || currentId_ == 0)) mode_ = UiMode::Manager;
}

void MainWindow::toggleFullscreen() {
    if (!fullscreen_) {
        GetWindowRect(hwnd_, &savedRect_);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
#ifdef _WIN32
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
#endif
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
#ifdef _WIN32
        SetWindowLongPtrW(hwnd_, GWL_STYLE, kWindowStyle | WS_VISIBLE);
#endif
        SetWindowPos(hwnd_, nullptr, savedRect_.left, savedRect_.top,
            savedRect_.right - savedRect_.left, savedRect_.bottom - savedRect_.top,
            SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
    resizeRenderer();
    applyWindowCorners();
}

void MainWindow::applyWindowCorners() {
    if (!hwnd_) return;

    // DWM's ROUND mode clips the first visible row of a fully borderless DX11
    // window, leaving a thin desktop-colored line across the top. Keep DWM in
    // DONOTROUND mode and clip the window ourselves instead. This gives real
    // rounded corners without eating the top row.
    DWORD preference = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
        &preference, sizeof(preference));

    if (fullscreen_ || IsZoomed(hwnd_) || !state_.settings.roundedCorners ||
        state_.settings.cornerRadius <= 0) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd_, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 2 || h <= 2) return;
    const int diameter = std::min(w - 1, std::min(h - 1, state_.settings.cornerRadius * 2));
    HRGN region = CreateRoundRectRgn(0, 0, w, h, diameter, diameter);
    if (region) SetWindowRgn(hwnd_, region, TRUE);
}

RECT MainWindow::headerDragRect() const {
    RECT r{};
    const int w = winW_;
    if (w <= 0) return r;
    const int controlsReserve = 144;

    switch (mode_) {
        case UiMode::Manager:
            // Header buttons start around w-730; leave a small gap so clicks on
            // them stay with ImGui while the rest of the top bar drags.
            r.right = std::max(0, w - 740);
            r.bottom = 64;
            break;
        case UiMode::Settings:
            r.left = 96;                 // keep the Back button clickable
            r.right = std::max<LONG>(r.left, (LONG)(w - controlsReserve));
            r.bottom = 72;
            break;
        default:
            r.left = 126;                // keep the Manager button clickable
            r.right = std::max<LONG>(r.left, (LONG)(w - controlsReserve));
            r.bottom = 48;
            break;
    }
    return r;
}

bool MainWindow::modalOpen() const {
    return naming_ || confirmDelete_ || showMessageModal_ || showFirstRunPrompt_ ||
           apkPickerOpen_ || menuPhoneId_ != 0;
}

void MainWindow::startUpdateCheck(bool manual) {
    const HWND hwnd = hwnd_;
    if (!hwnd) return;
    bool expected = false;
    if (!updateCheckRunning_.compare_exchange_strong(expected, true)) return;
    updateCheckManual_ = manual;
    LOG_INFO("Checking for updates (current version %s)", kAppVersion);

    // Never touch `this` from the worker thread. The result is marshalled back
    // through a window message, so closing Settings or switching phones cannot
    // race the updater.
    std::thread([hwnd]() {
        UpdateInfo info = Updater::checkAndDownload();
        auto* result = new UpdateInfo(std::move(info));
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, UPDATE_RESULT_MSG, 0,
                                             reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void MainWindow::update() {
    auto now = std::chrono::steady_clock::now();

    if (fontRebuildPending_) {
        fontRebuildPending_ = false;
        rebuildFonts();
        persist();
    }

    updateApkJobs();

    SetupStatus setup = AppSetup::status();
    if (setup.running) {
        setupBanner_ = setup.stage + " " + std::to_string(setup.percent) + "%";
    } else if (!setup.error.empty()) {
        setupBanner_ = "Android setup: " + setup.error;
    } else if (!setup.warning.empty()) {
        setupBanner_ = setup.warning;
    } else {
        setupBanner_.clear();
    }

    // Silent startup update check, once per launch, after the window is up.
    if (!updateCheckStarted_ && now - createdAt_ > std::chrono::seconds(5)) {
        updateCheckStarted_ = true;
        startUpdateCheck(false);
    }

    // Offer the new gamer settings once after the first successful setup.
    if (mode_ == UiMode::Manager && setup.done && setup.error.empty() &&
        !state_.settings.firstRunSettingsSeen && !modalOpen()) {
        showFirstRunPrompt_ = true;
    }

    if (mode_ == UiMode::Emulator && vm_) {
        auto* mock = dynamic_cast<MockVirtualMachine*>(vm_);
        if (mock) mock->tick();

        uint64_t f = vm_->frameCounter();
        auto sampleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsSample_).count();
        if (sampleMs >= 500) {
            fps_ = (double)(f - lastFpsFrames_) * 1000.0 / (double)sampleMs;
            lastFpsFrames_ = f;
            lastFpsSample_ = now;
        }
        lastFrame_ = f;
    }

    // Part 1 - anti-AFK keys. Harmless directional keys reset game AFK
    // timers. Letter keycodes are never used: Android turns them into real
    // text in any focused EditText. The active phone is skipped while you use it.
    if (state_.settings.antiAfk && now - lastAfkAt_ > std::chrono::seconds(4)) {
        bool sent = false;
        for (const auto& kv : vms_) {
            IVirtualMachine* phone = kv.second.get();
            if (phone->state() != VmState::Running) continue;
            if (phone == vm_ && now - lastInputAt_ < std::chrono::seconds(10)) continue;
            switch (afkStep_++ % 5) {
                case 0: phone->sendKey(19, true); break; // DPAD_UP
                case 1: phone->sendKey(20, true); break; // DPAD_DOWN
                case 2: phone->sendKey(21, true); break; // DPAD_LEFT
                case 3: phone->sendKey(22, true); break; // DPAD_RIGHT
                case 4: phone->sendKey(23, true); break; // DPAD_CENTER
            }
            sent = true;
        }
        if (sent) lastAfkAt_ = now;
    }

    // Part 2 - keep the phone on. WAKEUP is separate from the key sequence so
    // you can disable AFK input but still stop parked screens from sleeping.
    if (state_.settings.keepAwake && now - lastAwakeAt_ > std::chrono::seconds(15)) {
        bool sent = false;
        for (const auto& kv : vms_) {
            IVirtualMachine* phone = kv.second.get();
            if (phone->state() != VmState::Running) continue;
            phone->sendKey(224, true); // WAKEUP
            sent = true;
        }
        if (sent) lastAwakeAt_ = now;
    }

    if (!vms_.empty()) {
        sampleResources();
        refreshPreviews(false);
    }
}

// ---------------------------------------------------------------------------
// ImGui rendering
// ---------------------------------------------------------------------------

void MainWindow::ensurePhoneTexture(int id, int w, int h) {
    if (!renderer_ || w <= 0 || h <= 0) return;
    auto& slot = textures_[id];
    if (!slot) slot = std::make_unique<PhoneTexture>();
    if (slot->texture && slot->w == w && slot->h == h) return;
    releaseTexture(slot.get(), renderer_->context);

#ifdef _WIN32
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(renderer_->device->CreateTexture2D(&td, nullptr, &slot->texture))) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(renderer_->device->CreateShaderResourceView(slot->texture, &sv, &slot->srv))) {
        releaseTexture(slot.get(), renderer_->context);
        return;
    }
    slot->view = (ImTextureID)(uintptr_t)slot->srv;
#else
    glGenTextures(1, &slot->texture);
    glBindTexture(GL_TEXTURE_2D, slot->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    slot->view = (ImTextureID)(uintptr_t)slot->texture;
#endif
    slot->w = w;
    slot->h = h;
}

void MainWindow::updatePhoneTexture(int id, const uint8_t* rgba, int w, int h) {
    if (!renderer_ || !rgba || w <= 0 || h <= 0) return;
    ensurePhoneTexture(id, w, h);
    auto it = textures_.find(id);
    if (it == textures_.end() || !it->second || !it->second->view) return;
    PhoneTexture* tex = it->second.get();
    if (tex->w != w || tex->h != h) return;

#ifdef _WIN32
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(renderer_->context->Map(tex->texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* dst = (uint8_t*)mapped.pData;
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h; ++y) {
        memcpy(dst + (size_t)y * mapped.RowPitch, rgba + (size_t)y * rowBytes, rowBytes);
    }
    renderer_->context->Unmap(tex->texture, 0);
#else
    glBindTexture(GL_TEXTURE_2D, tex->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
}

void MainWindow::renderFrame() {
#ifdef _WIN32
    if (!renderer_ || !renderer_->context || !renderer_->rtv ||
        IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) return;
#else
    if (!renderer_ || !renderer_->glContext ||
        IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) return;
#endif

    update();

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("BlueDroidRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (mode_ == UiMode::Manager) drawManager();
    else if (mode_ == UiMode::Settings) drawSettings();
    else drawEmulator();
    drawDialogs();

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT width = (UINT)std::max(1, (int)rc.right);
    UINT height = (UINT)std::max(1, (int)rc.bottom);
#ifdef _WIN32
    const bool postShadersReady = renderer_->postVS && renderer_->postPS &&
        renderer_->postConstants && renderer_->postSampler;
    const bool usePost = postShadersReady && ensurePostTargets(width, height);

    ID3D11RenderTargetView* target = usePost
        ? (renderer_->msaaRtv ? renderer_->msaaRtv : renderer_->sceneRtv)
        : renderer_->rtv;
    if (!target) target = renderer_->rtv;

    auto* ctx = renderer_->context;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = (float)width;
    viewport.Height = (float)height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &viewport);

    ImVec4 bg = hdr::V4(g_uiTheme.windowBg);
    const float clear[4] = { bg.x, bg.y, bg.z, bg.w };
    ctx->ClearRenderTargetView(target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (usePost) renderPostPass();
    renderer_->swapChain->Present(1, 0);
#else
    ImVec4 bg = hdr::V4(g_uiTheme.windowBg);
    int drawableW = (int)width, drawableH = (int)height;
    SDL_GL_GetDrawableSize((SDL_Window*)hwnd_, &drawableW, &drawableH);
    glViewport(0, 0, (GLsizei)drawableW, (GLsizei)drawableH);
    glClearColor(bg.x, bg.y, bg.z, bg.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow((SDL_Window*)hwnd_);
#endif
}

void MainWindow::drawManager() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float toolbarY = 88.0f, toolbarH = 44.0f, contentY = toolbarY + toolbarH;
    // Header text sits deliberately low to balance the taller title area.
    const float headerTitleY = 32.0f;
    const float headerSubtitleY = 60.0f;

    std::vector<int> visible;
    for (int id : orderedProfileIds()) {
        if (phoneMatchesFilters(id)) visible.push_back(id);
    }

    auto toggleSelected = [&](int id) {
        auto it = std::find(selectedIds_.begin(), selectedIds_.end(), id);
        if (it != selectedIds_.end()) selectedIds_.erase(it);
        else selectedIds_.push_back(id);
    };

    // Header.
    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    ImGui::BeginChild("ManagerHeader", ImVec2(size.x, toolbarY), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorScreenPos(ImVec2(20, headerTitleY));
    ImGui::TextColored(ui::TextMain, "BlueDroid Manager");
    char countBuf[128];
    snprintf(countBuf, sizeof(countBuf), "%d shown / %d phone(s)  -  starred first",
        (int)visible.size(), (int)state_.phones.size());
    ImGui::SetCursorScreenPos(ImVec2(20, headerSubtitleY));
    ImGui::TextDisabled("%s", countBuf);
    if (!setupBanner_.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(390, headerTitleY + 10.0f));
        ImGui::TextColored(ui::Accent, "%s", setupBanner_.c_str());
    }

    // Leave room for the custom window controls on the right edge.
    const float winCtrlReserve = 126.0f;
    ImGui::SetCursorScreenPos(ImVec2(size.x - 100 - winCtrlReserve, 14));
    if (ui::Button("Settings", ImVec2(92, 36))) openSettings();
    ImGui::SetCursorScreenPos(ImVec2(size.x - 216 - winCtrlReserve, 14));
    if (ui::Button("Exit all", ImVec2(108, 36))) exitEverything();
    ImGui::SetCursorScreenPos(ImVec2(size.x - 332 - winCtrlReserve, 14));
    if (ui::Button("Stop all", ImVec2(108, 36))) closeAllPhones();
    ImGui::SetCursorScreenPos(ImVec2(size.x - 604 - winCtrlReserve, 14));
    if (ui::Button("Install APK", ImVec2(100, 36))) chooseApkFile();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop an APK anywhere, or click to browse");
    ImGui::SetCursorScreenPos(ImVec2(size.x - 492 - winCtrlReserve, 14));
    if (ui::ButtonAccent("+ New emulator", ImVec2(152, 36))) {
        naming_ = true;
        nameFocus_ = true;
        namingId_ = 0;
        templateIndex_ = 0;
        nameBuffer_[0] = 0;
    }
    drawWindowControls();
    ImGui::EndChild();

    // Search / filters / view / multi-select toolbar.
    ImGui::SetCursorScreenPos(ImVec2(0, toolbarY));
    ImGui::BeginChild("ManagerToolbar", ImVec2(size.x, toolbarH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorScreenPos(ImVec2(18, toolbarY + 8));
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("##phoneSearch", searchBuffer_, sizeof(searchBuffer_))) {
        state_.settings.searchText = searchBuffer_;
        persist();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search name, AVD, port, or template");

    float fx = 250.0f;
    auto filterButton = [&](const char* label, int value, float width) {
        ImGui::SetCursorScreenPos(ImVec2(fx, toolbarY + 8));
        bool on = state_.settings.statusFilter == value;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
        }
        if (ui::Button(label, ImVec2(width, 28))) {
            state_.settings.statusFilter = value;
            persist();
        }
        if (on) ImGui::PopStyleColor(2);
        fx += width + 6.0f;
    };
    filterButton("All", 0, 50);
    filterButton("Running", 1, 76);
    filterButton("Stopped", 2, 76);
    filterButton("Paused", 3, 72);
    filterButton("Starred", 4, 78);

    if (selectionMode_) {
        ImGui::SetCursorScreenPos(ImVec2(size.x - 430, toolbarY + 8));
        if (ui::Button("All", ImVec2(46, 28))) {
            selectedIds_ = visible;
        }
        ImGui::SetCursorScreenPos(ImVec2(size.x - 378, toolbarY + 8));
        if (ui::Button("Start", ImVec2(62, 28))) {
            for (int id : selectedIds_) startPhone(id);
        }
        ImGui::SetCursorScreenPos(ImVec2(size.x - 310, toolbarY + 8));
        if (ui::Button("Stop", ImVec2(58, 28))) {
            for (int id : selectedIds_) closePhone(id);
        }
        ImGui::SetCursorScreenPos(ImVec2(size.x - 246, toolbarY + 8));
        if (ui::Button("Backup", ImVec2(72, 28))) backupPhones(selectedIds_);
        ImGui::SetCursorScreenPos(ImVec2(size.x - 168, toolbarY + 8));
        if (ui::Button("Del", ImVec2(50, 28)) && !selectedIds_.empty()) {
            deleteIds_ = selectedIds_;
            confirmDelete_ = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(size.x - 112, toolbarY + 8));
        if (ui::Button("Done", ImVec2(96, 28))) {
            selectionMode_ = false;
            selectedIds_.clear();
        }
    } else {
        auto viewButton = [&](const char* label, bool list, float x, float width) {
            ImGui::SetCursorScreenPos(ImVec2(x, toolbarY + 8));
            bool on = state_.settings.listView == list;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
            }
            if (ui::Button(label, ImVec2(width, 28))) {
                state_.settings.listView = list;
                persist();
            }
            if (on) ImGui::PopStyleColor(2);
        };
        viewButton("Grid", false, size.x - 216, 62);
        viewButton("List", true, size.x - 148, 58);
        ImGui::SetCursorScreenPos(ImVec2(size.x - 84, toolbarY + 8));
        if (ui::Button("Select", ImVec2(68, 28))) selectionMode_ = true;
    }
    ImGui::EndChild();

    // Cards / rows. Leave room for the real transfer card at bottom-right.
    const float cardsH = size.y - contentY - (apkPanelVisible() ? 158.0f : 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(0, contentY));
    ImGui::BeginChild("ManagerCards", ImVec2(size.x, cardsH), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    cardHits_.clear();

    if (state_.phones.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(size.x * 0.5f - 110, contentY + size.y * 0.4f));
        ImGui::TextDisabled("No phones yet");
        ImGui::SetCursorScreenPos(ImVec2(size.x * 0.5f - 160, contentY + size.y * 0.4f + 24));
        ImGui::TextDisabled("Click \"+ New emulator\" to add one");
    } else if (visible.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(size.x * 0.5f - 130, contentY + size.y * 0.4f));
        ImGui::TextDisabled("No phones match this search/filter");
    } else if (state_.settings.listView) {
        const float margin = 18.0f, gap = 8.0f, rowH = 72.0f;
        for (size_t i = 0; i < visible.size(); ++i) {
            float y = contentY + margin + (float)i * (rowH + gap);
            drawPhoneRow(visible[i], margin, y, size.x - margin * 2, rowH);
        }
    } else {
        const float margin = 18.0f, gap = 14.0f, cardH = 216.0f;
        const int cols = 6;
        float cardW = (size.x - margin * 2 - gap * (cols - 1)) / (float)cols;
        if (cardW < 90) cardW = 90;
        for (size_t i = 0; i < visible.size(); ++i) {
            int row = (int)(i / cols);
            int col = (int)(i % cols);
            float x = margin + col * (cardW + gap);
            float y = contentY + margin + row * (cardH + gap);
            drawCard(visible[i], x, y, cardW, cardH);
        }
    }
    ImGui::EndChild();
    drawApkProgress();
}

void MainWindow::drawCard(int id, float x, float y, float w, float h) {
    PhoneProfile* p = findProfile(id);
    if (!p) return;
    ImGui::PushID(id);

    // Eased hover/selection state. The grid slot stays fixed, only the card
    // artwork lifts and expands a couple of pixels, so layout never reflows.
    const ImGuiID hoverId = ImGui::GetID("##cardHover");
    const ImGuiID selectId = ImGui::GetID("##cardSelect");
    const float prevHover = ImGui::GetStateStorage()->GetFloat(hoverId, 0.f);
    const float prevLift = 3.f * prevHover;
    const bool hovered = !modalOpen() &&
        ImGui::IsMouseHoveringRect(ImVec2(x, y - prevLift - 2.f),
                                   ImVec2(x + w, y + h - prevLift + 2.f), false);
    const float hover = hdr::Anim(hoverId, hovered, 16.f);
    const bool selected = std::find(selectedIds_.begin(), selectedIds_.end(), id) != selectedIds_.end();
    const float selT = hdr::Anim(selectId, selected, 15.f);

    const float lift = 3.f * hover;
    const float grow = 2.f * hover;
    const ImVec2 v0(x - grow, y - lift - grow);
    const ImVec2 v1(x + w + grow, y + h - lift + grow);
    const float vw = v1.x - v0.x;
    const float vh = v1.y - v0.y;

    // Soft glow in the parent draw list; the child below paints the card face.
    if (hover > 0.01f || selT > 0.01f) {
        ImDrawList* parent = ImGui::GetWindowDrawList();
        const float strength = std::min(1.f, hover + selT * 0.45f);
        for (int i = 3; i >= 1; --i) {
            const float pad = (float)i * 2.5f * strength;
            parent->AddRectFilled(ImVec2(v0.x - pad, v0.y - pad),
                                  ImVec2(v1.x + pad, v1.y + pad),
                                  hdr::Alpha(g_uiTheme.accent, 0.022f * strength * (4 - i)),
                                  14.f + pad);
        }
    }

    ImGui::SetCursorScreenPos(v0);
    ImGui::BeginChild(("Card" + std::to_string(id)).c_str(), ImVec2(vw, vh),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 p0 = v0;
    ImVec2 p1 = v1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    PhoneStatus status = phoneStatus(id);
    bool active = status == PhoneStatus::Running;

    const ImU32 baseBg = active
        ? ImGui::GetColorU32(ImVec4(0.12f, 0.20f, 0.18f, 1.f)) : ui::U32(ui::Card);
    const ImU32 cardBg = hdr::Mix(baseBg, hdr::Mix(baseBg, g_uiTheme.accent, 0.20f), hover);
    dl->AddRectFilled(p0, p1, cardBg, 12.f + 2.f * hover);

    const ImU32 quietBorder = hdr::Mix(g_uiTheme.border, IM_COL32(120, 120, 135, 255), 0.45f);
    const ImU32 baseBorder = active ? g_uiTheme.accent : quietBorder;
    const ImU32 border = hdr::Mix(baseBorder, g_uiTheme.accent,
        std::min(1.f, selT * 0.90f + hover * 0.45f));
    dl->AddRect(p0, p1, border, 12.f + 2.f * hover, 0, 1.f + selT + hover * 0.8f);

    // Star.
    ImVec4 star = p->starred ? ImVec4(1.f, 0.78f, 0.24f, 1.f) : ImVec4(0.35f, 0.35f, 0.42f, 1.f);
    dl->AddText(ImVec2(p0.x + 11, p0.y + 5), ImGui::GetColorU32(star), "*");

    // Selection checkbox (only visible while multi-select is on).
    if (selectionMode_) {
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 30, p0.y + 5));
        bool sel = selected;
        if (ImGui::Checkbox(("##selCard" + std::to_string(id)).c_str(), &sel)) {
            auto it = std::find(selectedIds_.begin(), selectedIds_.end(), id);
            if (sel && it == selectedIds_.end()) selectedIds_.push_back(id);
            else if (!sel && it != selectedIds_.end()) selectedIds_.erase(it);
        }
    }

    // Phone silhouette.
    float pw = std::min(68.0f, vw - 28.0f);
    float ph = pw * 1.90f;
    float px = p0.x + (vw - pw) * 0.5f;
    float py = p0.y + 10.0f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), IM_COL32(12, 12, 18, 255), 10.f);
    ImVec2 screenA(px + 4, py + 4);
    ImVec2 screenB(px + pw - 4, py + ph - 4);

    auto pit = previews_.find(id);
    if (pit != previews_.end() && pit->second.valid()) {
        ensurePhoneTexture(id, pit->second.w, pit->second.h);
        updatePhoneTexture(id, pit->second.rgba.data(), pit->second.w, pit->second.h);
        auto tit = textures_.find(id);
        if (tit != textures_.end() && tit->second && tit->second->view) {
            ImGui::SetCursorScreenPos(screenA);
            ImGui::Image(tit->second->view, ImVec2(screenB.x - screenA.x, screenB.y - screenA.y));
        }
    } else {
        ImU32 color = IM_COL32((p->color) & 0xFF, (p->color >> 8) & 0xFF, (p->color >> 16) & 0xFF, 255);
        dl->AddRectFilled(screenA, screenB, color, 7.f);
    }
    dl->AddRectFilled(ImVec2(px + pw * 0.5f - 12, py + 5),
                      ImVec2(px + pw * 0.5f + 12, py + 11), IM_COL32(12, 12, 18, 255), 3.f);

    // Name, status indicator, and live resource badge.
    auto statusLabel = [](PhoneStatus s) -> const char* {
        switch (s) {
            case PhoneStatus::Running: return "Active";
            case PhoneStatus::Parked: return "Parked";
            case PhoneStatus::Starting: return "Starting";
            case PhoneStatus::Paused: return "Paused";
            case PhoneStatus::Error: return "Error";
            default: return "Stopped";
        }
    };
    auto statusColor = [](PhoneStatus s) -> ImU32 {
        switch (s) {
            case PhoneStatus::Running: return IM_COL32(52, 168, 83, 255);
            case PhoneStatus::Parked: return IM_COL32(78, 140, 240, 255);
            case PhoneStatus::Starting: return IM_COL32(96, 165, 250, 255);
            case PhoneStatus::Paused: return IM_COL32(230, 180, 60, 255);
            case PhoneStatus::Error: return IM_COL32(230, 80, 80, 255);
            default: return IM_COL32(120, 120, 135, 255);
        }
    };

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 10, p1.y - 66));
    ImGui::TextColored(ui::TextMain, "%s", p->name.c_str());
    dl->AddCircleFilled(ImVec2(p0.x + 16, p1.y - 42), 5.f, statusColor(status), 12);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 28, p1.y - 47));
    ImGui::TextColored(hdr::V4(statusColor(status)), "%s", statusLabel(status));

    if (state_.settings.showResources) {
        auto rit = resourceSamples_.find(id);
        char res[64];
        if (rit != resourceSamples_.end() && rit->second.ramBytes > 0) {
            snprintf(res, sizeof(res), "CPU %2.0f%%   RAM %llu MB",
                rit->second.cpuPercent,
                (unsigned long long)(rit->second.ramBytes / (1024ULL * 1024ULL)));
        } else {
            snprintf(res, sizeof(res), "CPU --   RAM --");
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 10, p1.y - 25));
        ImGui::TextDisabled("%s", res);
    }

    // Three dots.
    for (int i = 0; i < 3; ++i) {
        float cx = p1.x - 25 + i * 6;
        dl->AddCircleFilled(ImVec2(cx, p1.y - 22), 2.f, IM_COL32(170, 170, 185, 255), 8);
    }

    CardHit hit;
    hit.id = id;
    hit.card = { (LONG)v0.x, (LONG)v0.y, (LONG)v1.x, (LONG)v1.y };
    hit.star = { (LONG)(v0.x + 6), (LONG)(v0.y + 4), (LONG)(v0.x + 36), (LONG)(v0.y + 34) };
    hit.menu = { (LONG)(v1.x - 38), (LONG)(v1.y - 38), (LONG)(v1.x - 6), (LONG)(v1.y - 8) };
    cardHits_.push_back(hit);

    // Interaction surfaces. Draw order is deliberate: the whole card opens
    // the phone, then the star/menu overlays take priority.
    if (modalOpen()) {
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }

    // Non-overlapping hit areas: two body strips, the star, and the menu.
    // Overlapping invisible buttons are ambiguous in ImGui's hover test.
    auto bodyButton = [&](float bx, float by, float bw, float bh, const char* suffix) {
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (ImGui::InvisibleButton((std::string("##open") + suffix + std::to_string(id)).c_str(),
                                   ImVec2(bw, bh))) {
            openEmulator(id);
        }
    };
    bodyButton(v0.x + 40, v0.y, vw - 80, 38, "Top");
    bodyButton(v0.x, v0.y + 38, vw - 40, vh - 38, "Main");

    ImGui::SetCursorScreenPos(ImVec2(v0.x + 6, v0.y + 4));
    if (ImGui::InvisibleButton(("##star" + std::to_string(id)).c_str(), ImVec2(30, 30))) {
        handleCardAction(id, 3);
    }

    ImGui::SetCursorScreenPos(ImVec2(v1.x - 40, v1.y - 38));
    if (ImGui::InvisibleButton(("##menu" + std::to_string(id)).c_str(), ImVec2(40, 38))) {
        menuPhoneId_ = id;
        menuX_ = (int)(v1.x - 150);
        menuY_ = (int)(v1.y - 40);
        menuRequested_ = true;
        menuOpenedAt_ = std::chrono::steady_clock::now();
    }

    ImGui::EndChild();
    ImGui::PopID();
}

void MainWindow::drawPhoneRow(int id, float x, float y, float w, float h) {
    PhoneProfile* p = findProfile(id);
    if (!p) return;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::BeginChild(("Row" + std::to_string(id)).c_str(), ImVec2(w, h),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + w, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    PhoneStatus status = phoneStatus(id);
    bool active = status == PhoneStatus::Running;
    bool selected = std::find(selectedIds_.begin(), selectedIds_.end(), id) != selectedIds_.end();

    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(active ? ImVec4(0.12f, 0.20f, 0.18f, 1) : ui::Card), 10.f);
    dl->AddRect(p0, p1, ImGui::GetColorU32(selected ? ui::Accent :
        (active ? ui::Accent : ImVec4(0.24f, 0.24f, 0.31f, 1))),
        10.f, 0, selected ? 2.f : 1.f);

    auto statusLabel = [](PhoneStatus s) -> const char* {
        switch (s) {
            case PhoneStatus::Running: return "Active";
            case PhoneStatus::Parked: return "Parked";
            case PhoneStatus::Starting: return "Starting";
            case PhoneStatus::Paused: return "Paused";
            case PhoneStatus::Error: return "Error";
            default: return "Stopped";
        }
    };
    auto statusColor = [](PhoneStatus s) -> ImU32 {
        switch (s) {
            case PhoneStatus::Running: return IM_COL32(52, 168, 83, 255);
            case PhoneStatus::Parked: return IM_COL32(78, 140, 240, 255);
            case PhoneStatus::Starting: return IM_COL32(96, 165, 250, 255);
            case PhoneStatus::Paused: return IM_COL32(230, 180, 60, 255);
            case PhoneStatus::Error: return IM_COL32(230, 80, 80, 255);
            default: return IM_COL32(120, 120, 135, 255);
        }
    };

    dl->AddCircleFilled(ImVec2(p0.x + 18, p0.y + h * 0.5f), 6.f, statusColor(status), 12);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 34, p0.y + 12));
    ImGui::TextColored(ui::TextMain, "%s", p->name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 34, p0.y + 38));
    ImGui::TextColored(hdr::V4(statusColor(status)), "%s", statusLabel(status));

    char detail[192];
    char res[64];
    auto rit = resourceSamples_.find(id);
    if (state_.settings.showResources && rit != resourceSamples_.end() && rit->second.ramBytes > 0) {
        snprintf(res, sizeof(res), "CPU %2.0f%%  RAM %llu MB", rit->second.cpuPercent,
            (unsigned long long)(rit->second.ramBytes / (1024ULL * 1024ULL)));
    } else {
        snprintf(res, sizeof(res), "CPU --  RAM --");
    }
    snprintf(detail, sizeof(detail), "Port %d   %s%s", p->port, res,
             p->templateName.empty() ? "" : ("   Template: " + p->templateName).c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 118, p0.y + 38));
    ImGui::TextDisabled("%s", detail);

    ImVec4 star = p->starred ? ImVec4(1.f, 0.78f, 0.24f, 1.f) : ImVec4(0.35f, 0.35f, 0.42f, 1.f);
    dl->AddText(ImVec2(p0.x + w - 104, p0.y + 12), ImGui::GetColorU32(star), "*");
    for (int i = 0; i < 3; ++i) {
        dl->AddCircleFilled(ImVec2(p1.x - 24 + i * 6, p0.y + h * 0.5f), 2.f,
            IM_COL32(170, 170, 185, 255), 8);
    }

    if (selectionMode_) {
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 70, p0.y + h * 0.5f - 11));
        bool sel = selected;
        if (ImGui::Checkbox(("##selRow" + std::to_string(id)).c_str(), &sel)) {
            auto it = std::find(selectedIds_.begin(), selectedIds_.end(), id);
            if (sel && it == selectedIds_.end()) selectedIds_.push_back(id);
            else if (!sel && it != selectedIds_.end()) selectedIds_.erase(it);
        }
    }

    CardHit hit;
    hit.id = id;
    hit.card = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
    hit.star = { (LONG)(x + w - 110), (LONG)y, (LONG)(x + w - 80), (LONG)(y + 36) };
    hit.menu = { (LONG)(x + w - 34), (LONG)(y + h * 0.5f - 16), (LONG)(x + w - 2), (LONG)(y + h * 0.5f + 16) };
    cardHits_.push_back(hit);

    if (modalOpen()) {
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }

    ImGui::SetCursorScreenPos(ImVec2(x + 34, y));
    if (ImGui::InvisibleButton(("##openRow" + std::to_string(id)).c_str(), ImVec2(w - 150, h))) {
        openEmulator(id);
    }
    ImGui::SetCursorScreenPos(ImVec2(x + w - 110, y + 4));
    if (ImGui::InvisibleButton(("##starRow" + std::to_string(id)).c_str(), ImVec2(28, 28))) {
        handleCardAction(id, 3);
    }
    ImGui::SetCursorScreenPos(ImVec2(x + w - 34, y + h * 0.5f - 14));
    if (ImGui::InvisibleButton(("##menuRow" + std::to_string(id)).c_str(), ImVec2(30, 28))) {
        menuPhoneId_ = id;
        menuX_ = (int)(x + w - 150);
        menuY_ = (int)(y + h - 40);
        menuRequested_ = true;
        menuOpenedAt_ = std::chrono::steady_clock::now();
    }

    ImGui::EndChild();
    ImGui::PopID();
}

void MainWindow::drawSettings() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const UINT width = (UINT)std::max(1, (int)size.x);
    const UINT height = (UINT)std::max(1, (int)size.y);
    const float headerH = 96.0f, navW = 190.0f;
    // Header text sits deliberately low to balance the taller title area.
    const float headerTitleY = 34.0f;
    const float headerSubtitleY = 64.0f;
    state_.settings.settingsTab = std::min(2, std::max(0, state_.settings.settingsTab));

    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    ImGui::BeginChild("SettingsHeader", ImVec2(size.x, headerH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorScreenPos(ImVec2(8, 12));
    if (ui::Button("< Back", ImVec2(78, 36))) closeSettings();
    ImGui::SetItemTooltip("Back (phone keeps running)");
    ImGui::SetCursorScreenPos(ImVec2(96, headerTitleY));
    ImGui::TextColored(ui::TextMain, "Settings");
    ImGui::SetCursorScreenPos(ImVec2(96, headerSubtitleY));
    ImGui::TextDisabled("Tweak your phones, then go back");
    drawWindowControls();
    ImGui::EndChild();

    // Left-hand tab rail.
    ImGui::SetCursorScreenPos(ImVec2(0, headerH));
    ImGui::BeginChild("SettingsNav", ImVec2(navW, size.y - headerH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const char* tabLabels[] = { "General", "Instances", "Appearance" };
    for (int i = 0; i < 3; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(16, headerH + 20 + i * 52));
        bool on = state_.settings.settingsTab == i;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
        }
        if (ui::Button(tabLabels[i], ImVec2(158, 40))) {
            state_.settings.settingsTab = i;
            persist();
        }
        if (on) ImGui::PopStyleColor(2);
    }
    ImGui::SetCursorScreenPos(ImVec2(16, size.y - 46));
    ImGui::TextDisabled("X hides to tray");
    ImGui::EndChild();

    // Content pane.
    ImGui::SetCursorScreenPos(ImVec2(navW, headerH));
    ImGui::BeginChild("SettingsContent", ImVec2(size.x - navW, size.y - headerH),
        ImGuiChildFlags_None, ImGuiWindowFlags_None);
    float px = navW + 24.0f;
    float panelW = std::min(760.0f, size.x - navW - 48.0f);
    if (panelW < 280.0f) panelW = std::max(240.0f, size.x - navW - 24.0f);
    float y = headerH + 20.0f;

    if (state_.settings.settingsTab == 0) {
        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("PerformancePresets");
        ImGui::TextColored(ui::TextMain, "Performance preset");
        ImGui::TextDisabled("One click sets global defaults for new phones");
        {
            const char* names[] = { "Compact", "Balanced", "Performance" };
            const char* info[] = {
                "2 cores | 2 GB | 720x1280",
                "4 cores | 4 GB | 1080x1920",
                "8 cores | 8 GB | 1080x1920 @ 480 dpi"
            };
            float cw = (panelW - 32.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine();
                bool on = state_.settings.performancePreset == names[i];
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
                }
                if (ui::Button(names[i], ImVec2(cw, 36))) applyPerformancePreset(names[i]);
                if (on) ImGui::PopStyleColor(2);
            }
            ImGui::TextWrapped("%s  |  %s  |  %s", info[0], info[1], info[2]);
        }
        ui::EndCard();
        y += 124;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("GraphicsSettings");
        ImGui::TextColored(ui::TextMain, "FPS cap and graphics renderer");
        ImGui::TextDisabled("Capture rate and GPU backend apply on the next phone start");
        {
            const char* fpsLabels[] = { "30 FPS", "60 FPS", "120 FPS" };
            const int fpsValues[] = { 30, 60, 120 };
            float cw = (panelW - 32.0f) / 3.0f;
            for (int i = 0; i < 3; ++i) {
                if (i) ImGui::SameLine();
                bool on = state_.settings.fpsCap == fpsValues[i];
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
                }
                if (ui::Button(fpsLabels[i], ImVec2(cw, 34))) {
                    state_.settings.fpsCap = fpsValues[i];
                    persist();
                }
                if (on) ImGui::PopStyleColor(2);
            }
            ImGui::Spacing();
            const char* gpuLabels[] = { "Host (DirectX)", "Software" };
            float gw = (panelW - 24.0f) / 2.0f;
            for (int i = 0; i < 2; ++i) {
                if (i) ImGui::SameLine();
                bool on = state_.settings.gpuMode == i;
                if (on) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
                }
                if (ui::Button(gpuLabels[i], ImVec2(gw, 34))) {
                    state_.settings.gpuMode = i;
                    persist();
                }
                if (on) ImGui::PopStyleColor(2);
            }
        }
        ui::EndCard();
        y += 146;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("CloseAudioSettings");
        ImGui::TextColored(ui::TextMain, "Close behavior and audio");
        if (ui::Toggle("Minimize to tray on close", &state_.settings.minimizeToTray)) persist();
        if (ui::Toggle("Mute emulator audio when parked", &state_.settings.muteParkedAudio)) {
            persist();
            applyParkedAudio();
        }
        ui::EndCard();
        y += 112;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("SettingFps");
        ImGui::TextDisabled("Display the live capture rate in the status bar");
        if (ui::Toggle("Show FPS counter", &state_.settings.saveFps)) persist();
        ui::EndCard();
        y += 84;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("SettingAfk");
        ImGui::TextDisabled("Harmless DPAD keys keep games from logging out");
        if (ui::Toggle("Anti-AFK keys", &state_.settings.antiAfk)) persist();
        ui::EndCard();
        y += 84;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("SettingAwake");
        ImGui::TextDisabled("Wake parked screens so they never go idle");
        if (ui::Toggle("Keep phones on", &state_.settings.keepAwake)) {
            if (state_.settings.keepAwake) lastAwakeAt_ = createdAt_;
            persist();
        }
        ui::EndCard();
        y += 84;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("SettingResources");
        ImGui::TextDisabled("Show live CPU and RAM usage on phone cards");
        if (ui::Toggle("Resource badges", &state_.settings.showResources)) persist();
        ui::EndCard();
        y += 84;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("SettingPreview");
        ImGui::TextDisabled("Still snapshot interval for parked phones");
        ImGui::TextUnformatted("Preview refresh");
        ImGui::SameLine(px + panelW - 196);
        if (ui::Button("-", ImVec2(36, 34))) {
            if (state_.settings.previewMinutes > 1) --state_.settings.previewMinutes;
            persist();
        }
        ImGui::SameLine();
        char interval[32];
        snprintf(interval, sizeof(interval), "%d min", state_.settings.previewMinutes);
        ImGui::TextUnformatted(interval);
        ImGui::SameLine(px + panelW - 52);
        if (ui::Button("+", ImVec2(36, 34))) {
            if (state_.settings.previewMinutes < 30) ++state_.settings.previewMinutes;
            persist();
        }
        ui::EndCard();
        y += 84;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("Updates");
        ImGui::TextColored(ui::TextMain, "Updates");
        ImGui::TextDisabled("Current version: %s", kAppVersion);
        if (ui::Button(updateCheckRunning_ ? "Checking..." : "Check for updates",
                       ImVec2(190, 34))) startUpdateCheck(true);
        ui::EndCard();

    } else if (state_.settings.settingsTab == 1) {
        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("InstanceActions");
        ImGui::TextColored(ui::TextMain, "Instance actions");
        ImGui::TextDisabled("These apply to every phone, including parked ones");
        float bw = (panelW - 24) / 3;
        if (ui::ButtonAccent("Refresh previews", ImVec2(bw, 40))) refreshPreviews(true);
        ImGui::SameLine();
        if (ui::Button("Stop all phones", ImVec2(bw, 40))) closeAllPhones();
        ImGui::SameLine();
        if (ui::Button("Exit all", ImVec2(bw, 40))) exitEverything();
        ui::EndCard();
        y += 120;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("InstanceView");
        ImGui::TextColored(ui::TextMain, "Manager view");
        ImGui::TextDisabled("Current: %s, filter %d, %d phone(s)",
            state_.settings.listView ? "List" : "Grid",
            state_.settings.statusFilter, (int)state_.phones.size());
        if (ui::Button("Clear search and filters", ImVec2(210, 34))) {
            searchBuffer_[0] = 0;
            state_.settings.searchText.clear();
            state_.settings.statusFilter = 0;
            persist();
        }
        ui::EndCard();
        y += 120;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("PhoneOverrides");
        ImGui::TextColored(ui::TextMain, "Per-phone overrides");
        if (state_.phones.empty()) {
            ImGui::TextDisabled("Add a phone to set per-instance hardware.");
        } else {
            std::vector<std::string> labels;
            for (const auto& p : state_.phones) {
                labels.push_back(p.name + " (emulator-" + std::to_string(p.port) + ")");
            }
            int selected = -1;
            for (int i = 0; i < (int)state_.phones.size(); ++i) {
                if (state_.phones[i].id == instanceEditId_) { selected = i; break; }
            }
            if (selected < 0) {
                selected = 0;
                instanceEditId_ = state_.phones.front().id;
            }
            std::vector<const char*> items;
            for (const auto& label : labels) items.push_back(label.c_str());
            ImGui::SetNextItemWidth(panelW - 28);
            if (ImGui::Combo("##overridePhone", &selected, items.data(), (int)items.size())) {
                instanceEditId_ = state_.phones[selected].id;
            }

            PhoneProfile* p = findProfile(instanceEditId_);
            if (p) {
                ImGui::TextDisabled("Screen orientation (applies on next boot)");
                float half = (panelW - 32.0f) / 2.0f;
                if (ui::Button("Portrait 1080x1920", ImVec2(half, 34))) {
                    p->screenWidth = 1080; p->screenHeight = 1920;
                    p->templateName = "Custom";
                    persist();
                }
                ImGui::SameLine();
                if (ui::Button("Landscape 1920x1080", ImVec2(half, 34))) {
                    p->screenWidth = 1920; p->screenHeight = 1080;
                    p->templateName = "Custom";
                    persist();
                }

                const auto& cfg = ConfigManager::instance().config();
                const int cores = p->cpuCores > 0 ? p->cpuCores : cfg.cpuCores;
                const int ram = p->ramMB > 0 ? p->ramMB : cfg.ramMB;
                const int dpi = p->dpi > 0 ? p->dpi : cfg.dpi;
                ImGui::TextDisabled("Current: %d cores, %d MB, %dx%d @ %d dpi",
                    cores, ram, p->screenWidth > 0 ? p->screenWidth : cfg.screenWidth,
                    p->screenHeight > 0 ? p->screenHeight : cfg.screenHeight, dpi);
                if (ui::Button("Use global defaults", ImVec2(190, 32))) {
                    p->cpuCores = 0; p->ramMB = 0; p->screenWidth = 0;
                    p->screenHeight = 0; p->dpi = 0; p->templateName.clear();
                    persist();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Stop and reopen the phone to apply");
            }
        }
        ui::EndCard();

    } else {
        bool dirty = false;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("UiScale");
        ImGui::TextColored(ui::TextMain, "UI scale");
        ImGui::TextDisabled("Great for 1440p and 4K displays. Applies when you release the slider.");
        // Do not rescale while the slider is active. Changing ImGui's sizes
        // mid-drag moves the slider out from under the pointer and makes it
        // impossible to slide back down.
        ImGui::SetNextItemWidth(panelW - 116.0f);
        ImGui::SliderInt("##uiScale", &state_.settings.uiScalePercent, 100, 150, "%d%%");
        const bool uiScaleSliderReleased = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ui::Button("Reset", ImVec2(80.0f, 0.0f))) {
            state_.settings.uiScalePercent = 100;
            applyThemeFromSettings();
            dirty = true;
        }
        if (uiScaleSliderReleased) {
            applyThemeFromSettings();
            dirty = true;
        }
        ImGui::TextDisabled("If the UI ever gets too large, press Ctrl+0.");
        ui::EndCard();
        y += 126;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("CustomColors");
        ImGui::TextColored(ui::TextMain, "Colors");
        ImGui::TextDisabled("Your custom palette is the default. Fine-tune any colour here.");
        if (ui::Toggle("Use custom palette", &state_.settings.useCustomColors)) {
            applyThemeFromSettings();
            dirty = true;
        }
        if (ui::Button("Reset palette to defaults", ImVec2(190.0f, 28.0f))) {
            seedPaletteDefaults(state_.settings);
            state_.settings.useCustomColors = true;
            applyThemeFromSettings();
            dirty = true;
        }

        auto& s = state_.settings;
        const float colorW = (panelW - 36.0f) / 2.0f;
        auto pickColor = [&](const char* label, unsigned int& value) {
            ImGui::SetNextItemWidth(colorW);
            if (colorEditPacked(label, value)) {
                // Editing any colour turns the custom palette on immediately.
                s.useCustomColors = true;
                applyThemeFromSettings();
                dirty = true;
            }
        };
        pickColor("Accent", s.accentColor);
        ImGui::SameLine();
        pickColor("Window", s.windowBgColor);
        pickColor("Title bar", s.titleBarColor);
        ImGui::SameLine();
        pickColor("Card panel", s.panelColor);
        pickColor("Nav bar", s.navBarColor);
        ImGui::SameLine();
        pickColor("Buttons", s.buttonColor);
        pickColor("Border", s.borderColor);
        ImGui::SameLine();
        pickColor("Text", s.textColor);
        pickColor("Muted text", s.textMutedColor);
        ImGui::SameLine();
        pickColor("Success", s.successColor);
        pickColor("Danger", s.dangerColor);
        ui::EndCard();
        y += 272;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("ThemeFont");
        ImGui::TextColored(ui::TextMain, "Font");
        const char* families[] = { "Calibri", "Segoe UI" };
        float bw = (panelW - 24) / 2;
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine();
            bool on = state_.settings.fontFamily == i;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
            }
            if (ui::Button(families[i], ImVec2(bw, 32))) {
                state_.settings.fontFamily = i;
                fontRebuildPending_ = true;
                dirty = true;
            }
            if (on) ImGui::PopStyleColor(2);
        }
        ImGui::TextDisabled("Font size");
        ImGui::SameLine(px + panelW - 196);
        if (ui::Button("-", ImVec2(36, 30)) && state_.settings.fontSize > 12) {
            --state_.settings.fontSize;
            fontRebuildPending_ = true;
            dirty = true;
        }
        ImGui::SameLine();
        char sizeText[32];
        snprintf(sizeText, sizeof(sizeText), "%d px", state_.settings.fontSize);
        ImGui::TextUnformatted(sizeText);
        ImGui::SameLine(px + panelW - 52);
        if (ui::Button("+", ImVec2(36, 30)) && state_.settings.fontSize < 22) {
            ++state_.settings.fontSize;
            fontRebuildPending_ = true;
            dirty = true;
        }
        ui::EndCard();
        y += 134;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("RenderQuality");
        ImGui::TextColored(ui::TextMain, "Render quality");
        ImGui::TextDisabled("DX11 post-processing over the custom UI");
        const char* modes[] = { "Standard", "HD", "Ultra HD" };
        bw = (panelW - 24) / 3;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            bool on = state_.settings.shaderMode == i;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
            }
            if (ui::Button(modes[i], ImVec2(bw, 38))) {
                state_.settings.shaderMode = i;
                dirty = true;
            }
            if (on) ImGui::PopStyleColor(2);
        }
        ImGui::TextDisabled("%s", state_.settings.shaderMode == 0
            ? "No shader pass." : (state_.settings.shaderMode == 1
                ? "HD shader: sharpen, tune, soften edges."
                : "Ultra HD: 4x MSAA plus the HD shader."));
        ui::EndCard();
        y += 142;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("ShaderTuning");
        ImGui::TextColored(ui::TextMain, "HD shader tuning");
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderInt("Sharpness", &state_.settings.sharpness, 0, 100, "%d")) {
            updatePostConstants(width, height);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderInt("Contrast", &state_.settings.contrast, 0, 100, "%d")) {
            updatePostConstants(width, height);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderInt("Saturation", &state_.settings.saturation, 0, 100, "%d")) {
            updatePostConstants(width, height);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderInt("Glow", &state_.settings.glow, 0, 100, "%d")) {
            updatePostConstants(width, height);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderInt("Vignette", &state_.settings.vignette, 0, 100, "%d")) {
            updatePostConstants(width, height);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ui::EndCard();
        y += 188;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("WindowShape");
        ImGui::TextColored(ui::TextMain, "Window shape");
        if (ui::Toggle("Rounded corners", &state_.settings.roundedCorners)) {
            applyWindowCorners();
            dirty = true;
        }
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderInt("Corner radius", &state_.settings.cornerRadius, 0, 48, "%d px")) {
            applyWindowCorners();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        float roundness = (float)state_.settings.cornerScale10 / 10.0f;
        ImGui::SetNextItemWidth(panelW - 28);
        if (ImGui::SliderFloat("UI roundness", &roundness, 0.0f, 2.0f, "%.1f")) {
            state_.settings.cornerScale10 = (int)std::lround(roundness * 10.0f);
            applyThemeFromSettings();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
        ui::EndCard();
        y += 156;

        ImGui::SetCursorScreenPos(ImVec2(px, y));
        ui::BeginCard("AppearanceReset");
        if (ui::Button("Reset appearance to defaults", ImVec2(panelW - 28, 36))) {
            seedPaletteDefaults(state_.settings);
            state_.settings.useCustomColors = true;
            state_.settings.uiScalePercent = 100;
            state_.settings.fontSize = 15;
            state_.settings.fontFamily = 0;
            state_.settings.cornerScale10 = 10;
            state_.settings.roundedCorners = true;
            state_.settings.cornerRadius = 18;
            state_.settings.shaderMode = 1;
            state_.settings.sharpness = 30;
            state_.settings.contrast = 12;
            state_.settings.saturation = 10;
            state_.settings.vignette = 10;
            state_.settings.glow = 0;
            applyThemeFromSettings();
            applyWindowCorners();
            fontRebuildPending_ = true;
            dirty = true;
        }
        ui::EndCard();

        if (dirty) persist();
    }

    ImGui::SetCursorScreenPos(ImVec2(px, size.y - 42));
    ImGui::TextDisabled(settingsReturnMode_ == UiMode::Emulator
        ? "Back arrow or Esc returns to the phone" : "Back arrow or Esc returns to the manager");
    ImGui::EndChild();
}

void MainWindow::drawEmulator() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float toolbarH = 48.0f, statusH = 24.0f;
    phoneHovered_ = false;

    // Navigation back: leaves the phone running and returns to the manager.
    ImGui::SetCursorScreenPos(ImVec2(8, 8));
    if (ui::Button("< Manager", ImVec2(118, 34))) leaveEmulator();
    ImGui::SetItemTooltip("Back to manager (phone keeps running)");
    drawWindowControls();

    bool loading = !vm_ || vm_->state() != VmState::Running;
    if (loading) {
        // This bar is driven by real backend boot milestones, not by a timer.
        // -1 means the backend cannot report progress, so show an honest
        // indeterminate bar instead of animating a fake percentage.
        int pct = vm_ ? vm_->bootPercent() : -1;
        float fraction = pct >= 0
            ? (std::min)(1.0f, (std::max)(0.01f, (float)pct / 100.0f))
            : -1.0f;
        char overlay[32];
        if (pct >= 0) snprintf(overlay, sizeof(overlay), "%d%%", pct);
        else snprintf(overlay, sizeof(overlay), "Working");

        ImGui::SetCursorScreenPos(ImVec2(size.x * 0.5f - 160, size.y * 0.5f - 58));
        ImGui::TextColored(ui::TextMain, "Starting Android Emulator");
        ImGui::SetCursorScreenPos(ImVec2(size.x * 0.5f - 130, size.y * 0.5f - 14));
        ImGui::SetNextItemWidth(260);
        ImGui::ProgressBar(fraction, ImVec2(260, 12), overlay);
        std::string status = vm_ ? vm_->statusMessage() : "Preparing phone";
        if (vm_ && vm_->state() == VmState::Error && !vm_->lastError().empty())
            status = vm_->lastError();
        if (status.empty()) status = "Starting phone";
        ImGui::SetCursorScreenPos(ImVec2(size.x * 0.5f - 170, size.y * 0.5f + 12));
        ImGui::TextDisabled("%s", status.c_str());
        phoneRect_ = {};
    } else {
        vm_->readFrame([&](const uint8_t* fb, int sw, int sh) {
            if (!fb || sw <= 0 || sh <= 0) return;
            updatePhoneTexture(currentId_ ? currentId_ : -1, fb, sw, sh);
            auto tit = textures_.find(currentId_ ? currentId_ : -1);
            if (tit == textures_.end() || !tit->second || !tit->second->view) return;

            ImVec2 avail(size.x, std::max(1.0f, size.y - toolbarH - statusH - 8));
            float aspect = (float)sw / (float)sh;
            float dw = avail.x, dh = avail.y;
            if (avail.x / avail.y > aspect) dw = avail.y * aspect;
            else dh = avail.x / aspect;
            ImVec2 pos((avail.x - dw) * 0.5f, 8.0f + (avail.y - dh) * 0.5f);
            ImGui::SetCursorScreenPos(pos);
            ImGui::Image(tit->second->view, ImVec2(dw, dh));
            phoneRect_ = { (LONG)pos.x, (LONG)pos.y, (LONG)(pos.x + dw), (LONG)(pos.y + dh) };

            // Invisible button gives the frame real hover/drag state; Image itself
            // is draw-only and never becomes the active item.
            ImGui::SetCursorScreenPos(pos);
            ImGui::InvisibleButton("##phoneTouch", ImVec2(dw, dh));
            bool itemActive = ImGui::IsItemActive();
            bool itemActivated = ImGui::IsItemActivated();
            phoneHovered_ = ImGui::IsItemHovered() && !modalOpen();

            // Drive touches from ImGui's button state. This avoids the mismatch
            // between ImGui's hover hit-test and raw Win32 mouse messages, and
            // keeps taps/swipes scoped to the phone image.
            if (vm_ && input_ && !modalOpen()) {
                ImVec2 mouse = ImGui::GetIO().MousePos;
                auto inside = [&]() {
                    return mouse.x >= pos.x && mouse.x < pos.x + dw &&
                           mouse.y >= pos.y && mouse.y < pos.y + dh;
                };
                if (itemActivated && inside()) {
                    int dx = (int)((mouse.x - pos.x) * vm_->screenWidth() / (dw > 0 ? dw : 1));
                    int dy = (int)((mouse.y - pos.y) * vm_->screenHeight() / (dh > 0 ? dh : 1));
                    input_->onMouseDown(dx, dy, 0);
                    touchActive_ = true;
                    lastInputAt_ = std::chrono::steady_clock::now();
                }
                if (touchActive_ && itemActive) {
                    int dx = (int)((mouse.x - pos.x) * vm_->screenWidth() / (dw > 0 ? dw : 1));
                    int dy = (int)((mouse.y - pos.y) * vm_->screenHeight() / (dh > 0 ? dh : 1));
                    input_->onMouseMove(dx, dy);
                    lastInputAt_ = std::chrono::steady_clock::now();
                }
                if (touchActive_ && !itemActive) {
                    int dx = (int)((mouse.x - pos.x) * vm_->screenWidth() / (dw > 0 ? dw : 1));
                    int dy = (int)((mouse.y - pos.y) * vm_->screenHeight() / (dh > 0 ? dh : 1));
                    input_->onMouseUp(dx, dy, 0);
                    touchActive_ = false;
                    lastInputAt_ = std::chrono::steady_clock::now();
                }
            } else {
                touchActive_ = false;
            }
        });
    }

    // Toolbar.
    ImGui::SetCursorScreenPos(ImVec2(0, size.y - toolbarH - statusH));
    ImGui::BeginChild("Toolbar", ImVec2(size.x, toolbarH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    float bw = (size.x - 16) / 6;
    const char* labels[] = { "Back", "Home", "Recent", "Full", "Shot", "Settings" };
    const char* tips[] = { "Android Back", "Android Home", "Android Recent",
                           "Toggle fullscreen", "Save screenshot", "Settings" };
    const float toolbarY = size.y - toolbarH - statusH;
    for (int i = 0; i < 6; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(8 + i * bw, toolbarY + 6));
        if (ui::Button(labels[i], ImVec2(bw - 8, 34))) {
            switch (i) {
                case 0: if (vm_) vm_->sendKey(4, true); break;
                case 1: if (vm_) vm_->sendKey(3, true); break;
                case 2: if (vm_) vm_->sendKey(187, true); break;
                case 3: toggleFullscreen(); break;
                case 4: {
                    if (vm_) {
                        std::vector<uint8_t> shot;
                        int sw = 0, sh = 0;
                        vm_->readFrame([&](const uint8_t* fb, int w, int h) {
                            sw = w; sh = h;
                            if (fb && w > 0 && h > 0) shot.assign(fb, fb + (size_t)w * h * 4);
                        });
                        std::string out = AppSetup::screenshotPath();
                        if (!shot.empty() && ppm_write(out.c_str(), sw, sh, shot.data()) == 0) {
                            showMessageModal_ = true;
                            modalTitle_ = "Screenshot";
                            modalMessage_ = "Screenshot saved to " + out;
                        }
                    }
                    break;
                }
                case 5: openSettings(); break;
            }
        }
        ImGui::SetItemTooltip("%s", tips[i]);
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(0, size.y - statusH));
    ImGui::BeginChild("StatusBar", ImVec2(size.x, statusH), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    char status[192];
    if (state_.settings.saveFps) {
        snprintf(status, sizeof(status), "FPS: %.0f    |    Mouse: tap / drag to swipe    |    F11: Fullscreen", fps_);
    } else {
        snprintf(status, sizeof(status), "Mouse: tap / drag to swipe    |    F11: Fullscreen");
    }
    ImGui::SetCursorScreenPos(ImVec2(10, size.y - statusH + 4));
    ImGui::TextDisabled("%s", status);
    ImGui::EndChild();
}

void MainWindow::drawWindowControls() {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ImGui::PushID("BlueDroidWindowControls");

    // Fallback header drag for any case where Win32 does not translate the hit
    // to HTCAPTION. WM_NCHITTEST normally owns top-bar dragging and edge resize.
    if (!modalOpen() && !draggingWindow_) {
        float dx = 0.0f, dy = 6.0f, dw = 0.0f, dh = 46.0f;
        if (mode_ == UiMode::Manager) {
            dx = 240.0f;
            dw = size.x - 736.0f - dx;
            dh = 52.0f;
        } else if (mode_ == UiMode::Settings) {
            dx = 320.0f;
            dw = size.x - 128.0f - dx;
            dh = 60.0f;
        } else {
            dx = 140.0f;
            dw = size.x - 128.0f - dx;
            dh = 42.0f;
        }
        if (dw > 40.0f) {
            ImGui::SetCursorScreenPos(ImVec2(dx, dy));
            ImGui::InvisibleButton("##windowDrag", ImVec2(dw, dh));
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                if (ImGui::IsItemActivated()) {
                    POINT cursor{};
                    RECT windowRect{};
                    if (GetCursorPos(&cursor) && GetWindowRect(hwnd_, &windowRect)) {
                        draggingWindow_ = true;
                        dragCursorOrigin_ = cursor;
                        dragWindowOrigin_ = { windowRect.left, windowRect.top };
                        SetCapture(hwnd_);
                    }
                }
            }
        }
    }

    const float buttonW = 34.0f, buttonH = 28.0f, gap = 4.0f;
    float x = size.x - 10.0f - (3.0f * buttonW + 2.0f * gap);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto drawControl = [&](const char* id, const char* tip, int kind) -> bool {
        const ImVec2 p(x, 8.0f);
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(buttonW, buttonH));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        ImU32 bg = ui::U32(ui::Widget);
        if (hovered) bg = kind == 2
            ? ImGui::GetColorU32(ImVec4(0.78f, 0.24f, 0.24f, 1.0f))
            : ui::U32(ui::WidgetHi);
        if (active) bg = ui::U32(ui::Accent);
        drawList->AddRectFilled(p, ImVec2(p.x + buttonW, p.y + buttonH), bg, 8.0f);

        const ImU32 fg = ui::U32(ui::TextMain);
        if (kind == 0) {
            drawList->AddLine(ImVec2(p.x + 10, p.y + 17), ImVec2(p.x + 24, p.y + 17),
                fg, 2.0f);
        } else if (kind == 1) {
            if (IsZoomed(hwnd_)) {
                drawList->AddRect(ImVec2(p.x + 9, p.y + 12), ImVec2(p.x + 22, p.y + 21),
                    fg, 1.0f, 0, 1.5f);
                drawList->AddRect(ImVec2(p.x + 12, p.y + 8), ImVec2(p.x + 25, p.y + 17),
                    fg, 1.0f, 0, 1.5f);
            } else {
                drawList->AddRect(ImVec2(p.x + 10, p.y + 9), ImVec2(p.x + 24, p.y + 19),
                    fg, 1.0f, 0, 1.5f);
            }
        } else {
            drawList->AddLine(ImVec2(p.x + 11, p.y + 10), ImVec2(p.x + 23, p.y + 18),
                fg, 2.0f);
            drawList->AddLine(ImVec2(p.x + 23, p.y + 10), ImVec2(p.x + 11, p.y + 18),
                fg, 2.0f);
        }
        if (hovered) ImGui::SetTooltip("%s", tip);
        x += buttonW + gap;
        return clicked;
    };

    if (drawControl("##winMin", "Minimize", 0)) ShowWindow(hwnd_, SW_MINIMIZE);
    if (drawControl("##winMax", IsZoomed(hwnd_) ? "Restore" : "Maximize", 1)) {
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        applyWindowCorners();
    }
    const char* closeTip = state_.settings.minimizeToTray
        ? "Hide to tray (phones keep running)" : "Exit all and quit";
    if (drawControl("##winClose", closeTip, 2)) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    ImGui::PopID();
}

void MainWindow::chooseApkFile() {
#ifdef _WIN32
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    const wchar_t filter[] = L"APK files\0*.apk\0All files\0*.*\0\0";
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"Select an APK install file";
    if (GetOpenFileNameW(&ofn)) openApkPicker(toUtf8(file));
#else
    // zenity on Linux, osascript on macOS. Both print the chosen path on stdout.
#ifdef __APPLE__
    const char* command = "osascript -e 'POSIX path of (choose file)'";
#else
    const char* command = "zenity --file-selection --file-filter='*.apk' 2>/dev/null";
#endif
    std::string output;
    int exitCode = -1;
    if (!emu::platform::runCommand(command, 0, true, &output, &exitCode) || exitCode != 0) {
        LOG_DEBUG("APK picker was cancelled or unavailable (exit %d)", exitCode);
        return;
    }
    auto firstNonSpace = output.find_first_not_of(" \t\r\n\"");
    if (firstNonSpace == std::string::npos) return;
    auto lastNonSpace = output.find_last_not_of(" \t\r\n\"");
    std::string path = output.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);
    if (!path.empty()) openApkPicker(path);
#endif
}

void MainWindow::openApkPicker(const std::string& path) {
    if (!isApkPath(path)) {
        showMessageModal_ = true;
        modalTitle_ = "Not an APK";
        modalMessage_ = "BlueDroid can install .apk files only: " + path;
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
        showMessageModal_ = true;
        modalTitle_ = "APK not found";
        modalMessage_ = path;
        return;
    }

    apkPath_ = path;
    apkName_ = fs::path(path).filename().string();
    apkSize_ = 0;
    if (!ec) apkSize_ = (size_t)fs::file_size(path, ec);

    apkPickerOpen_ = true;
    apkPickerOpenedAt_ = std::chrono::steady_clock::now();
    apkAllPhones_ = true;
    apkSelectedIds_.clear();
    apkSearchBuffer_[0] = 0;

    // The transfer card lives in the manager, so bring the manager forward.
    if (mode_ == UiMode::Emulator) leaveEmulator();
    if (mode_ == UiMode::Settings) closeSettings();

    LOG_INFO("Detected APK: %s (%s)", apkPath_.c_str(), formatBytes(apkSize_).c_str());
}

void MainWindow::startApkInstall() {
    std::vector<int> targets = apkAllPhones_ ? orderedProfileIds() : apkSelectedIds_;
    if (targets.empty()) {
        showMessageModal_ = true;
        modalTitle_ = "Install APK";
        modalMessage_ = "Select at least one phone first.";
        return;
    }
    if (apkPath_.empty()) return;

    auto job = std::make_shared<ApkInstallJob>();
    job->apkPath = apkPath_;
    job->apkName = apkName_;
    job->bytesTotal = apkSize_;
    if (job->bytesTotal == 0) {
        std::error_code ec;
        job->bytesTotal = (size_t)std::filesystem::file_size(apkPath_, ec);
    }

    for (int id : targets) {
        PhoneProfile* p = findProfile(id);
        auto dev = std::make_shared<ApkInstallDevice>();
        dev->phoneId = id;
        dev->name = p ? p->name : ("Phone " + std::to_string(id));
        dev->bytesTotal = job->bytesTotal;
        dev->stage = "Preparing phone";
        job->devices.push_back(dev);

        auto it = vms_.find(id);
        if (it == vms_.end()) {
            startPhone(id);
            it = vms_.find(id);
        }
        IVirtualMachine* raw = (it == vms_.end()) ? nullptr : it->second.get();

        apkThreads_.push_back(std::thread([job, dev, raw, path = apkPath_]() {
            auto set = [&](size_t done, size_t total, const std::string& stage,
                           bool finished, bool ok) {
                std::lock_guard<std::mutex> lk(job->mutex);
                dev->bytesDone = done;
                if (total) dev->bytesTotal = total;
                dev->stage = stage;
                dev->done = finished;
                dev->ok = ok;
                if (finished) {
                    LOG_INFO("APK target %s: %s (%zu/%zu bytes, ok=%d)",
                             dev->name.c_str(), stage.c_str(), done, total, (int)ok);
                }
            };

            auto* android = raw ? dynamic_cast<AndroidVirtualMachine*>(raw) : nullptr;
            if (!android) {
                set(0, job->bytesTotal, "No Android phone", true, false);
                return;
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
            while (!dev->cancel.load() && std::chrono::steady_clock::now() < deadline &&
                   android->state() != VmState::Running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (dev->cancel.load()) {
                set(0, job->bytesTotal, "Cancelled", true, false);
                return;
            }
            if (android->state() != VmState::Running) {
                set(0, job->bytesTotal, "Phone did not start", true, false);
                return;
            }

            bool online = android->isDeviceOnline();
            while (!dev->cancel.load() && !online &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                online = android->isDeviceOnline();
            }
            if (dev->cancel.load()) {
                set(0, job->bytesTotal, "Cancelled", true, false);
                return;
            }
            if (!online) {
                set(0, job->bytesTotal, "Phone offline", true, false);
                return;
            }

            bool ok = android->installApk(path,
                [job, dev](const ApkInstallProgress& p) {
                    std::lock_guard<std::mutex> lk(job->mutex);
                    dev->bytesDone = p.bytesDone;
                    dev->bytesTotal = p.bytesTotal;
                    dev->stage = p.stage;
                }, &dev->cancel);
            {
                std::lock_guard<std::mutex> lk(job->mutex);
                dev->done = true;
                dev->ok = ok;
            }
        }));
    }

    {
        std::lock_guard<std::mutex> lk(job->mutex);
        apkJobs_.push_back(job);
    }
    LOG_INFO("APK install started: %s on %d phone(s)", apkPath_.c_str(), (int)targets.size());
}

void MainWindow::updateApkJobs() {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& job : apkJobs_) {
        std::lock_guard<std::mutex> lk(job->mutex);
        if (job->finished || job->devices.empty()) continue;
        bool allDone = true;
        for (const auto& dev : job->devices) {
            if (!dev->done) { allDone = false; break; }
        }
        if (allDone) {
            job->finished = true;
            job->finishedAt = now;
            LOG_INFO("APK install finished: %s", job->apkName.c_str());
        }
    }
    apkJobs_.erase(std::remove_if(apkJobs_.begin(), apkJobs_.end(),
        [now](const std::shared_ptr<ApkInstallJob>& job) {
            std::lock_guard<std::mutex> lk(job->mutex);
            return job->finished && now - job->finishedAt > std::chrono::seconds(12);
        }), apkJobs_.end());
}

void MainWindow::cancelAllApkJobs() {
    for (const auto& job : apkJobs_) {
        std::lock_guard<std::mutex> lk(job->mutex);
        for (const auto& dev : job->devices) dev->cancel.store(true);
    }
    for (auto& t : apkThreads_) {
        if (t.joinable()) t.join();
    }
    apkThreads_.clear();
}

bool MainWindow::apkPanelVisible() const {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& job : apkJobs_) {
        std::lock_guard<std::mutex> lk(job->mutex);
        if (!job->finished) return true;
        if (now - job->finishedAt < std::chrono::seconds(10)) return true;
    }
    return false;
}

void MainWindow::drawApkPicker() {
    if (apkPickerOpen_ && !ImGui::IsPopupOpen("InstallApk")) ImGui::OpenPopup("InstallApk");
    PopupTheme theme(uiEntrance(apkPickerOpenedAt_), true);
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::BeginPopupModal("InstallApk", &apkPickerOpen_, flags))
        return;

    ImGui::TextColored(ui::TextMain, "Detected APK - %s", apkName_.c_str());
    ImGui::PushTextWrapPos(520);
    ImGui::TextDisabled("%s", apkPath_.c_str());
    ImGui::PopTextWrapPos();
    char sizeText[64];
    snprintf(sizeText, sizeof(sizeText), "Size: %s", formatBytes(apkSize_).c_str());
    ImGui::TextDisabled("%s", sizeText);

    ImGui::Separator();
    ImGui::TextUnformatted("Install on");
    float half = 250.0f;
    if (apkAllPhones_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
    }
    if (ui::Button("All phones", ImVec2(half, 32))) {
        apkAllPhones_ = true;
        apkSelectedIds_.clear();
    }
    if (apkAllPhones_) ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (!apkAllPhones_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ui::U32(ui::Accent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::U32(ui::Accent));
    }
    if (ui::Button("Specific phones", ImVec2(half, 32))) apkAllPhones_ = false;
    if (!apkAllPhones_) ImGui::PopStyleColor(2);

    ImGui::SetNextItemWidth(520);
    if (ImGui::InputText("##apkPhoneSearch", apkSearchBuffer_, sizeof(apkSearchBuffer_))) {
        // Filtering is immediate; nothing to persist for a one-shot picker.
    }
    ImGui::TextDisabled("Search phones by name, AVD, port, or template");

    std::string needle = apkSearchBuffer_;
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });

    ImGui::BeginChild("ApkPhoneList", ImVec2(520, 210), ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    int shown = 0;
    for (int id : orderedProfileIds()) {
        PhoneProfile* p = findProfile(id);
        if (!p) continue;
        std::string hay = p->name + " " + p->avd + " " + p->templateName + " " +
                          std::to_string(p->port);
        std::transform(hay.begin(), hay.end(), hay.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        if (!needle.empty() && hay.find(needle) == std::string::npos) continue;

        bool selected = apkAllPhones_ ||
            std::find(apkSelectedIds_.begin(), apkSelectedIds_.end(), id) != apkSelectedIds_.end();
        std::string cbId = "##apkSelect" + std::to_string(id);
        if (ImGui::Checkbox(cbId.c_str(), &selected)) {
            apkAllPhones_ = false;
            auto it = std::find(apkSelectedIds_.begin(), apkSelectedIds_.end(), id);
            if (selected && it == apkSelectedIds_.end()) apkSelectedIds_.push_back(id);
            else if (!selected && it != apkSelectedIds_.end()) apkSelectedIds_.erase(it);
        }
        ImGui::SameLine();
        const char* status = "Stopped";
        ImU32 dot = IM_COL32(120, 120, 135, 255);
        switch (phoneStatus(id)) {
            case PhoneStatus::Running: status = "Active"; dot = IM_COL32(52, 168, 83, 255); break;
            case PhoneStatus::Parked: status = "Parked"; dot = IM_COL32(78, 140, 240, 255); break;
            case PhoneStatus::Starting: status = "Starting"; dot = IM_COL32(96, 165, 250, 255); break;
            case PhoneStatus::Paused: status = "Paused"; dot = IM_COL32(230, 180, 60, 255); break;
            case PhoneStatus::Error: status = "Error"; dot = IM_COL32(230, 80, 80, 255); break;
            default: break;
        }
        ImGui::TextColored(hdr::V4(dot), "%s", status);
        ImGui::SameLine(110);
        ImGui::TextColored(ui::TextMain, "%s", p->name.c_str());
        ImGui::SameLine(280);
        ImGui::TextDisabled("port %d", p->port);
        ++shown;
    }
    if (!shown) ImGui::TextDisabled("No phones match this search");
    ImGui::EndChild();

    char summary[96];
    int count = apkAllPhones_ ? (int)state_.phones.size() : (int)apkSelectedIds_.size();
    snprintf(summary, sizeof(summary), "Ready to install on %d phone(s)", count);
    ImGui::TextDisabled("%s", summary);

    if (ui::ButtonAccent("Install APK", ImVec2(140, 36))) {
        startApkInstall();
        apkPickerOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::Button("Cancel", ImVec2(110, 36))) {
        apkPickerOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::drawApkProgress() {
    if (!apkPanelVisible()) return;
    ImGuiIO& io = ImGui::GetIO();
    const float w = std::min(430.0f, io.DisplaySize.x - 32.0f);
    if (w < 220.0f) return;
    const float h = 150.0f;
    const ImVec2 p0(io.DisplaySize.x - w - 16.0f, io.DisplaySize.y - h - 16.0f);
    const ImVec2 p1(p0.x + w, p0.y + h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ui::Card), 12.f);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ui::Accent), 12.f, 0, 1.f);

    // Always show the live transfer if one is running; a finished card that is
    // still fading out must not hide a new install.
    std::shared_ptr<ApkInstallJob> job;
    for (const auto& candidate : apkJobs_) {
        if (!job) { job = candidate; continue; }
        bool candidateFinished = false;
        bool currentFinished = false;
        {
            std::lock_guard<std::mutex> lk(candidate->mutex);
            candidateFinished = candidate->finished;
        }
        {
            std::lock_guard<std::mutex> lk(job->mutex);
            currentFinished = job->finished;
        }
        if (candidateFinished != currentFinished) {
            if (!candidateFinished) job = candidate;
        } else if (candidate->finishedAt > job->finishedAt) {
            job = candidate;
        }
    }
    if (!job) return;

    std::lock_guard<std::mutex> lk(job->mutex);
    size_t done = 0, total = 0;
    int finished = 0, okCount = 0, installing = 0, cancelled = 0;
    for (const auto& dev : job->devices) {
        size_t devTotal = dev->bytesTotal ? dev->bytesTotal : job->bytesTotal;
        done += dev->bytesDone;
        total += devTotal;
        if (dev->done) ++finished;
        if (dev->done && dev->ok) ++okCount;
        if (!dev->done && dev->stage == "Installing APK") ++installing;
        if (dev->stage == "Cancelled") ++cancelled;
    }

    float fraction = -1.0f;
    if (total) fraction = (float)done / (float)total;
    if (!job->finished && installing > 0) fraction = -1.0f;
    if (job->finished && finished > 0 && okCount == finished) fraction = 1.0f;

    char overlay[160];
    if (job->finished) {
        if (okCount == finished)
            snprintf(overlay, sizeof(overlay), "Installed on %d phone(s)", okCount);
        else
            snprintf(overlay, sizeof(overlay), "Finished - %d/%d installed", okCount, finished);
    } else if (cancelled) {
        snprintf(overlay, sizeof(overlay), "Cancelled");
    } else if (installing > 0) {
        snprintf(overlay, sizeof(overlay), "Installing on %d phone(s)", installing);
    } else if (total) {
        snprintf(overlay, sizeof(overlay), "Uploading %s / %s",
            formatBytes(done).c_str(), formatBytes(total).c_str());
    } else {
        snprintf(overlay, sizeof(overlay), "Preparing transfer");
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14, p0.y + 10));
    ImGui::TextColored(ui::TextMain, "APK install - %s", job->apkName.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p1.x - 78, p0.y + 6));
    if (!job->finished) {
        if (ui::Button("Cancel", ImVec2(64, 26))) {
            for (const auto& dev : job->devices) dev->cancel.store(true);
        }
    } else if (ui::Button("Close", ImVec2(64, 26))) {
        job->finishedAt = std::chrono::steady_clock::time_point{};
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14, p0.y + 46));
    ImGui::SetNextItemWidth(w - 28);
    ImGui::ProgressBar(fraction, ImVec2(w - 28, 14), overlay);

    int row = 0;
    for (const auto& dev : job->devices) {
        if (row >= 2) break;
        const float y = p0.y + 74 + row * 26;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 14, y));
        std::string label = dev->name + " - " + dev->stage;
        if (label.size() > 48) label.resize(48);
        ImVec4 rowColor = dev->done && !dev->ok
            ? hdr::V4(ui::U32(ui::Accent)) : ui::TextDim;
        ImGui::TextColored(rowColor, "%s", label.c_str());

        size_t dt = dev->bytesTotal ? dev->bytesTotal : job->bytesTotal;
        float df = dt ? (float)dev->bytesDone / (float)dt : -1.0f;
        if (!dev->done && dev->stage == "Installing APK") df = -1.0f;
        if (dev->done && dev->ok) df = 1.0f;
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 142, y + 1));
        ImGui::SetNextItemWidth(128);
        ImGui::ProgressBar(df, ImVec2(128, 9), "");
        ++row;
    }
    if (job->devices.size() > 2) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 14, p0.y + 126));
        ImGui::TextDisabled("+ %d more phone(s)", (int)job->devices.size() - 2);
    }
}

static const char* kPhoneTemplates[] = {
    "Global", "Balanced", "Compact", "Tablet", "Performance"
};

void MainWindow::drawDialogs() {
    drawApkPicker();

    // Card menu.
    if (menuRequested_) {
        ImGui::OpenPopup("CardMenu");
        menuRequested_ = false;
    }
    {
        PopupTheme theme(uiEntrance(menuOpenedAt_), false, ImVec2((float)menuX_, (float)menuY_));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::BeginPopup("CardMenu", flags)) {
            PhoneProfile* p = findProfile(menuPhoneId_);
            if (!p) ImGui::CloseCurrentPopup();
            else {
                bool active = vms_.count(menuPhoneId_) > 0;
                bool activeHere = (mode_ == UiMode::Emulator && currentId_ == menuPhoneId_);
                if (ImGui::MenuItem(activeHere ? "Return to phone" : "Open")) {
                    handleCardAction(menuPhoneId_, 1);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Rename...")) { handleCardAction(menuPhoneId_, 2); ImGui::CloseCurrentPopup(); }
                if (ImGui::MenuItem(p->starred ? "Remove star" : "Star phone")) {
                    handleCardAction(menuPhoneId_, 3); ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem(p->paused ? "Resume" : "Pause")) {
                    handleCardAction(menuPhoneId_, 4); ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Backup data")) { handleCardAction(menuPhoneId_, 5); ImGui::CloseCurrentPopup(); }
                if (ImGui::MenuItem("Stop phone", nullptr, false, active)) {
                    handleCardAction(menuPhoneId_, 6); ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete phone")) { handleCardAction(menuPhoneId_, 7); ImGui::CloseCurrentPopup(); }
            }
            ImGui::EndPopup();
        } else if (menuPhoneId_ != 0) {
            menuPhoneId_ = 0;
        }
    }

    // Name editor.
    if (naming_ && !ImGui::IsPopupOpen("PhoneName")) ImGui::OpenPopup("PhoneName");
    {
        PopupTheme theme(1.0f, true);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::BeginPopupModal("PhoneName", nullptr, flags)) {
        ImGui::TextUnformatted(namingId_ == 0 ? "New emulator" : "Rename phone");
        ImGui::SetNextItemWidth(360);
        if (nameFocus_) { ImGui::SetKeyboardFocusHere(); nameFocus_ = false; }
        bool enter = ImGui::InputText("##phoneName", nameBuffer_, sizeof(nameBuffer_),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (namingId_ == 0) {
            ImGui::TextDisabled("Create from template");
            if (templateIndex_ < 0 || templateIndex_ >= (int)IM_ARRAYSIZE(kPhoneTemplates))
                templateIndex_ = 0;
            ImGui::SetNextItemWidth(360);
            ImGui::Combo("##phoneTemplate", &templateIndex_, kPhoneTemplates,
                (int)IM_ARRAYSIZE(kPhoneTemplates));
            ImGui::TextDisabled("Global uses emulator.ini; the others store per-phone hardware");
        }
        ImGui::TextDisabled("Enter = save    Esc = cancel");
        if (enter || ui::Button("Save", ImVec2(110, 0))) {
            std::string name(nameBuffer_);
            while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
            if (!name.empty()) {
                if (namingId_ == 0) addPhone(name, kPhoneTemplates[templateIndex_]);
                else if (auto* p = findProfile(namingId_)) { p->name = name; persist(); }
            }
            naming_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button("Cancel", ImVec2(110, 0))) {
            naming_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (!naming_) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // Delete confirmation (single phone or a multi-select batch).
    if (confirmDelete_ && !ImGui::IsPopupOpen("DeletePhone")) ImGui::OpenPopup("DeletePhone");
    {
        PopupTheme theme(1.0f, true);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::BeginPopupModal("DeletePhone", nullptr, flags)) {
        if (deleteIds_.empty()) ImGui::CloseCurrentPopup();
        else {
            if (deleteIds_.size() == 1) {
                PhoneProfile* p = findProfile(deleteIds_.front());
                if (p) ImGui::Text("Delete \"%s\" and its phone data?", p->name.c_str());
                else ImGui::TextUnformatted("Delete this phone and its data?");
            } else {
                ImGui::Text("Delete %d phones and their data?", (int)deleteIds_.size());
            }
            if (ui::Button("Delete", ImVec2(110, 0))) {
                deletePhones(deleteIds_);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ui::Button("Cancel", ImVec2(110, 0))) {
                deleteIds_.clear();
                confirmDelete_ = false;
                ImGui::CloseCurrentPopup();
            }
        }
            ImGui::EndPopup();
        }
    }

    // First-run shortcut into the new gamer settings.
    if (showFirstRunPrompt_ && !ImGui::IsPopupOpen("FirstRunSettings")) ImGui::OpenPopup("FirstRunSettings");
    {
        PopupTheme theme(1.0f, true);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::BeginPopupModal("FirstRunSettings", nullptr, flags)) {
            ImGui::TextColored(ui::TextMain, "Welcome to BlueDroid");
            ImGui::TextWrapped("Setup is ready. Tune performance, theme, FPS cap, GPU mode and phone orientation in one place.");
            ImGui::Spacing();
            if (ui::Button("Open settings", ImVec2(140, 0))) {
                showFirstRunPrompt_ = false;
                state_.settings.firstRunSettingsSeen = true;
                persist();
                openSettings();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ui::Button("Got it", ImVec2(100, 0))) {
                showFirstRunPrompt_ = false;
                state_.settings.firstRunSettingsSeen = true;
                persist();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Generic message.
    if (showMessageModal_ && !ImGui::IsPopupOpen("BlueDroidMessage")) ImGui::OpenPopup("BlueDroidMessage");
    {
        PopupTheme theme(1.0f, true);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::BeginPopupModal("BlueDroidMessage", nullptr, flags)) {
        ImGui::TextUnformatted(modalTitle_.c_str());
        ImGui::TextUnformatted(modalMessage_.c_str());
        if (ui::Button("OK", ImVec2(110, 0))) {
            showMessageModal_ = false;
            ImGui::CloseCurrentPopup();
        }
            ImGui::EndPopup();
        }
    }
}

void MainWindow::backupPhones(const std::vector<int>& ids) {
    if (ids.empty()) {
        showMessageModal_ = true;
        modalTitle_ = "Backup";
        modalMessage_ = "Select at least one phone first.";
        return;
    }

    namespace fs = std::filesystem;
    char timeBuf[64];
    std::time_t t = std::time(nullptr);
    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&t));

    int ok = 0, failed = 0, files = 0;
    std::string firstDir;
    std::string errorText;
    for (int id : ids) {
        PhoneProfile* p = findProfile(id);
        if (!p) continue;
        fs::path src = fs::path(AppSetup::avdDir()) / (p->avd + ".avd");
        fs::path dest = fs::path(AppSetup::backupsDir()) / (p->avd + "_" + timeBuf);
        try {
            fs::create_directories(dest);
            // Include the qcow2 overlay: that is where the Android data actually
            // changes after the first boot. The base image alone is stale.
            for (const char* name : { "config.ini", "userdata-qemu.img",
                                      "userdata-qemu.img.qcow2", "sdcard.img" }) {
                fs::path file = src / name;
                if (fs::exists(file)) {
                    fs::copy_file(file, dest / name, fs::copy_options::overwrite_existing);
                    ++files;
                }
            }
            // Save this phone's APK library with its data backup.
            fs::path apkSrc = fs::path(AppSetup::phoneDataDir(p->port)) / "apks";
            if (fs::exists(apkSrc)) {
                fs::copy(apkSrc, dest / "apks",
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                for (fs::recursive_directory_iterator it(apkSrc), end; it != end; ++it) {
                    if (it->is_regular_file()) ++files;
                }
            }
            if (firstDir.empty()) firstDir = dest.string();
            ++ok;
        } catch (const std::exception& e) {
            ++failed;
            if (errorText.empty()) errorText = e.what();
        }
    }

    showMessageModal_ = true;
    if (ok && failed == 0) {
        modalTitle_ = "Backup";
        modalMessage_ = "Backed up " + std::to_string(ok) + " phone(s), " +
            std::to_string(files) + " file(s) to " + firstDir;
    } else if (ok) {
        modalTitle_ = "Backup partially complete";
        modalMessage_ = "Backed up " + std::to_string(ok) + " phone(s); " +
            std::to_string(failed) + " failed. " + errorText;
    } else {
        modalTitle_ = "Backup failed";
        modalMessage_ = errorText.empty() ? "No phone data was found." : errorText;
    }
}

void MainWindow::backupPhone(int id) {
    backupPhones({ id });
}

void MainWindow::handleCardAction(int id, int action) {
    PhoneProfile* p = findProfile(id);
    if (!p) return;
    switch (action) {
        case 1: openEmulator(id); break;
        case 2:
            naming_ = true;
            nameFocus_ = true;
            namingId_ = id;
            snprintf(nameBuffer_, sizeof(nameBuffer_), "%s", p->name.c_str());
            break;
        case 3:
            p->starred = !p->starred;
            persist();
            break;
        case 4: {
            p->paused = !p->paused;
            auto it = vms_.find(id);
            if (it != vms_.end()) {
                if (p->paused) it->second->pause();
                else it->second->resume();
            }
            persist();
            break;
        }
        case 5: backupPhone(id); break;
        case 6:
            closePhone(id);
            resourceSamples_.erase(id);
            selectedIds_.erase(std::remove(selectedIds_.begin(), selectedIds_.end(), id),
                               selectedIds_.end());
            break;
        case 7:
            confirmDelete_ = true;
            deleteId_ = id;
            deleteIds_ = { id };
            break;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

LRESULT MainWindow::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCHITTEST: {
            if (fullscreen_) return HTCLIENT;

            const int px = (short)LOWORD(lParam);
            const int py = (short)HIWORD(lParam);
            RECT rc{};
            if (!GetWindowRect(hwnd_, &rc)) return HTCLIENT;
            const int x = px - rc.left;
            const int y = py - rc.top;
            const bool maximized = IsZoomed(hwnd_) != FALSE;

            // Edge resize without a native frame. Maximized windows have no
            // resizable edges, so the header drag can take over there.
            if (!maximized) {
                constexpr int edge = 8;
                const bool left = x < edge;
                const bool right = x >= (rc.right - rc.left) - edge;
                const bool top = y < edge;
                const bool bottom = y >= (rc.bottom - rc.top) - edge;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }

            // Native caption behavior for the custom header: Windows moves the
            // whole window (and restores a maximized one) without ImGui seeing
            // the click, so header buttons still work normally.
            if (!modalOpen()) {
                const RECT drag = headerDragRect();
                if (x >= drag.left && x < drag.right &&
                    y >= drag.top && y < drag.bottom) return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_SIZE:
            winW_ = LOWORD(lParam);
            winH_ = HIWORD(lParam);
            resizeRenderer();
            applyWindowCorners();
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
#ifdef _WIN32
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            bool opened = false;
            for (UINT i = 0; i < count && !opened; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring wide(len + 1, L'\0');
                DragQueryFileW(drop, i, &wide[0], len + 1);
                wide.resize(len);
                std::string path = toUtf8(wide);
                if (isApkPath(path)) {
                    openApkPicker(path);
                    opened = true;
                }
            }
            if (count > 0 && !opened) {
                showMessageModal_ = true;
                modalTitle_ = "Not an APK";
                modalMessage_ = "Drop a file ending in .apk";
            }
#else
            const std::string path = emu::sdlcompat::dropPath(drop);
            if (!path.empty() && isApkPath(path)) {
                openApkPicker(path);
            } else if (!path.empty()) {
                showMessageModal_ = true;
                modalTitle_ = "Not an APK";
                modalMessage_ = "Drop a file ending in .apk";
            }
#endif
            DragFinish(drop);
            return 0;
        }
        case WM_PAINT:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case UPDATE_RESULT_MSG: {
            std::unique_ptr<UpdateInfo> info(
                reinterpret_cast<UpdateInfo*>(lParam));
            updateCheckRunning_ = false;
            const bool manual = updateCheckManual_;
            if (!info) return 0;

            if (!info->error.empty()) {
                LOG_WARN("Update check failed: %s", info->error.c_str());
                if (manual) {
                    showMessageModal_ = true;
                    modalTitle_ = "Update check failed";
                    modalMessage_ = info->error;
                }
                return 0;
            }
            if (!info->checked) {
                LOG_WARN("Update check failed: version file could not be read");
                if (manual) {
                    showMessageModal_ = true;
                    modalTitle_ = "Update check failed";
                    modalMessage_ = "The version file could not be read.";
                }
                return 0;
            }

            if (info->available && !info->downloadedPath.empty()) {
                LOG_INFO("Downloaded update %s to %s", info->remoteVersion.c_str(),
                         info->downloadedPath.c_str());
                std::string message = "Version " + info->remoteVersion +
                    " is available (you have " + info->currentVersion + ").\n"
                    "Downloaded to:\n" + info->downloadedPath;
                if (!info->releaseNotes.empty()) {
                    std::string notes = info->releaseNotes;
                    if (notes.size() > 700) notes = notes.substr(0, 700) + "...";
                    message += "\n\nRelease notes:\n" + notes;
                }
                showMessageModal_ = true;
                modalTitle_ = "Update available";
                modalMessage_ = message;
            } else if (manual) {
                LOG_INFO("Update check complete: version %s is up to date",
                         info->currentVersion.c_str());
                showMessageModal_ = true;
                modalTitle_ = "Up to date";
                modalMessage_ = "You are running the latest version (" +
                                info->currentVersion + ").";
            } else {
                LOG_INFO("No update available (version %s)", info->currentVersion.c_str());
            }
            return 0;
        }
        case DEBUG_MSG:
            switch ((int)wParam) {
                case 1: debugToggleAntiAfk(); break;
                case 2: debugToggleKeepAwake(); break;
                case 3: debugOpenSettings(); break;
                case 4: debugCloseSettings(); break;
                case 5: debugOpenFirstPhone(); break;
                case 6: debugLeaveEmulator(); break;
                case 7: debugStopAll(); break;
                case 8: debugExitAll(); break;
                case 9: debugToggleListView(); break;
                case 10: debugCycleFilter(); break;
                case 11: debugOpenSettingsTab((int)lParam); break;
                case 12: debugToggleFontSize(); break;
                case 13: {
                    const char* testPath = std::getenv("BD_TEST_APK");
                    if (testPath) debugOpenApkPicker(testPath);
                    break;
                }
                case 14: {
                    const char* testPath = std::getenv("BD_TEST_APK");
                    if (testPath) debugInstallTestApk(testPath);
                    break;
                }
                case 15:
                    state_.settings.shaderMode = (state_.settings.shaderMode + 1) % 3;
                    persist();
                    break;
                case 16: startUpdateCheck(true); break;
            }
            return 0;
        case TRAY_RESTORE_MSG:
            restoreFromTray();
            return 0;
        case TRAY_MSG:
            if (lParam == WM_LBUTTONDBLCLK ||
                (lParam == WM_RBUTTONUP && ((wParam & 0xFFFF) == TRAY_ID))) {
                if (lParam == WM_LBUTTONDBLCLK) {
                    restoreFromTray();
                } else {
                    SetForegroundWindow(hwnd);
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING, TRAY_MENU_RESTORE, L"Open BlueDroid");
                    AppendMenuW(menu, MF_STRING, TRAY_MENU_STOP_ALL, L"Stop all phones");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, TRAY_MENU_EXIT, L"Exit all and quit");
                    POINT pt{};
                    GetCursorPos(&pt);
                    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                        pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    if (cmd == TRAY_MENU_RESTORE) restoreFromTray();
                    else if (cmd == TRAY_MENU_STOP_ALL) closeAllPhones();
                    else if (cmd == TRAY_MENU_EXIT) exitEverything();
                }
            }
            return 0;
        case WM_KEYDOWN: {
            lastInputAt_ = std::chrono::steady_clock::now();
            ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
            // Emergency reset: works even when the UI is too large to reach
            // the Appearance tab.
            if (wParam == '0' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (state_.settings.uiScalePercent != 100) {
                    state_.settings.uiScalePercent = 100;
                    applyThemeFromSettings();
                    persist();
                    LOG_INFO("UI scale reset to 100 percent with Ctrl+0");
                }
                return 0;
            }
            if (mode_ == UiMode::Settings) {
                if (wParam == VK_ESCAPE && !modalOpen()) closeSettings();
                return 0;
            }
            if (mode_ != UiMode::Emulator || modalOpen()) return 0;
            if (wParam == VK_F11) { toggleFullscreen(); return 0; }
            if (wParam == VK_ESCAPE && fullscreen_) { toggleFullscreen(); return 0; }
            if (io && io->WantCaptureKeyboard) return 0;
            if (isTextKey((int)wParam)) return 0;
            if (input_) input_->onKeyDown((int)wParam);
            return 0;
        }
        case WM_KEYUP:
            lastInputAt_ = std::chrono::steady_clock::now();
            if (mode_ == UiMode::Emulator && input_ && !modalOpen() && !isTextKey((int)wParam)) {
                input_->onKeyUp((int)wParam);
            }
            return 0;
        case WM_CHAR: {
            ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
            if (modalOpen() || (io && io->WantTextInput)) return 0;
            if (mode_ != UiMode::Emulator || !vm_) return 0;
            wchar_t ch = (wchar_t)wParam;
            if (ch >= 32 && ch < 128) {
                char buf[8] = {};
                buf[0] = (char)ch;
                vm_->sendText(buf);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (draggingWindow_) {
                POINT pt{};
                GetCursorPos(&pt);
                SetWindowPos(hwnd_, nullptr,
                    dragWindowOrigin_.x + (pt.x - dragCursorOrigin_.x),
                    dragWindowOrigin_.y + (pt.y - dragCursorOrigin_.y),
                    0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (draggingWindow_) {
                draggingWindow_ = false;
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (draggingWindow_) draggingWindow_ = false;
            break;
        case WM_LBUTTONDOWN:
            return 0;
        case WM_CLOSE:
            // Default is a temporary close: hide to the tray and keep phones
            // running. Users who prefer a true quit can turn this off in
            // Settings > General.
            if (state_.settings.minimizeToTray) hideToTray();
            else exitEverything();
            return 0;
        case WM_DESTROY:
            removeTrayIcon();
            shutdownRenderer();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace emu
