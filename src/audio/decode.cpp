#include "audio/decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>

namespace audio {

namespace {

// Blackman-windowed sinc, tabulated in steps of 1/kPhases of a zero crossing (crossings at the cutoff frequency).
constexpr int kZeros = 32, kPhases = 512;

const std::vector<float>& sincTable() {
	static const std::vector<float> table = [] {
		std::vector<float> t(kZeros * kPhases + 2, 0.f);
		for (int j = 0; j < kZeros * kPhases; j++) {
			double x = (double)j / kPhases;
			double sinc = j == 0 ? 1 : std::sin(M_PI * x) / (M_PI * x);
			double w = 0.42 + 0.5 * std::cos(M_PI * x / kZeros) + 0.08 * std::cos(2 * M_PI * x / kZeros);
			t[j] = (float)(sinc * w);
		}
		return t;
	}();
	return table;
}

} // namespace

std::vector<float> resample(const std::vector<float>& in, double from, double to) {
	if (from == to)
		return in;
	const std::vector<float>& table = sincTable();
	const double fc = std::min(1.0, to / from); // cutoff relative to the input Nyquist
	const double half = kZeros / fc;            // kernel half-width in input frames
	const int64_t n = (int64_t)in.size();
	std::vector<float> out((size_t)((double)n / (from / to)));
	// ponytail: direct convolution, ~64 taps per output frame at decode time; polyphase if long files show up.
	for (size_t i = 0; i < out.size(); i++) {
		double x = (double)i * from / to;
		int64_t lo = std::max<int64_t>(0, (int64_t)std::ceil(x - half));
		int64_t hi = std::min<int64_t>(n - 1, (int64_t)std::floor(x + half));
		double sum = 0;
		for (int64_t k = lo; k <= hi; k++) {
			double u = std::fabs(x - (double)k) * fc * kPhases;
			int j = (int)u;
			if (j >= kZeros * kPhases)
				continue;
			sum += in[k] * (table[j] + (u - j) * (table[j + 1] - table[j]));
		}
		out[i] = (float)(sum * fc);
	}
	return out;
}

AudioBufferData* decodeAudio(const uint8_t* data, size_t size, double sampleRate, std::string& error) {
	auto is = [&](const char* magic) { return size >= 4 && std::memcmp(data, magic, 4) == 0; };
	unsigned channels = 0, rate = 0;
	uint64_t frames = 0;
	float* pcm = nullptr;
	void (*freePcm)(void*) = nullptr;
	if (is("RIFF") || is("RIFX") || is("RF64")) {
		drwav_uint64 n = 0;
		pcm = drwav_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &n, nullptr);
		frames = n;
		freePcm = [](void* p) { drwav_free(p, nullptr); };
	} else if (is("fLaC")) {
		drflac_uint64 n = 0;
		pcm = drflac_open_memory_and_read_pcm_frames_f32(data, size, &channels, &rate, &n, nullptr);
		frames = n;
		freePcm = [](void* p) { drflac_free(p, nullptr); };
	} else if (is("OggS")) {
		error = "Ogg audio is not supported (WAV, MP3 and FLAC are)";
		return nullptr;
	} else { // MP3 has no reliable magic (ID3 tag or a frame sync)
		drmp3_config config{};
		drmp3_uint64 n = 0;
		pcm = drmp3_open_memory_and_read_pcm_frames_f32(data, size, &config, &n, nullptr);
		channels = config.channels;
		rate = config.sampleRate;
		frames = n;
		freePcm = [](void* p) { drmp3_free(p, nullptr); };
	}
	if (!pcm || !frames || !channels || !rate) {
		if (pcm)
			freePcm(pcm);
		error = "unknown or corrupt audio data";
		return nullptr;
	}
	auto* b = new AudioBufferData((int)channels, 0, sampleRate);
	std::vector<float> ch(frames);
	for (unsigned c = 0; c < channels; c++) {
		for (uint64_t i = 0; i < frames; i++)
			ch[i] = pcm[i * channels + c];
		b->data[c] = resample(ch, rate, sampleRate);
	}
	freePcm(pcm);
	return b;
}

} // namespace audio
