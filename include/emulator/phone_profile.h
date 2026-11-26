#pragma once
#include <string>
#include <vector>

namespace emu {

struct PhoneProfile {
    int id = 0;
    std::string name;
    std::string avd;
    int port = 5554;
    bool starred = false;
    bool paused = false;
    unsigned int color = 0x3B78DE; // RGB packed
    // 0 means "use the global emulator.ini value". A template fills these in
    // so a phone can keep its own hardware profile across sessions.
    std::string templateName;
    int cpuCores = 0;
    int ramMB = 0;
    int screenWidth = 0;
    int screenHeight = 0;
    int dpi = 0;
};

struct AppSettings {
    bool saveFps = true;     // show the FPS counter in the status bar
    bool antiAfk = true;     // send harmless keys so games don't log out
    bool keepAwake = true;   // wake the screen so parked phones stay on
    int previewMinutes = 2;  // how often parked phones refresh their card snapshot

    // Essential gamer settings. Mirrored to emulator.ini by MainWindow.
    int fpsCap = 60;         // 30 / 60 / 120 capture cap, next boot applies to VM
    int gpuMode = 0;         // 0 host (DirectX), 1 software (SwiftShader)
    bool minimizeToTray = true;
    bool muteParkedAudio = false;
    std::string performancePreset = "Balanced";
    bool firstRunSettingsSeen = false;

    // Manager UX preferences.
    std::string searchText;  // card/list search text
    int statusFilter = 0;    // 0 all, 1 running, 2 stopped, 3 paused, 4 starred
    bool listView = false;   // false = grid, true = list
    bool showResources = true; // CPU/RAM badges on cards and rows
    unsigned int accentColor = 0xF496FF; // RGB packed; release default palette

    // Only palette shipped in the release. Preset themes were removed; when
    // useCustomColors is true these values are the active colours.
    bool useCustomColors = true;
    unsigned int windowBgColor = 0x000000;
    unsigned int titleBarColor = 0x1F252F;
    unsigned int panelColor = 0x000000;
    unsigned int navBarColor = 0x29303E;
    unsigned int buttonColor = 0x391E37;
    unsigned int borderColor = 0x4C576C;
    unsigned int textColor = 0xEBEEF5;
    unsigned int textMutedColor = 0x969FB1;
    unsigned int successColor = 0x34D378;
    unsigned int dangerColor = 0xE23E4E;

    int uiScalePercent = 100; // 100..150, applied live through ImGui
    int fontSize = 15;       // UI font size in pixels
    int fontFamily = 0;      // 0 calibri, 1 segoe ui
    int cornerScale10 = 10;  // UI rounding multiplier, stored as x10
    int settingsTab = 0;     // 0 general, 1 instances, 2 appearance

    // Appearance: window shape and DX11 HD post-processing.
    bool roundedCorners = true;
    int cornerRadius = 18;   // native window corner radius in pixels
    int shaderMode = 1;      // 0 standard, 1 HD, 2 Ultra HD (4x MSAA)
    int sharpness = 30;      // 0..100
    int contrast = 12;       // 0..100
    int saturation = 10;     // 0..100
    int vignette = 10;       // 0..100
    int glow = 0;            // 0..100
};

struct ManagerState {
    std::vector<PhoneProfile> phones;
    AppSettings settings;
};

// Loads/saves a simple UTF-8 ini file next to the executable.
ManagerState loadManagerState(const std::string& path);
void saveManagerState(const std::string& path, const ManagerState& state);

// Turns a display name into a safe AVD identifier.
std::string sanitizeAvdName(const std::string& name, int id);

} // namespace emu
