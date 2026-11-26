#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace emu {

class IVirtualMachine;

class InputManager {
public:
    InputManager(IVirtualMachine* vm);
    // mouse/touch
    void onMouseMove(int x, int y);
    void onMouseDown(int x, int y, int button);
    void onMouseUp(int x, int y, int button);
    // keyboard
    void onKeyDown(int vk);
    void onKeyUp(int vk);
    // mapping helpers
    static int vkToAndroidKeycode(int vk);
private:
    IVirtualMachine* vm_;
    bool mouseDown_ = false;
    int startX_ = 0, startY_ = 0;
    int lastX_ = 0, lastY_ = 0;
};

} // namespace emu