#include "emulator/mock_vm.h"
#include "emulator/logger.h"
#include <algorithm>
#include <cmath>

namespace emu {

MockVirtualMachine::MockVirtualMachine() {}

bool MockVirtualMachine::create(const VmSpec& spec) {
    spec_ = spec;
    w_ = spec.screenWidth;
    h_ = spec.screenHeight;
    if (w_ <= 0 || h_ <= 0) { err_ = "bad dimensions"; return false; }
    fb_.assign((size_t)w_ * h_ * 4, 0);
    LOG_INFO("Mock VM created %dx%d", w_, h_);
    return true;
}

bool MockVirtualMachine::start() {
    state_ = VmState::Starting;
    // simulate boot
    state_ = VmState::Running;
    LOG_INFO("Mock VM started (demo mode - no real Android)");
    return true;
}

void MockVirtualMachine::pause() { paused_ = true; state_ = VmState::Paused; }
void MockVirtualMachine::resume() { paused_ = false; state_ = VmState::Running; }
void MockVirtualMachine::stop() { state_ = VmState::Stopped; }
void MockVirtualMachine::reset() { frames_ = 0; ballX_ = 100; ballY_ = 100; }

void MockVirtualMachine::sendTouch(int x, int y, bool down) {
    // draw a ripple-ish dot
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    for (int dy = -8; dy <= 8; ++dy)
        for (int dx = -8; dx <= 8; ++dx) {
            int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= w_ || py >= h_) continue;
            if (dx*dx + dy*dy > 64) continue;
            int idx = (py * w_ + px) * 4;
            fb_[idx] = 255; fb_[idx+1] = 200; fb_[idx+2] = 50; fb_[idx+3] = 255;
        }
    LOG_DEBUG("touch %d,%d down=%d", x, y, down);
}

void MockVirtualMachine::sendKey(int keycode, bool down) {
    LOG_DEBUG("key %d down=%d", keycode, down);
}
void MockVirtualMachine::sendText(const std::string& utf8) {
    LOG_DEBUG("text: %s", utf8.c_str());
}

void MockVirtualMachine::tick() {
    if (state_ != VmState::Running || paused_) return;
    // background gradient
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            int idx = (y * w_ + x) * 4;
            fb_[idx]   = (uint8_t)((x * 255) / (w_ > 1 ? w_ - 1 : 1));
            fb_[idx+1] = (uint8_t)((y * 255) / (h_ > 1 ? h_ - 1 : 1));
            fb_[idx+2] = 80;
            fb_[idx+3] = 255;
        }
    }
    // moving ball
    ballX_ += vx_; ballY_ += vy_;
    if (ballX_ < 20 || ballX_ > w_ - 20) vx_ = -vx_;
    if (ballY_ < 20 || ballY_ > h_ - 20) vy_ = -vy_;
    for (int dy = -18; dy <= 18; ++dy)
        for (int dx = -18; dx <= 18; ++dx) {
            if (dx*dx + dy*dy > 18*18) continue;
            int px = ballX_ + dx, py = ballY_ + dy;
            if (px < 0 || py < 0 || px >= w_ || py >= h_) continue;
            int idx = (py * w_ + px) * 4;
            fb_[idx] = 255; fb_[idx+1] = 255; fb_[idx+2] = 255; fb_[idx+3] = 255;
        }
    // HUD text-ish: draw "DEMO MODE" bar
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < w_; ++x) {
            int idx = (y * w_ + x) * 4;
            fb_[idx] = 30; fb_[idx+1] = 30; fb_[idx+2] = 60; fb_[idx+3] = 255;
        }
    ++frames_;
    notifyFrame();
}

} // namespace emu