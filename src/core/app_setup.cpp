#include "emulator/app_setup.h"
#include "emulator/http_client.h"
#include "emulator/logger.h"
#include "emulator/platform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace emu {
namespace {

#ifdef _WIN32
static const char* const kSystemSdk = "C:\\AndroidSDK";
#else
static const char* const kSystemSdk = "/opt/android-sdk";
#endif
// Keep the historical WinHTTP agent string on Windows; the shared HTTP client
// also uses it as the libcurl User-Agent on POSIX.
const char* const kSdkHeaders = "User-Agent: BlueDroid SDK Installer\r\n";

fs::path localRoot() {
    return fs::path(emu::platform::dataRoot());
}

const fs::path& rootPath() {
    static const fs::path path = localRoot();
    return path;
}

void setEnv(const char* name, const fs::path& value) {
    // The platform helper updates both the process environment and the CRT
    // snapshot on Windows, and uses setenv() on POSIX.
    emu::platform::setEnvironment(name, value.string());
}

bool hasUsableSdk(const fs::path& sdk) {
    std::error_code ec;
    const std::string suffix = emu::platform::exeSuffix();
    return fs::is_regular_file(sdk / "platform-tools" / ("adb" + suffix), ec) &&
           fs::is_regular_file(sdk / "emulator" / ("emulator" + suffix), ec) &&
           fs::exists(sdk / "system-images" / "android-33" /
                      "google_apis_playstore" / "x86_64" / "system.img", ec);
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

std::string xmlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
        else if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
        else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
        else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
        else if (s.compare(i, 5, "&apos;") == 0) { out += '\''; i += 4; }
        else out += s[i];
    }
    return out;
}

// The Google manifests use namespace-prefixed root elements but plain child
// tags. Matching the local name keeps the parser independent of the schema
// version (repository2, sys-img2/01, sys-img2/02, and so on).
std::string localName(const std::string& tag) {
    size_t begin = tag.find_first_not_of("</ ");
    if (begin == std::string::npos) return std::string();
    size_t colon = tag.find(':', begin);
    size_t end = tag.find_first_of(" />", begin);
    if (end == std::string::npos) end = tag.size();
    size_t nameBegin = (colon != std::string::npos && colon < end) ? colon + 1 : begin;
    return tag.substr(nameBegin, end - nameBegin);
}

bool tagBlock(const std::string& text, const char* name, std::string* value,
              size_t from = 0) {
    if (!value) return false;
    const std::string openPrefix = "<" + std::string(name);
    const std::string closeTag = "</" + std::string(name) + ">";
    size_t pos = text.find(openPrefix, from);
    while (pos != std::string::npos) {
        // Do not confuse <url> with <urls>.
        const size_t afterName = pos + openPrefix.size();
        if (afterName < text.size() && text[afterName] != '>' &&
            text[afterName] != ' ' && text[afterName] != '/' &&
            text[afterName] != ':') {
            pos = text.find(openPrefix, afterName);
            continue;
        }
        size_t openEnd = text.find('>', pos);
        if (openEnd == std::string::npos) return false;
        size_t close = text.find(closeTag, openEnd + 1);
        if (close == std::string::npos) return false;
        *value = xmlDecode(text.substr(openEnd + 1, close - openEnd - 1));
        return true;
    }
    return false;
}

std::string tagValue(const std::string& text, const char* name) {
    std::string value;
    return tagBlock(text, name, &value) ? value : std::string();
}

bool attributeValue(const std::string& tag, const char* name, std::string* value) {
    if (!value) return false;
    const std::string needle = std::string(name) + "=\"";
    size_t pos = tag.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    size_t end = tag.find('"', pos);
    if (end == std::string::npos) return false;
    *value = xmlDecode(tag.substr(pos, end - pos));
    return true;
}

size_t elementEnd(const std::string& xml, size_t from, const char* name) {
    size_t pos = from;
    while ((pos = xml.find("</", pos)) != std::string::npos) {
        size_t close = xml.find('>', pos + 2);
        if (close == std::string::npos) return std::string::npos;
        if (localName(xml.substr(pos, close - pos + 1)) == name)
            return close + 1;
        pos = close + 1;
    }
    return std::string::npos;
}

