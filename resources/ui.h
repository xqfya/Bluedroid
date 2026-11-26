// ui.h  -  Dear ImGui header bar and single custom theme
//
//  * Single header, public ImGui API only (works with 1.89 - 1.92)
//  * No icon font needed: every icon is drawn with ImDrawList
//  * Pill navigation bar with an animated, expanding active tab
//  * Brand / status chips / donate on the left, utility icons + window controls on the right
//  * Theme struct (colours + corner scale) so you can recolour it like your settings page
//
//  Usage:
//      static hdr::Header header;
//      hdr::ApplyStyle(header.theme);                // once, after ImGui::CreateContext()
//
//      // every frame, before your content windows:
//      hdr::Events ev = header.Draw();
//      if (ev.navClicked >= 0) page = ev.navClicked;
//      if (ev.close)           running = false;
//      // ev.minimize / ev.maximize / ev.dragStart / ev.dragDoubleClick -> hook to your window API
//
//      // content window sits below it:
//      ImGuiViewport* vp = ImGui::GetMainViewport();
//      ImGui::SetNextWindowPos (ImVec2(vp->Pos.x, vp->Pos.y + header.height));
//      ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - header.height));
//
//  Note: hover/tab animations need frames to advance. If you only render on input events,
//  keep rendering for ~300 ms after a click or mouse move.

#pragma once
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hdr {

constexpr float kPi = 3.14159265f;

// ───────────────────────────── colour helpers ─────────────────────────────
inline ImVec4 V4(ImU32 c)   { return ImGui::ColorConvertU32ToFloat4(c); }
inline ImU32  U32(ImVec4 v) { return ImGui::ColorConvertFloat4ToU32(v); }
inline float  Lerp(float a, float b, float t) { return a + (b - a) * t; }

inline ImU32 Alpha(ImU32 c, float a) { ImVec4 v = V4(c); v.w *= a; return U32(v); }

inline ImU32 Mix(ImU32 a, ImU32 b, float t) {
    ImVec4 A = V4(a), B = V4(b);
    return U32(ImVec4(Lerp(A.x, B.x, t), Lerp(A.y, B.y, t), Lerp(A.z, B.z, t), Lerp(A.w, B.w, t)));
}

// "Text stays readable whatever you pick": white or near-black depending on the colour behind it.
inline ImU32 OnColor(ImU32 bg) {
    ImVec4 c = V4(bg);
    float lum = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    return lum > 0.70f ? IM_COL32(10, 14, 30, 255) : IM_COL32(255, 255, 255, 255);
}

// Smooth 0..1 value stored per ImGui ID (hover fades, tab expansion).
inline float Anim(ImGuiID id, bool on, float speed = 14.f) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float v = st->GetFloat(id, 0.f);
    v += ((on ? 1.f : 0.f) - v) * (1.f - expf(-speed * ImGui::GetIO().DeltaTime));
    st->SetFloat(id, v);
    return v;
}

// ───────────────────────────────── theme ──────────────────────────────────
struct Theme {
    ImU32 windowBg  = IM_COL32(10, 14, 31, 255);
    ImU32 titleBar  = IM_COL32(11, 16, 36, 255);
    ImU32 panel     = IM_COL32(15, 21, 48, 255);
    ImU32 navBar    = IM_COL32(19, 27, 58, 255);
    ImU32 button    = IM_COL32(23, 31, 66, 255);
    ImU32 border    = IM_COL32(52, 64, 112, 255);
    ImU32 text      = IM_COL32(232, 236, 255, 255);
    ImU32 textMuted = IM_COL32(140, 149, 186, 255);
    ImU32 accent    = IM_COL32(112, 150, 255, 255);
    ImU32 success   = IM_COL32(52, 211, 120, 255);
    ImU32 danger    = IM_COL32(226, 62, 78, 255);
    float cornerScale = 1.f;   // 0 square, .5 soft, 1 default, 1.5 round, 2 rounder

    float Pill(float h) const { return h * 0.5f * std::min(cornerScale, 1.f); }
    float Round(float base, float h) const { return std::min(h * 0.5f, base * cornerScale); }
};

