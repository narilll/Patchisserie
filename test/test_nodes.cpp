#include <complex>
#include <random>

#include "audio_test.hpp"
#include "check.hpp"

using namespace audiotest;

namespace {

// Standalone AudioParam, computed quantum by quantum like the graph does.
struct ParamRig {
	GainNode owner{kSr};
	AudioParam p;
	int64_t f0 = 0;
	uint64_t q = 0;
	ParamRig(bool aRate, float def = 0, float lo = -FLT_MAX, float hi = FLT_MAX) : p(&owner, def, lo, hi, aRate) {}
	void event(AudioParam::Kind k, double v, double t) { CHECK(p.insert(k, v, t, f0, kSr)); }
	std::vector<float> run(int quanta) {
		std::vector<float> out;
		for (int i = 0; i < quanta; i++, f0 += kQuantum) {
			p.compute(f0, kSr, ++q);
			for (int k = 0; k < kQuantum; k++)
				out.push_back(p.at(k));
		}
		return out;
	}
};

std::complex<double> dft(const std::vector<float>& x, double freq) {
	std::complex<double> s = 0, z = 1, step = std::polar(1.0, -2 * M_PI * freq / kSr);
	for (float v : x) {
		s += (double)v * z;
		z *= step;
	}
	return s;
}

float peakOf(const std::vector<float>& x, size_t from = 0) {
	float p = 0;
	for (size_t i = from; i < x.size(); i++)
		p = std::max(p, std::fabs(x[i]));
	return p;
}

} // namespace

// --- AudioParam timeline (spec "computation of value" and the automation formulas) ---

TEST(param_set_linear_exp_match_spec_formulas) {
	ParamRig r(true, 2);
	double T0 = 100.5 / kSr, T1 = 300 / kSr, T2 = 500 / kSr;
	r.event(AudioParam::Set, 1, T0); // between frames: takes effect at the first frame >= T0, i.e. 101
	r.event(AudioParam::Linear, 3, T1);
	r.event(AudioParam::Exp, 0.1, T2);
	std::vector<float> v = r.run(6);
	for (int k = 0; k < 700; k++) {
		double t = k / kSr, want;
		if (k <= 100)
			want = 2; // default until the first event
		else if (k < 300)
			want = 1 + (3 - 1) * (t - T0) / (T1 - T0); // ramp starts at the previous event's time/value
		else if (k < 500)
			want = 3 * std::pow(0.1 / 3, (t - T1) / (T2 - T1));
		else
			want = 0.1;
		CHECK_NEAR(v[k], want, 1e-6 * std::max(1.0, std::fabs(want)));
	}
}

TEST(param_krate_samples_quantum_start) {
	ParamRig r(false, 0);
	r.event(AudioParam::Set, 0, 0);
	r.event(AudioParam::Linear, 256, 256 / kSr);
	std::vector<float> v = r.run(3);
	for (int k = 0; k < 384; k++)
		CHECK_NEAR(v[k], std::min(k / 128 * 128, 256), 1e-4);
}

TEST(param_cancel_removes_ramp_and_holds) {
	ParamRig r(true, 0);
	r.event(AudioParam::Set, 1, 0);
	r.event(AudioParam::Linear, 5, 1000 / kSr);
	r.run(2);
	r.p.cancel(256 / kSr); // removes the ramp: back to the remaining set event's value
	std::vector<float> v = r.run(1);
	CHECK_NEAR(v[0], 1, 1e-6);
	CHECK_NEAR(v[127], 1, 1e-6);
	r.p.cancel(0); // no events left: [[current value]] holds
	v = r.run(1);
	CHECK_NEAR(v[0], 1, 1e-6);
}

TEST(param_ramp_without_previous_event_starts_at_call_time) {
	ParamRig r(true, 1);
	r.run(1); // now = frame 128
	r.event(AudioParam::Linear, 3, 384 / kSr);
	std::vector<float> v = r.run(3);
	for (int k = 0; k < 384; k++) {
		int f = 128 + k;
		CHECK_NEAR(v[k], f < 384 ? 1 + 2.0 * (f - 128) / 256 : 3, 1e-5);
	}
}