std::vector<std::string> packageBlocks(const std::string& xml, const std::string& path) {
    std::vector<std::string> blocks;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t openEnd = xml.find('>', pos + 1);
        if (openEnd == std::string::npos) break;
        const std::string tag = xml.substr(pos, openEnd - pos + 1);
        if (localName(tag) == "remotePackage") {
            const size_t close = elementEnd(xml, openEnd + 1, "remotePackage");
            if (close == std::string::npos) break;
            std::string found;
            if (attributeValue(tag, "path", &found) && found == path)
                blocks.push_back(xml.substr(pos, close - pos));
            pos = close;
        } else {
            pos = openEnd + 1;
        }
    }
    return blocks;
}

struct PackageVersion {
    int major = 0;
    int minor = 0;
    int micro = 0;

    bool operator<(const PackageVersion& other) const {
        if (major != other.major) return major < other.major;
        if (minor != other.minor) return minor < other.minor;
        return micro < other.micro;
    }
};

PackageVersion packageVersion(const std::string& block) {
    std::string rev;
    tagBlock(block, "revision", &rev);
    PackageVersion v;
    v.major = std::atoi(tagValue(rev, "major").c_str());
    v.minor = std::atoi(tagValue(rev, "minor").c_str());
    v.micro = std::atoi(tagValue(rev, "micro").c_str());
    return v;
}

const char* currentHostOs() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

bool hostMatches(const std::string& archiveHost, const std::string& wantedHost) {
    if (wantedHost.empty()) return true; // system images are host-neutral
    const std::string host = lowerCopy(archiveHost);
    if (wantedHost == "windows") return host.find("windows") != std::string::npos;
    if (wantedHost == "darwin") {
        return host.find("darwin") != std::string::npos ||
               host.find("mac") != std::string::npos;
    }
    if (wantedHost == "linux") return host.find("linux") != std::string::npos;
    return true;
}

std::string archiveUrl(const std::string& packageBlock, const std::string& wantedHost) {
    size_t pos = 0;
    while ((pos = packageBlock.find("<archive", pos)) != std::string::npos) {
        size_t openEnd = packageBlock.find('>', pos);
        if (openEnd == std::string::npos) break;
        const size_t close = elementEnd(packageBlock, openEnd + 1, "archive");
        if (close == std::string::npos) break;
        const std::string archive = packageBlock.substr(pos, close - pos);
        if (!hostMatches(tagValue(archive, "host-os"), wantedHost)) {
            pos = close;
            continue;
        }
        const std::string url = tagValue(archive, "url");
        // Keep the Windows behaviour byte-for-byte: only ZIP archives are
        // installed by this bootstrap path.
        if (!url.empty() && lowerCopy(url).find(".zip") != std::string::npos)
            return url;
        pos = close;
    }
    return std::string();
}

// Pick the newest non-obsolete archive for an exact package path. The system
// image manifest contains several revisions of the same package, so comparing
// revisions is safer than relying on document order.
std::string latestPackageUrl(const std::string& xml, const std::string& path,
                             const std::string& wantedHost) {
    std::string best;
    PackageVersion bestVersion;
    bool haveBest = false;
    for (const std::string& block : packageBlocks(xml, path)) {
        const size_t openEnd = block.find('>');
        if (openEnd == std::string::npos) continue;
        std::string obsolete;
        if (attributeValue(block.substr(0, openEnd + 1), "obsolete", &obsolete) &&
            lowerCopy(obsolete) == "true") {
            continue;
        }
        const std::string url = archiveUrl(block, wantedHost);
        if (url.empty()) continue;
        const PackageVersion version = packageVersion(block);
        if (!haveBest || bestVersion < version) {
            best = url;
            bestVersion = version;
            haveBest = true;
        }
    }
    return best;
}

std::string resolvePackageUrl(const std::string& manifestUrl, const std::string& url) {
    const std::string low = lowerCopy(url);
    if (low.compare(0, 7, "http://") == 0 || low.compare(0, 8, "https://") == 0)
        return url;
    const size_t slash = manifestUrl.find_last_of('/');
    if (slash == std::string::npos) return url;
    return manifestUrl.substr(0, slash + 1) + url;
}

void copyRecursive(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (fs::recursive_directory_iterator it(src, ec), end;
         !ec && it != end; it.increment(ec)) {
        const fs::path relative = it->path().lexically_relative(src);
        const fs::path target = dst / relative;
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (it->is_regular_file(ec)) {
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        }
        ec.clear();
    }
}

