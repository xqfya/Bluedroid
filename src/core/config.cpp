#include "emulator/config.h"
#include "emulator/logger.h"
#include <fstream>
#include <sstream>

namespace emu {

ConfigManager::ConfigManager() {
    cfg_.dataDir = "./data";
}

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

EmulatorConfig& ConfigManager::config() { return cfg_; }

bool ConfigManager::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) { LOG_WARN("Config file not found: %s", path.c_str()); return false; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        if (k == "cpuCores") cfg_.cpuCores = std::stoi(v);
        else if (k == "ramMB") cfg_.ramMB = std::stoi(v);
        else if (k == "screenWidth") cfg_.screenWidth = std::stoi(v);
        else if (k == "screenHeight") cfg_.screenHeight = std::stoi(v);
        else if (k == "dpi") cfg_.dpi = std::stoi(v);
        else if (k == "androidVersion") cfg_.androidVersion = v;
        else if (k == "systemImage") cfg_.systemImage = v;
        else if (k == "instanceName") cfg_.instanceName = v;
        else if (k == "enablePlayStore") cfg_.enablePlayStore = (v == "true" || v == "1");
        else if (k == "enableGoogleServices") cfg_.enableGoogleServices = (v == "true" || v == "1");
        else if (k == "rootAccess") cfg_.rootAccess = (v == "true" || v == "1");
        else if (k == "dataDir") cfg_.dataDir = v;
        else if (k == "sharedFolder") cfg_.sharedFolder = v;
        else if (k == "vmBackend") cfg_.vmBackend = v;
        else if (k == "theme") cfg_.theme = v;
        else if (k == "uiScalePercent") cfg_.uiScalePercent = std::stoi(v);
        else if (k == "fpsCap") cfg_.fpsCap = std::stoi(v);
        else if (k == "gpuMode") cfg_.gpuMode = std::stoi(v);
        else if (k == "minimizeToTray") cfg_.minimizeToTray = (v == "true" || v == "1");
        else if (k == "muteParkedAudio") cfg_.muteParkedAudio = (v == "true" || v == "1");
        else if (k == "performancePreset") cfg_.performancePreset = v;
    }
    LOG_INFO("Config loaded from %s", path.c_str());
    return true;
}

bool ConfigManager::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    const auto& c = cfg_;
    f << "vmBackend=" << c.vmBackend << "\n";
    f << "cpuCores=" << c.cpuCores << "\n";
    f << "ramMB=" << c.ramMB << "\n";
    f << "screenWidth=" << c.screenWidth << "\n";
    f << "screenHeight=" << c.screenHeight << "\n";
    f << "dpi=" << c.dpi << "\n";
    f << "androidVersion=" << c.androidVersion << "\n";
    f << "systemImage=" << c.systemImage << "\n";
    f << "instanceName=" << c.instanceName << "\n";
    f << "enablePlayStore=" << (c.enablePlayStore ? "true" : "false") << "\n";
    f << "enableGoogleServices=" << (c.enableGoogleServices ? "true" : "false") << "\n";
    f << "rootAccess=" << (c.rootAccess ? "true" : "false") << "\n";
    f << "dataDir=" << c.dataDir << "\n";
    f << "sharedFolder=" << c.sharedFolder << "\n";
    f << "theme=" << c.theme << "\n";
    f << "uiScalePercent=" << c.uiScalePercent << "\n";
    f << "fpsCap=" << c.fpsCap << "\n";
    f << "gpuMode=" << c.gpuMode << "\n";
    f << "minimizeToTray=" << (c.minimizeToTray ? "true" : "false") << "\n";
    f << "muteParkedAudio=" << (c.muteParkedAudio ? "true" : "false") << "\n";
    f << "performancePreset=" << c.performancePreset << "\n";
    return true;
}

} // namespace emu