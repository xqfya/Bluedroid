#include "emulator/phone_profile.h"

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace emu {

std::string sanitizeAvdName(const std::string& name, int id);
void saveManagerState(const std::string& path, const ManagerState& st);

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static int toInt(const std::string& s, int fallback = 0) {
    try { return std::stoi(s); } catch (...) { return fallback; }
}

static unsigned int toUInt(const std::string& s, unsigned int fallback = 0) {
    try { return (unsigned int)std::stoul(s); } catch (...) { return fallback; }
}

static int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int normalizeFpsCap(int v) {
    return v == 30 || v == 120 ? v : 60;
}

ManagerState loadManagerState(const std::string& path) {
    ManagerState st;
    std::ifstream f(path);
    if (!f) return st;

    PhoneProfile cur;
    bool inPhone = false;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line == "[settings]") {
            if (inPhone && cur.id > 0) { st.phones.push_back(cur); cur = {}; }
            inPhone = false;
            continue;
        }
        if (line == "[phone]") {
            if (inPhone && cur.id > 0) { st.phones.push_back(cur); cur = {}; }
            inPhone = true;
            continue;
        }
        if (line[0] == '[') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (!inPhone) {
            // keepOpen is ignored now: the back arrow always parks a running phone.
            if (key == "saveFps") st.settings.saveFps = toInt(val) != 0;
            else if (key == "antiAfk") st.settings.antiAfk = toInt(val) != 0;
            else if (key == "keepAwake") st.settings.keepAwake = toInt(val) != 0;
            else if (key == "previewMinutes") {
                int m = toInt(val, 2);
                st.settings.previewMinutes = m < 1 ? 1 : (m > 30 ? 30 : m);
            }
            else if (key == "fpsCap") st.settings.fpsCap = normalizeFpsCap(toInt(val, 60));
            else if (key == "gpuMode") st.settings.gpuMode = clampInt(toInt(val), 0, 1);
            else if (key == "minimizeToTray") st.settings.minimizeToTray = toInt(val, 1) != 0;
            else if (key == "muteParkedAudio") st.settings.muteParkedAudio = toInt(val, 0) != 0;
            else if (key == "performancePreset") st.settings.performancePreset = val.empty() ? "Balanced" : val;
            else if (key == "firstRunSettingsSeen") st.settings.firstRunSettingsSeen = toInt(val) != 0;
            else if (key == "searchText") st.settings.searchText = val;
            else if (key == "statusFilter") st.settings.statusFilter = clampInt(toInt(val), 0, 4);
            else if (key == "listView") st.settings.listView = toInt(val) != 0;
            else if (key == "showResources") st.settings.showResources = toInt(val, 1) != 0;
            else if (key == "accentColor") st.settings.accentColor = toUInt(val, 0xF496FF);
            else if (key == "useCustomColors") st.settings.useCustomColors = toInt(val) != 0;
            else if (key == "windowBgColor") st.settings.windowBgColor = toUInt(val, st.settings.windowBgColor);
            else if (key == "titleBarColor") st.settings.titleBarColor = toUInt(val, st.settings.titleBarColor);
            else if (key == "panelColor") st.settings.panelColor = toUInt(val, st.settings.panelColor);
            else if (key == "navBarColor") st.settings.navBarColor = toUInt(val, st.settings.navBarColor);
            else if (key == "buttonColor") st.settings.buttonColor = toUInt(val, st.settings.buttonColor);
            else if (key == "borderColor") st.settings.borderColor = toUInt(val, st.settings.borderColor);
            else if (key == "textColor") st.settings.textColor = toUInt(val, st.settings.textColor);
            else if (key == "textMutedColor") st.settings.textMutedColor = toUInt(val, st.settings.textMutedColor);
            else if (key == "successColor") st.settings.successColor = toUInt(val, st.settings.successColor);
            else if (key == "dangerColor") st.settings.dangerColor = toUInt(val, st.settings.dangerColor);
            else if (key == "uiScalePercent") st.settings.uiScalePercent = clampInt(toInt(val, 100), 100, 150);
            else if (key == "fontSize") st.settings.fontSize = clampInt(toInt(val, 15), 12, 22);
            else if (key == "fontFamily") st.settings.fontFamily = clampInt(toInt(val), 0, 1);
            else if (key == "cornerScale10") st.settings.cornerScale10 = clampInt(toInt(val, 10), 0, 20);
            else if (key == "settingsTab") st.settings.settingsTab = clampInt(toInt(val), 0, 2);
            else if (key == "roundedCorners") st.settings.roundedCorners = toInt(val, 1) != 0;
            else if (key == "cornerRadius") st.settings.cornerRadius = clampInt(toInt(val, 18), 0, 48);
            else if (key == "shaderMode") st.settings.shaderMode = clampInt(toInt(val, 1), 0, 2);
            else if (key == "sharpness") st.settings.sharpness = clampInt(toInt(val, 30), 0, 100);
            else if (key == "contrast") st.settings.contrast = clampInt(toInt(val, 12), 0, 100);
            else if (key == "saturation") st.settings.saturation = clampInt(toInt(val, 10), 0, 100);
            else if (key == "vignette") st.settings.vignette = clampInt(toInt(val, 10), 0, 100);
            else if (key == "glow") st.settings.glow = clampInt(toInt(val), 0, 100);
            continue;
        }

        if (key == "id") cur.id = toInt(val);
        else if (key == "name") cur.name = val;
        else if (key == "avd") cur.avd = val;
        else if (key == "port") cur.port = toInt(val, 5554);
        else if (key == "starred") cur.starred = toInt(val) != 0;
        else if (key == "paused") cur.paused = toInt(val) != 0;
        else if (key == "color") cur.color = (unsigned int)toInt(val, 3899742);
        else if (key == "templateName") cur.templateName = val;
        else if (key == "cpuCores") cur.cpuCores = clampInt(toInt(val), 0, 32);
        else if (key == "ramMB") cur.ramMB = clampInt(toInt(val), 0, 65536);
        else if (key == "screenWidth") cur.screenWidth = clampInt(toInt(val), 0, 7680);
        else if (key == "screenHeight") cur.screenHeight = clampInt(toInt(val), 0, 7680);
        else if (key == "dpi") cur.dpi = clampInt(toInt(val), 0, 640);
    }
    if (inPhone && cur.id > 0) st.phones.push_back(cur);

    // Isolation is a hard requirement: never let a hand-edited or migrated
    // profile file make two logical phones share one AVD userdata image or one
    // ADB serial/APK library. First profile keeps its identity; later ones get
    // a unique generated identity and the repaired file is written back.
    bool repaired = false;
    std::set<int> ids, ports;
    std::set<std::string> avds;
    for (auto& p : st.phones) {
        if (p.id <= 0 || ids.count(p.id)) {
            int next = 1;
            while (ids.count(next)) ++next;
            p.id = next;
            repaired = true;
        }
        ids.insert(p.id);

        if (p.avd.empty()) {
            p.avd = "BD_" + sanitizeAvdName(p.name, p.id);
            repaired = true;
        }
        std::string key = p.avd;
        for (char& c : key) c = (char)std::tolower((unsigned char)c);
        if (avds.count(key)) {
            p.avd = "BD_" + sanitizeAvdName(p.name, p.id) + "_" + std::to_string(p.id);
            key = p.avd;
            for (char& c : key) c = (char)std::tolower((unsigned char)c);
            repaired = true;
        }
        avds.insert(key);

        if (p.port <= 0 || ports.count(p.port)) {
            int port = 5554;
            while (ports.count(port)) port += 2;
            p.port = port;
            repaired = true;
        }
        ports.insert(p.port);
    }
    if (repaired) saveManagerState(path, st);
    return st;
}

