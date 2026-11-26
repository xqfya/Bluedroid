#pragma once
#include "emulator/vm.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstddef>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace emu {

// Snapshot published while an APK is really being transferred/installed.
struct ApkInstallProgress {
    size_t bytesDone = 0;
    size_t bytesTotal = 0;
    std::string stage;
};

using ApkProgressCallback = std::function<void(const ApkInstallProgress&)>;

class AndroidVirtualMachine : public IVirtualMachine {
public:
    AndroidVirtualMachine();
    ~AndroidVirtualMachine();
    // Select the AVD and ADB serial for this logical phone before create().
    // attachExisting=true reconnects to an emulator left running by a previous
    // (temporarily closed) manager session instead of launching a new one.
    void configure(const std::string& avdName, int port, bool attachExisting = false);
    // Stable name of the per-phone APK library folder under data/.
    std::string apkStoreDir() const;
    bool create(const VmSpec& spec) override;
    bool start() override;
    void pause() override;
    void resume() override;
    void stop() override;
    void reset() override;
    bool attached() const { return attachOnly_; }
    // Process that owns the emulator surface, used by the manager for live
    // CPU/RAM badges. Returns 0 when no emulator process is available.
    DWORD processId() const { return emuPid_ ? emuPid_ : procId_; }
    bool isDeviceOnline();
    // Shut down an emulator that this VM is attached to but did not launch.
    void terminate();
    VmState state() const override { return state_.load(); }
    std::string lastError() const override { return err_; }
    std::string statusMessage() const override;
    int bootPercent() const override { return bootPercent_.load(); }

    // Push an APK to this device and install it. Progress is driven by real
    // APK bytes represented by the encoded blocks written to the adb pipe,
    // not by a timer.
    // cancel may be flipped from the UI to abort the transfer.
    bool installApk(const std::string& apkPath, const ApkProgressCallback& progress,
                    const std::atomic<bool>* cancel = nullptr);
    const uint8_t* framebuffer() const override { std::lock_guard<std::mutex> lk(fbMtx_); return fb_.data(); }
    int fbWidth() const override { return w_; }
    int fbHeight() const override { return h_; }
    int screenWidth() const override { return screenW_; }
    int screenHeight() const override { return screenH_; }
    uint64_t frameCounter() const override { return frames_.load(); }
    void readFrame(const std::function<void(const uint8_t*, int, int)>& fn) const override;
    void sendTouch(int x, int y, bool down) override;
    void sendKey(int keycode, bool down) override;
    void sendText(const std::string& utf8) override;
    void sendSwipe(int x1, int y1, int x2, int y2, int durationMs) override;
    void launchApp(const std::string& packageName) override;
    void setAudioMuted(bool muted) override;
private:
    bool findSdk();
    bool ensureAvd();
    bool checkAcceleration();
    void launchEmulator();
    void waitForBoot();
    void captureLoop();
    bool captureAdbFrame();
    HWND findEmulatorWindow() const;
    bool ensureInputShell();
    void writeInput(const std::string& cmd);
    void setBootStatus(const char* text, int percent = -1);
    std::string runCmd(const std::string& cmd, DWORD timeoutMs = 10000);
    std::string adbPath_;
    std::string emulatorPath_;
    std::string emulatorCheckPath_;
    std::string sdkRoot_;
    std::string avdName_ = "BlueDroid";
    int port_ = 5554;
    std::string serial_ = "emulator-5554";
    VmSpec spec_;
    std::atomic<VmState> state_{VmState::Stopped};
    std::string err_;
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> fb_;
    std::atomic<uint64_t> frames_{0};
    std::thread capThread_;
    std::atomic<bool> running_{false};
    mutable std::mutex fbMtx_;
    HANDLE procHandle_ = nullptr;
    DWORD procId_ = 0;
    HWND emuHwnd_ = nullptr;
    DWORD emuPid_ = 0;
    HANDLE inputProc_ = nullptr;
    HANDLE inputStdin_ = nullptr;
    std::mutex inputMtx_;
    int screenW_ = 1080;
    int screenH_ = 1920;
    std::string bootStatus_ = "Preparing emulator";
    std::atomic<int> bootPercent_{0};
    int volumeBeforeMute_ = 8;
    bool audioMuted_ = false;
    mutable std::mutex statusMtx_;
    std::string framePath_;
    std::string tmpDir_;
    bool attachOnly_ = false;
};

} // namespace emu