#pragma once
#include <string>
#include <map>

namespace emu {

struct EmulatorConfig {
    std::string vmBackend = "qemu";          // qemu | hyperv | kvm (host-dependent)
    int cpuCores = 4;
    int ramMB = 4096;
    int screenWidth = 1080;
    int screenHeight = 1920;
    int dpi = 320;
    std::string androidVersion = "13";       // Android release
    std::string systemImage = "android-33";  // system image tag
    std::string instanceName = "BlueDroid";   // instance name shown in UI
    bool enablePlayStore = true;
    bool enableGoogleServices = true;
    bool rootAccess = false;
    std::string dataDir;                     // per-instance data directory
    std::string sharedFolder;                // host<->guest shared folder

    // Global gamer-facing preferences. These are written back to emulator.ini
    // when the user changes a setting in the Manager.
    std::string theme;                       // "Custom"; no preset themes in the release
    int uiScalePercent = 100;                // 100..150
    int fpsCap = 60;                         // 30 / 60 / 120 capture cap
    int gpuMode = 0;                         // 0 host (DirectX), 1 swiftshader (software)
    bool minimizeToTray = true;
    bool muteParkedAudio = false;
    std::string performancePreset = "Balanced";

    std::map<std::string,std::string> extraProps;
};

class ConfigManager {
public:
    static ConfigManager& instance();
    EmulatorConfig& config();
    bool load(const std::string& path);
    bool save(const std::string& path) const;
private:
    ConfigManager();
    EmulatorConfig cfg_;
};

} // namespace emu