TEST(param_exp_ramp_from_zero_holds_and_clamps) {
	ParamRig r(true, 0, -1, 1);
	r.event(AudioParam::Set, 0, 0);
	r.event(AudioParam::Exp, 2, 100 / kSr); // V0 = 0: hold V0 until T1 (spec), then V1 clamped to max
	std::vector<float> v = r.run(1);
	CHECK(v[50] == 0);
	CHECK(v[100] == 1);
}

TEST(param_input_is_summed_into_value) {
	Rig r;
	auto* g = r.make<GainNode>();
	r.g->connect(r.constant(1), g);
	r.g->connect(g, r.dest());
	r.g->connect(r.constant(0.25f), &g->gain); // gain = 1 (intrinsic) + 0.25 (input)
	auto* lfo = r.make<OscillatorNode>();
	lfo->frequency.value = 375;
	r.g->start(lfo);
	auto* g2 = r.make<GainNode>();
	g2->gain.value = 0;
	r.g->connect(r.constant(1), g2);
	r.g->connect(g2, r.dest());
	r.g->connect(lfo, &g2->gain); // a-rate input: the sine itself comes out
	r.render(256);
	for (int k = 0; k < 256; k++)
		CHECK_NEAR(r.L[k], 1.25 + std::sin(2 * M_PI * 375 * k / kSr), 2e-4);
}

// --- Gain / ConstantSource ---

TEST(gain_scales_and_constant_source_is_frame_exact) {
	Rig r;
	auto* c = r.make<ConstantSourceNode>();
	c->offset.value = 2;
	r.g->start(c, r.at(100));
	r.g->stop(c, r.at(200));
	auto* g = r.make<GainNode>();
	g->gain.value = 0.5f;
	r.g->connect(c, g);
	r.g->connect(g, r.dest());
	r.render(512);
	for (int k = 0; k < 512; k++)
		CHECK(r.L[k] == (k >= 100 && k < 200 ? 1.f : 0.f));
	Event e;
	CHECK(r.g->pollEvent(e) && e.type == Event::Ended);
}

// --- BiquadFilter: measured impulse response vs the cookbook transfer function ---

namespace {
// Independent oracle: Audio EQ Cookbook as written in the spec (Q in dB for lowpass/highpass).
std::complex<double> cookbook(int type, double f0, double Q, double G, double f) {
	double A = std::pow(10, G / 40), w0 = 2 * M_PI * f0 / kSr, c = std::cos(w0), s = std::sin(w0);
	double aQ = s / (2 * Q), aQdB = s / (2 * std::pow(10, Q / 20)), aS = s / 2 * std::sqrt(2.0), rA = std::sqrt(A);
	double b[3], a[3];
	auto set = [&](double b0, double b1, double b2, double a0, double a1, double a2) {
		b[0] = b0, b[1] = b1, b[2] = b2, a[0] = a0, a[1] = a1, a[2] = a2;
	};
	switch (type) {
	case BiquadFilterNode::Lowpass:
		set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + aQdB, -2 * c, 1 - aQdB);
		break;
	case BiquadFilterNode::Highpass:
		set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + aQdB, -2 * c, 1 - aQdB);
		break;
	case BiquadFilterNode::Bandpass:
		set(aQ, 0, -aQ, 1 + aQ, -2 * c, 1 - aQ);
		break;
	case BiquadFilterNode::Notch:
		set(1, -2 * c, 1, 1 + aQ, -2 * c, 1 - aQ);
		break;
	case BiquadFilterNode::Allpass:
		set(1 - aQ, -2 * c, 1 + aQ, 1 + aQ, -2 * c, 1 - aQ);
		break;
	case BiquadFilterNode::Peaking:
		set(1 + aQ * A, -2 * c, 1 - aQ * A, 1 + aQ / A, -2 * c, 1 - aQ / A);
		break;
	case BiquadFilterNode::Lowshelf:
		set(A * ((A + 1) - (A - 1) * c + 2 * aS * rA), 2 * A * ((A - 1) - (A + 1) * c),
			A * ((A + 1) - (A - 1) * c - 2 * aS * rA), (A + 1) + (A - 1) * c + 2 * aS * rA,
			-2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - 2 * aS * rA);
		break;
	default:
		set(A * ((A + 1) + (A - 1) * c + 2 * aS * rA), -2 * A * ((A - 1) + (A + 1) * c),
			A * ((A + 1) + (A - 1) * c - 2 * aS * rA), (A + 1) - (A - 1) * c + 2 * aS * rA, 2 * ((A - 1) - (A + 1) * c),
			(A + 1) - (A - 1) * c - 2 * aS * rA);
	}
	std::complex<double> z1 = std::polar(1.0, -2 * M_PI * f / kSr), z2 = z1 * z1;
	return (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2);
}
} // namespace

