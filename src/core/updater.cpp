#include "emulator/updater.h"
#include "emulator/app_setup.h"
#include "emulator/http_client.h"
#include "emulator/platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace emu {
namespace {

namespace fs = std::filesystem;

// Preferred release asset per platform. The first asset is still used as a
// fallback so an unusual release layout does not break the updater.
#ifdef _WIN32
constexpr char kPreferredAssetExtension[] = ".zip";
#elif defined(__APPLE__)
constexpr char kPreferredAssetExtension[] = ".dmg";
#else
constexpr char kPreferredAssetExtension[] = ".AppImage";
#endif
constexpr char kApiHeaders[] =
    "User-Agent: BlueDroid-Updater/1.0.3\r\n"
    "Accept: application/vnd.github+json\r\n";

bool skipWhitespace(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return i < s.size();
}

void appendUtf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool readUnicodeEscape(const std::string& s, size_t& i, unsigned int& cp) {
    if (i + 4 > s.size()) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
        const int v = hexDigit(s[i++]);
        if (v < 0) return false;
        cp = (cp << 4) | (unsigned int)v;
    }
    return true;
}

// Small JSON string reader. Handles escapes, \\u sequences and surrogate
// pairs so GitHub release notes do not need a third-party JSON dependency.
bool parseJsonString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= s.size()) return false;
        const char e = s[i++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned int cp = 0;
                if (!readUnicodeEscape(s, i, cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 5 < s.size() &&
                    s[i] == '\\' && s[i + 1] == 'u') {
                    size_t save = i;
                    i += 2;
                    unsigned int low = 0;
                    if (readUnicodeEscape(s, i, low) &&
                        low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        i = save;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool findKeyString(const std::string& json, const std::string& key,
                   size_t from, std::string& out, size_t* after = nullptr) {
    const std::string needle = "\"" + key + "\"";
    const size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return false;
    size_t i = pos + needle.size();
    if (!skipWhitespace(json, i) || json[i] != ':') return false;
    ++i;
    if (!skipWhitespace(json, i)) return false;
    if (!parseJsonString(json, i, out)) return false;
    if (after) *after = i;
    return true;
}

size_t findMatching(const std::string& s, size_t open, char openCh,
                    char closeCh, size_t end) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = open; i < end; ++i) {
        const char c = s[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == openCh) ++depth;
        else if (c == closeCh && --depth == 0) return i;
    }
    return std::string::npos;
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

std::string stripQuery(const std::string& url) {
    const size_t cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}

bool endsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<int> versionParts(std::string v) {
    std::vector<int> parts;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(v.begin());
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && !std::isdigit((unsigned char)v[i])) ++i;
        if (i >= v.size()) break;
        int value = 0;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) {
            value = value * 10 + (v[i] - '0');
            ++i;
        }
        parts.push_back(value);
        while (i < v.size() && v[i] != '.') ++i;
        if (i < v.size() && v[i] == '.') ++i;
    }
    return parts;
}

