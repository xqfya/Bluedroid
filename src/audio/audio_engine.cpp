#include "emulator/audio_engine.h"
#include "emulator/logger.h"

namespace emu {
AudioEngine::AudioEngine() {}
AudioEngine& AudioEngine::instance() { static AudioEngine inst; return inst; }
bool AudioEngine::init() { LOG_INFO("Audio engine init (stub)"); return true; }
void AudioEngine::shutdown() { LOG_INFO("Audio engine shutdown"); }
void AudioEngine::setVolume(float v) { volume_ = v; }
} // namespace emu