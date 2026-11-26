#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "emulator/android_vm.h"
#include "emulator/app_setup.h"
#include "emulator/logger.h"
#include "emulator/config.h"
#include "emulator/platform.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwchar>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace emu {

namespace fs = std::filesystem;

static void runAsync(const std::string& cmd) {
    // Hidden, detached launch. On Windows this is CreateProcessA with
    // CREATE_NO_WINDOW; on POSIX it is /bin/sh with /dev/null stdio.
    emu::platform::Process process;
    process.start(cmd, false, false, true);
}

static std::string getenvStr(const char* name) {
    char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

namespace {

static const char* pathSep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

#ifdef _WIN32

struct FindEmuCtx {
    std::wstring avd;
    std::wstring port;
    HWND hwnd = nullptr;
};

BOOL CALLBACK findEmuWindowProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindEmuCtx*>(lp);
    wchar_t title[512] = {};
    int n = GetWindowTextW(h, title, 512);
    if (n <= 0) return TRUE;

    // Qt owns a Unicode window title. Match both the AVD name and the serial
    // port so multiple phones never steal each other's capture surface.
    if (std::wcsstr(title, L"Android Emulator") == nullptr) return TRUE;
    bool avdMatch = !ctx->avd.empty() && std::wcsstr(title, ctx->avd.c_str()) != nullptr;
    bool portMatch = !ctx->port.empty() && std::wcsstr(title, ctx->port.c_str()) != nullptr;
    if (avdMatch || portMatch) {
        ctx->hwnd = h;
        return FALSE;
    }
    return TRUE;
}

struct HideEmuCtx {
    HWND keep;
    DWORD pid;
};

BOOL CALLBACK hideEmuWindowProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<HideEmuCtx*>(lp);
    if (h == ctx->keep) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != ctx->pid) return TRUE;

    // Qt also creates a visible side/tool window. Hide every other top-level
    // window owned by the emulator so only BlueDroid's phone frame is seen.
    char cls[128] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (std::strncmp(cls, "Qt", 2) == 0 && IsWindowVisible(h)) {
        ShowWindow(h, SW_HIDE);
    }
    return TRUE;
}

static void keepEmulatorHidden(HWND hwnd, DWORD pid) {
    if (!hwnd || !IsWindow(hwnd)) return;

    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex &= ~((LONG_PTR)WS_EX_APPWINDOW);
    ex |= (LONG_PTR)WS_EX_TOOLWINDOW;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);

    // Qt re-shows and re-centers its main window during boot, so re-apply the
    // offscreen position repeatedly instead of only once at startup.
    ShowWindow(hwnd, SW_HIDE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    // Move last: Qt re-centers the window while it is being shown.
    SetWindowPos(hwnd, nullptr, -4000, -4000, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    HideEmuCtx ctx{ hwnd, pid };
    EnumWindows(hideEmuWindowProc, reinterpret_cast<LPARAM>(&ctx));
}

#endif // _WIN32

static void trimIniWhitespace(std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s.clear(); return; }
    size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, last - first + 1);
}

static bool setIniValue(std::vector<std::string>& lines, const std::string& key,
                        const std::string& value) {
    const std::string replacement = key + "=" + value;
    bool changed = false;
    bool seen = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t eq = lines[i].find('=');
        if (eq == std::string::npos) continue;
        std::string k = lines[i].substr(0, eq);
        trimIniWhitespace(k);
        if (k != key) continue;
        if (!seen) {
            seen = true;
            if (lines[i] != replacement) {
                lines[i] = replacement;
                changed = true;
            }
        } else {
            // Remove older duplicates so an emulator never reads <temp> after
            // we have written the real per-phone data path.
            lines.erase(lines.begin() + (ptrdiff_t)i);
            --i;
            changed = true;
        }
    }
    if (!seen) {
        lines.push_back(replacement);
        changed = true;
    }
    return changed;
}

static const char* gpuModeName(int mode) {
    return mode == 1 ? "swiftshader" : "host";
}

