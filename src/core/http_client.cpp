#include "emulator/http_client.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_NO_HEADERS
#define WINHTTP_NO_HEADERS nullptr
#endif
#else
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace emu {
namespace platform {

namespace {

std::string winError(const char* what) {
#ifdef _WIN32
    const DWORD code = GetLastError();
    return std::string(what) + " (Win32 error " + std::to_string(code) + ")";
#else
    return what;
#endif
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

std::string trim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Use an explicit User-Agent from the caller when it supplied one; this keeps
// the old WinHTTP agent strings (SDK installer vs. updater) on Windows.
std::string userAgent(const std::string& headers, const char* fallback) {
    std::istringstream in(headers);
    std::string line;
    while (std::getline(in, line)) {
        const std::string lowered = lowerCopy(line);
        const std::string prefix = "user-agent:";
        if (lowered.compare(0, prefix.size(), prefix) == 0) {
            const std::string value = trim(line.substr(prefix.size()));
            if (!value.empty()) return value;
        }
    }
    return fallback;
}

void reportProgress(const HttpProgress& progress, size_t& lastReported,
                    size_t done, size_t total) {
    if (!progress) return;
    try {
        const size_t step = total ? (total / 100) + 1 : (2 * 1024 * 1024);
        if (done >= lastReported && done - lastReported >= step) {
            progress(done, total);
            lastReported = done;
        }
    } catch (...) {
        // A throwing UI callback must not abort a download.
    }
}

#ifdef _WIN32

bool splitUrl(const std::string& url, std::wstring& host, INTERNET_PORT& port,
              std::wstring& path, bool& secure) {
    const std::wstring wide(url.begin(), url.end());
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

bool addRequestHeaders(HINTERNET request, const std::string& headers) {
    if (headers.empty()) return true;
    const std::wstring wide(headers.begin(), headers.end());
    return WinHttpAddRequestHeaders(request, wide.c_str(), (DWORD)-1,
                                    WINHTTP_ADDREQ_FLAG_ADD) != FALSE;
}

std::string resolveRedirect(const std::string& current, const std::string& location) {
    if (location.compare(0, 4, "http") == 0) return location;
    const size_t slash = current.find_last_of('/');
    if (slash == std::string::npos) return location;
    const std::string base = current.substr(0, slash + 1);
    if (!location.empty() && location[0] == '/') return base + location.substr(1);
    return base + location;
}

#else

struct CurlContext {
    std::string* memory = nullptr;
    std::ofstream* file = nullptr;
    HttpProgress progress;
    HttpCancel cancel;
    size_t done = 0;
    size_t lastReported = 0;
    bool writeFailed = false;

    bool cancelled() const { return cancel && cancel(); }

    void report(size_t total) {
        if (cancelled()) return;
        reportProgress(progress, lastReported, done, total);
    }
};

size_t writeCallback(char* data, size_t size, size_t count, void* userData) {
    auto* context = static_cast<CurlContext*>(userData);
    if (!context || context->cancelled()) return 0;
    const size_t bytes = size * count;
    if (context->memory) context->memory->append(data, bytes);
    if (context->file) {
        context->file->write(data, (std::streamsize)bytes);
        if (!*context->file) context->writeFailed = true;
    }
    context->done += bytes;
    return bytes;
}

#if LIBCURL_VERSION_NUM >= 0x072000
int progressCallback(void* userData, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t, curl_off_t) {
    auto* context = static_cast<CurlContext*>(userData);
    if (!context) return 0;
    if (context->cancelled()) return 1;
    context->report((size_t)dltotal);
    (void)dlnow;
    return 0;
}
#else
int progressCallback(void* userData, double dltotal, double dlnow,
                     double, double) {
    auto* context = static_cast<CurlContext*>(userData);
    if (!context) return 0;
    if (context->cancelled()) return 1;
    context->report((size_t)dltotal);
    (void)dlnow;
    return 0;
}
#endif

void initCurl() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::vector<std::string> splitHeaders(const std::string& headers) {
    std::vector<std::string> lines;
    std::istringstream in(headers);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trim(line).empty()) lines.push_back(line);
    }
    return lines;
}

#endif

} // namespace

bool httpGet(const std::string& url,
             std::string* memory,
             const fs::path* file,
             const std::string& headers,
             std::string& error,
             const HttpProgress& progress,
             const HttpCancel& cancel,
             int maxRedirects) {
    error.clear();
    const auto cancelled = [&]() { return cancel && cancel(); };
    if (cancelled()) {
        error = "cancelled by uninstall";
        return false;
    }
    if (!memory && !file) {
        error = "no output destination for HTTP GET";
        return false;
    }

    try {
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

#ifdef _WIN32
        std::string current = url;
        const int redirects = std::max(0, maxRedirects);
        const std::string agent = userAgent(headers, "BlueDroid/1.0");
        const std::wstring agentWide(agent.begin(), agent.end());
        for (int attempt = 0; attempt <= redirects; ++attempt) {
            std::wstring host, path;
            INTERNET_PORT port = 0;
            bool secure = false;
            if (!splitUrl(current, host, port, path, secure)) {
                error = "invalid URL: " + current;
                return false;
            }

            HINTERNET session = WinHttpOpen(agentWide.c_str(),
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
            if (!addRequestHeaders(request, headers)) {
                error = winError("WinHttpAddRequestHeaders failed");
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connect);
                WinHttpCloseHandle(session);
                return false;
            }
            if (!WinHttpSendRequest(request, WINHTTP_NO_HEADERS, 0,
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
                    const std::wstring loc(location);
                    current = resolveRedirect(current,
                        std::string(loc.begin(), loc.end()));
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
                if (cancelled()) {
                    error = "cancelled by uninstall";
                    ok = false;
                    break;
                }
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
                reportProgress(progress, lastReported, done, total);
                if (cancelled()) {
                    error = "cancelled by uninstall";
                    ok = false;
                    break;
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
            if (progress) {
                try { progress(done, total ? total : done); } catch (...) {}
            }
            return true;
        }
        error = "too many HTTP redirects for " + url;
        return false;
#else
        initCurl();
        CURL* curl = curl_easy_init();
        if (!curl) { error = "curl_easy_init failed"; return false; }

        CurlContext context;
        context.memory = memory;
        context.file = out ? &out : nullptr;
        context.progress = progress;
        context.cancel = cancel;

        const std::string agent = userAgent(headers, "BlueDroid/1.0");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)std::max(0, maxRedirects));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 600000L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
#if LIBCURL_VERSION_NUM >= 0x072000
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
#else
        curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, &progressCallback);
        curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &context);
#endif
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (progress || cancel) ? 0L : 1L);

        curl_slist* headerList = nullptr;
        for (const std::string& line : splitHeaders(headers))
            headerList = curl_slist_append(headerList, line.c_str());
        if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        const CURLcode result = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

        if (headerList) curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);

        if (cancelled()) {
            error = "cancelled by uninstall";
            return false;
        }
        if (result != CURLE_OK) {
            error = std::string("HTTP request failed: ") + curl_easy_strerror(result);
            return false;
        }
        if (responseCode < 200 || responseCode >= 300) {
            error = "HTTP " + std::to_string(responseCode) + " from " + url;
            return false;
        }
        if (context.writeFailed || (out && !out)) {
            error = "failed while writing download";
            return false;
        }
        if (progress) context.report(context.done);
        return true;
#endif
    } catch (...) {
        if (error.empty()) error = "unexpected HTTP client error";
        return false;
    }
}

} // namespace platform
} // namespace emu