TEST(biquad_magnitude_matches_cookbook_for_all_types) {
	const double f0 = 1000, Q = 3, G = 6;
	for (int type = 0; type < 8; type++) {
		Rig r;
		auto* bq = r.make<BiquadFilterNode>();
		bq->frequency.value = f0;
		bq->Q.value = Q;
		bq->gain.value = G;
		r.g->setAttr(bq, AttrType, type);
		r.g->connect(r.play(click()), bq);
		r.g->connect(bq, r.dest());
		r.render(16384);
		for (double f : {100.0, 500.0, 1000.0, 2000.0, 8000.0, 20000.0}) {
			double want = std::abs(cookbook(type, f0, Q, G, f)), got = std::abs(dft(r.L, f));
			CHECK_NEAR(got, want, 1e-3 * std::max(1.0, want));
		}
	}
	// Anchors: peaking gain at f0 is A^2, notch nulls f0, allpass is flat.
	CHECK_NEAR(std::abs(cookbook(BiquadFilterNode::Peaking, f0, Q, G, f0)), std::pow(10, G / 20), 1e-9);
	CHECK_NEAR(std::abs(cookbook(BiquadFilterNode::Notch, f0, Q, G, f0)), 0, 1e-9);
	CHECK_NEAR(std::abs(cookbook(BiquadFilterNode::Allpass, f0, Q, G, 3456)), 1, 1e-9);
}

TEST(biquad_detune_and_degenerate_frequencies) {
	double c[5];
	BiquadFilterNode::coefficients(BiquadFilterNode::Lowpass, 24000, 1, 0, kSr, c); // at Nyquist: pass-through
	CHECK(c[0] == 1 && c[1] == 0 && c[3] == 0);
	BiquadFilterNode::coefficients(BiquadFilterNode::Highpass, 0, 1, 0, kSr, c);
	CHECK(c[0] == 1 && c[4] == 0);
	// detune +1200 cents doubles the frequency: 500 Hz detuned == 1000 Hz.
	Rig r;
	auto* bq = r.make<BiquadFilterNode>();
	bq->frequency.value = 500;
	bq->detune.value = 1200;
	bq->Q.value = 3;
	r.g->connect(r.play(click()), bq);
	r.g->connect(bq, r.dest());
	r.render(16384);
	CHECK_NEAR(std::abs(dft(r.L, 1000)), std::abs(cookbook(BiquadFilterNode::Lowpass, 1000, 3, 0, 1000)), 1e-3);
}

// --- Oscillator ---

TEST(oscillator_sine_has_exact_frequency_and_phase) {
	Rig r;
	auto* o = r.make<OscillatorNode>();
	o->frequency.value = 1000;
	r.g->start(o);
	r.g->connect(o, r.dest());
	r.render(4800);
	for (int k = 0; k < 4800; k++)
		CHECK_NEAR(r.L[k], std::sin(2 * M_PI * 1000 * k / kSr), 1e-4);
}

TEST(oscillator_saw_is_band_limited) {
	// 5250 Hz: harmonics 1..4 below Nyquist; a naive saw folds the rest back onto non-harmonic bins.
	Rig r;
	auto* o = r.make<OscillatorNode>();
	o->frequency.value = 5250;
	r.g->setAttr(o, AttrType, OscillatorNode::Sawtooth);
	r.g->start(o);
	r.g->connect(o, r.dest());
	r.render(6400); // exactly 700 periods: harmonic k sits on bin 700 k
	double harmonic = 0, alias = 0;
	for (int bin = 1; bin < 3200; bin++) {
		double e = std::norm(dft(r.L, bin * kSr / 6400));
		(bin % 700 == 0 ? harmonic : alias) += e;
	}
	CHECK(alias < 1e-6 * harmonic);
	CHECK(peakOf(r.L) <= 1.01f);
}

