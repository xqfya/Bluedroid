#include "emulator/image_manager.h"
#include "emulator/logger.h"

namespace emu {

ImageManager::ImageManager() { refresh(); }

ImageManager& ImageManager::instance() {
    static ImageManager inst;
    return inst;
}

void ImageManager::refresh() {
    images_.clear();
    // Catalog of commonly used Android system images. In a real build these
    // would be fetched from the Android SDK repository (repository2-3.xml).
    auto add = [&](const char* tag, const char* name, const char* abi, bool ps, bool gms) {
        SystemImage si; si.tag = tag; si.name = name; si.abi = abi;
        si.hasPlayStore = ps; si.hasGoogleApis = gms; si.installed = false;
        images_.push_back(si);
    };
    add("android-34", "Android 14 (Upside Down Cake)", "x86_64", true, true);
    add("android-33", "Android 13 (Tiramisu)", "x86_64", true, true);
    add("android-32", "Android 12L (Snow Cone v2)", "x86_64", true, true);
    add("android-31", "Android 12 (Snow Cone)", "x86_64", true, true);
    add("android-30", "Android 11 (Red Velvet Cake)", "x86_64", true, true);
    add("android-29", "Android 10 (Quince Tart)", "x86_64", false, true);
    add("android-28", "Android 9 (Pie)", "x86_64", false, true);
    LOG_INFO("Image catalog refreshed: %zu images", images_.size());
}

bool ImageManager::download(const std::string& tag) {
    auto* si = find(tag);
    if (!si) { LOG_ERROR("Unknown image tag: %s", tag.c_str()); return false; }
    LOG_INFO("Downloading system image %s (stub - manifest-driven installer required)", tag.c_str());
    // Real: resolve system-images;android-XX;google_apis_playstore;x86_64 from
    // the official sys-img manifest and extract its ZIP directly.
    si->installed = true;
    return true;
}

bool ImageManager::install(const std::string& tag, const std::string& destDir) {
    auto* si = find(tag);
    if (!si) return false;
    si->path = destDir + "/" + tag;
    si->installed = true;
    LOG_INFO("Installed %s -> %s", tag.c_str(), si->path.c_str());
    return true;
}

SystemImage* ImageManager::find(const std::string& tag) {
    for (auto& si : images_) if (si.tag == tag) return &si;
    return nullptr;
}

} // namespace emu