#include "audio/nodes.hpp"

#include <algorithm>

namespace audio {

namespace {

// Rack's dsp::RealFFT needs pffft from libRack, which the host-built tests don't link. A radix-2 complex FFT is
// enough here: the convolver's cost is the spectral multiply-add, not the transforms.
struct Fft {
	explicit Fft(int n) : n(n), cosT(n / 2), sinT(n / 2), rev(n) {
		for (int i = 0; i < n / 2; i++) {
			cosT[i] = (float)std::cos(2 * M_PI * i / n);
			sinT[i] = (float)std::sin(2 * M_PI * i / n);
		}
		int bits = 0;
		while ((1 << bits) < n)
			bits++;
		for (int i = 0; i < n; i++) {
			int r = 0;
			for (int b = 0; b < bits; b++)
				r |= ((i >> b) & 1) << (bits - 1 - b);
			rev[i] = r;
		}
	}
	/** Unnormalized; forward uses e^-i. */
	void run(float* re, float* im, bool inverse) const {
		for (int i = 0; i < n; i++)
			if (i < rev[i]) {
				std::swap(re[i], re[rev[i]]);
				std::swap(im[i], im[rev[i]]);
			}
		for (int len = 2; len <= n; len <<= 1) {
			int half = len / 2, step = n / len;
			for (int i = 0; i < n; i += len)
				for (int k = 0; k < half; k++) {
					float wr = cosT[k * step], wi = inverse ? sinT[k * step] : -sinT[k * step];
					int a = i + k, b = a + half;
					float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
					re[b] = re[a] - tr;
					im[b] = im[a] - ti;
					re[a] += tr;
					im[a] += ti;
				}
		}
	}
	int n;
	std::vector<float> cosT, sinT;
	std::vector<int> rev;
};

// Function statics: first use is always a producer-side constructor, so the render thread never initializes them.
const Fft& convolverFft() {
	static const Fft f(2 * kQuantum);
	return f;
}

const Fft& waveFft() {
	static const Fft f(WaveTable::kSize);
	return f;
}

const float* halfbandTaps() {
	static float taps[32];
	static bool init = [] {
		const int n = 32;
		double sum = 0;
		for (int k = 0; k < n; k++) {
			double x = 0.5 * (k - (n - 1) / 2.0);
			double sinc = x == 0 ? 1 : std::sin(M_PI * x) / (M_PI * x);
			double blackman = 0.42 - 0.5 * std::cos(2 * M_PI * k / (n - 1)) + 0.08 * std::cos(4 * M_PI * k / (n - 1));
			taps[k] = (float)(sinc * blackman);
			sum += taps[k];
		}
		for (float& t : taps)
			t = (float)(t / sum);
		return true;
	}();
	(void)init;
	return taps;
}

float peak(const Bus& b, int channels) {
	float p = 0;
	for (int c = 0; c < channels; c++)
		for (int i = 0; i < kQuantum; i++)
			p = std::max(p, std::fabs(b.data[c][i]));
	return p;
}

} // namespace

// --- Gain ---

void GainNode::process(int64_t) {
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	if (in.silent)
		return out.silence();
	out.begin(in.channels);
	for (int c = 0; c < in.channels; c++)
		for (int i = 0; i < kQuantum; i++)
			out.data[c][i] = in.data[c][i] * gain.at(i);
}

// --- BiquadFilter ---

void BiquadFilterNode::coefficients(Type type, double frequency, double q, double gainDb, double sampleRate,
									double c[5]) {
	double w = frequency / (0.5 * sampleRate); // 0..1 = 0..Nyquist
	double A = std::pow(10, gainDb / 40);
	bool linearQ = type == Bandpass || type == Notch || type == Allpass || type == Peaking;
	// Degenerate cases (w0 = 0 or pi, Q <= 0) use the formulas' limits, as Chrome does.
	double k = NAN;
	if (w >= 1 || w <= 0) {
		bool top = w >= 1;
		k = type == Lowpass     ? (top ? 1 : 0)
			: type == Highpass  ? (top ? 0 : 1)
			: type == Bandpass  ? 0
			: type == Lowshelf  ? (top ? A * A : 1)
			: type == Highshelf ? (top ? 1 : A * A)
								: 1;
	} else if (linearQ && q <= 0) {
		k = type == Bandpass ? 1 : type == Notch ? 0 : type == Allpass ? -1 : A * A;
	}
	if (!std::isnan(k)) {
		c[0] = k;
		c[1] = c[2] = c[3] = c[4] = 0;
		return;
	}
	double w0 = M_PI * w, cw = std::cos(w0), sw = std::sin(w0);
	double aQ = sw / (2 * q);
	double aQdB = sw / (2 * std::pow(10, q / 20));
	double aS = sw / 2 * std::sqrt(2.0); // S = 1
	double sA = 2 * aS * std::sqrt(A);
	double b0, b1, b2, a0, a1, a2;
	switch (type) {
	case Lowpass:
		b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + aQdB, a1 = -2 * cw, a2 = 1 - aQdB;
		break;
	case Highpass:
		b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + aQdB, a1 = -2 * cw, a2 = 1 - aQdB;
		break;
	case Bandpass:
		b0 = aQ, b1 = 0, b2 = -aQ, a0 = 1 + aQ, a1 = -2 * cw, a2 = 1 - aQ;
		break;
	case Notch:
		b0 = 1, b1 = -2 * cw, b2 = 1, a0 = 1 + aQ, a1 = -2 * cw, a2 = 1 - aQ;
		break;
	case Allpass:
		b0 = 1 - aQ, b1 = -2 * cw, b2 = 1 + aQ, a0 = 1 + aQ, a1 = -2 * cw, a2 = 1 - aQ;
		break;
	case Peaking:
		b0 = 1 + aQ * A, b1 = -2 * cw, b2 = 1 - aQ * A, a0 = 1 + aQ / A, a1 = -2 * cw, a2 = 1 - aQ / A;
		break;
	case Lowshelf:
		b0 = A * ((A + 1) - (A - 1) * cw + sA), b1 = 2 * A * ((A - 1) - (A + 1) * cw),
		b2 = A * ((A + 1) - (A - 1) * cw - sA), a0 = (A + 1) + (A - 1) * cw + sA, a1 = -2 * ((A - 1) + (A + 1) * cw),
		a2 = (A + 1) + (A - 1) * cw - sA;
		break;
	default: // Highshelf
		b0 = A * ((A + 1) + (A - 1) * cw + sA), b1 = -2 * A * ((A - 1) + (A + 1) * cw),
		b2 = A * ((A + 1) + (A - 1) * cw - sA), a0 = (A + 1) - (A - 1) * cw + sA, a1 = 2 * ((A - 1) - (A + 1) * cw),
		a2 = (A + 1) - (A - 1) * cw - sA;
		break;
	}
	c[0] = b0 / a0, c[1] = b1 / a0, c[2] = b2 / a0, c[3] = a1 / a0, c[4] = a2 / a0;
}

void BiquadFilterNode::process(int64_t) {
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	if (in.silent) {
		double m = 0;
		for (auto& st : s)
			for (double v : st)
				m = std::max(m, std::fabs(v));
		if (m < 1e-10) { // tail decayed: flush and go quiet
			std::memset(s, 0, sizeof s);
			return out.silence();
		}
	} else {
		lastChannels = in.channels;
	}
	int ch = in.silent ? lastChannels : in.channels;
	out.begin(ch);
	double nyquist = 0.5 * sampleRate;
	auto freqAt = [&](int i) {
		double f = frequency.at(i) * (detune.constant && detune.values[0] == 0 ? 1 : std::exp2(detune.at(i) / 1200));
		return std::min(std::max(f, 0.0), nyquist);
	};
	bool fixed = frequency.constant && detune.constant && Q.constant && gain.constant;
	double c[5];
	if (fixed)
		coefficients(type, freqAt(0), Q.values[0], gain.values[0], sampleRate, c);
	// ponytail: a-rate automation recomputes coefficients every frame (as Chrome does); sub-block them if the
	// benchmark ever shows filter sweeps dominating.
	for (int i = 0; i < kQuantum; i++) {
		if (!fixed)
			coefficients(type, freqAt(i), Q.at(i), gain.at(i), sampleRate, c);
		for (int k = 0; k < ch; k++) {
			double* st = s[k];
			double x = in.data[k][i]; // silent input bus is zeros
			double y = c[0] * x + c[1] * st[0] + c[2] * st[1] - c[3] * st[2] - c[4] * st[3];
			st[1] = st[0], st[0] = x, st[3] = st[2], st[2] = y;
			out.data[k][i] = (float)y;
		}
	}
}

void BiquadFilterNode::setAttr(int attr, double v, Resource* r) {
	if (attr == AttrType)
		type = (Type)(int)v;
	else
		Node::setAttr(attr, v, r);
}

// --- Scheduled sources ---

void ScheduledSource::start(double when, double, double) {
	if (started)
		return;
	started = true;
	startFrame = timeToFrame(std::max(when, 0.0), sampleRate);
}

void ScheduledSource::stop(double when) {
	if (started && !ended)
		stopFrame = timeToFrame(when, sampleRate);
}

void ScheduledSource::range(int64_t f0, int& begin, int& end) const {
	begin = (int)std::min<int64_t>(std::max<int64_t>(startFrame - f0, 0), kQuantum);
	end = (int)std::min<int64_t>(std::max<int64_t>(stopFrame - f0, 0), kQuantum);
}

void ScheduledSource::finish(int64_t f0) {
	if (started && !ended && stopFrame <= f0 + kQuantum) {
		ended = true;
		graph->emit({Event::Ended, id, 0});
	}
}

// --- Oscillator ---

WaveTable::WaveTable(const float* real, const float* imag, int length, bool normalize) {
	const Fft& fft = waveFft();
	// ponytail: partials above kSize / 2 are dropped (spec only requires 8192-element support for construction).
	int maxPartial = std::min(length - 1, kSize / 2);
	std::vector<float> re(kSize), im(kSize);
	int prev = -1;
	for (int r = 0; r < kRanges; r++) {
		int partials = std::min(maxPartial, (int)((kSize / 2) * std::exp2(-(double)r / kRangesPerOctave)));
		if (partials == prev) {
			offset[r] = offset[r - 1];
			continue;
		}
		prev = partials;
		std::fill(re.begin(), re.end(), 0.f);
		std::fill(im.begin(), im.end(), 0.f);
		// x(n) = sum a[k] cos + b[k] sin = Re(sum (a[k] - i b[k]) e^(i 2 pi k n / N))
		for (int k = 1; k <= partials; k++) {
			re[k] = real ? real[k] : 0;
			im[k] = imag ? -imag[k] : 0;
		}
		fft.run(re.data(), im.data(), true);
		offset[r] = data.size();
		data.insert(data.end(), re.begin(), re.end());
		data.push_back(re[0]);
	}
	if (normalize) { // spec: one factor, from the full-band waveform, for all tables
		float p = 0;
		for (int i = 0; i < kSize; i++)
			p = std::max(p, std::fabs(data[i]));
		if (p > 0)
			for (float& v : data)
				v /= p;
	}
}

const WaveTable* WaveTable::builtin(int type) {
	static const WaveTable* tables[4] = {};
	static bool init = [] {
		const int n = kSize / 2 + 1;
		std::vector<float> b(n);
		for (int t = 0; t < 4; t++) {
			for (int k = 1; k < n; k++) {
				double pk = M_PI * k;
				b[k] = t == OscillatorNode::Sine       ? (k == 1)
					   : t == OscillatorNode::Square   ? 2 / pk * (1 - ((k & 1) ? -1 : 1))
					   : t == OscillatorNode::Sawtooth ? ((k & 1) ? 2 : -2) / pk
													   : 8 * std::sin(pk / 2) / (pk * pk);
			}
			tables[t] = new WaveTable(nullptr, b.data(), t == OscillatorNode::Sine ? 2 : n, true);
		}
		return true;
	}();
	(void)init;
	return tables[type];
}

void WaveTable::choose(double f, double sampleRate, const float*& rich, const float*& poor, float& mix) const {
	// Fractional range whose partial limit fits under Nyquist (Chrome's pitchRange): no table read here can alias.
	double r = f > 0 ? 1 + kRangesPerOctave * std::log2(f * (kSize / 2) / (0.5 * sampleRate)) : 0;
	r = std::min(std::max(r, 0.0), (double)(kRanges - 1));
	int i = (int)r;
	rich = &data[offset[i]];
	poor = &data[offset[std::min(i + 1, kRanges - 1)]];
	mix = (float)(r - i);
}

void OscillatorNode::process(int64_t f0) {
	Bus& out = outputs[0];
	int a, b;
	range(f0, a, b);
	if (a >= b) {
		out.silence();
		return finish(f0);
	}
	out.begin(1);
	float* o = out.data[0];
	std::fill(o, o + a, 0.f);
	std::fill(o + b, o + kQuantum, 0.f);
	double nyquist = 0.5 * sampleRate;
	float freq[kQuantum];
	bool fixed = frequency.constant && detune.constant;
	bool detuned = !(detune.constant && detune.values[0] == 0);
	int len = fixed ? 1 : kQuantum;
	double maxAbs = 0;
	for (int i = 0; i < len; i++) {
		double f = frequency.at(i) * (detuned ? std::exp2(detune.at(i) / 1200) : 1);
		freq[i] = (float)std::min(std::max(f, -nyquist), nyquist);
		maxAbs = std::max(maxAbs, std::fabs((double)freq[i]));
	}
	// ponytail: table choice per quantum from the highest pitch in it (never aliases); per frame if FM ever needs it.
	const float *rich, *poor;
	float mix;
	wave->choose(maxAbs, sampleRate, rich, poor, mix);
	for (int i = a; i < b; i++) {
		double p = phase * WaveTable::kSize;
		int k = (int)p;
		float fr = (float)(p - k);
		float v1 = rich[k] + fr * (rich[k + 1] - rich[k]);
		float v2 = poor[k] + fr * (poor[k + 1] - poor[k]);
		o[i] = v1 + mix * (v2 - v1);
		phase += freq[fixed ? 0 : i] / sampleRate;
		phase -= std::floor(phase);
		if (phase >= 1) // -tiny wraps to 1.0 in double
			phase = 0;
	}
	finish(f0);
}

void OscillatorNode::setAttr(int attr, double v, Resource* r) {
	if (attr == AttrType && (int)v != Custom) {
		graph->drop(custom);
		custom = nullptr;
		wave = WaveTable::builtin((int)v);
	} else if (attr == AttrPeriodicWave) {
		graph->drop(custom);
		wave = custom = static_cast<WaveTable*>(r);
	} else {
		Node::setAttr(attr, v, r);
	}
}

// --- ConstantSource ---

void ConstantSourceNode::process(int64_t f0) {
	Bus& out = outputs[0];
	int a, b;
	range(f0, a, b);
	if (a >= b) {
		out.silence();
		return finish(f0);
	}
	out.begin(1);
	for (int i = 0; i < kQuantum; i++)
		out.data[0][i] = i >= a && i < b ? offset.at(i) : 0.f;
	finish(f0);
}

// --- AudioBufferSource: the spec's playback algorithm ---

void AudioBufferSourceNode::start(double when, double offset, double duration) {
	if (started)
		return;
	ScheduledSource::start(when, offset, duration);
	this->offset = std::max(offset, 0.0);
	this->duration = duration < 0 ? INFINITY : duration;
}

namespace {
// Buffer positions in frames: snap values that are integers up to JS rounding (n / sampleRate * sampleRate).
double snap(double frames) {
	double r = std::round(frames);
	return std::fabs(frames - r) < 1e-6 ? r : frames;
}
} // namespace

void AudioBufferSourceNode::process(int64_t f0) {
	Bus& out = outputs[0];
	if (!started || ended)
		return out.silence();
	if (!buffer)
		stopFrame = std::min(stopFrame, f0); // spec: null buffer stops the node
	int ch = buffer ? std::min((int)buffer->data.size(), kMaxChannels) : 1;
	// The spec algorithm in buffer frames instead of seconds, so integer playheads stay exact.
	double bufSr = buffer ? buffer->sampleRate : sampleRate;
	int64_t n = buffer ? buffer->length() : 0;
	double len = (double)n;
	double step = playbackRate.values[0] * std::exp2(detune.values[0] / 1200) * bufSr / sampleRate; // k-rate
	double ls = 0, le = len, durationFrames = snap(duration * bufSr);
	if (loop && buffer) {
		if (loopStart >= 0 && loopEnd > 0 && loopStart < loopEnd)
			ls = snap(loopStart * bufSr), le = std::min(snap(loopEnd * bufSr), len);
	} else {
		enteredLoop = false;
	}
	int64_t loopStartFrame = std::min<int64_t>((int64_t)ls, std::max<int64_t>(n - 1, 0));
	bool any = false;
	for (int i = 0; i < kQuantum; i++) {
		int64_t f = f0 + i;
		if (f < startFrame || f >= stopFrame || elapsed >= durationFrames) {
			for (int c = 0; c < ch; c++)
				out.data[c][i] = 0;
			continue;
		}
		if (!playing) {
			startPos = std::min(snap(offset * bufSr), len);
			if (loop && step >= 0 && startPos >= le)
				startPos = le;
			if (loop && step < 0 && startPos < ls)
				startPos = ls;
			pos = startPos;
			playing = true;
		}
		if (loop) {
			if (!enteredLoop && ((startPos < le && pos >= ls) || (startPos >= le && pos < le)))
				enteredLoop = true;
			if (enteredLoop && le > ls) {
				while (pos >= le)
					pos -= le - ls;
				while (pos < ls)
					pos += le - ls;
			}
		}
		if (pos >= 0 && pos < len) {
			int64_t k = std::min((int64_t)pos, n - 1);
			float fr = (float)(pos - (double)k);
			for (int c = 0; c < ch; c++) {
				const std::vector<float>& d = buffer->data[c];
				float s0 = d[k];
				float s1 = k + 1 < n ? d[k + 1] : loop ? d[loopStartFrame] : s0;
				out.data[c][i] = fr == 0 ? s0 : s0 + fr * (s1 - s0);
			}
			any = true;
		} else {
			for (int c = 0; c < ch; c++)
				out.data[c][i] = 0;
		}
		pos += step;
		elapsed += step;
		// Not looping and past either end, or the requested duration played: playback is over.
		if ((!loop && (pos >= len || pos < 0)) || elapsed >= durationFrames)
			stopFrame = std::min(stopFrame, f + 1);
	}
	if (any)
		out.begin(ch);
	else
		out.silence();
	finish(f0);
}

void AudioBufferSourceNode::setAttr(int attr, double v, Resource* r) {
	if (attr == AttrBuffer) {
		graph->drop(buffer);
		buffer = static_cast<AudioBufferData*>(r);
	} else if (attr == AttrLoop) {
		loop = v != 0;
	} else if (attr == AttrLoopStart) {
		loopStart = v;
	} else if (attr == AttrLoopEnd) {
		loopEnd = v;
	} else {
		Node::setAttr(attr, v, r);
	}
}

// --- StereoPanner (spec equal-power algorithm) ---

void StereoPannerNode::process(int64_t) {
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	if (in.silent)
		return out.silence();
	out.begin(2);
	bool stereo = in.channels == 2;
	float gl = 0, gr = 0, p = 0;
	for (int i = 0; i < kQuantum; i++) {
		if (i == 0 || !pan.constant) {
			p = std::min(std::max(pan.at(i), -1.f), 1.f);
			float x = stereo ? (p <= 0 ? p + 1 : p) : (p + 1) / 2;
			gl = std::cos(x * (float)M_PI / 2);
			gr = std::sin(x * (float)M_PI / 2);
		}
		float l = in.data[0][i], r = in.data[stereo ? 1 : 0][i];
		if (!stereo) {
			out.data[0][i] = l * gl;
			out.data[1][i] = l * gr;
		} else if (p <= 0) {
			out.data[0][i] = l + r * gl;
			out.data[1][i] = r * gr;
		} else {
			out.data[0][i] = l * gl;
			out.data[1][i] = r + l * gr;
		}
	}
}

// --- WaveShaper ---

WaveShaperNode::WaveShaperNode(double sr) : Node(sr, 1, 1) {
	halfbandTaps();
}

float WaveShaperNode::shape(const std::vector<float>& c, float x) {
	int n = (int)c.size();
	float v = (n - 1) * 0.5f * (x + 1);
	if (!(v > 0)) // also NaN
		return c[0];
	if (v >= n - 1)
		return c[n - 1];
	int k = (int)v;
	float f = v - k;
	return (1 - f) * c[k] + f * c[k + 1];
}

// Up: zero-stuffed input through the FIR (x2 gain), split into its two polyphase halves.
void WaveShaperNode::Stage::up(const float* in, int n, float* out) {
	const float* h = halfbandTaps();
	const int half = kTaps / 2;
	float x[half + 2 * kQuantum];
	std::memcpy(x, upHistory, sizeof upHistory);
	std::memcpy(x + half, in, n * sizeof(float));
	for (int i = 0; i < n; i++) {
		const float* p = x + half + i;
		float even = 0, odd = 0;
		for (int k = 0; k < half; k++) {
			even += h[2 * k] * p[-k];
			odd += h[2 * k + 1] * p[-k];
		}
		out[2 * i] = 2 * even;
		out[2 * i + 1] = 2 * odd;
	}
	std::memcpy(upHistory, x + n, sizeof upHistory);
}

void WaveShaperNode::Stage::down(const float* in, int n, float* out) {
	const float* h = halfbandTaps();
	float x[kTaps + 4 * kQuantum];
	std::memcpy(x, downHistory, sizeof downHistory);
	std::memcpy(x + kTaps, in, n * sizeof(float));
	for (int i = 0; i < n / 2; i++) {
		const float* p = x + kTaps + 2 * i + 1;
		float y = 0;
		for (int k = 0; k < kTaps; k++)
			y += h[k] * p[-k];
		out[i] = y;
	}
	std::memcpy(downHistory, x + n, sizeof downHistory);
}

void WaveShaperNode::process(int64_t) {
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	// Spec: a curve with non-zero output for zero input makes DC even without input.
	if (in.silent && (!curve || shape(curve->data, 0) == 0))
		return out.silence();
	out.begin(in.channels);
	for (int c = 0; c < in.channels; c++) {
		if (!curve) {
			std::memcpy(out.data[c], in.data[c], sizeof out.data[c]);
			continue;
		}
		const std::vector<float>& cv = curve->data;
		if (oversample == 1) {
			for (int i = 0; i < kQuantum; i++)
				out.data[c][i] = shape(cv, in.data[c][i]);
			continue;
		}
		float a[4 * kQuantum], b[4 * kQuantum];
		Stage* st = stages[c];
		st[0].up(in.data[c], kQuantum, a);
		int n = 2 * kQuantum;
		float* x = a;
		if (oversample == 4) {
			st[1].up(a, n, b);
			n *= 2;
			x = b;
		}
		for (int i = 0; i < n; i++)
			x[i] = shape(cv, x[i]);
		if (oversample == 4) {
			st[1].down(b, n, a);
			n /= 2;
		}
		st[0].down(a, n, out.data[c]);
	}
}

void WaveShaperNode::setAttr(int attr, double v, Resource* r) {
	if (attr == AttrCurve) {
		graph->drop(curve);
		curve = static_cast<CurveData*>(r);
	} else if (attr == AttrOversample) {
		oversample = v >= 4 ? 4 : v >= 2 ? 2 : 1;
	} else {
		Node::setAttr(attr, v, r);
	}
}

// --- Delay ---

DelayNode::DelayNode(double sr, double maxDelayTime)
	: Node(sr, 1, 1), delayTime(this, 0, 0, (float)maxDelayTime, true), maxFrames(maxDelayTime * sr),
	  len((int64_t)std::ceil(maxFrames) + 2 * kQuantum + 2) {
	for (auto& b : buf)
		b.assign(len, 0.f);
}

void DelayNode::write(int64_t f0) {
	const Bus& in = inputs[0].bus;
	if (!in.silent) {
		lastInput = f0;
		if (in.channels == 2)
			lastStereo = f0;
	}
	for (int c = 0; c < kMaxChannels; c++) {
		const float* src = in.data[std::min(c, in.channels - 1)]; // mono stored up-mixed: one channel layout
		for (int i = 0; i < kQuantum; i++)
			buf[c][(f0 + i) % len] = src[i];
	}
}

void DelayNode::read(int64_t f0) {
	lastF0 = f0;
	Bus& out = outputs[0];
	if (tailDone(f0))
		return out.silence();
	int ch = f0 - lastStereo <= maxFrames + kQuantum ? 2 : 1;
	out.begin(ch);
	// Spec: in a cycle the delay is at least one quantum, so the reader only touches frames already written.
	double minFrames = inCycle ? kQuantum : 0;
	for (int i = 0; i < kQuantum; i++) {
		double d = std::min(std::max(delayTime.at(i) * sampleRate, minFrames), maxFrames);
		double r = (double)(f0 + i) - d;
		int64_t k = (int64_t)std::floor(r);
		float fr = (float)(r - (double)k);
		int64_t i0 = ((k % len) + len) % len, i1 = (i0 + 1) % len;
		for (int c = 0; c < ch; c++)
			out.data[c][i] = fr == 0 ? buf[c][i0] : buf[c][i0] + fr * (buf[c][i1] - buf[c][i0]);
	}
	// Spec: a delay (in a cycle) stops being active once its output is below 2^-126.
	if (peak(out, ch) < 1.1754944e-38f)
		out.silence();
}

bool DelayNode::reclaimable() const {
	return !inputs[0].edges && (!outEdges || tailDone(lastF0));
}

// --- Convolver: uniformly partitioned overlap-save, one partition per quantum, zero latency ---

ConvolverKernel::ConvolverKernel(const AudioBufferData& ir, bool normalize) {
	const Fft& fft = convolverFft();
	// ponytail: 4-channel "true stereo" IRs use their first two channels (superdough builds mono/stereo IRs).
	irChannels = std::min((int)ir.data.size(), 2);
	int64_t len = ir.length();
	partitions = std::max<int>(1, (int)((len + kQuantum - 1) / kQuantum));
	scale = 1;
	if (normalize) { // spec calculateNormalizationScale
		double power = 0;
		for (int c = 0; c < irChannels; c++)
			for (float s : ir.data[c])
				power += (double)s * s;
		power = std::sqrt(power / (irChannels * (double)std::max<int64_t>(len, 1)));
		if (!std::isfinite(power) || power < 0.000125)
			power = 0.000125;
		scale = (float)(1 / power * 0.00125 * 44100 / ir.sampleRate);
	}
	hre.assign((size_t)irChannels * partitions * kBins, 0.f);
	him.assign(hre.size(), 0.f);
	xre.assign((size_t)2 * partitions * kBins, 0.f);
	xim.assign(xre.size(), 0.f);
	float re[2 * kQuantum], im[2 * kQuantum];
	for (int c = 0; c < irChannels; c++)
		for (int p = 0; p < partitions; p++) {
			std::fill(re, re + 2 * kQuantum, 0.f);
			std::fill(im, im + 2 * kQuantum, 0.f);
			for (int i = 0; i < kQuantum && p * kQuantum + i < len; i++)
				re[i] = ir.data[c][p * kQuantum + i];
			fft.run(re, im, false);
			size_t o = ((size_t)c * partitions + p) * kBins;
			std::copy(re, re + kBins, &hre[o]);
			std::copy(im, im + kBins, &him[o]);
		}
}

void ConvolverKernel::reset() {
	std::fill(xre.begin(), xre.end(), 0.f);
	std::fill(xim.begin(), xim.end(), 0.f);
	std::memset(history, 0, sizeof history);
}

void ConvolverKernel::process(const float* const in[2], int outChannels, Bus& out) {
	const Fft& fft = convolverFft();
	const int n = 2 * kQuantum;
	float re[n], im[n];
	for (int c = 0; c < 2; c++) {
		std::copy(history[c], history[c] + kQuantum, re);
		std::copy(in[c], in[c] + kQuantum, re + kQuantum);
		std::copy(in[c], in[c] + kQuantum, history[c]);
		std::fill(im, im + n, 0.f);
		fft.run(re, im, false);
		size_t o = ((size_t)c * partitions + pos) * kBins;
		std::copy(re, re + kBins, &xre[o]);
		std::copy(im, im + kBins, &xim[o]);
	}
	for (int c = 0; c < outChannels; c++) {
		float yr[kBins] = {}, yi[kBins] = {};
		const float* xr = &xre[(size_t)c * partitions * kBins];
		const float* xi = &xim[(size_t)c * partitions * kBins];
		const float* hr = &hre[(size_t)std::min(c, irChannels - 1) * partitions * kBins];
		const float* hi = &him[(size_t)std::min(c, irChannels - 1) * partitions * kBins];
		for (int p = 0; p < partitions; p++) { // input block (pos - p) against IR partition p
			int q = pos - p < 0 ? pos - p + partitions : pos - p;
			const float *ar = xr + q * kBins, *ai = xi + q * kBins, *br = hr + p * kBins, *bi = hi + p * kBins;
			for (int k = 0; k < kBins; k++) {
				yr[k] += ar[k] * br[k] - ai[k] * bi[k];
				yi[k] += ar[k] * bi[k] + ai[k] * br[k];
			}
		}
		for (int k = 0; k < kBins; k++)
			re[k] = yr[k], im[k] = yi[k];
		for (int k = kBins; k < n; k++) // Hermitian: real output
			re[k] = yr[n - k], im[k] = -yi[n - k];
		fft.run(re, im, true);
		for (int i = 0; i < kQuantum; i++)
			out.data[c][i] = re[kQuantum + i] * scale / n;
	}
	pos = (pos + 1) % partitions;
}

void ConvolverNode::process(int64_t f0) {
	lastF0 = f0;
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	if (!in.silent)
		lastInput = f0;
	if (tailDone(f0)) {
		if (!idle && kernel)
			kernel->reset(); // stale spectra must not ring when input resumes
		idle = true;
		return out.silence();
	}
	idle = false;
	const float* x[2] = {in.data[0], in.data[in.channels == 2 ? 1 : 0]};
	int ch = in.channels == 1 && kernel->irChannels == 1 ? 1 : 2; // spec: mono only for mono in + mono IR
	out.begin(ch);
	kernel->process(x, ch, out);
}

void ConvolverNode::setAttr(int attr, double v, Resource* r) {
	if (attr == AttrBuffer) {
		graph->drop(kernel);
		kernel = static_cast<ConvolverKernel*>(r);
		idle = true;
	} else {
		Node::setAttr(attr, v, r);
	}
}

bool ConvolverNode::reclaimable() const {
	return !inputs[0].edges && (!outEdges || tailDone(lastF0));
}

// --- Merger / Splitter ---

ChannelMergerNode::ChannelMergerNode(double sr, int numberOfInputs)
	: Node(sr, std::min(std::max(numberOfInputs, 1), kMaxChannels), 1) {
	channelCount = 1;
	countMode = CountMode::Explicit;
}

void ChannelMergerNode::process(int64_t) {
	Bus& out = outputs[0];
	bool any = false;
	for (const Input& in : inputs)
		any |= !in.bus.silent;
	if (!any)
		return out.silence();
	out.begin((int)inputs.size());
	for (size_t i = 0; i < inputs.size(); i++)
		std::memcpy(out.data[i], inputs[i].bus.data[0], sizeof out.data[i]);
}

ChannelSplitterNode::ChannelSplitterNode(double sr, int numberOfOutputs)
	: Node(sr, 1, std::min(std::max(numberOfOutputs, 1), kMaxChannels)) {
	channelCount = (int)outputs.size();
	countMode = CountMode::Explicit;
	interpretation = Interpretation::Discrete;
}

void ChannelSplitterNode::process(int64_t) {
	const Bus& in = inputs[0].bus;
	for (size_t i = 0; i < outputs.size(); i++) {
		if (in.silent) {
			outputs[i].silence();
			continue;
		}
		outputs[i].begin(1);
		std::memcpy(outputs[i].data[0], in.data[i], sizeof outputs[i].data[0]);
	}
}

// --- DynamicsCompressor (spec structure; envelope and detector curves are UA-defined) ---

DynamicsCompressorNode::DynamicsCompressorNode(double sr) : Node(sr, 1, 1) {
	countMode = CountMode::ClampedMax;
	for (auto& p : pre)
		p.assign(std::max(1, (int)std::lround(0.006 * sr)), 0.f);
}

double DynamicsCompressorNode::curveDb(double x, double t, double k, double r) {
	if (x <= t)
		return x;
	if (x < t + k) // quadratic knee from the threshold up, slope 1 -> 1/ratio
		return x + (1 / r - 1) * (x - t) * (x - t) / (2 * k);
	return t + k / 2 + k / (2 * r) + (x - t - k) / r;
}

void DynamicsCompressorNode::process(int64_t f0) {
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	int n = (int)pre[0].size();
	if (!in.silent) {
		lastInput = f0;
		lastChannels = in.channels;
	} else if (f0 > lastInput + n + kQuantum) {
		return out.silence();
	}
	int ch = in.silent ? lastChannels : in.channels;
	double t = threshold.values[0], k = knee.values[0], r = ratio.values[0];
	auto gainFor = [&](double x) { // shaped / input, linear
		double db = 20 * std::log10(x);
		return std::pow(10, (curveDb(db, t, k, r) - db) / 20);
	};
	double makeup = std::pow(1 / gainFor(1), 0.6); // spec "computing the makeup gain"
	double atkSamples = attack.values[0] * sampleRate, relSamples = release.values[0] * sampleRate;
	double attackRate = atkSamples < 1 ? 1 : 1 - std::exp(-1 / atkSamples);
	double releaseExp = 1 / std::max(relSamples, 1.0);
	out.begin(ch);
	for (int i = 0; i < kQuantum; i++) {
		double x = 0;
		for (int c = 0; c < ch; c++)
			x = std::max(x, (double)std::fabs(in.data[c][i]));
		double attenuation = x < 0.0001 ? 1 : gainFor(x);
		bool releasing = attenuation > compressorGain;
		detectorAverage = std::min(attenuation, 1.0); // detector curve: rate 1
		if (releasing)                                // envelope rate > 1, from the gain / average ratio
			compressorGain = std::min(1.0, compressorGain * std::pow(detectorAverage / compressorGain, releaseExp));
		else
			compressorGain += (detectorAverage - compressorGain) * attackRate;
		float g = (float)(compressorGain * makeup);
		for (int c = 0; c < ch; c++) {
			float delayed = pre[c][prePos];
			pre[c][prePos] = in.data[c][i];
			out.data[c][i] = delayed * g;
		}
		prePos = (prePos + 1) % n;
	}
}

// --- Analyser ---

void AnalyserNode::process(int64_t) {
	const Bus& in = inputs[0].bus;
	Bus& out = outputs[0];
	if (in.silent)
		return out.silence();
	out.begin(in.channels);
	std::memcpy(out.data, in.data, sizeof out.data);
}

void AnalyserNode::setAttr(int attr, double v, Resource* r) {
	int n = (int)v;
	if (attr == AttrFftSize && n >= 32 && n <= 32768 && (n & (n - 1)) == 0)
		fftSize = n;
	else if (attr != AttrFftSize)
		Node::setAttr(attr, v, r);
}

} // namespace audio