// Optional: make the rest of the app match the header.
inline void ApplyStyle(const Theme& t) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]             = V4(t.text);
    c[ImGuiCol_TextDisabled]     = V4(t.textMuted);
    c[ImGuiCol_WindowBg]         = V4(t.windowBg);
    c[ImGuiCol_ChildBg]          = V4(t.panel);
    c[ImGuiCol_PopupBg]          = V4(t.navBar);
    c[ImGuiCol_Border]           = V4(Alpha(t.border, 0.45f));
    c[ImGuiCol_FrameBg]          = V4(t.button);
    c[ImGuiCol_FrameBgHovered]   = V4(Mix(t.button, t.border, 0.35f));
    c[ImGuiCol_FrameBgActive]    = V4(Mix(t.button, t.border, 0.60f));
    c[ImGuiCol_Button]           = V4(t.button);
    c[ImGuiCol_ButtonHovered]    = V4(Mix(t.button, t.border, 0.40f));
    c[ImGuiCol_ButtonActive]     = V4(Alpha(t.accent, 0.80f));
    c[ImGuiCol_Header]           = V4(Alpha(t.accent, 0.25f));
    c[ImGuiCol_HeaderHovered]    = V4(Alpha(t.accent, 0.40f));
    c[ImGuiCol_HeaderActive]     = V4(Alpha(t.accent, 0.55f));
    c[ImGuiCol_CheckMark]        = V4(t.accent);
    c[ImGuiCol_SliderGrab]       = V4(t.accent);
    c[ImGuiCol_SliderGrabActive] = V4(Mix(t.accent, IM_COL32_WHITE, 0.25f));
    s.WindowRounding = 12.f * t.cornerScale;
    s.ChildRounding  = 12.f * t.cornerScale;
    s.PopupRounding  = 10.f * t.cornerScale;
    s.FrameRounding  = 8.f  * t.cornerScale;
    s.GrabRounding   = 8.f  * t.cornerScale;
    s.WindowBorderSize = 1.f;
    s.FramePadding   = ImVec2(10.f, 6.f);
    s.ItemSpacing    = ImVec2(10.f, 8.f);
}

// ───────────────────────────────── icons ──────────────────────────────────
enum class Icon {
    Home, Play, Globe, User, Wrench, Settings, Download, Refresh, Sidebar,
    Sun, Shield, Minimize, Maximize, Close, Chevron, Heart, Discord
};