TEST(periodic_wave_normalization) {
	auto peakFor = [](std::vector<float> imag, bool normalize) {
		Rig r;
		auto* o = r.make<OscillatorNode>();
		o->frequency.value = 100;
		r.g->setAttr(o, AttrPeriodicWave, 0, new WaveTable(nullptr, imag.data(), (int)imag.size(), normalize));
		r.g->start(o);
		r.g->connect(o, r.dest());
		r.render(4800);
		return peakOf(r.L);
	};
	CHECK_NEAR(peakFor({0, 3}, true), 1, 1e-3);
	CHECK_NEAR(peakFor({0, 3}, false), 3, 3e-3);
	CHECK_NEAR(peakFor({0, 1, 1}, true), 1, 1e-3); // sin x + sin 2x peaks at 1.76 before normalization
	CHECK_NEAR(peakFor({0, 1, 1}, false), 1.7602, 2e-3);
}

// --- AudioBufferSource ---

TEST(buffer_source_starts_on_exact_frame_inside_quantum) {
	Rig r;
	AudioBufferData* b = click();
	r.g->connect(r.play(b, r.at(1000)), r.dest());
	r.g->connect(r.play(Resource::retain(b), r.at(1501)), r.dest()); // one buffer, two sources: one ref each
	r.render(2048);
	for (int k = 0; k < 2048; k++)
		CHECK(r.L[k] == (k == 1000 || k == 1501 ? 1.f : 0.f));
}

TEST(buffer_source_offset_loop_rate_and_onended_once) {
	std::vector<float> ramp(100);
	for (int i = 0; i < 100; i++)
		ramp[i] = (float)i;
	{ // offset
		Rig r;
		r.g->connect(r.play(buffer(ramp), 0, 30 / kSr), r.dest());
		r.render(128);
		CHECK(r.L[0] == 30 && r.L[69] == 99 && r.L[70] == 0);
	}
	{ // loop points [10, 20)
		Rig r;
		auto* s = r.play(buffer(ramp));
		r.g->setAttr(s, AttrLoop, 1);
		r.g->setAttr(s, AttrLoopStart, 10 / kSr);
		r.g->setAttr(s, AttrLoopEnd, 20 / kSr);
		r.g->connect(s, r.dest());
		r.render(256);
		for (int k = 0; k < 256; k++)
			CHECK(r.L[k] == (k < 20 ? k : 10 + (k - 20) % 10));
	}
	{ // playbackRate 0.5 interpolates linearly, 2 plays half the length and ends once
		Rig r;
		auto* slow = r.play(buffer(ramp));
		slow->playbackRate.value = 0.5f;
		r.g->connect(slow, r.dest());
		r.render(4);
		CHECK(r.L[0] == 0 && r.L[1] == 0.5f && r.L[2] == 1 && r.L[3] == 1.5f);
	}
	Rig r;
	auto* fast = r.play(buffer(std::vector<float>(1000, 1.f)));
	fast->playbackRate.value = 2;
	r.g->connect(fast, r.dest());
	r.render(2048);
	for (int k = 0; k < 2048; k++)
		CHECK(r.L[k] == (k < 500 ? 1.f : 0.f));
	int ended = 0;
	Event e;
	while (r.g->pollEvent(e))
		ended += e.type == Event::Ended;
	r.render(1024);
	while (r.g->pollEvent(e))
		ended += e.type == Event::Ended;
	CHECK(ended == 1);
}

TEST(buffer_source_resamples_foreign_sample_rate) {
	// A 24 kHz buffer at rate 1 plays at half speed in a 48 kHz context (soundfont zones do this).
	Rig r;
	r.g->connect(r.play(buffer({0, 2, 4, 6}, kSr / 2)), r.dest());
	r.render(10);
	for (int k = 0; k < 10; k++)
		CHECK(r.L[k] == (k < 7 ? k : k == 7 ? 6 : 0)); // past the last frame: hold, then end
}

// --- Delay ---

// Delays below are exact in float: delayTime is a float AudioParam (spec), so e.g. 300 / 48000 is 300.0000045 frames.
TEST(delay_shifts_by_exact_frames) {
	for (int n : {3, 375}) {
		Rig r;
		auto* d = r.make<DelayNode>();
		d->delayTime.value = (float)(n / kSr);
		r.g->connect(r.play(click()), d);
		r.g->connect(d, r.dest());
		r.render(512);
		for (int k = 0; k < 512; k++)
			CHECK_NEAR(r.L[k], k == n ? 1 : 0, 1e-6);
	}
}

