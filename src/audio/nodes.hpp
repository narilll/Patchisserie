#pragma once
// Web Audio nodes superdough uses. Construct on the producer thread, hand over with Graph::create().
// Constructor options (initial param values, channel config) may be set directly before create(); afterwards
// only through commands.
#include <cfloat>

#include "audio/graph.hpp"

namespace audio {

/** Decoded audio. Immutable once handed to the graph. */
struct AudioBufferData : Resource {
	AudioBufferData(int channels, int64_t length, double sampleRate)
		: data(channels, std::vector<float>(length)), sampleRate(sampleRate) {}
	std::vector<std::vector<float>> data;
	double sampleRate;
	int64_t length() const { return data.empty() ? 0 : (int64_t)data[0].size(); }
	double duration() const { return (double)length() / sampleRate; }
};

struct CurveData : Resource {
	std::vector<float> data;
};

/** Band-limited tables for one PeriodicWave (spec normalization), built on the producer thread. Ranges are a third
of an octave apart; playback crossfades between the two tables that cannot alias at the current pitch. */
struct WaveTable : Resource {
	static constexpr int kSize = 2048;
	static constexpr int kRangesPerOctave = 3;
	static constexpr int kRanges = 30;
	/** real/imag may be null (zeros); index 0 (DC) is ignored. */
	WaveTable(const float* real, const float* imag, int length, bool normalize);
	/** Built-in types (OscillatorNode::Type), shared and never freed. */
	static const WaveTable* builtin(int type);
	void choose(double maxAbsFrequency, double sampleRate, const float*& rich, const float*& poor, float& mix) const;
	std::vector<float> data; // distinct tables of kSize + 1 (wrap guard)
	size_t offset[kRanges];
};

/** Prepared impulse response (spec normalization + FFT partitions) plus the convolver state that goes with it.
One kernel per ConvolverNode buffer assignment. */
struct ConvolverKernel : Resource {
	static constexpr int kBins = kQuantum + 1; // FFT size 2 * kQuantum, real input
	ConvolverKernel(const AudioBufferData& ir, bool normalize);
	/** in: 2 channels (mono up-mixed); writes outChannels of out. */
	void process(const float* const in[2], int outChannels, Bus& out);
	int irChannels, partitions;
	float scale;
	std::vector<float> hre, him; // [irChannel][partition][bin]
	std::vector<float> xre, xim; // input spectra delay line [channel][partition][bin]
	float history[2][kQuantum] = {};
	int pos = 0;
	void reset();
};

struct DestinationNode : Node {
	explicit DestinationNode(double sr) : Node(sr, 1, 0) { countMode = CountMode::Explicit; }
	void process(int64_t) override {}
};

struct GainNode : Node {
	explicit GainNode(double sr) : Node(sr, 1, 1) {}
	AudioParam gain{this, 1, -FLT_MAX, FLT_MAX, true};
	void process(int64_t f0) override;
};

struct BiquadFilterNode : Node {
	enum Type { Lowpass, Highpass, Bandpass, Lowshelf, Highshelf, Peaking, Notch, Allpass };
	explicit BiquadFilterNode(double sr) : Node(sr, 1, 1) {}
	// frequency + detune form a compound parameter: clamped to [0, Nyquist] after combining.
	AudioParam frequency{this, 350, -FLT_MAX, FLT_MAX, true};
	AudioParam detune{this, 0, -FLT_MAX, FLT_MAX, true};
	AudioParam Q{this, 1, -FLT_MAX, FLT_MAX, true};
	AudioParam gain{this, 0, -FLT_MAX, 1541, true};
	Type type = Lowpass;
	/** Spec coefficients, normalized by a0: {b0, b1, b2, a1, a2}. */
	static void coefficients(Type type, double frequency, double q, double gainDb, double sampleRate, double c[5]);
	void process(int64_t f0) override;
	void setAttr(int attr, double v, Resource* r) override;

  private:
	double s[kMaxChannels][4] = {}; // x1 x2 y1 y2
	int lastChannels = 1;
};

/** AudioScheduledSourceNode: start/stop, onended once. */
struct ScheduledSource : Node {
	using Node::Node;
	void start(double when, double offset, double duration) override;
	void stop(double when) override;
	bool reclaimable() const override { return !started || ended; }
	int64_t startFrame = INT64_MAX, stopFrame = INT64_MAX;
	bool started = false, ended = false;

  protected:
	/** Frames [begin, end) of this quantum where the source plays. */
	void range(int64_t f0, int& begin, int& end) const;
	/** Fires onended once, in the quantum where playback stops. */
	void finish(int64_t f0);
};

struct OscillatorNode : ScheduledSource {
	enum Type { Sine, Square, Sawtooth, Triangle, Custom };
	explicit OscillatorNode(double sr) : ScheduledSource(sr, 0, 1), wave(WaveTable::builtin(Sine)) {}
	~OscillatorNode() override { release(custom); }
	// Compound with detune, clamped to [-Nyquist, Nyquist].
	AudioParam frequency{this, 440, -FLT_MAX, FLT_MAX, true};
	AudioParam detune{this, 0, -FLT_MAX, FLT_MAX, true};
	void process(int64_t f0) override;
	/** AttrType: built-in Type; AttrPeriodicWave: WaveTable resource. */
	void setAttr(int attr, double v, Resource* r) override;

