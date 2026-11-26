#pragma once

#include <functional>
#include <string>

namespace emu {

// Version baked into this build. version.json on GitHub is compared against it.
inline constexpr char kAppVersion[] = "1.0.3";

// GitHub endpoints used by the two-step updater.
inline constexpr char kVersionUrl[] =
    "https://raw.githubusercontent.com/xqfya/Bluedroid/refs/heads/main/version.json";
inline constexpr char kLatestReleaseUrl[] =
    "https://api.github.com/repos/xqfya/Bluedroid/releases/latest";

struct UpdateInfo {
    bool checked = false;        // version.json was fetched and parsed
    bool available = false;      // remote version is newer than kAppVersion
    std::string currentVersion;
    std::string remoteVersion;
    std::string downloadUrl;
    std::string releaseNotes;
    std::string downloadedPath;  // non-empty after a successful download
    std::string error;           // non-empty on network/parse failure
};

class Updater {
public:
    using ProgressCallback = std::function<void(size_t /*done*/, size_t /*total*/)>;

    // Step 1: fetch version.json. If (and only if) its "version" is newer than
    // kAppVersion, fetch the latest release, pick a download asset, and save it
    // under the per-user BlueDroid runtime\updates folder.
    //
    // Network/parse failures are returned in UpdateInfo::error instead of
    // throwing. The caller may ignore them for silent startup checks.
    static UpdateInfo checkAndDownload(const ProgressCallback& progress = {});
};

} // namespace emu