bool moveTree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!fs::exists(src, ec)) return false;
    fs::create_directories(dst.parent_path(), ec);
    if (fs::exists(dst, ec)) fs::remove_all(dst, ec);
    fs::rename(src, dst, ec);
    if (!ec) return true;
    copyRecursive(src, dst);
    fs::remove_all(src, ec);
    return fs::exists(dst, ec);
}

bool extractZip(const fs::path& zip, const fs::path& staging, const char* label) {
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
#ifdef _WIN32
    const std::string cmd = "\"C:\\Windows\\System32\\tar.exe\" -xf \"" +
                            zip.string() + "\" -C \"" + staging.string() + "\"";
#else
    const std::string cmd = "tar -xf \"" + zip.string() + "\" -C \"" +
                            staging.string() + "\"";
#endif
    if (!AppSetup::runHidden(cmd, 0)) {
        LOG_ERROR("%s extraction failed for %s", label, zip.string().c_str());
        return false;
    }
    return fs::exists(staging, ec) && !fs::is_empty(staging, ec);
}

// SDK ZIPs sometimes contain the package folder and sometimes place the files
// at the root. Locate the directory that actually holds the expected binary.
fs::path extractedSource(const fs::path& staging, const char* preferredName,
                         const char* marker) {
    std::error_code ec;
    const fs::path preferred = staging / preferredName;
    if (fs::exists(preferred / marker, ec)) return preferred;
    if (fs::exists(staging / marker, ec)) return staging;
    for (fs::directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && fs::exists(it->path() / marker, ec))
            return it->path();
    }
    return fs::path();
}

// Installer state. Scalars are atomic; strings are protected by a mutex so a
// UI thread polling status() never sees a partially updated message.
std::atomic<bool> g_running{false};
std::atomic<bool> g_done{false};
std::atomic<bool> g_sdkReady{false};
std::atomic<int> g_percent{0};
std::mutex g_statusMutex;
std::string g_stage;
std::string g_error;
std::string g_warning;

std::mutex g_callbackMutex;
AppSetup::ProgressCallback g_callback;

void publish(const char* stage, int percent, const char* error = nullptr) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (stage) g_stage = stage;
        if (error) g_error = error;
    }
    g_percent.store(std::max(0, std::min(100, percent)));

    SetupStatus snapshot;
    snapshot.running = g_running.load();
    snapshot.done = g_done.load();
    snapshot.percent = g_percent.load();
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        snapshot.stage = g_stage;
        snapshot.error = g_error;
        snapshot.warning = g_warning;
    }

    AppSetup::ProgressCallback callback;
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);
        callback = g_callback;
    }
    if (callback) {
        try { callback(snapshot); } catch (...) { /* UI callbacks must not stop setup */ }
    }
}

void publish(const std::string& stage, int percent, const char* error = nullptr) {
    publish(stage.c_str(), percent, error);
}

void writeDefaultEmulatorIni(const fs::path& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    f << "vmBackend=android\n"
      << "cpuCores=4\n"
      << "ramMB=4096\n"
      << "screenWidth=1080\n"
      << "screenHeight=1920\n"
      << "dpi=320\n"
      << "androidVersion=13\n"
      << "systemImage=android-33\n"
      << "instanceName=BlueDroid\n"
      << "enablePlayStore=true\n"
      << "enableGoogleServices=true\n"
      << "rootAccess=false\n"
      << "dataDir=" << AppSetup::dataDir() << "\n"
      << "sharedFolder=\n"
      << "theme=Custom\n"
      << "uiScalePercent=100\n"
      << "fpsCap=60\n"
      << "gpuMode=0\n"
      << "minimizeToTray=true\n"
      << "muteParkedAudio=false\n"
      << "performancePreset=Balanced\n";
}

