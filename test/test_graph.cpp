#include <algorithm>
#include <atomic>
#include <thread>

#include "audio_test.hpp"
#include "check.hpp"

using namespace audiotest;

// Single-threaded tests write node fields right after make(): safe until the next render applies the Create.

TEST(disconnect_variants_follow_spec) {
	Rig r;
	auto* src = r.constant(1);
	auto* g1 = r.make<GainNode>();
	auto* g2 = r.make<GainNode>();
	g2->gain.value = 10;
	r.g->connect(src, g1);
	r.g->connect(src, g2);
	r.g->connect(src, g2); // same termini twice: ignored
	r.g->connect(g1, r.dest());
	r.g->connect(g2, r.dest());
	r.render(128);
	CHECK(r.L[0] == 11);
	r.g->disconnect(src, g1); // disconnect(destination)
	r.render(128);
	CHECK(r.L[0] == 10);
	auto* g3 = r.make<GainNode>();
	g3->gain.value = 0;
	r.g->connect(r.constant(1), g3);
	r.g->connect(g3, r.dest());
	r.g->connect(src, &g3->gain);
	r.render(128);
	CHECK(r.L[0] == 11);
	r.g->disconnect(src, &g3->gain); // disconnect(param) leaves the audio edges
	r.render(128);
	CHECK(r.L[0] == 10);
	r.g->disconnect(src); // disconnect(): everything
	r.render(128);
	CHECK(r.L[0] == 0);

	Rig s;
	auto* m = s.make<ChannelMergerNode>(2);
	s.g->connect(s.constant(1), m, 0, 0);
	s.g->connect(s.constant(2), m, 0, 1);
	auto* split = s.make<ChannelSplitterNode>(2);
	s.g->connect(m, split);
	s.g->connect(split, s.dest(), 0, 0);
	s.g->connect(split, s.dest(), 1, 0);
	s.render(128);
	CHECK(s.L[0] == 3);
	s.g->disconnect(split, s.dest(), 1); // disconnect(destination, output)
	s.render(128);
	CHECK(s.L[0] == 1);
	s.g->disconnect(split, s.dest(), 0, 1); // no such input: nothing changes
	s.render(128);
	CHECK(s.L[0] == 1);
	s.g->disconnect(split, s.dest(), 0, 0); // disconnect(destination, output, input)
	s.render(128);
	CHECK(s.L[0] == 0);
}

TEST(channel_count_modes_and_mixing_rules) {
	struct Case {
		bool stereoIn;
		int count;
		CountMode mode;
		Interpretation interp;
		float l, r;
	};
	for (Case c : {
			 Case{true, 1, CountMode::Explicit, Interpretation::Speakers, 2, 2},   // down-mix: (L + R) / 2
			 Case{true, 1, CountMode::Explicit, Interpretation::Discrete, 1, 1},   // discrete: keep L
			 Case{true, 1, CountMode::ClampedMax, Interpretation::Speakers, 2, 2}, // clamped to 1
			 Case{true, 2, CountMode::Max, Interpretation::Speakers, 1, 3},        // passes stereo
			 Case{false, 2, CountMode::Max, Interpretation::Speakers, 1, 1},       // mono stays mono
			 Case{false, 2, CountMode::Explicit, Interpretation::Speakers, 1, 1},  // up-mix: copy
			 Case{false, 2, CountMode::Explicit, Interpretation::Discrete, 1, 0},  // discrete: L only
		 }) {
		Rig r;
		Node* in = r.constant(1);
		if (c.stereoIn) {
			auto* m = r.make<ChannelMergerNode>(2);
			r.g->connect(in, m, 0, 0);
			r.g->connect(r.constant(3), m, 0, 1);
			in = m;
		}
		auto* g = r.make<GainNode>();
		r.g->setAttr(g, AttrChannelCount, c.count);
		r.g->setAttr(g, AttrChannelCountMode, (int)c.mode);
		r.g->setAttr(g, AttrChannelInterpretation, (int)c.interp);
		r.g->connect(in, g);
		r.g->connect(g, r.dest());
		r.render(128);
		CHECK(r.L[7] == c.l && r.R[7] == c.r);
	}
}