  private:
	const WaveTable* wave;
	WaveTable* custom = nullptr;
	double phase = 0;
};

struct ConstantSourceNode : ScheduledSource {
	explicit ConstantSourceNode(double sr) : ScheduledSource(sr, 0, 1) {}
	AudioParam offset{this, 1, -FLT_MAX, FLT_MAX, true};
	void process(int64_t f0) override;
};

struct AudioBufferSourceNode : ScheduledSource {
	explicit AudioBufferSourceNode(double sr) : ScheduledSource(sr, 0, 1) {}
	~AudioBufferSourceNode() override { release(buffer); }
	AudioParam playbackRate{this, 1, -FLT_MAX, FLT_MAX, false};
	AudioParam detune{this, 0, -FLT_MAX, FLT_MAX, false};
	void start(double when, double offset, double duration) override;
	void process(int64_t f0) override;
	/** AttrBuffer (AudioBufferData), AttrLoop, AttrLoopStart, AttrLoopEnd (seconds). */
	void setAttr(int attr, double v, Resource* r) override;

  private:
	AudioBufferData* buffer = nullptr;
	bool loop = false, playing = false, enteredLoop = false;
	double loopStart = 0, loopEnd = 0, offset = 0, duration = INFINITY; // seconds
	double pos = 0, startPos = 0, elapsed = 0;                          // buffer frames
};

struct StereoPannerNode : Node {
	explicit StereoPannerNode(double sr) : Node(sr, 1, 1) { countMode = CountMode::ClampedMax; }
	AudioParam pan{this, 0, -1, 1, true};
	void process(int64_t f0) override;
};

struct WaveShaperNode : Node {
	explicit WaveShaperNode(double sr);
	~WaveShaperNode() override { release(curve); }
	void process(int64_t f0) override;
	/** AttrCurve (CurveData), AttrOversample (1, 2, 4). */
	void setAttr(int attr, double v, Resource* r) override;
	/** Spec curve lookup with linear interpolation. */
	static float shape(const std::vector<float>& c, float x);

  private:
	/** 2x up/down sampler stage (FIR lowpass at the original Nyquist). */
	struct Stage {
		static constexpr int kTaps = 32;
		float upHistory[kTaps / 2] = {}, downHistory[kTaps] = {};
		void up(const float* in, int n, float* out);
		void down(const float* in, int n, float* out);
	};
	CurveData* curve = nullptr;
	int oversample = 1;
	Stage stages[kMaxChannels][2];
};

struct DelayNode : Node {
	explicit DelayNode(double sr, double maxDelayTime = 1);
	AudioParam delayTime;
	DelayNode* asDelay() override { return this; }
	void process(int64_t f0) override {
		write(f0);
		read(f0);
	}
	/** Writer and reader halves; the graph calls them separately when the delay breaks a cycle. */
	void write(int64_t f0);
	void read(int64_t f0);
	bool reclaimable() const override;

  private:
	bool tailDone(int64_t f0) const { return f0 > lastInput + maxFrames + kQuantum; }
	double maxFrames;
	int64_t len;
	std::vector<float> buf[kMaxChannels];
	int64_t lastInput = INT64_MIN / 2, lastStereo = INT64_MIN / 2, lastF0 = 0;
};

struct ConvolverNode : Node {
	explicit ConvolverNode(double sr) : Node(sr, 1, 1) { countMode = CountMode::ClampedMax; }
	~ConvolverNode() override { release(kernel); }
	void process(int64_t f0) override;
	/** AttrBuffer: ConvolverKernel (normalization is applied when the producer builds it). */
	void setAttr(int attr, double v, Resource* r) override;
	bool reclaimable() const override;

  private:
	bool tailDone(int64_t f0) const { return !kernel || f0 > lastInput + (int64_t)(kernel->partitions + 1) * kQuantum; }
	ConvolverKernel* kernel = nullptr;
	int64_t lastInput = INT64_MIN / 2, lastF0 = 0;
	bool idle = true;
};

struct ChannelMergerNode : Node {
	ChannelMergerNode(double sr, int numberOfInputs = 2);
	void process(int64_t f0) override;
};

struct ChannelSplitterNode : Node {
	ChannelSplitterNode(double sr, int numberOfOutputs = 2);
	void process(int64_t f0) override;
};

struct DynamicsCompressorNode : Node {
	explicit DynamicsCompressorNode(double sr);
	AudioParam threshold{this, -24, -100, 0, false};
	AudioParam knee{this, 30, 0, 40, false};
	AudioParam ratio{this, 12, 1, 20, false};
	AudioParam attack{this, 0.003f, 0, 1, false};
	AudioParam release{this, 0.25f, 0, 1, false};
	void process(int64_t f0) override;
	/** Static curve in dB (spec: identity, soft knee, 1/ratio slope). */
	static double curveDb(double x, double threshold, double knee, double ratio);

  private:
	std::vector<float> pre[kMaxChannels]; // fixed 6 ms look-ahead
	int prePos = 0, lastChannels = 1;
	double detectorAverage = 0, compressorGain = 1;
	int64_t lastInput = INT64_MIN / 2;
};

/** ponytail: analysis stub. Spec pass-through and fftSize bookkeeping; the JS data getters return zeros. Add an
FFT + smoothing here when a scope needs real data. */
struct AnalyserNode : Node {
	explicit AnalyserNode(double sr) : Node(sr, 1, 1) {}
	int fftSize = 2048;
	int frequencyBinCount() const { return fftSize / 2; }
	void process(int64_t f0) override;
	void setAttr(int attr, double v, Resource* r) override;
};

} // namespace audio