void writeDefaultManagerIni(const fs::path& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return;
    // A fresh, empty manager: no phones until the user adds one.
    f << "[settings]\n"
      << "saveFps=1\n"
      << "antiAfk=1\n"
      << "keepAwake=1\n"
      << "previewMinutes=2\n"
      << "searchText=\n"
      << "statusFilter=0\n"
      << "listView=0\n"
      << "showResources=1\n"
      << "accentColor=16029439\n"
      << "useCustomColors=1\n"
      << "windowBgColor=0\n"
      << "titleBarColor=2041135\n"
      << "panelColor=0\n"
      << "navBarColor=2699326\n"
      << "buttonColor=3743287\n"
      << "borderColor=5003116\n"
      << "textColor=15462133\n"
      << "textMutedColor=9871281\n"
      << "successColor=3462008\n"
      << "dangerColor=14827086\n"
      << "fontSize=15\n"
      << "fontFamily=0\n"
      << "cornerScale10=10\n"
      << "settingsTab=0\n"
      << "uiScalePercent=100\n"
      << "fpsCap=60\n"
      << "gpuMode=0\n"
      << "minimizeToTray=1\n"
      << "muteParkedAudio=0\n"
      << "performancePreset=Balanced\n"
      << "firstRunSettingsSeen=0\n";
}

} // namespace

const std::string& AppSetup::root() {
    static const std::string value = rootPath().string();
    return value;
}

std::string AppSetup::configPath()  { return (rootPath() / "config" / "emulator.ini").string(); }
std::string AppSetup::profilesPath(){ return (rootPath() / "config" / "emulators.ini").string(); }
std::string AppSetup::thumbsDir()   { return (rootPath() / "thumbs").string(); }
std::string AppSetup::backupsDir()  { return (rootPath() / "backups").string(); }
std::string AppSetup::dataDir()     { return (rootPath() / "data").string(); }
std::string AppSetup::phoneDataDir(int port) {
    return (rootPath() / "data" / ("phone_" + std::to_string(port))).string();
}
std::string AppSetup::imagesDir()   { return (rootPath() / "images").string(); }
std::string AppSetup::avdDir()      { return (rootPath() / "avd").string(); }
std::string AppSetup::sdkDir()      { return (rootPath() / "sdk").string(); }
std::string AppSetup::runtimeDir()  { return (rootPath() / "runtime").string(); }
std::string AppSetup::tempDir()     { return (rootPath() / "temp").string(); }
std::string AppSetup::logPath()     { return (rootPath() / "logs" / "emulator.log").string(); }
std::string AppSetup::screenshotPath(){ return (rootPath() / "screenshot.ppm").string(); }

bool AppSetup::sdkReady() { return g_sdkReady.load(); }

SetupStatus AppSetup::status() {
    SetupStatus snapshot;
    snapshot.running = g_running.load();
    snapshot.done = g_done.load();
    snapshot.percent = g_percent.load();
    std::lock_guard<std::mutex> lock(g_statusMutex);
    snapshot.stage = g_stage;
    snapshot.error = g_error;
    snapshot.warning = g_warning;
    return snapshot;
}

void AppSetup::setWarning(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_warning = text;
    }
    LOG_WARN("Manager banner warning: %s", text.empty() ? "(cleared)" : text.c_str());
    publish(nullptr, g_percent.load());
}

void AppSetup::setProgressCallback(ProgressCallback cb) {
    std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_callback = std::move(cb);
}

bool AppSetup::runHidden(const std::string& cmd, int timeoutMs,
                          bool capture, std::string* output) {
    int exitCode = 1;
    const bool ok = emu::platform::runCommand(cmd, timeoutMs, capture, output, &exitCode);
    return ok && exitCode == 0;
}