inline void DrawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float size, ImU32 col) {
    const float r = size * 0.5f, th = 1.5f;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    auto Ellipse = [&](float rx, float ry) {
        for (int i = 0; i < 32; i++) {
            float a = i * 2.f * kPi / 32.f;
            dl->PathLineTo(ImVec2(c.x + cosf(a) * rx * r, c.y + sinf(a) * ry * r));
        }
        dl->PathStroke(col, ImDrawFlags_Closed, th);
    };

    switch (ic) {
    case Icon::Home: {
        ImVec2 roof[] = { P(-1.f, 0.f), P(0.f, -0.95f), P(1.f, 0.f) };
        ImVec2 body[] = { P(-0.7f, -0.25f), P(-0.7f, 0.85f), P(0.7f, 0.85f), P(0.7f, -0.25f) };
        dl->AddPolyline(roof, 3, col, 0, th);
        dl->AddPolyline(body, 4, col, 0, th);
    } break;
    case Icon::Play:
        dl->AddTriangle(P(-0.5f, -0.85f), P(-0.5f, 0.85f), P(0.85f, 0.f), col, th);
        break;
    case Icon::Globe:
        dl->AddCircle(c, r * 0.9f, col, 28, th);
        Ellipse(0.38f, 0.9f);
        dl->AddLine(P(-0.9f, 0.f), P(0.9f, 0.f), col, th);
        break;
    case Icon::User:
        dl->AddCircle(P(0.f, -0.4f), r * 0.38f, col, 20, th);
        dl->PathArcTo(P(0.f, 1.0f), r * 0.85f, kPi, kPi * 2.f, 18);
        dl->PathStroke(col, 0, th);
        break;
    case Icon::Wrench:
        dl->AddLine(P(-0.85f, 0.85f), P(0.1f, -0.1f), col, th + 0.9f);
        dl->AddCircle(P(0.45f, -0.45f), r * 0.5f, col, 18, th);
        break;
    case Icon::Settings:
        dl->AddCircle(c, r * 0.36f, col, 18, th);
        dl->AddCircle(c, r * 0.70f, col, 24, th);
        for (int i = 0; i < 8; i++) {
            float a = i * kPi / 4.f;
            dl->AddLine(P(cosf(a) * 0.70f, sinf(a) * 0.70f), P(cosf(a) * 1.0f, sinf(a) * 1.0f), col, th + 0.7f);
        }
        break;
    case Icon::Download: {
        ImVec2 head[] = { P(-0.5f, -0.15f), P(0.f, 0.4f), P(0.5f, -0.15f) };
        ImVec2 tray[] = { P(-0.85f, 0.45f), P(-0.85f, 0.9f), P(0.85f, 0.9f), P(0.85f, 0.45f) };
        dl->AddLine(P(0.f, -0.9f), P(0.f, 0.35f), col, th);
        dl->AddPolyline(head, 3, col, 0, th);
        dl->AddPolyline(tray, 4, col, 0, th);
    } break;
    case Icon::Refresh: {
        const float a0 = -1.0f, a1 = a0 + 5.0f, rr = r * 0.78f;
        dl->PathArcTo(c, rr, a0, a1, 24);
        dl->PathStroke(col, 0, th);
        ImVec2 e(c.x + cosf(a1) * rr, c.y + sinf(a1) * rr);
        ImVec2 n(cosf(a1), sinf(a1)), tg(-sinf(a1), cosf(a1));
        float k = r * 0.38f;
        dl->AddTriangleFilled(ImVec2(e.x + tg.x * k * 1.1f, e.y + tg.y * k * 1.1f),
                              ImVec2(e.x + n.x * k, e.y + n.y * k),
                              ImVec2(e.x - n.x * k, e.y - n.y * k), col);
    } break;
    case Icon::Sidebar:
        dl->AddRect(P(-0.9f, -0.75f), P(0.9f, 0.75f), col, r * 0.25f, 0, th);
        dl->AddLine(P(-0.2f, -0.75f), P(-0.2f, 0.75f), col, th);
        break;
    case Icon::Sun:
        dl->AddCircle(c, r * 0.38f, col, 18, th);
        for (int i = 0; i < 8; i++) {
            float a = i * kPi / 4.f;
            dl->AddLine(P(cosf(a) * 0.68f, sinf(a) * 0.68f), P(cosf(a) * 0.98f, sinf(a) * 0.98f), col, th);
        }
        break;
    case Icon::Shield: {
        ImVec2 pts[] = { P(0.f, -0.95f), P(0.8f, -0.6f), P(0.8f, 0.1f), P(0.45f, 0.65f),
                         P(0.f, 0.95f), P(-0.45f, 0.65f), P(-0.8f, 0.1f), P(-0.8f, -0.6f) };
        dl->AddPolyline(pts, 8, col, ImDrawFlags_Closed, th);
    } break;
    case Icon::Minimize:
        dl->AddLine(P(-0.7f, 0.f), P(0.7f, 0.f), col, th);
        break;
    case Icon::Maximize:
        dl->AddRect(P(-0.65f, -0.65f), P(0.65f, 0.65f), col, 1.5f, 0, th);
        break;
    case Icon::Close:
        dl->AddLine(P(-0.65f, -0.65f), P(0.65f, 0.65f), col, th);
        dl->AddLine(P(-0.65f, 0.65f), P(0.65f, -0.65f), col, th);
        break;
    case Icon::Chevron: {
        ImVec2 v[] = { P(-0.6f, -0.3f), P(0.f, 0.3f), P(0.6f, -0.3f) };
        dl->AddPolyline(v, 3, col, 0, th);
    } break;
    case Icon::Heart:
        for (int i = 0; i < 36; i++) {
            float t = i * 2.f * kPi / 36.f;
            float hx = 16.f * powf(sinf(t), 3.f);
            float hy = -(13.f * cosf(t) - 5.f * cosf(2 * t) - 2.f * cosf(3 * t) - cosf(4 * t));
            dl->PathLineTo(P(hx / 17.f, hy / 17.f - 0.12f));
        }
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        break;
    case Icon::Discord:
        dl->AddRect(P(-0.95f, -0.6f), P(0.95f, 0.6f), col, r * 0.55f, 0, th);
        dl->AddCircleFilled(P(-0.36f, 0.02f), r * 0.17f, col, 12);
        dl->AddCircleFilled(P(0.36f, 0.02f), r * 0.17f, col, 12);
        break;
    }
}