// Templates are stored with the phone profile and applied to the AVD config
// before launch. Only values explicitly set by a template are written, so old
// profiles keep using the global emulator.ini defaults.
// Force Android Studio's emulator to use the AVD's own userdata file and its
// own snapshot directory. The default names are already per-AVD, but older AVDs
// may contain overrides that point at a shared image. Writing these keys makes
// every BlueDroid phone own its Android apps, settings and account data.
static void applyAvdStoragePaths(const std::string& iniPath, const std::string& avdDir) {
    std::vector<std::string> lines;
    std::ifstream in(iniPath);
    if (in) {
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    bool changed = false;
    // <temp> is Android's "do not persist this data" marker. Point it at the
    // AVD's own image so apps, accounts and settings survive a phone restart.
    const fs::path avd = fs::path(avdDir);
    changed |= setIniValue(lines, "disk.dataPartition.path",
                           (avd / "userdata-qemu.img").string());

    if (!changed && in) return;
    std::ofstream out(iniPath, std::ios::trunc);
    for (const auto& line : lines) out << line << "\n";
    std::error_code ec;
    fs::create_directories(avd / "snapshots", ec);
}

static void applyAvdHardware(const std::string& iniPath, const VmSpec& spec) {
    std::vector<std::string> lines;
    std::ifstream in(iniPath);
    if (in) {
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    bool changed = false;
    auto setInt = [&](const std::string& key, int value) {
        if (value <= 0) return;
        if (setIniValue(lines, key, std::to_string(value))) changed = true;
    };

    setInt("hw.lcd.width", spec.screenWidth);
    setInt("hw.lcd.height", spec.screenHeight);
    setInt("hw.lcd.density", spec.dpi);
    setInt("hw.ramSize", spec.ramMB);
    setInt("hw.cpu.ncore", spec.cpuCores);
    // Older AVDs could be left with hw.gpu.enabled=no while the launcher still
    // passed -gpu <mode>. That mismatch made the Android surface paint white.
    // Keep the AVD flag in sync with the selected GPU mode on every start.
    if (setIniValue(lines, "hw.gpu.enabled", "yes")) changed = true;
    if (setIniValue(lines, "hw.gpu.mode", gpuModeName(spec.gpuMode))) changed = true;
    if (spec.ramMB > 0) setInt("vm.heapSize", std::max(64, spec.ramMB / 8));

    if (!changed && in) return;
    std::ofstream out(iniPath, std::ios::trunc);
    for (const auto& line : lines) out << line << "\n";
}

// avdmanager used to write these two files through Java. BlueDroid now creates
// them directly so the emulator never needs the JDK or command-line tools.
static bool writeNativeAvd(const std::string& avdBase, const std::string& name,
                           const std::string& avdDir, const VmSpec& spec) {
    std::error_code ec;
    const fs::path avd = fs::path(avdDir);
    fs::create_directories(avd, ec);
    fs::create_directories(avd / "snapshots", ec);
    const std::string sep = pathSep();

    const fs::path descriptor = fs::path(avdBase) / (name + ".ini");
    const std::string iniPath = descriptor.string();
    std::ofstream ini(iniPath, std::ios::trunc);
    if (!ini) {
        LOG_ERROR("Could not write AVD descriptor %s", iniPath.c_str());
        return false;
    }
    ini << "avd.ini.encoding=UTF-8\n"
        << "path=" << avdDir << "\n"
        << "path.rel=avd" << sep << name << ".avd\n"
        << "target=android-33\n";
    if (!ini) {
        LOG_ERROR("Could not finish AVD descriptor %s", iniPath.c_str());
        return false;
    }

    const int cores = spec.cpuCores > 0 ? spec.cpuCores : 4;
    const int ram = spec.ramMB > 0 ? spec.ramMB : 4096;
    const int width = spec.screenWidth > 0 ? spec.screenWidth : 1080;
    const int height = spec.screenHeight > 0 ? spec.screenHeight : 1920;
    const int dpi = spec.dpi > 0 ? spec.dpi : 320;

    const fs::path config = avd / "config.ini";
    std::ofstream cfg(config.string(), std::ios::trunc);
    if (!cfg) {
        LOG_ERROR("Could not write AVD config %s", config.string().c_str());
        return false;
    }
    cfg << "avd.ini.encoding=UTF-8\n"
        << "PlayStore.enabled=yes\n"
        << "abi.type=x86_64\n"
        << "avd.id=<build>\n"
        << "avd.name=<build>\n"
        << "disk.cachePartition=yes\n"
        << "disk.cachePartition.size=66MB\n"
        << "disk.dataPartition.path=" << (avd / "userdata-qemu.img").string() << "\n"
        << "disk.dataPartition.size=6442450944\n"
        << "hw.accelerometer=yes\n"
        << "hw.audioInput=yes\n"
        << "hw.audioOutput=yes\n"
        << "hw.battery=yes\n"
        << "hw.camera.back=emulated\n"
        << "hw.camera.front=none\n"
        << "hw.cpu.arch=x86_64\n"
        << "hw.cpu.ncore=" << cores << "\n"
        << "hw.dPad=no\n"
        << "hw.device.hash2=MD5:3274126e0242a0d86339850416b0ce34\n"
        << "hw.device.manufacturer=Google\n"
        << "hw.device.name=pixel_5\n"
        << "hw.gpu.enabled=yes\n"
        << "hw.gpu.mode=" << gpuModeName(spec.gpuMode) << "\n"
        << "hw.lcd.density=" << dpi << "\n"
        << "hw.lcd.height=" << height << "\n"
        << "hw.lcd.width=" << width << "\n"
        << "hw.ramSize=" << ram << "\n"
        << "hw.sdCard=yes\n"
        << "hw.tactile=yes\n"
        << "hw.usb=yes\n"
        << "hw.wifi=yes\n"
        << "image.sysdir.1=system-images" << sep << "android-33" << sep
        << "google_apis_playstore" << sep << "x86_64" << sep << "\n"
        << "tag.display=Google Play\n"
        << "tag.id=google_apis_playstore\n"
        << "vm.heapSize=" << std::max(64, ram / 8) << "\n";
    if (!cfg) {
        LOG_ERROR("Could not finish AVD config %s", config.string().c_str());
        return false;
    }
    LOG_INFO("Created AVD descriptor %s", iniPath.c_str());
    return true;
}

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

static void appendBase64(const char* data, size_t len, std::string& out) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = ((uint32_t)(unsigned char)data[i] << 16) |
                           ((uint32_t)(unsigned char)data[i + 1] << 8) |
                            (uint32_t)(unsigned char)data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < len) {
        const size_t rem = len - i;
        uint32_t v = (uint32_t)(unsigned char)data[i] << 16;
        if (rem == 2) v |= (uint32_t)(unsigned char)data[i + 1] << 8;
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += rem == 1 ? '=' : table[(v >> 6) & 63];
        out += '=';
    }
}

static std::string apkRemoteName(const std::string& localPath) {
    std::string base = std::filesystem::path(localPath).filename().string();
    for (char& c : base) {
        unsigned char u = (unsigned char)c;
        if (!(std::isalnum(u) || c == '.' || c == '_' || c == '-')) c = '_';
    }
    if (base.empty()) base = "app.apk";
    if (base.size() > 80) base = base.substr(base.size() - 80);
    return base;
}