bool AppSetup::ensure() {
    static std::once_flag once;
    bool ok = true;
    std::call_once(once, [&ok]() {
        try {
            const fs::path root = rootPath();
            const fs::path directories[] = {
                root / "config", root / "data", root / "images",
                root / "thumbs", root / "backups", root / "logs",
                root / "sdk", root / "avd", root / "runtime", root / "temp"
            };
            for (const fs::path& dir : directories) {
                std::error_code ec;
                fs::create_directories(dir, ec);
            }

            // Migrate an existing portable install, otherwise start clean.
            const fs::path config = root / "config" / "emulator.ini";
            if (!fs::exists(config)) {
                std::error_code ec;
                if (fs::exists("emulator.ini"))
                    fs::copy_file("emulator.ini", config, ec);
                if (!fs::exists(config)) writeDefaultEmulatorIni(config);
            }

            const fs::path profiles = root / "config" / "emulators.ini";
            if (!fs::exists(profiles)) {
                std::error_code ec;
                if (fs::exists("emulators.ini"))
                    fs::copy_file("emulators.ini", profiles, ec);
                if (!fs::exists(profiles)) writeDefaultManagerIni(profiles);
            }

            const fs::path systemSdk(kSystemSdk);
            const fs::path localSdk = root / "sdk";
            const bool systemReady = hasUsableSdk(systemSdk);
            const bool localReady = hasUsableSdk(localSdk);
            g_sdkReady.store(systemReady || localReady);
            const fs::path& chosenSdk = systemReady ? systemSdk : localSdk;

            setEnv("ANDROID_SDK_ROOT", chosenSdk);
            setEnv("ANDROID_HOME", chosenSdk);
            setEnv("ANDROID_AVD_HOME", root / "avd");
            setEnv("TMP", root / "temp");
            setEnv("TEMP", root / "temp");
#ifndef _WIN32
            // POSIX tools (tar, sh, adb wrappers) use TMPDIR for scratch files.
            setEnv("TMPDIR", root / "temp");
#endif

            if (g_sdkReady.load()) {
                g_done.store(true);
                g_percent.store(100);
                publish("SDK ready", 100);
            } else {
                g_done.store(false);
                g_percent.store(0);
                {
                    std::lock_guard<std::mutex> lock(g_statusMutex);
                    g_error.clear();
                }
                publish("Preparing SDK installer", 0);
                std::thread(&AppSetup::installSdkAsync).detach();
            }
        } catch (...) {
            ok = false;
        }
    });
    return ok;
}

