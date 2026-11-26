#pragma once
#include <cstdint>
#include <string>

namespace emu {

// Renders the VM framebuffer to a host window using GDI (Windows) or OpenGL.
class Renderer {
public:
    Renderer();
    bool init(int width, int height);
    void resize(int width, int height);
    void render(const uint8_t* fb, int srcW, int srcH);
    void shutdown();
    std::string backend() const { return backend_; }
private:
    std::string backend_ = "gdi";
    int w_ = 0, h_ = 0;
};

} // namespace emu