// ──────────────────────────────── header ──────────────────────────────────
struct NavItem { Icon icon; std::string label; };

struct Events {
    int  navClicked = -1;
    bool brand = false, donate = false, discord = false, download = false;
    bool refresh = false, sidebar = false, themeToggle = false, shield = false;
    bool minimize = false, maximize = false, close = false;
    bool dragStart = false, dragDoubleClick = false;   // empty header area
};

struct Header {
    Theme       theme;
    float       height = 48.f;
    int         active = 5;
    std::string appName = "z", online = "z", plays = "z";
    bool        windowControls = true;   // set false if the OS draws its own title bar
    bool        showBorder = false;
    ImFont*     fontBold = nullptr;      // optional; falls back to faux-bold
    std::vector<NavItem> nav = {
        { Icon::Home, "Home" },     { Icon::Play, "Play" },   { Icon::Globe, "Browse" },
        { Icon::User, "Profile" },  { Icon::Wrench, "Tools" }, { Icon::Settings, "Settings" },
    };

    Events Draw() {
        Events ev;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.titleBar);

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##roalt_header", nullptr, flags)) {
            dl_  = ImGui::GetWindowDrawList();
            o_   = ImGui::GetWindowPos();
            W_   = ImGui::GetWindowWidth();
            cy_  = o_.y + height * 0.5f;

            if (showBorder)
                dl_->AddLine(ImVec2(o_.x, o_.y + height - 1), ImVec2(o_.x + W_, o_.y + height - 1),
                             Alpha(theme.border, 0.5f));

            DrawNav(ev);     // first: it tells the side clusters how much room they have
            DrawLeft(ev);
            DrawRight(ev);

            const bool bare = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
            ev.dragStart       = bare && ImGui::IsMouseClicked(0);
            ev.dragDoubleClick = bare && ImGui::IsMouseDoubleClicked(0);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        return ev;
    }