static bool copyFileIfChanged(const std::string& src, const std::string& dst) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(dst, ec) && fs::file_size(dst, ec) == fs::file_size(src, ec) &&
        fs::last_write_time(dst, ec) == fs::last_write_time(src, ec)) {
        return true;
    }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

} // namespace

AndroidVirtualMachine::AndroidVirtualMachine() {
    tmpDir_ = AppSetup::tempDir();
    framePath_ = (fs::path(tmpDir_) / "frame.png").string();
}

AndroidVirtualMachine::~AndroidVirtualMachine() {
    stop();
}

std::string AndroidVirtualMachine::apkStoreDir() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(AppSetup::phoneDataDir(port_)) / "apks";
    fs::create_directories(dir, ec);
    return dir.string();
}

void AndroidVirtualMachine::configure(const std::string& avdName, int port, bool attachExisting) {
    avdName_ = avdName.empty() ? "BlueDroid" : avdName;
    port_ = port > 0 ? port : 5554;
    serial_ = "emulator-" + std::to_string(port_);
    attachOnly_ = attachExisting;
    // Each phone needs its own scratch frame in the ADB fallback path.
    framePath_ = (fs::path(tmpDir_) /
                  ("frame_" + std::to_string(port_) + ".png")).string();
}

bool AndroidVirtualMachine::isDeviceOnline() {
    if (adbPath_.empty() && !findSdk()) return false;
    std::string out = runCmd("\"" + adbPath_ + "\" -s " + serial_ + " shell getprop sys.boot_completed");
    return out.find("1") != std::string::npos;
}

bool AndroidVirtualMachine::installApk(const std::string& apkPath,
                                       const ApkProgressCallback& progress,
                                       const std::atomic<bool>* cancel) {
    auto report = [&](size_t done, size_t total, const std::string& stage) {
        if (stage != "Uploading APK" || done == 0 || done == total) {
            LOG_INFO("APK install %s: %s (%zu/%zu bytes)", serial_.c_str(),
                     stage.c_str(), done, total);
        }
        if (progress) progress(ApkInstallProgress{ done, total, stage });
    };

    if (apkPath.empty()) {
        report(0, 0, "No APK selected");
        return false;
    }
    if (adbPath_.empty() && !findSdk()) {
        report(0, 0, "ADB not available");
        return false;
    }

    // Keep the APK inside this phone's own library and install from that copy
    // so the saved file is exactly what was pushed to this phone.
    const std::string store = apkStoreDir();
    const std::string apkFileName = apkRemoteName(apkPath);
    const std::string storedApk = (fs::path(store) / apkFileName).string();
    std::string sourceApk = apkPath;
    if (copyFileIfChanged(apkPath, storedApk)) {
        // Install from the saved copy so the phone library is exactly what was
        // installed, even if the original dragged file is later moved/deleted.
        sourceApk = storedApk;
        std::ofstream manifest((fs::path(store) / "installed.txt").string(), std::ios::app);
        if (manifest) {
            char stamp[32] = {};
            std::time_t now = std::time(nullptr);
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            manifest << apkFileName << "  " << stamp << "\n";
        }
    } else {
        LOG_WARN("Could not save APK to phone library %s", storedApk.c_str());
    }

    std::ifstream in(sourceApk, std::ios::binary | std::ios::ate);
    if (!in) {
        report(0, 0, "Cannot read APK");
        return false;
    }
    const size_t total = (size_t)in.tellg();
    in.seekg(0, std::ios::beg);
    if (total == 0) {
        report(0, 0, "APK is empty");
        return false;
    }

    const std::string remote = "/data/local/tmp/bluedroid_" +
        std::to_string(port_) + "_" + apkFileName;
    auto removeRemote = [&]() {
        runCmd("\"" + adbPath_ + "\" -s " + serial_ + " shell rm -f " + remote);
    };
    removeRemote();
    report(0, total, "Uploading APK");

    // Stream a base64 encoding into a remote shell. Raw binary is unsafe here:
    // adb.exe on Windows treats stdin as text and stops at the first 0x1A byte
    // in an APK. Base64 is pure ASCII, so every block really reaches the phone.
    // Progress is counted from APK bytes represented by successfully written
    // blocks, never from a timer.
    const std::string cmd = "\"" + adbPath_ + "\" -s " + serial_ +
        " shell toybox base64 -d > " + remote;
    emu::platform::Process transfer;
    bool ok = transfer.start(cmd, true, false, true);
    if (!ok) {
        report(0, total, "ADB transfer failed to start");
        return false;
    }

    // Divisible by three, so only the final block needs base64 padding.
    const size_t chunk = 48 * 1024;
    std::vector<char> raw(chunk);
    std::string encoded;
    size_t done = 0;
    bool cancelled = false;

    while (done < total) {
        if (cancel && cancel->load()) { cancelled = true; break; }
        if (!transfer.running()) break;

        const size_t want = std::min(chunk, total - done);
        in.read(raw.data(), (std::streamsize)want);
        const std::streamsize got = in.gcount();
        if (got <= 0) break;

        encoded.clear();
        encoded.reserve(((size_t)got + 2) / 3 * 4);
        appendBase64(raw.data(), (size_t)got, encoded);

        // writeStdin is binary-safe on both Windows and POSIX.
        if (!transfer.writeStdin(encoded.data(), encoded.size(), 15000)) {
            ok = false;
            break;
        }
        done += (size_t)got;
        report(done, total, "Uploading APK");
    }

    transfer.close(); // signals EOF to the remote base64 decoder
    bool timedOut = false;
    const int exitCode = transfer.wait(cancelled ? 2000 : 60000, &timedOut);

    if (cancelled) {
        transfer.terminate(true);
        removeRemote();
        report(done, total, "Cancelled");
        return false;
    }
    if (!ok || timedOut || exitCode != 0 || done != total) {
        LOG_WARN("APK transfer failed: ok=%d timeout=%d exit=%d done=%zu/%zu",
                 (int)ok, (int)timedOut, exitCode, done, total);
        removeRemote();
        report(done, total, "Upload failed");
        return false;
    }

    LOG_INFO("APK upload complete: %zu bytes on %s", total, serial_.c_str());
    report(total, total, "Installing APK");
    const std::string install = "\"" + adbPath_ + "\" -s " + serial_ +
        " shell pm install -r -t " + remote;
    const std::string out = runCmd(install, 180000);
    const std::string low = lowerCopy(out);
    bool installed = low.find("success") != std::string::npos;
    if (!installed && low.find("failure") == std::string::npos &&
        low.find("error") == std::string::npos && low.find("unknown") == std::string::npos) {
        installed = true; // pm install reported no failure
    }

    removeRemote();
    if (installed) {
        report(total, total, "Installed");
    } else {
        std::string detail = out;
        const size_t nl = detail.find_first_of("\r\n");
        if (nl != std::string::npos) detail.resize(nl);
        if (detail.size() > 100) detail.resize(100);
        if (detail.empty()) detail = "pm install failed";
        report(total, total, "Failed: " + detail);
    }
    LOG_INFO("APK install %s on %s (%s)", installed ? "succeeded" : "failed",
             serial_.c_str(), apkPath.c_str());
    return installed;
}