TEST(delay_feedback_loop_adds_no_extra_quantum) {
	Rig r;
	auto* d = r.make<DelayNode>();
	d->delayTime.value = (float)(1500 / kSr);
	auto* fb = r.make<GainNode>();
	fb->gain.value = 0.5f;
	r.g->connect(r.play(click()), d);
	r.g->connect(d, fb);
	r.g->connect(fb, d);
	r.g->connect(d, r.dest());
	r.render(6144);
	for (int k = 0; k < 6144; k++)
		CHECK(r.L[k] == (k == 1500 ? 1 : k == 3000 ? 0.5f : k == 4500 ? 0.25f : k == 6000 ? 0.125f : 0));
}

TEST(delay_in_cycle_is_clamped_to_one_quantum) {
	Rig r;
	auto* d = r.make<DelayNode>();
	d->delayTime.value = (float)(10 / kSr);
	auto* fb = r.make<GainNode>();
	fb->gain.value = 0.5f;
	r.g->connect(r.play(click()), d);
	r.g->connect(d, fb);
	r.g->connect(fb, d);
	r.g->connect(d, r.dest());
	r.render(512);
	CHECK(r.L[10] == 0);
	CHECK_NEAR(r.L[128], 1, 1e-6);
	CHECK_NEAR(r.L[256], 0.5, 1e-6);
}

// --- Convolver ---

TEST(convolver_matches_direct_convolution) {
	std::mt19937 rng(1);
	std::uniform_real_distribution<float> u(-1, 1);
	const int irLen = 1000, inLen = 2000, total = 3200;
	auto* ir = new AudioBufferData(2, irLen, kSr);
	for (auto& ch : ir->data)
		for (float& s : ch)
			s = u(rng);
	std::vector<float> x(inLen);
	for (float& s : x)
		s = u(rng);
	auto run = [&](bool normalize, std::vector<float>& L, std::vector<float>& R) {
		Rig r;
		auto* cv = r.make<ConvolverNode>();
		r.g->setAttr(cv, AttrBuffer, 0, new ConvolverKernel(*ir, normalize));
		r.g->connect(r.play(buffer(x)), cv);
		r.g->connect(cv, r.dest());
		r.render(total);
		L = r.L, R = r.R;
	};
	std::vector<float> L, R;
	run(false, L, R);
	for (int c = 0; c < 2; c++) {
		const std::vector<float>& out = c ? R : L;
		double err = 0;
		for (int n = 0; n < total; n++) {
			double want = 0;
			for (int k = 0; k < irLen && k <= n; k++)
				if (n - k < inLen)
					want += (double)ir->data[c][k] * x[n - k];
			err = std::max(err, std::fabs(out[n] - want));
		}
		CHECK(err < 1e-3);
	}
	// normalize = true: same shape times the spec's calculateNormalizationScale.
	double power = 0;
	for (auto& ch : ir->data)
		for (float s : ch)
			power += (double)s * s;
	double scale = 1 / std::sqrt(power / (2 * irLen)) * 0.00125 * 44100 / kSr;
	std::vector<float> nL, nR;
	run(true, nL, nR);
	CHECK_NEAR(nL[1500], L[1500] * scale, 1e-4);
	delete ir;
}

// --- StereoPanner / WaveShaper / Merger / Splitter / Compressor / Analyser ---

