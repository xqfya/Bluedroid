#include "emulator/renderer.h"
#include "emulator/logger.h"

namespace emu {

Renderer::Renderer() {
#ifdef _WIN32
    backend_ = "gdi";
#else
    backend_ = "stub";
#endif
}
bool Renderer::init(int width, int height) {
    w_ = width; h_ = height;
    LOG_INFO("Renderer init %dx%d (%s)", w_, h_, backend_.c_str());
    return true;
}
void Renderer::resize(int width, int height) { w_ = width; h_ = height; }
void Renderer::render(const uint8_t* fb, int srcW, int srcH) {
    // The actual blit is handled by the UI layer (HWND on Windows, SDL/OpenGL
    // elsewhere); this is a no-op stub kept for API completeness.
    (void)fb; (void)srcW; (void)srcH;
}
void Renderer::shutdown() { LOG_INFO("Renderer shutdown"); }

} // namespace emu