bool AndroidVirtualMachine::findSdk() {
    const std::string suffix = emu::platform::exeSuffix();
    const std::string adbName = "adb" + suffix;
    const std::string emulatorName = "emulator" + suffix;
    const std::string checkName = "emulator-check" + suffix;
    auto usable = [&](const fs::path& candidate) {
        if (candidate.empty()) return false;
        std::error_code ec;
        return fs::is_regular_file(candidate / "platform-tools" / adbName, ec) &&
               fs::is_regular_file(candidate / "emulator" / emulatorName, ec);
    };

    // AppSetup publishes these variables for every child process. Prefer a
    // usable SDK from the environment, then the system location, then the
    // per-user SDK installed by bootstrap.
#ifdef _WIN32
    const fs::path systemSdk("C:\\AndroidSDK");
#else
    const fs::path systemSdk("/opt/android-sdk");
#endif
    const std::string envRoot = getenvStr("ANDROID_SDK_ROOT");
    const std::string envHome = getenvStr("ANDROID_HOME");
    const fs::path candidates[] = {
        fs::path(envRoot), fs::path(envHome), systemSdk, fs::path(AppSetup::sdkDir())
    };
    fs::path root;
    for (const fs::path& candidate : candidates) {
        if (usable(candidate)) { root = candidate; break; }
    }
    if (root.empty()) {
        // Keep the existing diagnostics pointing at the most useful location.
        if (!envRoot.empty()) root = envRoot;
        else if (!envHome.empty()) root = envHome;
        else root = systemSdk;
    }

    sdkRoot_ = root.string();
    adbPath_ = (root / "platform-tools" / adbName).string();
    emulatorPath_ = (root / "emulator" / emulatorName).string();
    emulatorCheckPath_ = (root / "emulator" / checkName).string();
    std::ifstream f1(adbPath_); if (!f1) { LOG_ERROR("adb not found at %s", adbPath_.c_str()); return false; }
    std::ifstream f2(emulatorPath_); if (!f2) { LOG_ERROR("emulator not found at %s", emulatorPath_.c_str()); return false; }
    std::string avdHome = getenvStr("ANDROID_AVD_HOME");
    if (avdHome.empty()) {
#ifdef _WIN32
        avdHome = (fs::path(getenvStr("USERPROFILE")) / ".android" / "avd").string();
#else
        avdHome = (fs::path(getenvStr("HOME")) / ".android" / "avd").string();
#endif
    }
    LOG_INFO("SDK root: %s", sdkRoot_.c_str());
    LOG_INFO("AVD home: %s", avdHome.c_str());
    return true;
}

bool AndroidVirtualMachine::ensureAvd() {
    // ANDROID_AVD_HOME is set by AppSetup to the per-user avd folder. The
    // descriptor and config are written natively; no avdmanager/JDK is needed.
    std::string avdBase = getenvStr("ANDROID_AVD_HOME");
    if (avdBase.empty()) avdBase = AppSetup::avdDir();
    std::error_code ec;
    const fs::path avdRoot(avdBase);
    fs::create_directories(avdRoot, ec);
    const fs::path avd = avdRoot / (avdName_ + ".avd");
    avdBase = avdRoot.string();
    const std::string avdDir = avd.string();
    const std::string configPath = (avd / "config.ini").string();
    std::ifstream f(configPath);
    if (f) {
        f.close();
        applyAvdHardware(configPath, spec_);
        applyAvdStoragePaths(configPath, avdDir);
        LOG_INFO("AVD %s exists", avdName_.c_str());
        return true;
    }

    LOG_INFO("Creating AVD %s natively", avdName_.c_str());
    if (!writeNativeAvd(avdBase, avdName_, avdDir, spec_)) {
        err_ = "Failed to create AVD files";
        LOG_ERROR("Native AVD creation failed for %s", avdName_.c_str());
        return false;
    }
    applyAvdHardware(configPath, spec_);
    applyAvdStoragePaths(configPath, avdDir);
    // The QEMU data partition lives in the AVD directory. It is created on
    // first boot and persists there on every later launch.
    return true;
}

