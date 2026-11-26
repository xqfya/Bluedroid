#pragma once

#include <functional>
#include <string>

namespace emu {

// Snapshot of the background SDK installer. All fields are published from
// atomic state (strings are guarded by a mutex), so this is safe to read from
// the UI thread at any time.
struct SetupStatus {
    bool running = false;   // installer thread is active
    bool done = false;      // a usable SDK is available
    std::string error;      // non-empty when the last/final step failed
    std::string warning;    // non-empty for recoverable runtime warnings
    std::string stage;      // human readable current step
    int percent = 0;        // 0..100
};

// One-time, per-user application bootstrap. Call ensure() before anything
// else reads configuration or writes logs.
class AppSetup {
public:
    using ProgressCallback = std::function<void(const SetupStatus&)>;

    // Creates the directory tree, migrates/creates config files, publishes
    // child-process environment variables and starts the SDK installer when
    // no usable SDK is present. Safe to call more than once.
    static bool ensure();

    // Per-user paths rooted at the platform data directory (LOCALAPPDATA on
    // Windows, XDG data home on Linux, Application Support on macOS).
    static const std::string& root();
    static std::string configPath();
    static std::string profilesPath();
    static std::string thumbsDir();
    static std::string backupsDir();
    static std::string dataDir();
    // Isolated persistent storage for one phone (keyed by its ADB port).
    static std::string phoneDataDir(int port);
    static std::string imagesDir();
    static std::string avdDir();
    static std::string sdkDir();
    static std::string runtimeDir();
    static std::string tempDir();
    static std::string logPath();
    static std::string screenshotPath();

    // True when a usable SDK exists (system SDK or the local SDK).
    static bool sdkReady();
    static SetupStatus status();
    static void setProgressCallback(ProgressCallback cb);
    // Publishes a recoverable warning in the Manager banner. Empty clears it.
    static void setWarning(const std::string& text);

    // Runs a process with no console/window flashes. timeoutMs == 0 waits
    // indefinitely. When capture is true, child stdout/stderr are collected
    // into output (if supplied).
    static bool runHidden(const std::string& cmd, int timeoutMs = 0,
                          bool capture = false, std::string* output = nullptr);

private:
    AppSetup() = delete;
    static void installSdkAsync();
};

} // namespace emu
