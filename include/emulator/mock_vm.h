#pragma once
#include "emulator/vm.h"

namespace emu {

class MockVirtualMachine : public IVirtualMachine {
public:
    MockVirtualMachine();
    bool create(const VmSpec& spec) override;
    bool start() override;
    void pause() override;
    void resume() override;
    void stop() override;
    void reset() override;
    VmState state() const override { return state_; }
    std::string lastError() const override { return err_; }
    const uint8_t* framebuffer() const override { return fb_.data(); }
    int fbWidth() const override { return w_; }
    int fbHeight() const override { return h_; }
    uint64_t frameCounter() const override { return frames_; }
    void sendTouch(int x, int y, bool down) override;
    void sendKey(int keycode, bool down) override;
    void sendText(const std::string& utf8) override;
    void tick(); // advance simulation
private:
    VmSpec spec_;
    VmState state_ = VmState::Stopped;
    std::string err_;
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> fb_;
    uint64_t frames_ = 0;
    int ballX_ = 100, ballY_ = 100, vx_ = 4, vy_ = 3;
    bool paused_ = false;
};

} // namespace emu