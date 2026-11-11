// Rough CPU benchmark (make bench): 64 voices of saw -> biquad -> gain -> panner plus one 2 s stereo convolver reverb.
// Prints microseconds per 128-frame quantum; asserts nothing.
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>

#include "audio/nodes.hpp"

using namespace audio;

static double run(bool sweep, bool reverb) {
	const double sr = 48000;
	std::unique_ptr<Graph> g(new Graph(sr));
	ConvolverNode* rev = nullptr;
	if (reverb) {
		std::mt19937 rng(1);
		std::uniform_real_distribution<float> u(-1, 1);
		AudioBufferData ir(2, (int64_t)(2 * sr), sr);
		for (auto& ch : ir.data)
			for (size_t i = 0; i < ch.size(); i++)
				ch[i] = u(rng) * std::exp(-3.0f * i / ch.size());
		rev = new ConvolverNode(sr);
		g->create(rev);
		g->setAttr(rev, AttrBuffer, 0, new ConvolverKernel(ir, true));
		g->connect(rev, g->destination());
	}
	const double seconds = 4;
	for (int v = 0; v < 64; v++) {
		auto* o = new OscillatorNode(sr);
		o->frequency.value = 55.f * (1 + v % 24);
		auto* f = new BiquadFilterNode(sr);
		auto* gain = new GainNode(sr);
		gain->gain.value = 0.01f;
		auto* p = new StereoPannerNode(sr);
		p->pan.value = (v % 9) / 4.f - 1;
		for (Node* n : std::initializer_list<Node*>{o, f, gain, p})
			g->create(n);
		g->setAttr(o, AttrType, OscillatorNode::Sawtooth);
		if (sweep) { // filter envelope running the whole time: per-frame coefficients
			g->setValueAtTime(&f->frequency, 200, 0);
			g->exponentialRampToValueAtTime(&f->frequency, 8000, seconds);
		} else {
			f->frequency.value = 2000;
		}
		g->connect(o, f);
		g->connect(f, gain);
		g->connect(gain, p);
		g->connect(p, g->destination());
		if (rev)
			g->connect(p, rev);
		g->start(o, 0);
	}
	const int quanta = (int)(seconds * sr / kQuantum);
	float l[kQuantum], r[kQuantum];
	g->render(l, r, kQuantum); // apply commands outside the timing
	auto t0 = std::chrono::steady_clock::now();
	for (int i = 1; i < quanta; i++)
		g->render(l, r, kQuantum);
	double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
	return us / (quanta - 1);
}

int main() {
	const double budget = 1e6 * kQuantum / 48000;
	std::printf("budget per quantum at 48 kHz: %.1f us\n", budget);
	for (bool sweep : {false, true})
		for (bool reverb : {false, true}) {
			double us = run(sweep, reverb);
			std::printf("64 voices%s%s: %.1f us/quantum (%.1f%% of one core)\n", sweep ? " + filter sweep" : "",
						reverb ? " + 2 s reverb" : "", us, 100 * us / budget);
		}
	return 0;
}
