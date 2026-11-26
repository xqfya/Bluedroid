// POSIX process/lock APIs (kill, setenv, ftruncate, flock) are hidden by the
// strict-standard feature set selected by -std=c++17 unless a feature macro is
// requested before the first system header.
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "emulator/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

namespace fs = std::filesystem;

namespace emu {
namespace platform {

namespace {

std::string envA(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string homeDir() {
    std::string home = envA("HOME");
    if (!home.empty()) return home;
#if !defined(_WIN32)
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
#endif
    return std::string();
}

} // namespace

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

#ifdef _WIN32

struct Process::Impl {
    HANDLE process = nullptr;
    DWORD pid = 0;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    int exitCode = 1;
    bool reaped = false;
    mutable std::mutex ioMutex;

    ~Impl() {
        close();
        if (process) {
            CloseHandle(process);
            process = nullptr;
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(ioMutex);
        closeLocked();
    }

    void closeLocked() {
        if (stdinWrite) { CloseHandle(stdinWrite); stdinWrite = nullptr; }
        if (stdoutRead) { CloseHandle(stdoutRead); stdoutRead = nullptr; }
    }

    bool running() const {
        return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    int wait(int timeoutMs, bool* timedOut) {
        if (timedOut) *timedOut = false;
        if (!process) return exitCode;
        if (reaped) return exitCode;
        const DWORD waitResult = WaitForSingleObject(
            process, timeoutMs <= 0 ? INFINITE : (DWORD)timeoutMs);
        if (waitResult == WAIT_TIMEOUT) {
            if (timedOut) *timedOut = true;
            return 1;
        }
        DWORD code = 1;
        if (waitResult == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code) &&
            code != STILL_ACTIVE) {
            exitCode = (int)code;
        }
        reaped = true;
        return exitCode;
    }

    bool writeStdin(const void* data, size_t length, int timeoutMs) {
        (void)timeoutMs;
        if (!data || length == 0) return true;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!stdinWrite) return false;
        const char* bytes = static_cast<const char*>(data);
        size_t offset = 0;
        while (offset < length) {
            const DWORD chunk = (DWORD)(std::min<size_t>(length - offset, 64 * 1024));
            DWORD written = 0;
            if (!WriteFile(stdinWrite, bytes + offset, chunk, &written, nullptr) || written == 0)
                return false;
            offset += written;
        }
        return true;
    }

    std::string readAllStdout(int timeoutMs) const {
        std::string result;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (!stdoutRead) return result;

        const bool infinite = timeoutMs <= 0;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(infinite ? 0 : timeoutMs);
        char chunk[16 * 1024];
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(stdoutRead, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available == 0) {
                if (!running()) {
                    DWORD read = 0;
                    while (ReadFile(stdoutRead, chunk, sizeof(chunk), &read, nullptr) && read > 0)
                        result.append(chunk, read);
                    break;
                }
                Sleep(10);
            } else {
                DWORD read = 0;
                if (!ReadFile(stdoutRead, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
                result.append(chunk, read);
            }
            if (!infinite && std::chrono::steady_clock::now() >= deadline) break;
        }
        return result;
    }

    void terminate(bool processTree) {
        if (processTree && pid) {
            std::string cmd = "C:\\Windows\\System32\\taskkill.exe /PID " +
                              std::to_string(pid) + " /T /F";
            runCommand(cmd, 5000, false, nullptr, nullptr);
        }
        if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            TerminateProcess(process, 1);
    }
};

#else

struct Process::Impl {
    pid_t pid = -1;
    int stdinFd = -1;
    int stdoutFd = -1;
    mutable int exitCode = 1;
    mutable bool reaped = true;
    mutable std::mutex ioMutex;

    ~Impl() {
        close();
        if (pid > 0 && !reaped) {
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid) reaped = true;
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (stdinFd >= 0) { ::close(stdinFd); stdinFd = -1; }
        if (stdoutFd >= 0) { ::close(stdoutFd); stdoutFd = -1; }
    }

