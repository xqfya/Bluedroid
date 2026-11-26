#pragma once
#include <string>
#include <vector>

namespace emu {

struct SystemImage {
    std::string tag;          // e.g. "android-33"
    std::string name;         // display name
    std::string abi;          // x86_64 | arm64-v8a
    std::string path;         // local path to image
    bool hasPlayStore = false;
    bool hasGoogleApis = false;
    bool installed = false;
};

class ImageManager {
public:
    static ImageManager& instance();
    void refresh();
    const std::vector<SystemImage>& images() const { return images_; }
    bool download(const std::string& tag);
    bool install(const std::string& tag, const std::string& destDir);
    SystemImage* find(const std::string& tag);
private:
    ImageManager();
    std::vector<SystemImage> images_;
};

} // namespace emu