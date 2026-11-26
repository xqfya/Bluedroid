#include "emulator/qemu_vm.h"
#include "emulator/logger.h"
#include <cstdlib>

namespace emu {

static std::string findQemu() {
    // very light probe
#ifdef _WIN32
    const char* cands[] = { "qemu-system-x86_64", "qemu-system-x86_64.exe",
                            "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" };
#else
    const char* cands[] = { "qemu-system-x86_64" };
#endif
    for (auto c : cands) {
        // We can't easily check; just return first candidate name and let start() fail.
    }
    return "qemu-system-x86_64";
}

QemuVirtualMachine::QemuVirtualMachine() : qemuPath_(findQemu()) {}

bool QemuVirtualMachine::create(const VmSpec& spec) {
    spec_ = spec;
    w_ = spec.screenWidth; h_ = spec.screenHeight;
    fb_.assign((size_t)w_ * h_ * 4, 0);
    LOG_INFO("QEMU VM configured %dx%d (backend=%s)", w_, h_, qemuPath_.c_str());
    // NOTE: real implementation would validate systemImagePath/kernelPath here.
    if (spec.systemImagePath.empty()) {
        err_ = "systemImagePath not set";
        // not fatal for demo scaffolding
    }
    return true;
}

bool QemuVirtualMachine::start() {
    state_ = VmState::Starting;
    // In a full implementation: spawn qemu process with -accel, -drive, -vnc, etc.
    // Here we mark error because no real qemu integration is wired.
    err_ = "QEMU backend not wired in this build; use 'mock' backend or install qemu-system-x86_64 and extend QemuVirtualMachine::start().";
    state_ = VmState::Error;
    LOG_ERROR("%s", err_.c_str());
    return false;
}

void QemuVirtualMachine::pause() { state_ = VmState::Paused; }
void QemuVirtualMachine::resume() { state_ = VmState::Running; }
void QemuVirtualMachine::stop() { state_ = VmState::Stopped; }
void QemuVirtualMachine::reset() { frames_ = 0; }
void QemuVirtualMachine::sendTouch(int, int, bool) {}
void QemuVirtualMachine::sendKey(int, bool) {}
void QemuVirtualMachine::sendText(const std::string&) {}

} // namespace emu