TEST(stereo_panner_equal_power_law) {
	for (float pan : {-1.f, 0.f, 1.f}) {
		Rig r;
		auto* p = r.make<StereoPannerNode>();
		p->pan.value = pan;
		r.g->connect(r.constant(1), p);
		r.g->connect(p, r.dest());
		auto* m = r.make<ChannelMergerNode>(2); // stereo (1, 2) through a second panner
		r.g->connect(r.constant(1), m, 0, 0);
		r.g->connect(r.constant(2), m, 0, 1);
		auto* ps = r.make<StereoPannerNode>();
		ps->pan.value = pan;
		r.g->connect(m, ps);
		auto* sink = r.make<GainNode>();
		r.g->connect(ps, sink);
		r.render(128);
		double x = (pan + 1) / 2;
		CHECK_NEAR(r.L[5], std::cos(x * M_PI / 2), 1e-6);
		CHECK_NEAR(r.R[5], std::sin(x * M_PI / 2), 1e-6);
		r.g->disconnect(p);
		r.g->connect(sink, r.dest());
		r.render(128);
		// stereo law: pan -1 folds R into L, 0 passes, 1 folds L into R
		CHECK_NEAR(r.L[5], pan < 0 ? 3 : pan == 0 ? 1 : 0, 1e-6);
		CHECK_NEAR(r.R[5], pan < 0 ? 0 : pan == 0 ? 2 : 3, 1e-6);
	}
}

TEST(waveshaper_curve_interpolation_and_oversampling) {
	std::vector<float> c = {0, 1, 4};
	CHECK(WaveShaperNode::shape(c, -2) == 0);
	CHECK(WaveShaperNode::shape(c, 0) == 1);
	CHECK(WaveShaperNode::shape(c, 0.5f) == 2.5f);
	CHECK(WaveShaperNode::shape(c, 3) == 4);
	CHECK(WaveShaperNode::shape({0, 2}, 0) == 1); // even length: between the two center values
	for (int over : {1, 2, 4}) {
		Rig r;
		auto* o = r.make<OscillatorNode>();
		o->frequency.value = 1000;
		r.g->start(o);
		auto* ws = r.make<WaveShaperNode>();
		auto* identity = new CurveData;
		identity->data = {-1, 1};
		r.g->setAttr(ws, AttrCurve, 0, identity);
		r.g->setAttr(ws, AttrOversample, over);
		r.g->connect(o, ws);
		r.g->connect(ws, r.dest());
		r.render(4800);
		CHECK_NEAR(peakOf(r.L, 480), 1, 0.01); // the up/down filters pass 1 kHz at unity
	}
}

TEST(merger_and_splitter_channel_indices) {
	Rig r;
	auto* m = r.make<ChannelMergerNode>(2);
	r.g->connect(r.constant(1), m, 0, 0);
	r.g->connect(r.constant(2), m, 0, 1);
	auto* s = r.make<ChannelSplitterNode>(2);
	r.g->connect(m, s);
	r.g->connect(m, r.dest());
	r.render(128);
	CHECK(r.L[0] == 1 && r.R[0] == 2);
	r.g->disconnect(m, r.dest());
	r.g->connect(s, r.dest(), 1); // output 1 = channel 1, mono -> both speakers
	r.render(128);
	CHECK(r.L[0] == 2 && r.R[0] == 2);
}

TEST(compressor_reduces_loud_signal_and_keeps_quiet_one) {
	CHECK_NEAR(DynamicsCompressorNode::curveDb(-40, -24, 30, 12), -40, 1e-12);
	CHECK_NEAR(DynamicsCompressorNode::curveDb(6, -24, 30, 12), -24 + 15 + 15.0 / 12, 1e-9); // knee/line meet
	auto level = [](float amp) {
		Rig r;
		auto* o = r.make<OscillatorNode>();
		r.g->start(o);
		auto* g = r.make<GainNode>();
		g->gain.value = amp;
		auto* comp = r.make<DynamicsCompressorNode>();
		r.g->connect(o, g);
		r.g->connect(g, comp);
		r.g->connect(comp, r.dest());
		r.render(48000);
		return peakOf(r.L, 24000) / amp;
	};
	float quiet = level(0.001f), loud = level(4);
	CHECK(quiet > 1.5f && quiet < 2.5f); // below threshold: only the automatic makeup gain
	CHECK(loud < quiet / 3);             // well above threshold: compressed
}

TEST(analyser_passes_through_with_fft_bookkeeping) {
	Rig r;
	auto* a = r.make<AnalyserNode>();
	r.g->setAttr(a, AttrFftSize, 1024);
	r.g->setAttr(a, AttrFftSize, 1000); // not a power of two: ignored (the binding throws)
	r.g->connect(r.constant(0.5f), a);
	r.g->connect(a, r.dest());
	r.render(128);
	CHECK(r.L[0] == 0.5f);
	CHECK(a->fftSize == 1024 && a->frequencyBinCount() == 512);
}
