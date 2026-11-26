#pragma once
#include <string>

namespace emu {

class AudioEngine {
public:
    static AudioEngine& instance();
    bool init();
    void shutdown();
    void setVolume(float v);
    float volume() const { return volume_; }
private:
    AudioEngine();
    float volume_ = 1.0f;
};

} // namespace emu