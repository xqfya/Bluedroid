#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace emu {

struct VmSpec {
    int cpuCores = 4;
    int ramMB = 4096;
    int screenWidth = 1080;
    int screenHeight = 1920;
    int dpi = 320;
    std::string systemImagePath;
    std::string dataImagePath;
    std::string kernelPath;
    bool enableGpu = true;
    bool enableNetwork = true;
    int fpsCap = 60;   // 30 / 60 / 120 capture cap
    int gpuMode = 0;   // 0 host (DirectX), 1 software (SwiftShader)
};

enum class VmState { Stopped, Starting, Running, Paused, Stopping, Error };

class IVirtualMachine {
public:
    virtual ~IVirtualMachine() = default;
    virtual bool create(const VmSpec& spec) = 0;
    virtual bool start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void reset() = 0;
    virtual VmState state() const = 0;
    virtual std::string lastError() const = 0;
    // Short generic boot/status message for the loading screen
    virtual std::string statusMessage() const { return {}; }
    // Real boot progress in 0..100. -1 means the backend has no progress info
    // and the UI should show an indeterminate bar instead of animating a fake one.
    virtual int bootPercent() const { return -1; }

    // Framebuffer access (RGBA8)
    virtual const uint8_t* framebuffer() const = 0;
    virtual int fbWidth() const = 0;
    virtual int fbHeight() const = 0;
    // Logical Android screen size (used for input mapping). Defaults to the
    // captured framebuffer size for simple backends.
    virtual int screenWidth() const { return fbWidth(); }
    virtual int screenHeight() const { return fbHeight(); }
    virtual uint64_t frameCounter() const = 0;
    // Run a callback while the framebuffer is locked. Overrides should hold
    // their frame mutex for the duration of the callback.
    virtual void readFrame(const std::function<void(const uint8_t*, int, int)>& fn) const {
        fn(framebuffer(), fbWidth(), fbHeight());
    }

    // Input injection
    virtual void sendTouch(int x, int y, bool down) = 0;
    virtual void sendKey(int keycode, bool down) = 0;
    virtual void sendText(const std::string& utf8) = 0;
    virtual void sendSwipe(int x1, int y1, int x2, int y2, int durationMs) {}
    virtual void launchApp(const std::string& packageName) {}
    // Guest audio control used to mute phones parked in the Manager.
    virtual void setAudioMuted(bool /*muted*/) {}

    // Callbacks
    using FrameCallback = std::function<void()>;
    void setFrameCallback(FrameCallback cb) { frameCb_ = std::move(cb); }
    void notifyFrame() { if (frameCb_) frameCb_(); }

protected:
    FrameCallback frameCb_;
};

// Factory
IVirtualMachine* createVirtualMachine(const std::string& backend);

} // namespace emu