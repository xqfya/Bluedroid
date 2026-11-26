#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace emu {
namespace platform {

using ProcessId = uint64_t;

// Platform-neutral child process. Command lines are passed through the native
// shell (CreateProcessA on Windows, /bin/sh on POSIX) so existing BlueDroid
// command construction keeps working on every OS.
class Process {
public:
    Process();
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;

    bool start(const std::string& command, bool provideStdin = false,
               bool captureStdout = false, bool hidden = true);
    bool writeStdin(const void* data, size_t length, int timeoutMs = 5000);
    std::string readAllStdout(int timeoutMs = 0) const;
    int wait(int timeoutMs = 0, bool* timedOut = nullptr);
    bool running() const;
    void terminate(bool processTree = false);
    void close();
    ProcessId id() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience wrapper around Process for short-lived commands.
bool runCommand(const std::string& command, int timeoutMs, bool capture,
                std::string* output, int* exitCode = nullptr);

// CPU/RAM sampling for the resource badges. cpuTime100ns is monotonic
// user+kernel time in 100ns units; rssBytes is resident memory.
bool processStats(ProcessId pid, uint64_t* cpuTime100ns, size_t* rssBytes);

void terminateTree(ProcessId pid);

// Process-wide one-instance guard. Returns false when another instance owns
// the lock. The lock is released by releaseSingleInstance() or process exit.
bool acquireSingleInstance();
void releaseSingleInstance();

const char* exeSuffix();
// "x64", "arm64", "x86", or "unknown" for the native OS/CPU combination.
std::string systemArchitecture();
std::string dataRoot();
std::string tempRoot();
bool setEnvironment(const char* name, const std::string& value);

} // namespace platform
} // namespace emu
