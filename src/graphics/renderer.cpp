#include "emulator/renderer.h"
#include "emulator/logger.h"
#include <windows.h>

namespace emu {

Renderer::Renderer() {}
bool Renderer::init(int width, int height) { w_ = width; h_ = height; LOG_INFO("Renderer init %dx%d (gdi)", w_, h_); return true; }
void Renderer::resize(int width, int height) { w_ = width; h_ = height; }
void Renderer::render(const uint8_t* fb, int srcW, int srcH) {
    // Actual blit to HWND is handled by the UI layer; this is a no-op stub
    // kept for API completeness. Real impl: StretchDIBits / glTexSubImage2D.
    (void)fb; (void)srcW; (void)srcH;
}
void Renderer::shutdown() { LOG_INFO("Renderer shutdown"); }

} // namespace emu