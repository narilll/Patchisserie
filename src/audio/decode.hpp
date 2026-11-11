#pragma once
// decodeAudioData's work: compressed bytes -> AudioBufferData at the context's sample rate. Pure logic.
#include <string>

#include "audio/nodes.hpp"

namespace audio {

/** WAV, MP3 or FLAC -> a new buffer resampled to sampleRate; nullptr and `error` on failure. */
AudioBufferData* decodeAudio(const uint8_t* data, size_t size, double sampleRate, std::string& error);

/** Offline windowed-sinc resampling of one channel, band-limited to the lower Nyquist, zero delay.
Output length floor(n * to / from), like Chrome's decodeAudioData. */
std::vector<float> resample(const std::vector<float>& in, double from, double to);

} // namespace audio