TEST(dense_short_notes_are_reclaimed_to_baseline) {
	Rig r;
	const int notes = 10000, perBatch = 1000;
	const double spacing = 0.001, length = 0.005;
	int ended = 0;
	Event e;
	for (int b = 0; b < notes / perBatch; b++) {
		for (int i = 0; i < perBatch; i++) {
			double t = (b * perBatch + i) * spacing + 0.01;
			auto* o = r.make<OscillatorNode>();
			o->frequency.value = 220 + i;
			auto* g = r.make<GainNode>();
			r.g->connect(o, g);
			r.g->connect(g, r.dest());
			r.g->setValueAtTime(&g->gain, 0, t);
			r.g->linearRampToValueAtTime(&g->gain, 1, t + 0.001);
			r.g->linearRampToValueAtTime(&g->gain, 0, t + length);
			r.g->start(o, t);
			r.g->stop(o, t + length);
			r.g->dispose(o); // JS dropped both right away: the playing source keeps the chain alive
			r.g->dispose(g);
		}
		r.render((int)(perBatch * spacing * kSr));
		while (r.g->pollEvent(e))
			ended += e.type == Event::Ended;
		CHECK(r.g->nodeCount() < 40); // only the few overlapping notes are alive
	}
	r.render((int)kSr);
	while (r.g->pollEvent(e))
		ended += e.type == Event::Ended;
	CHECK(ended == notes);
	CHECK(r.g->nodeCount() == 1); // destination only; ASan reports anything collect() missed
}

TEST(tail_keeps_disposed_effects_alive) {
	Rig r;
	auto* d = r.make<DelayNode>();
	d->delayTime.value = (float)(1500 / kSr); // exact in float
	auto* s = r.play(click());
	r.g->connect(s, d);
	r.g->connect(d, r.dest());
	r.g->dispose(s);
	r.g->dispose(d);
	r.render(1024);
	CHECK(r.g->nodeCount() == 2); // the ended click is gone; the delay still holds it
	r.render(2048);
	CHECK(r.L[1500 - 1024] == 1);
	CHECK(r.g->nodeCount() == 2);
	r.render((int)kSr); // no inputs and its tail (maxDelayTime 1 s) is over: reclaimed
	CHECK(r.g->nodeCount() == 1);
}

namespace {
// A deterministic scene touching sources, params, filters, a feedback delay and a convolver.
void scene(Rig& r, int64_t start = 0) {
	auto* rev = r.make<ConvolverNode>();
	auto* ir = new AudioBufferData(2, 3000, kSr);
	for (int c = 0; c < 2; c++)
		for (int i = 0; i < 3000; i++)
			ir->data[c][i] = std::sin(0.37f * i + c) * std::exp(-i / 600.f);
	r.g->setAttr(rev, AttrBuffer, 0, new ConvolverKernel(*ir, true));
	delete ir;
	auto* echo = r.make<DelayNode>();
	echo->delayTime.value = 0.0101f;
	auto* fb = r.make<GainNode>();
	fb->gain.value = 0.6f;
	r.g->connect(echo, fb);
	r.g->connect(fb, echo);
	r.g->connect(echo, r.dest());
	r.g->connect(rev, r.dest());
	for (int i = 0; i < 12; i++) {
		double t = (double)(start + 300 + i * 1777) / kSr;
		auto* o = r.make<OscillatorNode>();
		r.g->setAttr(o, AttrType, i % 4);
		o->frequency.value = 110 * (1 + i % 5);
		auto* f = r.make<BiquadFilterNode>();
		r.g->setValueAtTime(&f->frequency, 200, t);
		r.g->exponentialRampToValueAtTime(&f->frequency, 4000, t + 0.02);
		auto* g = r.make<GainNode>();
		r.g->setValueAtTime(&g->gain, 0, t);
		r.g->linearRampToValueAtTime(&g->gain, 0.3, t + 0.005);
		r.g->linearRampToValueAtTime(&g->gain, 0, t + 0.04);
		auto* p = r.make<StereoPannerNode>();
		p->pan.value = i % 3 - 1.f;
		r.g->connect(o, f);
		r.g->connect(f, g);
		r.g->connect(g, p);
		r.g->connect(p, r.dest());
		r.g->connect(p, rev);
		r.g->connect(p, echo);
		r.g->start(o, t);
		r.g->stop(o, t + 0.04);
		for (Node* n : std::initializer_list<Node*>{o, f, g, p})
			r.g->dispose(n);
	}
}
} // namespace