std::string AndroidVirtualMachine::runCmd(const std::string& cmd, int timeoutMs) {
    std::string result;
    emu::platform::runCommand(cmd, timeoutMs, true, &result, nullptr);
    return result;
}

bool AndroidVirtualMachine::checkAcceleration() {
#ifdef _WIN32
    const std::string warning =
        "Virtualization unavailable: enable VT-x/AMD-V in BIOS or Windows "
        "Hypervisor Platform, then restart BlueDroid.";
#else
    const std::string warning =
        "Virtualization unavailable: enable KVM in BIOS and kernel support, "
        "then restart BlueDroid.";
#endif

    std::ifstream present(emulatorCheckPath_);
    if (!present) {
        LOG_WARN("Acceleration checker not found at %s", emulatorCheckPath_.c_str());
        AppSetup::setWarning(warning);
        setBootStatus(warning.c_str(), -1);
        return false;
    }
    present.close();

    std::string output;
    const bool ok = AppSetup::runHidden("\"" + emulatorCheckPath_ + "\" accel",
                                        30000, true, &output);
    const std::string low = lowerCopy(output);
    bool unavailable = !ok;

    // emulator-check prints a status code on the line after "accel:".
    const size_t marker = low.find("accel:");
    if (marker != std::string::npos) {
        size_t lineEnd = low.find_first_of("\r\n", marker + 6);
        if (lineEnd != std::string::npos) {
            std::string code = low.substr(marker + 6, lineEnd - marker - 6);
            trimIniWhitespace(code);
            if (!code.empty() && code.find_first_not_of("0123456789") == std::string::npos &&
                std::atoi(code.c_str()) != 0) {
                unavailable = true;
            }
        }
    }

    static const char* const failureTexts[] = {
        "not available", "not usable", "is not installed", "not installed",
        "unavailable", "disabled", "could not", "failed", "no acceleration"
    };
    for (const char* text : failureTexts) {
        if (low.find(text) != std::string::npos) {
            unavailable = true;
            break;
        }
    }

    if (unavailable) {
        LOG_WARN("Emulator acceleration check failed: %s", output.c_str());
        AppSetup::setWarning(warning);
        setBootStatus(warning.c_str(), -1);
        return false;
    }

    AppSetup::setWarning("");
    LOG_INFO("Emulator acceleration is available: %s", output.c_str());
    return true;
}

void AndroidVirtualMachine::launchEmulator() {
    std::ostringstream cmd;
    // Windows keeps its real (offscreen) window for PrintWindow capture. POSIX
    // has no capture window and uses adb's screencap instead.
    cmd << "\"" << emulatorPath_ << "\" -avd " << avdName_
        << " -port " << port_ << " -gpu " << gpuModeName(spec_.gpuMode)
        << " -no-snapshot";
#ifndef _WIN32
    // Hidden processes are a core BlueDroid feature on every OS. POSIX has no
    // capture window: frames come from adb screencap, so the emulator itself
    // never needs to open a visible QEMU window.
    cmd << " -no-window";
#endif
    if (spec_.ramMB > 0) cmd << " -memory " << spec_.ramMB;
    LOG_INFO("Launching: %s", cmd.str().c_str());

    if (proc_.start(cmd.str(), false, false, true)) {
        procId_ = proc_.id();
        LOG_INFO("Emulator launched pid=%llu", (unsigned long long)procId_);
    } else {
        err_ = "Failed to launch emulator";
        LOG_ERROR("Could not launch emulator process");
    }
}

void AndroidVirtualMachine::setBootStatus(const char* text, int percent) {
    {
        std::lock_guard<std::mutex> lk(statusMtx_);
        bootStatus_ = text ? text : "";
    }
    if (percent >= 0) bootPercent_.store(std::min(100, percent));
}

std::string AndroidVirtualMachine::statusMessage() const {
    std::lock_guard<std::mutex> lk(statusMtx_);
    return bootStatus_;
}

