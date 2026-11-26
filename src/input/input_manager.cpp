#include "emulator/input_manager.h"
#include "emulator/vm.h"

#ifdef _WIN32
#include <windows.h>
#else
// sdlcompat maps SDL keys to the same VK_* values the UI shim forwards, so the
// mapping below stays shared and no direct SDL header is needed here.
#include "emulator/sdl_compat.h"
#endif

namespace emu {

InputManager::InputManager(IVirtualMachine* vm) : vm_(vm) {}

void InputManager::onMouseMove(int x, int y) {
    lastX_ = x; lastY_ = y;
}

void InputManager::onMouseDown(int x, int y, int button) {
    (void)button;
    mouseDown_ = true;
    startX_ = x; startY_ = y;
    lastX_ = x; lastY_ = y;
}

void InputManager::onMouseUp(int x, int y, int button) {
    (void)button;
    if (!mouseDown_) return;
    mouseDown_ = false;
    lastX_ = x; lastY_ = y;
    int dx = lastX_ - startX_;
    int dy = lastY_ - startY_;
    if (dx*dx + dy*dy < 100) {
        // tap
        if (vm_) vm_->sendTouch(startX_, startY_, true);
    } else {
        // swipe
        if (vm_) vm_->sendSwipe(startX_, startY_, lastX_, lastY_, 250);
    }
}

void InputManager::onKeyDown(int vk) {
    int keycode = vkToAndroidKeycode(vk);
    if (keycode > 0 && vm_) vm_->sendKey(keycode, true);
}

void InputManager::onKeyUp(int vk) {
    int keycode = vkToAndroidKeycode(vk);
    if (keycode > 0 && vm_) vm_->sendKey(keycode, false);
}

int InputManager::vkToAndroidKeycode(int vk) {
    // Only non-printable keys go through here. Printable keys are sent by
    // WM_CHAR as `input text`, so a Windows VK must never be passed through as
    // an Android keycode (VK_A=65 is not AKEYCODE_A=29).
    switch (vk) {
        case VK_RETURN:    return 66;  // ENTER
        case VK_ESCAPE:    return 4;   // BACK
        case VK_BACK:      return 67;  // DEL
        case VK_TAB:       return 61;  // TAB
        case VK_LEFT:      return 21;
        case VK_UP:        return 19;
        case VK_RIGHT:     return 22;
        case VK_DOWN:      return 20;
        case VK_INSERT:    return 124;
        case VK_DELETE:    return 112;
        case VK_HOME:      return 122;
        case VK_END:       return 123;
        case VK_PRIOR:     return 92;  // PAGE_UP
        case VK_NEXT:      return 93;  // PAGE_DOWN
        default:
            if (vk >= VK_F1 && vk <= VK_F12) return 130 + (vk - VK_F1);
            return 0; // modifier/unmapped keys are intentionally ignored
    }
}

} // namespace emu