void saveManagerState(const std::string& path, const ManagerState& st) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;

    int previewMinutes = st.settings.previewMinutes;
    if (previewMinutes < 1) previewMinutes = 1;
    if (previewMinutes > 30) previewMinutes = 30;
    f << "[settings]\n"
      << "saveFps=" << (st.settings.saveFps ? 1 : 0) << "\n"
      << "antiAfk=" << (st.settings.antiAfk ? 1 : 0) << "\n"
      << "keepAwake=" << (st.settings.keepAwake ? 1 : 0) << "\n"
      << "previewMinutes=" << previewMinutes << "\n"
      << "fpsCap=" << normalizeFpsCap(st.settings.fpsCap) << "\n"
      << "gpuMode=" << clampInt(st.settings.gpuMode, 0, 1) << "\n"
      << "minimizeToTray=" << (st.settings.minimizeToTray ? 1 : 0) << "\n"
      << "muteParkedAudio=" << (st.settings.muteParkedAudio ? 1 : 0) << "\n"
      << "performancePreset=" << (st.settings.performancePreset.empty() ? "Balanced" : st.settings.performancePreset) << "\n"
      << "firstRunSettingsSeen=" << (st.settings.firstRunSettingsSeen ? 1 : 0) << "\n"
      << "searchText=" << st.settings.searchText << "\n"
      << "statusFilter=" << clampInt(st.settings.statusFilter, 0, 4) << "\n"
      << "listView=" << (st.settings.listView ? 1 : 0) << "\n"
      << "showResources=" << (st.settings.showResources ? 1 : 0) << "\n"
      << "accentColor=" << st.settings.accentColor << "\n"
      << "useCustomColors=" << (st.settings.useCustomColors ? 1 : 0) << "\n"
      << "windowBgColor=" << st.settings.windowBgColor << "\n"
      << "titleBarColor=" << st.settings.titleBarColor << "\n"
      << "panelColor=" << st.settings.panelColor << "\n"
      << "navBarColor=" << st.settings.navBarColor << "\n"
      << "buttonColor=" << st.settings.buttonColor << "\n"
      << "borderColor=" << st.settings.borderColor << "\n"
      << "textColor=" << st.settings.textColor << "\n"
      << "textMutedColor=" << st.settings.textMutedColor << "\n"
      << "successColor=" << st.settings.successColor << "\n"
      << "dangerColor=" << st.settings.dangerColor << "\n"
      << "uiScalePercent=" << clampInt(st.settings.uiScalePercent, 100, 150) << "\n"
      << "fontSize=" << clampInt(st.settings.fontSize, 12, 22) << "\n"
      << "fontFamily=" << clampInt(st.settings.fontFamily, 0, 1) << "\n"
      << "cornerScale10=" << clampInt(st.settings.cornerScale10, 0, 20) << "\n"
      << "settingsTab=" << clampInt(st.settings.settingsTab, 0, 2) << "\n"
      << "roundedCorners=" << (st.settings.roundedCorners ? 1 : 0) << "\n"
      << "cornerRadius=" << clampInt(st.settings.cornerRadius, 0, 48) << "\n"
      << "shaderMode=" << clampInt(st.settings.shaderMode, 0, 2) << "\n"
      << "sharpness=" << clampInt(st.settings.sharpness, 0, 100) << "\n"
      << "contrast=" << clampInt(st.settings.contrast, 0, 100) << "\n"
      << "saturation=" << clampInt(st.settings.saturation, 0, 100) << "\n"
      << "vignette=" << clampInt(st.settings.vignette, 0, 100) << "\n"
      << "glow=" << clampInt(st.settings.glow, 0, 100) << "\n";

    for (const auto& p : st.phones) {
        f << "\n[phone]\n"
          << "id=" << p.id << "\n"
          << "name=" << p.name << "\n"
          << "avd=" << p.avd << "\n"
          << "port=" << p.port << "\n"
          << "starred=" << (p.starred ? 1 : 0) << "\n"
          << "paused=" << (p.paused ? 1 : 0) << "\n"
          << "color=" << p.color << "\n"
          << "templateName=" << p.templateName << "\n"
          << "cpuCores=" << p.cpuCores << "\n"
          << "ramMB=" << p.ramMB << "\n"
          << "screenWidth=" << p.screenWidth << "\n"
          << "screenHeight=" << p.screenHeight << "\n"
          << "dpi=" << p.dpi << "\n";
    }
}

std::string sanitizeAvdName(const std::string& name, int id) {
    std::string out;
    bool lastUnderscore = false;
    for (char ch : name) {
        if (std::isalnum((unsigned char)ch)) {
            out += ch;
            lastUnderscore = false;
        } else if (!lastUnderscore) {
            out += '_';
            lastUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "Phone_" + std::to_string(id);
    return out;
}

} // namespace emu
