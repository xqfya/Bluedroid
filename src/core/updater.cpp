#include "emulator/updater.h"
#include "emulator/app_setup.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

namespace emu {
namespace {

namespace fs = std::filesystem;

// GitHub release assets are expected as a ZIP first, then a standalone EXE.
// The first asset is still used as a fallback so an unusual release layout
// does not break the updater.
constexpr char kPreferredAssetExtension[] = ".zip";
constexpr wchar_t kApiHeaders[] =
    L"User-Agent: BlueDroid-Updater/1.0.0\r\n"
    L"Accept: application/vnd.github+json\r\n";

std::string winError(const char* what) {
    return std::string(what) + " (Win32 error " + std::to_string(GetLastError()) + ")";
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                        out.data(), n);
    return out;
}

bool splitUrl(const std::string& url, std::wstring& host, INTERNET_PORT& port,
              std::wstring& path, bool& secure) {
    const std::wstring wide = utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[8192] = {};
    parts.lpszHostName = hostBuf;
    parts.dwHostNameLength = _countof(hostBuf);
    parts.lpszUrlPath = pathBuf;
    parts.dwUrlPathLength = _countof(pathBuf);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return false;
    host = hostBuf;
    path = pathBuf[0] ? pathBuf : L"/";
    port = parts.nPort;
    secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return !host.empty();
}

std::string resolveRedirect(const std::string& current, const std::string& location) {
    const std::string low = location;
    if (low.compare(0, 4, "http") == 0) return location;
    const size_t slash = current.find_last_of('/');
    if (slash == std::string::npos) return location;
    const std::string base = current.substr(0, slash + 1);
    if (!location.empty() && location[0] == '/') return base + location.substr(1);
    return base + location;
}

// HTTPS GET with redirects, timeouts and real byte progress. Writes either to
// memory (JSON) or to disk (release asset). All failures are reported through
// `error` so callers never need to catch exceptions.
bool httpGet(const std::string& url, std::string* memory, const fs::path* file,
             std::string& error, const wchar_t* headers,
             const Updater::ProgressCallback& progress) {
    if (!memory && !file) {
        error = "no output destination for HTTP GET";
        return false;
    }

    std::ofstream out;
    if (file) {
        std::error_code ec;
        fs::create_directories(file->parent_path(), ec);
        out.open(*file, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not open download destination: " + file->string();
            return false;
        }
    }

    std::string current = url;
    for (int redirect = 0; redirect < 8; ++redirect) {
        std::wstring host, path;
        INTERNET_PORT port = 0; // WinHttpCrackUrl fills this (e.g. 443).
        bool secure = false;
        if (!splitUrl(current, host, port, path, secure)) {
            error = "invalid URL: " + current;
            return false;
        }

        HINTERNET session = WinHttpOpen(L"BlueDroid Updater",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { error = winError("WinHttpOpen failed"); return false; }
        WinHttpSetTimeouts(session, 15000, 15000, 60000, 0);

        HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
        if (!connect) {
            error = winError("WinHttpConnect failed");
            WinHttpCloseHandle(session);
            return false;
        }
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            error = winError("WinHttpOpenRequest failed");
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (headers) {
            WinHttpAddRequestHeaders(request, headers, (DWORD)-1,
                                     WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (!WinHttpSendRequest(request, nullptr, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            error = winError("WinHttpSendRequest failed");
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            error = winError("WinHttpReceiveResponse failed");
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE |
            WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

        if (status >= 300 && status < 400) {
            wchar_t location[2048] = {};
            DWORD locationSize = sizeof(location);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX, location, &locationSize,
                    WINHTTP_NO_HEADER_INDEX)) {
                current = resolveRedirect(current, wideToUtf8(location));
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                continue;
            }
        }
        if (status != 200) {
            error = "HTTP " + std::to_string(status) + " from " + current;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        size_t total = 0;
        DWORD totalDword = 0;
        DWORD totalSize = sizeof(totalDword);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH |
                WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                &totalDword, &totalSize, WINHTTP_NO_HEADER_INDEX)) {
            total = totalDword;
        }

        size_t done = 0;
        size_t lastReported = 0;
        char chunk[64 * 1024];
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk, sizeof(chunk), &read)) {
                error = winError("WinHttpReadData failed");
                ok = false;
                break;
            }
            if (read == 0) break;
            done += read;
            if (memory) memory->append(chunk, read);
            if (out) out.write(chunk, read);
            if (progress) {
                const size_t step = total ? (total / 100) + 1 : (2 * 1024 * 1024);
                if (done - lastReported >= step) {
                    progress(done, total);
                    lastReported = done;
                }
            }
        }
        if (out && (!ok || !out)) {
            if (error.empty()) error = "failed while writing download";
            ok = false;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (!ok) return false;
        if (progress) progress(done, total ? total : done);
        return true;
    }

    error = "too many HTTP redirects for " + url;
    return false;
}

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

std::string chooseAssetUrl(const std::vector<Asset>& assets, std::string& error) {
    if (assets.empty()) {
        error = "release has no downloadable assets";
        return {};
    }

    const char* extensions[] = { kPreferredAssetExtension, ".exe", ".7z" };
    for (const char* ext : extensions) {
        for (const Asset& asset : assets) {
            if (endsWith(lowerCopy(stripQuery(asset.url)), ext) ||
                endsWith(lowerCopy(asset.name), ext)) {
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
    if (!httpGet(kVersionUrl, &versionJson, nullptr, error, nullptr, {})) {
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
    if (!httpGet(kLatestReleaseUrl, &releaseJson, nullptr, error, kApiHeaders, {})) {
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
    if (!httpGet(info.downloadUrl, nullptr, &destination, error, nullptr, progress)) {
        info.error = "Download failed: " + error;
        return info;
    }
    info.downloadedPath = destination.string();
    return info;
}

} // namespace emu