int compareVersions(const std::string& a, const std::string& b) {
    const auto pa = versionParts(a);
    const auto pb = versionParts(b);
    const size_t n = (std::max)(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        const int av = i < pa.size() ? pa[i] : 0;
        const int bv = i < pb.size() ? pb[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

struct Asset {
    std::string url;
    std::string name;
};

std::vector<Asset> parseAssets(const std::string& releaseJson) {
    std::vector<Asset> assets;
    size_t i = releaseJson.find("\"assets\"");
    if (i == std::string::npos) return assets;
    i += 8; // length of "\"assets\""
    if (!skipWhitespace(releaseJson, i) || releaseJson[i] != ':') return assets;
    ++i;
    if (!skipWhitespace(releaseJson, i) || releaseJson[i] != '[') return assets;
    const size_t end = findMatching(releaseJson, i, '[', ']', releaseJson.size());
    if (end == std::string::npos) return assets;

    size_t pos = i + 1;
    while (pos < end) {
        skipWhitespace(releaseJson, pos);
        if (pos >= end) break;
        if (releaseJson[pos] == ',') { ++pos; continue; }
        if (releaseJson[pos] != '{') { ++pos; continue; }
        const size_t close = findMatching(releaseJson, pos, '{', '}', end);
        if (close == std::string::npos) break;
        const std::string object = releaseJson.substr(pos + 1, close - pos - 1);
        Asset asset;
        findKeyString(object, "browser_download_url", 0, asset.url);
        findKeyString(object, "name", 0, asset.name);
        if (!asset.url.empty()) assets.push_back(std::move(asset));
        pos = close + 1;
    }
    return assets;
}

std::string releaseAssetToken() {
    const bool arm64 = emu::platform::systemArchitecture() == "arm64";
#if defined(_WIN32)
    return arm64 ? "win-arm64" : "win-x64";
#elif defined(__APPLE__)
    return arm64 ? "macos-arm64" : "macos-x64";
#else
    return arm64 ? "linux-arm64" : "linux-x64";
#endif
}

std::string chooseAssetUrl(const std::vector<Asset>& assets, std::string& error) {
    if (assets.empty()) {
        error = "release has no downloadable assets";
        return {};
    }

    // Preferred path: an asset explicitly named for this OS and CPU. Releases
    // from before multi-arch packages fall through to the extension rules.
    const std::string token = releaseAssetToken();
    for (const Asset& asset : assets) {
        const std::string name = lowerCopy(asset.name.empty() ? stripQuery(asset.url)
                                                             : asset.name);
        if (name.find(token) != std::string::npos) return asset.url;
    }

#if defined(_WIN32)
    const char* extensions[] = { kPreferredAssetExtension, ".exe", ".7z" };
#elif defined(__APPLE__)
    const char* extensions[] = { kPreferredAssetExtension, ".zip", ".tar.gz" };
#else
    const char* extensions[] = { kPreferredAssetExtension, ".tar.gz", ".zip" };
#endif
    for (const char* ext : extensions) {
        const std::string wanted = lowerCopy(ext);
        for (const Asset& asset : assets) {
            if (endsWith(lowerCopy(stripQuery(asset.url)), wanted) ||
                endsWith(lowerCopy(asset.name), wanted)) {
                return asset.url;
            }
        }
    }
    // No recognised extension: use assets[0] as the user requested.
    return assets.front().url;
}

std::string fileNameFromUrl(const std::string& url) {
    std::string name = stripQuery(url);
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty()) return "BlueDroidUpdate.zip";
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == '?' || c == '*' || c == ':' ||
            c == '|' || c == '"' || c == '<' || c == '>') {
            c = '_';
        }
    }
    return name;
}

} // namespace

UpdateInfo Updater::checkAndDownload(const ProgressCallback& progress) {
    UpdateInfo info;
    info.currentVersion = kAppVersion;

    // Step 1 - version.json.
    std::string versionJson;
    std::string error;
    if (!emu::platform::httpGet(kVersionUrl, &versionJson, nullptr, "", error)) {
        info.error = "Update check failed: " + error;
        return info;
    }
    info.checked = true;
    std::string remoteVersion;
    if (!findKeyString(versionJson, "version", 0, remoteVersion)) {
        info.error = "Update check failed: version.json has no \"version\"";
        return info;
    }
    info.remoteVersion = remoteVersion;

    // Step 2 - only download release metadata when this build is older.
    if (compareVersions(remoteVersion, kAppVersion) <= 0) return info;
    info.available = true;

    // Step 3 - latest release JSON and its first/matching asset.
    std::string releaseJson;
    if (!emu::platform::httpGet(kLatestReleaseUrl, &releaseJson, nullptr,
                                kApiHeaders, error)) {
        info.error = "Release lookup failed: " + error;
        return info;
    }
    findKeyString(releaseJson, "body", 0, info.releaseNotes);

    const std::vector<Asset> assets = parseAssets(releaseJson);
    info.downloadUrl = chooseAssetUrl(assets, error);
    if (info.downloadUrl.empty()) {
        info.error = "Release lookup failed: " + error;
        return info;
    }

    // Step 4 - save the asset under the per-user runtime folder.
    const fs::path dir = fs::path(AppSetup::runtimeDir()) / "updates";
    const fs::path destination = dir / fileNameFromUrl(info.downloadUrl);
    if (!emu::platform::httpGet(info.downloadUrl, nullptr, &destination, "",
                                error, progress)) {
        info.error = "Download failed: " + error;
        return info;
    }
    info.downloadedPath = destination.string();
    return info;
}

} // namespace emu