private:
    ImDrawList* dl_ = nullptr;
    ImVec2 o_{};
    float  W_ = 0, cy_ = 0, navL_ = 0, navR_ = 0;

    // ---- small primitives ----
    ImVec2 TextSize(const char* t, bool bold = false) const {
        if (bold && fontBold)
            return fontBold->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.f, t);
        return ImGui::CalcTextSize(t);
    }
    void Txt(ImVec2 p, ImU32 col, const char* t, bool bold = false) const {
        if (bold && fontBold) { dl_->AddText(fontBold, ImGui::GetFontSize(), p, col, t); return; }
        dl_->AddText(p, col, t);
        if (bold) dl_->AddText(ImVec2(p.x + 0.5f, p.y), col, t);   // faux bold
    }
    bool Hit(const char* id, ImVec2 p, ImVec2 s, float& hov) {
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton(id, s);
        const bool h = ImGui::IsItemHovered();
        hov = Anim(ImGui::GetItemID(), h);
        if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        return clicked;
    }
    void Plate(ImVec2 p, ImVec2 s, float hov) const {
        if (hov < 0.01f) return;
        dl_->AddRectFilled(p, ImVec2(p.x + s.x, p.y + s.y),
                           Alpha(Mix(theme.button, theme.border, 0.35f), hov), theme.Round(9.f, s.y));
    }
    bool IconBtn(const char* id, Icon ic, ImVec2 p, float w, float h, float isz = 16.f) {
        float hov;
        const bool clicked = Hit(id, p, ImVec2(w, h), hov);
        Plate(p, ImVec2(w, h), hov);
        DrawIcon(dl_, ic, ImVec2(p.x + w * 0.5f, p.y + h * 0.5f), isz, Mix(theme.textMuted, theme.text, hov));
        return clicked;
    }
    void Logo(ImVec2 c, float s) const {
        const float q = s * 0.58f, rr = 3.f * theme.cornerScale;
        dl_->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.5f), ImVec2(c.x - s * 0.5f + q, c.y - s * 0.5f + q),
                           Alpha(theme.accent, 0.55f), rr);
        dl_->AddRectFilled(ImVec2(c.x + s * 0.5f - q, c.y + s * 0.5f - q), ImVec2(c.x + s * 0.5f, c.y + s * 0.5f),
                           theme.accent, rr);
    }

    // ---- left: brand, status chips, discord, donate ----
    void DrawLeft(Events& ev) {
        const float bh = 32.f, room = 12.f;
        float x = o_.x + 10.f, hov;
        auto fits = [&](float w) { return x + w < navL_ - room; };

        { // brand
            const ImVec2 ts = TextSize(appName.c_str(), true);
            const float w = 10 + 18 + 8 + ts.x + 8 + 10 + 10;
            ImVec2 p(x, cy_ - bh * 0.5f);
            if (Hit("##brand", p, ImVec2(w, bh), hov)) ev.brand = true;
            Plate(p, ImVec2(w, bh), hov);
            Logo(ImVec2(p.x + 19.f, cy_), 18.f);
            Txt(ImVec2(p.x + 36.f, cy_ - ts.y * 0.5f), theme.text, appName.c_str(), true);
            DrawIcon(dl_, Icon::Chevron, ImVec2(p.x + w - 15.f, cy_), 10.f, theme.textMuted);
            x += w + 12.f;
        }
        { // online chip
            const ImVec2 ts = TextSize(online.c_str(), true);
            const float w = 14 + ts.x;
            if (!fits(w)) return;
            dl_->AddCircleFilled(ImVec2(x + 4.f, cy_), 6.5f, Alpha(theme.success, 0.18f), 16);
            dl_->AddCircleFilled(ImVec2(x + 4.f, cy_), 3.5f, theme.success, 12);
            Txt(ImVec2(x + 14.f, cy_ - ts.y * 0.5f), theme.text, online.c_str(), true);
            x += w + 14.f;
        }
        { // plays chip
            const ImVec2 ts = TextSize(plays.c_str());
            const float w = 16 + ts.x;
            if (!fits(w)) return;
            DrawIcon(dl_, Icon::Play, ImVec2(x + 5.f, cy_), 10.f, theme.textMuted);
            Txt(ImVec2(x + 16.f, cy_ - ts.y * 0.5f), theme.textMuted, plays.c_str());
            x += w + 10.f;
        }
        if (!fits(32.f)) return;
        if (IconBtn("##discord", Icon::Discord, ImVec2(x, cy_ - 16.f), 32.f, 32.f, 17.f)) ev.discord = true;
        x += 36.f;

        { // donate
            const ImVec2 ts = TextSize("Donate");
            const float w = 10 + 14 + 7 + ts.x + 8 + 10 + 10;
            if (!fits(w)) return;
            ImVec2 p(x, cy_ - bh * 0.5f);
            if (Hit("##donate", p, ImVec2(w, bh), hov)) ev.donate = true;
            Plate(p, ImVec2(w, bh), hov);
            const ImU32 col = Mix(theme.textMuted, theme.text, hov);
            DrawIcon(dl_, Icon::Heart, ImVec2(p.x + 17.f, cy_), 14.f, col);
            Txt(ImVec2(p.x + 31.f, cy_ - ts.y * 0.5f), col, "Donate");
            DrawIcon(dl_, Icon::Chevron, ImVec2(p.x + w - 15.f, cy_), 10.f, theme.textMuted);
        }
    }

    // ---- centre: pill navigation bar ----
    void DrawNav(Events& ev) {
        const float pillH = height - 10.f, itemH = pillH - 8.f, base = 36.f, gap = 2.f, pad = 4.f;
        const int n = (int)nav.size();
        std::vector<float> t(n), w(n);

        float total = pad * 2 + itemH + 4.f;
        for (int i = 0; i < n; i++) {
            ImGui::PushID("act");
            const ImGuiID id = ImGui::GetID((const void*)(intptr_t)(i + 1));
            ImGui::PopID();
            t[i] = Anim(id, i == active);
            const float expW = 12 + 16 + 7 + TextSize(nav[i].label.c_str(), true).x + 14;
            w[i] = Lerp(base, expW, t[i]);
            total += w[i] + gap;
        }

        const float x0 = o_.x + (W_ - total) * 0.5f;
        navL_ = x0; navR_ = x0 + total;
        const ImVec2 p0(x0, cy_ - pillH * 0.5f), p1(x0 + total, cy_ + pillH * 0.5f);
        const float pillR = theme.Pill(pillH), itemR = theme.Pill(itemH);
        dl_->AddRectFilled(p0, p1, theme.navBar, pillR);
        dl_->AddRect(p0, p1, Alpha(theme.border, 0.55f), pillR);

        const ImU32 onAcc = OnColor(theme.accent);
        const float iy = cy_ - itemH * 0.5f;
        float px = x0 + pad, hov;

        for (int i = 0; i < n; i++) {
            const std::string id = "##nav" + std::to_string(i);
            const ImVec2 p(px, iy), q(px + w[i], iy + itemH);
            if (Hit(id.c_str(), p, ImVec2(w[i], itemH), hov)) { active = i; ev.navClicked = i; }

            if (t[i] > 0.01f)
                dl_->AddRectFilled(p, q, Alpha(theme.accent, t[i]), itemR);
            if (hov * (1.f - t[i]) > 0.01f)
                dl_->AddRectFilled(p, q, Alpha(Mix(theme.button, theme.border, 0.35f), hov * (1.f - t[i])), itemR);

            const ImU32 icol = Mix(Mix(theme.textMuted, theme.text, hov), onAcc, t[i]);
            const float icx  = Lerp(p.x + w[i] * 0.5f, p.x + 20.f, t[i]);
            dl_->PushClipRect(p, q, true);
            DrawIcon(dl_, nav[i].icon, ImVec2(icx, cy_), 16.f, icol);
            if (t[i] > 0.4f) {
                const ImVec2 ts = TextSize(nav[i].label.c_str(), true);
                Txt(ImVec2(p.x + 35.f, cy_ - ts.y * 0.5f), Alpha(onAcc, (t[i] - 0.4f) / 0.6f),
                    nav[i].label.c_str(), true);
            }
            dl_->PopClipRect();
            px += w[i] + gap;
        }

        { // download button inside the pill
            px += 4.f;
            const ImVec2 p(px, iy), q(px + itemH, iy + itemH);
            if (Hit("##download", p, ImVec2(itemH, itemH), hov)) ev.download = true;
            dl_->AddRectFilled(p, q, Mix(theme.button, theme.border, 0.15f + 0.35f * hov), itemR);
            DrawIcon(dl_, Icon::Download, ImVec2(p.x + itemH * 0.5f, cy_), 15.f,
                     Mix(theme.textMuted, theme.text, hov));
        }
    }

    // ---- right: utility icons + window controls ----
    void DrawRight(Events& ev) {
        float x = o_.x + W_, hov;

        if (windowControls) {
            const float cw = 46.f;
            struct WC { const char* id; Icon ic; bool* flag; bool danger; };
            WC wc[] = { { "##close", Icon::Close, &ev.close, true },
                        { "##max",   Icon::Maximize, &ev.maximize, false },
                        { "##min",   Icon::Minimize, &ev.minimize, false } };
            for (auto& c : wc) {
                x -= cw;
                const ImVec2 p(x, o_.y);
                if (Hit(c.id, p, ImVec2(cw, height), hov)) *c.flag = true;
                if (hov > 0.01f)
                    dl_->AddRectFilled(p, ImVec2(x + cw, o_.y + height),
                                       c.danger ? Alpha(theme.danger, hov)
                                                : Alpha(IM_COL32(255, 255, 255, 255), 0.08f * hov));
                const ImU32 col = (c.danger && hov > 0.5f) ? IM_COL32(255, 255, 255, 255)
                                                           : Mix(theme.textMuted, theme.text, hov);
                DrawIcon(dl_, c.ic, ImVec2(x + cw * 0.5f, cy_), 10.f, col);
            }
        }
        x -= 8.f;

        struct IB { const char* id; Icon ic; bool* flag; };
        IB ib[] = { { "##shield",  Icon::Shield,  &ev.shield },
                    { "##theme",   Icon::Sun,     &ev.themeToggle },
                    { "##sidebar", Icon::Sidebar, &ev.sidebar },
                    { "##refresh", Icon::Refresh, &ev.refresh } };
        for (auto& b : ib) {
            if (x - 32.f < navR_ + 12.f) break;          // no room left next to the nav pill
            x -= 32.f;
            if (IconBtn(b.id, b.ic, ImVec2(x, cy_ - 16.f), 32.f, 32.f)) *b.flag = true;
            x -= 2.f;
        }
    }
};

} // namespace hdr