TEST(chunked_render_equals_offline_render) {
	const int total = 24000;
	Rig offline, live;
	scene(offline);
	scene(live);
	offline.render(total);
	std::vector<float> l, rr;
	uint32_t seed = 7;
	for (int done = 0; done < total;) {
		seed = seed * 1664525 + 1013904223;
		int n = std::min<int>(1 + seed % 300, total - done);
		live.render(n);
		l.insert(l.end(), live.L.begin(), live.L.end());
		rr.insert(rr.end(), live.R.begin(), live.R.end());
		done += n;
	}
	CHECK(l == offline.L && rr == offline.R);
	CHECK(*std::max_element(l.begin(), l.end()) > 0.05f); // the scene is not silent
}

TEST(underflow_discard_keeps_timeline) {
	Rig a, b;
	scene(a);
	scene(b);
	a.render(5000);
	a.g->discard(3000); // the audio thread missed 3000 frames: render and drop them
	a.render(8000);
	b.render(16000);
	CHECK(std::equal(a.L.begin(), a.L.end(), b.L.begin() + 8000));
	CHECK(a.g->currentTime() == b.g->currentTime());
}

TEST(onsets_are_sample_exact_at_ten_hours) {
	const int64_t F = 10LL * 3600 * 48000 + 37; // ~10 h, not quantum-aligned
	auto run = [](int64_t start, bool withScene) {
		Rig r(start);
		r.g->connect(r.play(click(), r.at(1000, start)), r.dest());
		auto* c = r.make<ConstantSourceNode>();
		r.g->start(c, r.at(77, start));
		r.g->stop(c, r.at(300, start));
		auto* g = r.make<GainNode>();
		r.g->setValueAtTime(&g->gain, 0.5, r.at(200, start));
		r.g->connect(c, g);
		r.g->connect(g, r.dest());
		if (withScene)
			scene(r, start);
		r.render(30000);
		return r.L;
	};
	std::vector<float> exact = run(F, false);
	for (int k = 0; k < 2000; k++)
		CHECK(exact[k] == (k >= 77 && k < 300 ? (k < 200 ? 1.f : 0.5f) : 0.f) + (k == 1000 ? 1.f : 0.f));
	std::vector<float> late = run(F, true), early = run(0, true);
	double err = 0;
	for (size_t k = 0; k < early.size(); k++)
		err = std::max(err, (double)std::fabs(late[k] - early[k]));
	CHECK(err < 1e-4); // the whole scene renders the same 10 h later
}

TEST(producer_posts_while_render_thread_renders) {
	Rig r;
	const int notes = 2000;
	std::atomic<bool> done{false};
	std::thread render([&] {
		float buf[2][256];
		while (!done.load())
			r.g->render(buf[0], buf[1], 256);
	});
	int ended = 0;
	Event e;
	for (int i = 0; i < notes; i++) {
		double t = r.g->currentTime() + 0.005;
		auto* o = new OscillatorNode(kSr);
		o->frequency.value = 300;
		auto* g = new GainNode(kSr);
		g->gain.value = 0.1f;
		Command cmds[] = {
			{Command::Create, AudioParam::Set, 0, 0, 0, o},
			{Command::Create, AudioParam::Set, 0, 0, 0, g},
			{Command::Connect, AudioParam::Set, 0, 0, 0, o, g},
			{Command::Connect, AudioParam::Set, 0, 0, 0, g, r.dest()},
			{Command::ParamEvent, AudioParam::Linear, 0, 0, 0, nullptr, nullptr, &g->gain, 0, t + 0.002},
			{Command::Start, AudioParam::Set, 0, 0, 0, o, nullptr, nullptr, t},
			{Command::Stop, AudioParam::Set, 0, 0, 0, o, nullptr, nullptr, t + 0.002},
			{Command::Dispose, AudioParam::Set, 0, 0, 0, o},
			{Command::Dispose, AudioParam::Set, 0, 0, 0, g},
		};
		for (const Command& c : cmds)
			while (!r.g->post(c))
				std::this_thread::yield();
		while (r.g->pollEvent(e))
			ended += e.type == Event::Ended;
		r.g->collect();
	}
	double until = r.g->currentTime() + 0.1;
	while (r.g->currentTime() < until)
		std::this_thread::yield();
	done = true;
	render.join();
	r.render(4800);
	while (r.g->pollEvent(e))
		ended += e.type == Event::Ended;
	CHECK(ended == notes);
	CHECK(r.g->nodeCount() == 1);
}