    bool running() const {
        if (pid <= 0 || reaped) return false;
        int status = 0;
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            reaped = true;
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            return false;
        }
        if (result < 0 && errno == ECHILD) {
            reaped = true;
            return false;
        }
        return true;
    }

    int wait(int timeoutMs, bool* timedOut) {
        if (timedOut) *timedOut = false;
        if (pid <= 0 || reaped) return exitCode;
        const bool infinite = timeoutMs <= 0;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(infinite ? 0 : timeoutMs);
        for (;;) {
            int status = 0;
            const pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                reaped = true;
                exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                return exitCode;
            }
            if (result < 0 && errno == ECHILD) {
                reaped = true;
                return exitCode;
            }
            if (!infinite && std::chrono::steady_clock::now() >= deadline) {
                if (timedOut) *timedOut = true;
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool writeStdin(const void* data, size_t length, int timeoutMs) {
        if (!data || length == 0) return true;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (stdinFd < 0) return false;

        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMs <= 0 ? 60000 : timeoutMs);
        const char* bytes = static_cast<const char*>(data);
        size_t offset = 0;
        while (offset < length) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            struct pollfd pfd{};
            pfd.fd = stdinFd;
            pfd.events = POLLOUT;
            const int ready = poll(&pfd, 1, (int)remaining);
            if (ready == 0) return false;
            if (ready < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            const ssize_t written = ::write(stdinFd, bytes + offset, length - offset);
            if (written > 0) {
                offset += (size_t)written;
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            } else {
                return false; // EPIPE is ignored globally, so this is a clean failure.
            }
        }
        return true;
    }

    std::string readAllStdout(int timeoutMs) const {
        std::string result;
        std::lock_guard<std::mutex> lock(ioMutex);
        if (stdoutFd < 0) return result;

        // timeoutMs == 0: drain only what is already available.
        // timeoutMs < 0: keep reading until EOF.
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
        char chunk[16 * 1024];
        for (;;) {
            int waitMs = 0;
            if (timeoutMs > 0) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) break;
                waitMs = (int)(std::min<long long>(remaining, 50));
            } else if (timeoutMs < 0) {
                waitMs = 50;
            }

            struct pollfd pfd{};
            pfd.fd = stdoutFd;
            pfd.events = POLLIN;
            const int ready = poll(&pfd, 1, waitMs);
            if (ready == 0) {
                if (timeoutMs <= 0) break;
                continue;
            }
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            const ssize_t read = ::read(stdoutFd, chunk, sizeof(chunk));
            if (read > 0) {
                result.append(chunk, (size_t)read);
            } else if (read == 0) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (timeoutMs <= 0) break;
                continue;
            } else {
                break;
            }
        }
        return result;
    }