void AppSetup::installSdkAsync() {
    g_running.store(true);
    g_done.store(false);
    g_percent.store(0);

    auto failed = [&](const std::string& why) {
        LOG_ERROR("SDK setup failed: %s", why.c_str());
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_error = why;
        }
        g_running.store(false);
        g_done.store(false);
        publish(nullptr, g_percent.load(), why.c_str());
    };

    try {
        const fs::path root = rootPath();
        const fs::path temp = root / "temp";
        const fs::path sdk = root / "sdk";

        const std::string repoManifest =
            "https://dl.google.com/android/repository/repository2-1.xml";
        const std::string sysImageManifest =
            "https://dl.google.com/android/repository/sys-img/"
            "google_apis_playstore/sys-img2-1.xml";
        const std::string imagePackage =
            "system-images;android-33;google_apis_playstore;x86_64";

#ifdef _WIN32
        const fs::path platformZip = temp / "platform-tools-win.zip";
        const fs::path emulatorZip = temp / "emulator-win.zip";
#else
        const fs::path platformZip = temp / "platform-tools.zip";
        const fs::path emulatorZip = temp / "emulator.zip";
#endif
        const fs::path systemImageZip = temp / "android-33-google-apis-playstore-x86_64.zip";
        const std::string suffix = emu::platform::exeSuffix();
        const std::string adbMarker = "adb" + suffix;
        const std::string emulatorMarker = "emulator" + suffix;

        auto download = [&](const std::string& label, const std::string& url,
                            const fs::path& dest, int from, int to) -> bool {
            std::error_code ec;
            fs::remove(dest, ec);
            int lastPercent = from - 1;
            publish(label, from);
            LOG_INFO("Downloading %s from %s", label.c_str(), url.c_str());
            std::string httpError;
            const bool ok = emu::platform::httpGet(url, nullptr, &dest,
                kSdkHeaders, httpError, [&](size_t done, size_t total) {
                    int percent = from;
                    if (total > 0) {
                        const int span = std::max(1, to - from);
                        percent = from + (int)((done * 100ULL) / total) * span / 100;
                    } else {
                        percent = from + (int)((done / (2 * 1024 * 1024)) %
                                               (size_t)std::max(1, to - from + 1));
                    }
                    percent = std::max(from, std::min(to, percent));
                    if (percent > lastPercent) {
                        lastPercent = percent;
                        publish(label, percent);
                    }
                });
            if (!ok || !fs::exists(dest, ec) || fs::file_size(dest, ec) == 0) {
                LOG_ERROR("Download failed: %s (%s)", label.c_str(), httpError.c_str());
                return false;
            }
            LOG_INFO("Downloaded %s (%llu bytes)", label.c_str(),
                     (unsigned long long)fs::file_size(dest, ec));
            return true;
        };

        auto install = [&](const std::string& label, const fs::path& zip,
                           const fs::path& staging, const char* preferredName,
                           const char* marker, const fs::path& dest,
                           int percent) -> bool {
            publish("Installing " + label, percent);
            if (!extractZip(zip, staging, label.c_str())) {
                failed("Failed to extract " + label);
                return false;
            }
            const fs::path source = extractedSource(staging, preferredName, marker);
            if (source.empty()) {
                LOG_ERROR("Archive for %s did not contain %s", label.c_str(), marker);
                failed("Archive for " + label + " is missing " + marker);
                return false;
            }
            if (!moveTree(source, dest)) {
                failed("Failed to install " + label);
                return false;
            }
            std::error_code ec;
            fs::remove(zip, ec);
            fs::remove_all(staging, ec);
            LOG_INFO("Installed %s to %s", label.c_str(), dest.string().c_str());
            return true;
        };

        // 1. Read Google's official repository manifests over HTTPS. No Java,
        // sdkmanager.bat, or license prompts are involved.
        publish("Reading Google SDK manifests", 2);
        std::string repositoryXml;
        std::string sysImageXml;
        std::string httpError;
        if (!emu::platform::httpGet(repoManifest, &repositoryXml, nullptr,
                                    kSdkHeaders, httpError)) {
            failed("Failed to read the Android repository manifest: " + httpError);
            return;
        }
        if (!emu::platform::httpGet(sysImageManifest, &sysImageXml, nullptr,
                                    kSdkHeaders, httpError)) {
            failed("Failed to read the Android system image manifest: " + httpError);
            return;
        }
        LOG_INFO("Repository manifest: %zu bytes; system image manifest: %zu bytes",
                 repositoryXml.size(), sysImageXml.size());

        const std::string platformPath =
            resolvePackageUrl(repoManifest,
                latestPackageUrl(repositoryXml, "platform-tools", currentHostOs()));
        const std::string emulatorPath =
            resolvePackageUrl(repoManifest,
                latestPackageUrl(repositoryXml, "emulator", currentHostOs()));
        const std::string systemImagePath =
            resolvePackageUrl(sysImageManifest,
                latestPackageUrl(sysImageXml, imagePackage, ""));
        if (platformPath.empty() || emulatorPath.empty() || systemImagePath.empty()) {
            LOG_ERROR("Manifest lookup failed: platform=%s emulator=%s image=%s",
                      platformPath.c_str(), emulatorPath.c_str(),
                      systemImagePath.c_str());
            failed("Required SDK packages are missing from Google's manifests");
            return;
        }
        LOG_INFO("SDK packages: platform-tools=%s emulator=%s image=%s",
                 platformPath.c_str(), emulatorPath.c_str(), systemImagePath.c_str());
        publish("Downloading SDK packages", 5);

        // 2. Download and extract each ZIP straight into its final SDK folder.
        if (!download("Downloading platform-tools", platformPath, platformZip, 6, 24))
            { failed("Failed to download platform-tools"); return; }
        if (!install("platform-tools", platformZip, temp / "platform-tools-stage",
                     "platform-tools", adbMarker.c_str(), sdk / "platform-tools", 25))
            return;

        if (!download("Downloading Android emulator", emulatorPath, emulatorZip, 29, 58))
            { failed("Failed to download the Android emulator"); return; }
        if (!install("emulator", emulatorZip, temp / "emulator-stage",
                     "emulator", emulatorMarker.c_str(), sdk / "emulator", 59))
            return;

        if (!download("Downloading Android 13 system image", systemImagePath,
                      systemImageZip, 63, 94))
            { failed("Failed to download the Android 13 system image"); return; }
        if (!install("Android 13 system image", systemImageZip,
                     temp / "system-image-stage", "", "system.img",
                     sdk / "system-images" / "android-33" /
                         "google_apis_playstore" / "x86_64", 95))
            return;

        if (!hasUsableSdk(sdk)) {
            failed("SDK installation finished but adb/emulator/system image is missing");
            return;
        }

        g_sdkReady.store(true);
        g_percent.store(100);
        g_running.store(false);
        g_done.store(true);
        publish("SDK ready", 100);
    } catch (...) {
        failed("Unexpected SDK installer error");
    }
}

} // namespace emu