void AndroidVirtualMachine::waitForBoot() {
    setBootStatus("Connecting to emulator", 8);
    LOG_INFO("Waiting for device boot...");
    for (int i = 0; i < 180 && running_; ++i) {
        std::string prop = runCmd(adbPath_ + " -s " + serial_ + " shell getprop sys.boot_completed");
        if (prop.find("device offline") != std::string::npos ||
            prop.find("device not found") != std::string::npos ||
            prop.find("no devices") != std::string::npos) {
            // Stay low until adb actually sees the device.
            setBootStatus("Waiting for emulator to start", 12);
        } else if (prop.find('1') != std::string::npos) {
            setBootStatus("Android is ready", 92);
            LOG_INFO("Boot completed");
            return;
        } else {
            std::string anim = runCmd(adbPath_ + " -s " + serial_ + " shell getprop init.svc.bootanim");
            if (anim.find("running") != std::string::npos) {
                setBootStatus("Booting Android...", 48);
            } else {
                setBootStatus("Starting Android services...", 72);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    setBootStatus("Emulator ready", 96);
    LOG_WARN("Boot wait timed out");
}

uint64_t AndroidVirtualMachine::findEmulatorWindow() const {
#ifdef _WIN32
    FindEmuCtx ctx;
    ctx.avd.assign(avdName_.begin(), avdName_.end());
    auto dash = serial_.find('-');
    if (dash != std::string::npos && dash + 1 < serial_.size()) {
        std::string port = serial_.substr(dash + 1);
        ctx.port.assign(port.begin(), port.end());
    }
    EnumWindows(findEmuWindowProc, reinterpret_cast<LPARAM>(&ctx));
    return (uint64_t)ctx.hwnd;
#else
    return 0; // No host window on POSIX; capture comes from adb.
#endif
}

bool AndroidVirtualMachine::ensureInputShell() {
    if (inputProc_.running()) return true;
    inputProc_.terminate(true);

    const std::string cmd = "\"" + adbPath_ + "\" -s " + serial_ + " shell";
    if (!inputProc_.start(cmd, true, false, true)) return false;
    LOG_INFO("Persistent input shell ready");
    return true;
}

void AndroidVirtualMachine::writeInput(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(inputMtx_);
    const std::string line = cmd + "\n";

    auto send = [&]() { return inputProc_.writeStdin(line.data(), line.size(), 3000); };

    if (!ensureInputShell()) { runAsync(cmd); return; }
    if (send()) return;

    // Pipe may have gone stale; restart the shell and retry once.
    inputProc_.terminate(true);
    if (ensureInputShell() && send()) return;
    runAsync(cmd);
}

bool AndroidVirtualMachine::create(const VmSpec& spec) {
    spec_ = spec;
    if (!findSdk()) return false;
    if (!ensureAvd()) return false;
    w_ = spec.screenWidth;
    h_ = spec.screenHeight;
    screenW_ = spec.screenWidth;
    screenH_ = spec.screenHeight;
    fb_.assign((size_t)w_ * h_ * 4, 0);
    LOG_INFO("Android VM created %dx%d", w_, h_);
    return true;
}

bool AndroidVirtualMachine::start() {
    state_ = VmState::Starting;
    setBootStatus("Preparing emulator", 0);
    if (attachOnly_) {
        if (!isDeviceOnline()) {
            err_ = "Running emulator not found on " + serial_;
            state_ = VmState::Error;
            LOG_ERROR("%s", err_.c_str());
            return false;
        }
        LOG_INFO("Reattached to existing emulator %s", serial_.c_str());
    } else {
        // Silent pre-flight. A warning is published to the Manager banner when
        // VT-x/AMD-V or Windows Hypervisor Platform is not usable.
        checkAcceleration();
        launchEmulator();
        if (procId_ == 0) { state_ = VmState::Error; return false; }
    }
    running_ = true;
    capThread_ = std::thread(&AndroidVirtualMachine::captureLoop, this);
    LOG_INFO("Android VM starting (booting in background)");
    return true;
}

void AndroidVirtualMachine::pause() { state_ = VmState::Paused; }
void AndroidVirtualMachine::resume() { state_ = VmState::Running; }

void AndroidVirtualMachine::stop() {
    running_ = false;

    // On Windows the capture thread may be inside PrintWindow(); tear the
    // emulator down BEFORE joining. Attached VMs were already killed by
    // terminate().
    if (!attachOnly_) {
#ifdef _WIN32
        if (emuHwnd_) {
            PostMessageA(reinterpret_cast<HWND>(emuHwnd_), WM_CLOSE, 0, 0);
            emuHwnd_ = 0;
        }
#else
        // Graceful shutdown: adb asks the emulator to save state and exit.
        if (!adbPath_.empty())
            runCmd("\"" + adbPath_ + "\" -s " + serial_ + " emu kill", 5000);
#endif
        // The launcher spawns a separate qemu child. Kill both process trees
        // (taskkill /T /F on Windows, kill(-pgid) on POSIX).
        if (procId_) emu::platform::terminateTree(procId_);
        if (emuPid_ && emuPid_ != procId_) emu::platform::terminateTree(emuPid_);
        proc_.terminate(true);
        emuPid_ = 0;
        procId_ = 0;
    } else {
        emuHwnd_ = 0;
        emuPid_ = 0;
    }

    if (capThread_.joinable()) capThread_.join();

    inputProc_.terminate(true);
    state_ = VmState::Stopped;
    LOG_INFO("Android VM stopped%s", attachOnly_ ? " (detached)" : "");
}

void AndroidVirtualMachine::terminate() {
    if (attachOnly_) {
        if (adbPath_.empty()) findSdk();
        runAsync("\"" + adbPath_ + "\" -s " + serial_ + " emu kill");
#ifdef _WIN32
        if (emuHwnd_) {
            PostMessageA(reinterpret_cast<HWND>(emuHwnd_), WM_CLOSE, 0, 0);
            emuHwnd_ = 0;
        }
#endif
        if (emuPid_) {
            emu::platform::terminateTree(emuPid_);
            emuPid_ = 0;
        }
        LOG_INFO("Terminated attached emulator %s", serial_.c_str());
    }
    stop();
}

void AndroidVirtualMachine::reset() { frames_ = 0; }

void AndroidVirtualMachine::readFrame(const std::function<void(const uint8_t*, int, int)>& fn) const {
    std::lock_guard<std::mutex> lk(fbMtx_);
    fn(fb_.data(), w_, h_);
}

bool AndroidVirtualMachine::captureAdbFrame() {
    // Hidden adb exec-out screencap -p: binary-safe pipe on every platform.
    const std::string cmd = "\"" + adbPath_ + "\" -s " + serial_ +
                            " exec-out screencap -p";
    emu::platform::Process process;
    if (!process.start(cmd, false, true, true)) return false;
    const std::string bytes = process.readAllStdout(10000);
    bool timedOut = false;
    process.wait(1000, &timedOut);
    if (timedOut) process.terminate(true);
    if (bytes.empty()) return false;
    const auto* buf = reinterpret_cast<const unsigned char*>(bytes.data());
    int cw = 0, ch = 0, c = 0;
    unsigned char* img = stbi_load_from_memory(buf, (int)bytes.size(), &cw, &ch, &c, 4);
    if (!img) return false;
    std::lock_guard<std::mutex> lk(fbMtx_);
    if (w_ != cw || h_ != ch) {
        w_ = cw; h_ = ch;
        fb_.assign((size_t)w_ * h_ * 4, 0);
        LOG_INFO("Screen size %dx%d (adb fallback)", w_, h_);
    }
    memcpy(fb_.data(), img, (size_t)w_ * h_ * 4);
    stbi_image_free(img);
    ++frames_;
    notifyFrame();
    return true;
}

void AndroidVirtualMachine::captureLoop() {
#ifdef _WIN32
    setBootStatus("Waiting for emulator window");
#else
    setBootStatus("Waiting for Android");
#endif
    waitForBoot();
    if (!running_) return;
    state_ = VmState::Running;
    setBootStatus("Emulator ready", 100);
    LOG_INFO("Android VM running");

#ifdef _WIN32
    // Find and hide the capture surface immediately. ADB queries below can take
    // tens of seconds; doing them first used to leave Qt's window on top of
    // BlueDroid and steal mouse clicks.
    for (int i = 0; i < 300 && running_ && !emuHwnd_; ++i) {
        emuHwnd_ = findEmulatorWindow();
        if (emuHwnd_) {
            HWND hwnd = reinterpret_cast<HWND>(emuHwnd_);
            DWORD windowPid = 0;
            GetWindowThreadProcessId(hwnd, &windowPid);
            emuPid_ = windowPid;
            keepEmulatorHidden(hwnd, windowPid);
            LOG_INFO("Capturing emulator window hwnd=%p pid=%llu (hidden from taskbar)",
                (void*)hwnd, (unsigned long long)emuPid_);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif

    // Use the real Android display size for input mapping (the captured
    // surface can be scaled independently).
    std::string sizeOut = runCmd("\"" + adbPath_ + "\" -s " + serial_ + " shell wm size");
    unsigned sw = 0, sh = 0;
    if (std::sscanf(sizeOut.c_str(), "Physical size: %ux%u", &sw, &sh) == 2 && sw > 0 && sh > 0) {
        screenW_ = (int)sw;
        screenH_ = (int)sh;
        LOG_INFO("Logical screen %dx%d", screenW_, screenH_);
    }

    // One hidden shell for all taps/swipes/keys - avoids spawning adb on every
    // input event and removes the noticeable click delay.
    if (!ensureInputShell()) {
        LOG_WARN("Persistent input shell unavailable - using one-shot adb");
    }

#ifdef _WIN32
    if (!emuHwnd_) {
        LOG_WARN("Emulator window not found yet - using ADB capture until it appears");
        setBootStatus("Emulator ready (compat mode)");
    }

    // Created unconditionally: a late-discovered window can start being used
    // without rebuilding the capture path.
    HDC memDC = CreateCompatibleDC(nullptr);
    HBITMAP hBmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    int capW = 0, capH = 0;
    std::vector<uint8_t> bgra;

    const int capFps = std::clamp(spec_.fpsCap, 30, 120);
    const auto interval = std::chrono::milliseconds(std::max(1, 1000 / capFps));
    auto lastLog = std::chrono::steady_clock::now();
    auto lastHide = std::chrono::steady_clock::now();
    auto lastFind = std::chrono::steady_clock::now();
    int framesSinceLog = 0;

    while (running_) {
        if (state_.load() != VmState::Running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        auto now = std::chrono::steady_clock::now();
        if (!emuHwnd_ && now - lastFind >= std::chrono::milliseconds(1000)) {
            emuHwnd_ = findEmulatorWindow();
            if (emuHwnd_) {
                HWND hwnd = reinterpret_cast<HWND>(emuHwnd_);
                DWORD windowPid = 0;
                GetWindowThreadProcessId(hwnd, &windowPid);
                emuPid_ = windowPid;
                keepEmulatorHidden(hwnd, windowPid);
                LOG_INFO("Capturing emulator window hwnd=%p pid=%llu (late discovery)",
                    (void*)hwnd, (unsigned long long)emuPid_);
                setBootStatus("Emulator ready");
            }
            lastFind = now;
        }
        if (emuHwnd_ && now - lastHide >= std::chrono::milliseconds(250)) {
            keepEmulatorHidden(reinterpret_cast<HWND>(emuHwnd_), (DWORD)emuPid_);
            lastHide = now;
        }
        auto t0 = std::chrono::steady_clock::now();
        bool captured = false;
        HWND hwnd = reinterpret_cast<HWND>(emuHwnd_);

        if (memDC && hwnd && IsWindow(hwnd)) {
            RECT rc{};
            if (GetClientRect(hwnd, &rc)) {
                int cw = rc.right - rc.left;
                int ch = rc.bottom - rc.top;
                if (cw > 0 && ch > 0) {
                    if (cw != capW || ch != capH) {
                        if (hBmp) {
                            SelectObject(memDC, oldBmp);
                            DeleteObject(hBmp);
                            hBmp = nullptr;
                        }
                        HDC screenDC = GetDC(nullptr);
                        hBmp = CreateCompatibleBitmap(screenDC, cw, ch);
                        ReleaseDC(nullptr, screenDC);
                        if (hBmp) {
                            oldBmp = SelectObject(memDC, hBmp);
                            capW = cw; capH = ch;
                            bgra.assign((size_t)cw * ch * 4, 0);
                            LOG_INFO("Capture surface %dx%d", cw, ch);
                        }
                    }
                    // 2 == PW_RENDERFULLCONTENT (not in older MinGW headers)
                    if (hBmp && PrintWindow(hwnd, memDC, 2)) {
                        BITMAPINFO bi{};
                        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bi.bmiHeader.biWidth = capW;
                        bi.bmiHeader.biHeight = -capH;
                        bi.bmiHeader.biPlanes = 1;
                        bi.bmiHeader.biBitCount = 32;
                        bi.bmiHeader.biCompression = BI_RGB;
                        bi.bmiHeader.biSizeImage = (DWORD)bgra.size();
                        if (GetDIBits(memDC, hBmp, 0, capH, bgra.data(), &bi, DIB_RGB_COLORS)) {
                            const size_t pixels = (size_t)capW * capH;
                            std::lock_guard<std::mutex> lk(fbMtx_);
                            if (w_ != capW || h_ != capH) {
                                w_ = capW; h_ = capH;
                                fb_.assign((size_t)w_ * h_ * 4, 0);
                                LOG_INFO("Screen size %dx%d", w_, h_);
                            }
                            uint8_t* dst = fb_.data();
                            for (size_t p = 0, i = 0; p < pixels; ++p, i += 4) {
                                dst[i + 0] = bgra[i + 2];
                                dst[i + 1] = bgra[i + 1];
                                dst[i + 2] = bgra[i + 0];
                                dst[i + 3] = 255;
                            }
                            ++frames_;
                            notifyFrame();
                            captured = true;
                        }
                    }
                }
            }
        }

        if (!captured) {
            // Only reached in compat mode; keep it slow on purpose.
            captureAdbFrame();
            std::this_thread::sleep_for(interval);
        }

        ++framesSinceLog;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count();
        if (ms >= 5000) {
            LOG_INFO("Capture %d fps", (int)(framesSinceLog * 1000 / (ms > 0 ? ms : 1)));
            framesSinceLog = 0;
            lastLog = now;
        }

        auto cost = std::chrono::steady_clock::now() - t0;
        if (cost < interval) std::this_thread::sleep_for(interval - cost);
    }

    if (hBmp) {
        SelectObject(memDC, oldBmp);
        DeleteObject(hBmp);
    }
    if (memDC) DeleteDC(memDC);
#else
    // POSIX has no capture window and no GDI surface. The hidden adb
    // exec-out screencap path is the only frame source.
    const int capFps = std::clamp(spec_.fpsCap, 10, 60);
    const auto interval = std::chrono::milliseconds(std::max(1, 1000 / capFps));
    auto lastLog = std::chrono::steady_clock::now();
    int framesSinceLog = 0;
    while (running_) {
        if (state_.load() != VmState::Running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        captureAdbFrame();
        ++framesSinceLog;
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count();
        if (ms >= 5000) {
            LOG_INFO("Capture %d fps (adb)", (int)(framesSinceLog * 1000 / (ms > 0 ? ms : 1)));
            framesSinceLog = 0;
            lastLog = now;
        }
        std::this_thread::sleep_for(interval);
    }
#endif
}

void AndroidVirtualMachine::sendTouch(int x, int y, bool down) {
    if (!down) return;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "input tap %d %d", x, y);
    writeInput(buf);
}

void AndroidVirtualMachine::sendKey(int keycode, bool down) {
    if (!down) return;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "input keyevent %d", keycode);
    writeInput(buf);
}

void AndroidVirtualMachine::sendText(const std::string& utf8) {
    if (utf8.empty()) return;
    // Quote for the adb shell so spaces and punctuation survive intact.
    std::string arg;
    for (char ch : utf8) {
        if (ch == '\'') arg += "'\\''";
        else arg += ch;
    }
    writeInput("input text '" + arg + "'");
}

void AndroidVirtualMachine::sendSwipe(int x1, int y1, int x2, int y2, int durationMs) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "input swipe %d %d %d %d %d", x1, y1, x2, y2, durationMs);
    writeInput(buf);
}

void AndroidVirtualMachine::launchApp(const std::string& packageName) {
    LOG_INFO("launchApp: %s", packageName.c_str());
    writeInput("monkey -p " + packageName + " -c android.intent.category.LAUNCHER 1");
}

void AndroidVirtualMachine::setAudioMuted(bool muted) {
    if (audioMuted_ == muted) return;
    if (adbPath_.empty()) findSdk();
    if (adbPath_.empty()) return;

    auto shell = [&](const std::string& body, int timeoutMs = 3000) {
        return runCmd("\"" + adbPath_ + "\" -s " + serial_ + " shell " + body, timeoutMs);
    };

    if (muted) {
        const std::string out = shell("settings get system volume_music");
        const int current = std::atoi(out.c_str());
        if (current > 0 && current <= 15) volumeBeforeMute_ = current;
        shell("cmd media_session volume --stream 3 --set 0");
        LOG_INFO("Muted parked phone audio on %s", serial_.c_str());
    } else {
        const int restore = volumeBeforeMute_ > 0 ? volumeBeforeMute_ : 8;
        shell("cmd media_session volume --stream 3 --set " + std::to_string(restore));
        LOG_INFO("Restored phone audio on %s to %d", serial_.c_str(), restore);
    }
    audioMuted_ = muted;
}

} // namespace emu