    void terminate(bool processTree) {
        if (pid <= 0) return;
        const pid_t target = processTree ? -pid : pid;
        ::kill(target, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (processTree || (pid > 0 && !reaped)) ::kill(target, SIGKILL);
    }
};

void ignoreSigPipe() {
    static std::once_flag once;
    std::call_once(once, []() { std::signal(SIGPIPE, SIG_IGN); });
}

#endif

Process::Process() : impl_(new Impl()) {}
Process::~Process() = default;
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;

bool Process::start(const std::string& command, bool provideStdin,
                    bool captureStdout, bool hidden) {
    if (!impl_) return false;
    try {
        impl_->close();
#ifdef _WIN32
        (void)hidden; // CreateProcessA is always hidden, with no window flashes.
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE inRead = nullptr, inWrite = nullptr;
        HANDLE outRead = nullptr, outWrite = nullptr;
        if (provideStdin) {
            if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
            SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = inRead;
        }
        if (captureStdout) {
            if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
                if (inRead) CloseHandle(inRead);
                if (inWrite) CloseHandle(inWrite);
                return false;
            }
            SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = outWrite;
            si.hStdError = outWrite;
        }

        std::vector<char> buffer(command.begin(), command.end());
        buffer.push_back('\0');
        PROCESS_INFORMATION pi{};
        const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
        const BOOL inheritHandles = (provideStdin || captureStdout) ? TRUE : FALSE;
        const BOOL started = CreateProcessA(nullptr, buffer.data(), nullptr, nullptr,
                                            inheritHandles, flags, nullptr, nullptr, &si, &pi);
        if (inRead) CloseHandle(inRead);
        if (outWrite) CloseHandle(outWrite);
        if (!started) {
            if (inWrite) CloseHandle(inWrite);
            if (outRead) CloseHandle(outRead);
            return false;
        }
        CloseHandle(pi.hThread);
        {
            std::lock_guard<std::mutex> lock(impl_->ioMutex);
            impl_->process = pi.hProcess;
            impl_->pid = (uint64_t)pi.dwProcessId;
            impl_->exitCode = 1;
            impl_->reaped = false;
            impl_->stdinWrite = inWrite;
            impl_->stdoutRead = outRead;
        }
        return true;
#else
        (void)hidden; // POSIX children are detached and have no controlling terminal.
        ignoreSigPipe();

        int inPipe[2] = { -1, -1 };
        int outPipe[2] = { -1, -1 };
        if (provideStdin && ::pipe(inPipe) != 0) return false;
        if (captureStdout && ::pipe(outPipe) != 0) {
            if (inPipe[0] >= 0) ::close(inPipe[0]);
            if (inPipe[1] >= 0) ::close(inPipe[1]);
            return false;
        }

        const pid_t child = ::fork();
        if (child < 0) {
            if (inPipe[0] >= 0) ::close(inPipe[0]);
            if (inPipe[1] >= 0) ::close(inPipe[1]);
            if (outPipe[0] >= 0) ::close(outPipe[0]);
            if (outPipe[1] >= 0) ::close(outPipe[1]);
            return false;
        }
        if (child == 0) {
            ::setpgid(0, 0);
            if (provideStdin) {
                ::dup2(inPipe[0], STDIN_FILENO);
                ::close(inPipe[0]);
                ::close(inPipe[1]);
            } else {
                const int devNull = ::open("/dev/null", O_RDONLY);
                if (devNull >= 0) {
                    ::dup2(devNull, STDIN_FILENO);
                    ::close(devNull);
                }
            }
            if (captureStdout) {
                ::dup2(outPipe[1], STDOUT_FILENO);
                ::dup2(outPipe[1], STDERR_FILENO);
                ::close(outPipe[0]);
                ::close(outPipe[1]);
            } else {
                const int devNull = ::open("/dev/null", O_WRONLY);
                if (devNull >= 0) {
                    ::dup2(devNull, STDOUT_FILENO);
                    ::dup2(devNull, STDERR_FILENO);
                    ::close(devNull);
                }
            }
            ::execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
            ::_exit(127);
        }

        ::setpgid(child, child); // Ignore EACCES if the child exec'd first.
        if (inPipe[0] >= 0) ::close(inPipe[0]);
        if (outPipe[1] >= 0) ::close(outPipe[1]);
        {
            std::lock_guard<std::mutex> lock(impl_->ioMutex);
            impl_->pid = (uint64_t)child;
            impl_->reaped = false;
            impl_->exitCode = 1;
            impl_->stdinFd = provideStdin ? inPipe[1] : -1;
            impl_->stdoutFd = captureStdout ? outPipe[0] : -1;
            if (impl_->stdoutFd >= 0) {
                const int flags = fcntl(impl_->stdoutFd, F_GETFL, 0);
                if (flags >= 0) fcntl(impl_->stdoutFd, F_SETFL, flags | O_NONBLOCK);
            }
        }
        if (!provideStdin && inPipe[1] >= 0) ::close(inPipe[1]);
        if (!captureStdout && outPipe[0] >= 0) ::close(outPipe[0]);
        return true;
#endif
    } catch (...) {
        return false;
    }
}

bool Process::writeStdin(const void* data, size_t length, int timeoutMs) {
    if (!impl_) return false;
    try {
        return impl_->writeStdin(data, length, timeoutMs);
    } catch (...) {
        return false;
    }
}

std::string Process::readAllStdout(int timeoutMs) const {
    if (!impl_) return {};
    try {
        return impl_->readAllStdout(timeoutMs);
    } catch (...) {
        return {};
    }
}

int Process::wait(int timeoutMs, bool* timedOut) {
    if (timedOut) *timedOut = false;
    if (!impl_) return 1;
    try {
        return impl_->wait(timeoutMs, timedOut);
    } catch (...) {
        return 1;
    }
}

bool Process::running() const {
    if (!impl_) return false;
    try {
        return impl_->running();
    } catch (...) {
        return false;
    }
}

void Process::terminate(bool processTree) {
    if (!impl_) return;
    try {
        impl_->terminate(processTree);
    } catch (...) {
        // Best-effort teardown; never propagate.
    }
}

void Process::close() {
    if (impl_) impl_->close();
}

ProcessId Process::id() const {
#ifdef _WIN32
    return impl_ ? (ProcessId)impl_->pid : 0;
#else
    return impl_ ? (ProcessId)impl_->pid : 0;
#endif
}

bool runCommand(const std::string& command, int timeoutMs, bool capture,
                std::string* output, int* exitCode) {
    if (output) output->clear();
    if (exitCode) *exitCode = 1;

    Process process;
    if (!process.start(command, false, capture, true)) return false;

    bool timedOut = false;
    if (capture) {
        const std::string data = process.readAllStdout(timeoutMs <= 0 ? -1 : timeoutMs);
        if (output) *output = data;
    }
    const int remaining = timeoutMs;
    const int code = process.wait(remaining, &timedOut);
    if (timedOut) process.terminate(true);
    if (exitCode) *exitCode = timedOut ? 1 : code;
    return !timedOut && code == 0;
}

// ---------------------------------------------------------------------------
// Resource sampling
// ---------------------------------------------------------------------------

bool processStats(ProcessId pid, uint64_t* cpuTime100ns, size_t* rssBytes) {
    if (cpuTime100ns) *cpuTime100ns = 0;
    if (rssBytes) *rssBytes = 0;
    if (!cpuTime100ns || !rssBytes || pid == 0) return false;

#if defined(_WIN32)
    HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, (DWORD)pid);
    if (!handle) return false;
    FILETIME create{}, exit{}, kernel{}, user{};
    bool ok = GetProcessTimes(handle, &create, &exit, &kernel, &user) != FALSE;
    if (ok) {
        const auto ticks = [](const FILETIME& ft) -> uint64_t {
            return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        };
        *cpuTime100ns = ticks(kernel) + ticks(user);
    }
    PROCESS_MEMORY_COUNTERS counters{};
    if (ok && GetProcessMemoryInfo(handle, &counters, sizeof(counters)))
        *rssBytes = (size_t)counters.WorkingSetSize;
    else
        ok = false;
    CloseHandle(handle);
    return ok;
#elif defined(__APPLE__)
#if defined(PROC_PIDTASKINFO)
    proc_taskinfo info{};
    const int result = proc_pidinfo((int)pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
    if (result == (int)sizeof(info)) {
        const uint64_t nanos = info.pti_total_user + info.pti_total_system;
        *cpuTime100ns = nanos / 100; // ns -> 100ns units
        *rssBytes = (size_t)info.pti_resident_size;
        return true;
    }
#endif
    // No libproc on this deployment (or the process went away): zero, not a
    // compile failure and not an exception.
    return true;
#else
    try {
        const std::string prefix = "/proc/" + std::to_string(pid) + "/";
        std::ifstream stat(prefix + "stat");
        if (!stat) return false;
        std::string line;
        if (!std::getline(stat, line)) return false;
        const size_t close = line.rfind(')');
        if (close == std::string::npos) return false;
        std::istringstream fields(line.substr(close + 1));
        // Field 3 is state; fields 4..13 are skipped; 14/15 are utime/stime.
        std::string token;
        unsigned long long utime = 0, stime = 0;
        for (int i = 0; i < 13; ++i) {
            if (!(fields >> token)) return false;
            if (i == 11) utime = std::strtoull(token.c_str(), nullptr, 10);
            if (i == 12) stime = std::strtoull(token.c_str(), nullptr, 10);
        }
        const long ticks = sysconf(_SC_CLK_TCK);
        if (ticks > 0) {
            *cpuTime100ns = (utime + stime) * 10000000ULL / (unsigned long long)ticks;
        }

        std::ifstream statm(prefix + "statm");
        if (statm) {
            unsigned long long residentPages = 0;
            statm >> residentPages >> residentPages; // first field is total size
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pageSize > 0) *rssBytes = (size_t)(residentPages * (unsigned long long)pageSize);
        }
        if (*rssBytes == 0) {
            std::ifstream status(prefix + "status");
            std::string key;
            unsigned long long kb = 0;
            while (std::getline(status, line)) {
                std::istringstream ls(line);
                if (ls >> key && key == "VmRSS:" && ls >> kb) {
                    *rssBytes = (size_t)(kb * 1024ULL);
                    break;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
#endif
}

void terminateTree(ProcessId pid) {
    if (!pid) return;
#ifdef _WIN32
    std::string cmd = "C:\\Windows\\System32\\taskkill.exe /PID " +
                      std::to_string(pid) + " /T /F";
    runCommand(cmd, 5000, false, nullptr, nullptr);
#else
    const pid_t target = (pid_t)pid;
    ::kill(-target, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ::kill(-target, SIGKILL);
#endif
}

// ---------------------------------------------------------------------------
// One-instance guard, paths and environment
// ---------------------------------------------------------------------------

namespace {
std::mutex g_instanceMutex;
#ifdef _WIN32
HANDLE g_instanceHandle = nullptr;
#else
int g_instanceFd = -1;
std::string g_instancePath;
#endif
} // namespace

const char* exeSuffix() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

std::string systemArchitecture() {
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        case PROCESSOR_ARCHITECTURE_ARM: return "arm";
        default: break;
    }
    const std::string wow = envA("PROCESSOR_ARCHITEW6432");
    if (wow == "ARM64") return "arm64";
    if (wow == "AMD64") return "x64";
    const std::string native = envA("PROCESSOR_ARCHITECTURE");
    if (native == "ARM64") return "arm64";
    if (native == "AMD64") return "x64";
    if (native == "x86") return "x86";
    return "unknown";
#else
    utsname uts{};
    if (uname(&uts) != 0) return "unknown";
    const std::string machine = uts.machine;
    if (machine == "x86_64" || machine == "amd64") return "x64";
    if (machine == "aarch64" || machine == "arm64") return "arm64";
    if (machine == "i386" || machine == "i486" || machine == "i586" ||
        machine == "i686") {
        return "x86";
    }
    if (machine.compare(0, 3, "arm") == 0) return "arm";
    return machine.empty() ? "unknown" : machine;
#endif
}

std::string dataRoot() {
    try {
#ifdef _WIN32
        std::string base = envA("LOCALAPPDATA");
        if (base.empty()) {
            const std::string profile = envA("USERPROFILE");
            base = profile.empty() ? fs::current_path().string()
                                   : (fs::path(profile) / "AppData" / "Local").string();
        }
        return (fs::path(base) / "BlueDroid").string();
#elif defined(__APPLE__)
        const std::string home = homeDir();
        if (home.empty()) return (fs::path("Library") / "Application Support" / "BlueDroid").string();
        return (fs::path(home) / "Library" / "Application Support" / "BlueDroid").string();
#else
        std::string base = envA("XDG_DATA_HOME");
        if (base.empty()) {
            const std::string home = homeDir();
            base = home.empty() ? ".local/share" : (fs::path(home) / ".local" / "share").string();
        }
        return (fs::path(base) / "BlueDroid").string();
#endif
    } catch (...) {
        return "BlueDroid";
    }
}

std::string tempRoot() {
    try {
#ifdef _WIN32
        return (fs::path(dataRoot()) / "temp").string();
#elif defined(__APPLE__)
        const std::string home = homeDir();
        if (home.empty()) return (fs::path("Library") / "Caches" / "BlueDroid").string();
        return (fs::path(home) / "Library" / "Caches" / "BlueDroid").string();
#else
        const std::string runtime = envA("XDG_RUNTIME_DIR");
        if (!runtime.empty()) return (fs::path(runtime) / "BlueDroid").string();
        return "/tmp/BlueDroid-" + std::to_string(getuid());
#endif
    } catch (...) {
        return "BlueDroid";
    }
}

bool setEnvironment(const char* name, const std::string& value) {
    if (!name || !*name) return false;
    try {
#ifdef _WIN32
        const std::string wideName(name);
        SetEnvironmentVariableA(name, value.c_str());
        const std::wstring wide(wideName.begin(), wideName.end());
        SetEnvironmentVariableW(wide.c_str(),
            std::wstring(value.begin(), value.end()).c_str());
        // Keep std::getenv() in this process in sync with the child environment.
        const std::string assignment = std::string(name) + "=" + value;
        return _putenv(assignment.c_str()) == 0;
#else
        return ::setenv(name, value.c_str(), 1) == 0;
#endif
    } catch (...) {
        return false;
    }
}

bool acquireSingleInstance() {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
    try {
        const fs::path dir(tempRoot());
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path file = dir / "BlueDroid.lock";
#ifdef _WIN32
        if (g_instanceHandle) return true;
        g_instanceHandle = CreateFileA(file.string().c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_instanceHandle == INVALID_HANDLE_VALUE) {
            g_instanceHandle = nullptr;
            return false;
        }
        const std::string pid = std::to_string(GetCurrentProcessId());
        DWORD written = 0;
        WriteFile(g_instanceHandle, pid.data(), (DWORD)pid.size(), &written, nullptr);
        return true;
#else
        if (g_instanceFd >= 0) return true;
        g_instanceFd = ::open(file.string().c_str(), O_CREAT | O_RDWR, 0600);
        if (g_instanceFd < 0) return false;
        if (::flock(g_instanceFd, LOCK_EX | LOCK_NB) != 0) {
            ::close(g_instanceFd);
            g_instanceFd = -1;
            return false;
        }
        const std::string pid = std::to_string(getpid());
        if (::ftruncate(g_instanceFd, 0) == 0)
            ::write(g_instanceFd, pid.data(), pid.size());
        g_instancePath = file.string();
        return true;
#endif
    } catch (...) {
        return false;
    }
}

void releaseSingleInstance() {
    std::lock_guard<std::mutex> lock(g_instanceMutex);
#ifdef _WIN32
    if (g_instanceHandle) {
        CloseHandle(g_instanceHandle);
        g_instanceHandle = nullptr;
        std::error_code ec;
        fs::remove(fs::path(tempRoot()) / "BlueDroid.lock", ec);
    }
#else
    if (g_instanceFd >= 0) {
        ::flock(g_instanceFd, LOCK_UN);
        ::close(g_instanceFd);
        g_instanceFd = -1;
        if (!g_instancePath.empty()) {
            std::error_code ec;
            fs::remove(g_instancePath, ec);
            g_instancePath.clear();
        }
    }
#endif
}

} // namespace platform
} // namespace emu
