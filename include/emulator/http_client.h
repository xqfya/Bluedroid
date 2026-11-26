#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace emu {
namespace platform {

using HttpProgress = std::function<void(size_t done, size_t total)>;

// HTTPS/HTTP GET with redirects and timeouts. Writes the response body to
// `memory` and/or `file`. `headers` is a CRLF-separated header block. Errors
// are returned through `error`; the function never throws.
bool httpGet(const std::string& url,
             std::string* memory,
             const std::filesystem::path* file,
             const std::string& headers,
             std::string& error,
             const HttpProgress& progress = HttpProgress(),
             int maxRedirects = 8);

} // namespace platform
} // namespace emu
