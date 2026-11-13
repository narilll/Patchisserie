#include "check.hpp"
#include "dsp.hpp"

#include <memory>

using namespace filo;

static Event ev(double b, double e, uint8_t kind = Note, float note = 60, uint32_t epoch = 0) {
	Event x{};
	x.epoch = epoch, x.begin = b, x.end = e, x.kind = kind, x.note = note, x.vel = 0.5f, x.midichan = -1;
	return x;
}

// ---- EventQueue ----
TEST(queue_order_by_begin) {
	auto q = std::make_unique<EventQueue>();
	q->push(ev(2, 10, Note, 62));
	q->push(ev(1, 10, Note, 61));
	q->push(ev(3, 10, Note, 63));
	Event out[8];
	CHECK(q->popDue(0.5, out, 8) == 0);
	CHECK(q->popDue(2.5, out, 8) == 2);
	CHECK(out[0].note == 61 && out[1].note == 62);
	CHECK(q->popDue(3.5, out, 8) == 1 && out[0].note == 63);
	CHECK(q->popDue(9, out, 8) == 0);
}
TEST(queue_late_fires_ended_dropped) {
	auto q = std::make_unique<EventQueue>();
	q->push(ev(1, 5, Note, 61)); // late but still sounding
	q->push(ev(0, 1, Note, 62)); // already ended
	Event out[8];
	CHECK(q->popDue(2, out, 8) == 1 && out[0].note == 61);
	CHECK(q->popDue(2, out, 8) == 0);
}
TEST(queue_epoch_filter) {
	auto q = std::make_unique<EventQueue>();
	q->push(ev(0, 9, Note, 60, 0));
	q->push(ev(0, 9, Note, 61, 1));
	Event out[8];
	q->setEpoch(1);
	CHECK(q->popDue(1, out, 8) == 1 && out[0].note == 61);
	q->push(ev(2, 9, Note, 62, 1));
	CHECK(q->popDue(0, out, 8) == 0); // moves it to pending
	q->setEpoch(2);                   // pending drops too
	CHECK(q->popDue(5, out, 8) == 0);
}
TEST(queue_full_and_clear) {
	auto q = std::make_unique<EventQueue>();
	for (uint32_t i = 0; i < EventQueue::kRing; i++)
		CHECK(q->push(ev(i, i + 1)));
	CHECK(!q->push(ev(0, 1)));
	Event out[8];
	CHECK(q->popDue(1e9, out, 0) == 0); // drains up to the pending capacity, nothing returned
	CHECK(q->push(ev(0, 1)));
	q->clear();
	CHECK(q->popDue(1e9, out, 8) == 0);
	CHECK(q->push(ev(0, 5)));
	CHECK(q->popDue(1, out, 8) == 1);
}

// ---- CycleClock internal ----
TEST(clock_internal_exact) {
	CycleClock c;
	c.setSampleRate(48000);
	c.setCps(0.5);
	c.start();
	for (int i = 0; i < 48000; i++)
		c.step(false, false, false);
	CHECK_NEAR(c.cycle(), 0.5, 1e-9);
	CHECK(c.cps() == 0.5);
}
TEST(clock_stop_start) {
	CycleClock c;
	c.setSampleRate(1000);
	c.setCps(1);
	CHECK(!c.started());
	for (int i = 0; i < 10; i++)
		c.step(false, false, false);
	CHECK(c.cycle() == 0);
	c.start();
	for (int i = 0; i < 100; i++)
		c.step(false, false, false);
	c.stop();
	double at = c.cycle();
	for (int i = 0; i < 100; i++)
		c.step(false, false, false);
	CHECK(c.cycle() == at);
	c.start();
	c.step(false, false, false);
	CHECK(c.cycle() > at);
}

// ---- PLL ----
TEST(pll_steady_120bpm) {
	CycleClock c;
	c.setSampleRate(48000);
	c.start();
	int pulses = 0;
	double last = 0;
	for (int i = 0; i < 6000 * 12; i++) {
		bool edge = i % 6000 == 0;
		c.step(true, edge, false);
		CHECK(c.cycle() >= last);
		last = c.cycle();
		if (edge && ++pulses == 4)
			CHECK_NEAR(c.cps(), 0.5, 0.0005);
	}
	CHECK_NEAR(c.cps(), 0.5, 1e-9);
	CHECK(c.cycle() >= 12.0 / 16 && c.cycle() < 13.0 / 16);
}
TEST(pll_ramp_120_to_140) {
	CycleClock c;
	c.setSampleRate(48000);
	c.start();
	auto cpsAt = [](double cy) { return (120 + 20 * cy / 8) / 240.0; }; // BPM / 60 / 4 beats per cycle
	double t = 0;
	long edge = 0;
	int k = 1;
	double last = 0;
	long total = 0;
	std::vector<long> edges;
	for (; k <= 128; k++) {
		t += 48000.0 / 16 / cpsAt((k - 0.5) / 16);
		edges.push_back(std::lround(t));
	}
	total = edges.back() + 10;
	k = 0;
	for (long i = 0; i < total; i++) {
		bool e = k < 128 && i == edges[k];
		c.step(true, e, false);
		CHECK(c.cycle() >= last);
		last = c.cycle();
		if (e) {
			k++;
			CHECK(std::fabs(c.cycle() - k / 16.0) < 1.0 / 16);
			if (k >= 5)
				CHECK(std::fabs(c.cps() / cpsAt((k - 2) / 16.0) - 1) < 0.01);
		}
	}
	CHECK(k == 128);
	(void)edge;
}
TEST(pll_jitter) {
	CycleClock c;
	c.setSampleRate(48000);
	c.start();
	uint32_t s = 12345;
	std::vector<long> edges;
	for (int k = 1; k <= 64; k++) {
		s = s * 1664525u + 1013904223u;
		long j = (long)((s >> 8) % 97) - 48; // +-1 ms
		edges.push_back(k * 6000 + j);
	}
	int k = 0;
	double last = 0;
	for (long i = 0; i < 65 * 6000; i++) {
		bool e = k < 64 && i == edges[k];
		c.step(true, e, false);
		CHECK(c.cycle() >= last);
		last = c.cycle();
		if (e && ++k >= 8)
			CHECK(std::fabs(c.cps() / 0.5 - 1) < 0.02);
	}
}
TEST(pll_ppc_change_recounts) {
	// External clock, a pulse every 100 samples. Changing the pulses per cycle mid-play re-counts the cycle in the
	// new unit: no stall (4 -> 24) and no jump forward (24 -> 4).
	CycleClock c;
	c.setSampleRate(48000);
	c.setPulsesPerCycle(4);
	c.start();
	int i = 0;
	auto pulses = [&](int n) {
		for (int k = 0; k < n; k++, i++)
			c.step(true, i % 100 == 0, false);
	};
	pulses(1000); // 10 pulses: cycle 2.25
	double before = c.cycle();
	c.setPulsesPerCycle(24);
	CHECK(c.cycle() == before);
	pulses(2400); // 24 pulses = 1 beat
	CHECK_NEAR(c.cycle() - before, 1.0, 0.1);
	before = c.cycle();
	c.setPulsesPerCycle(4);
	pulses(400); // 4 pulses = 1 beat
	CHECK_NEAR(c.cycle() - before, 1.0, 0.1);
}

TEST(pll_early_pulse_jumps) {
	CycleClock c;
	c.setSampleRate(48000);
	c.start();
	for (int i = 0; i < 6000 * 5 + 1; i++) // edges at 0, 6000, ... 30000: 6 pulses
		c.step(true, i % 6000 == 0, false);
	double before = c.cycle();
	CHECK_NEAR(before, 6 / 16.0, 1e-4);
	for (int i = 0; i < 2000; i++) // pulse 7 arrives at 2001/6000 of the period
		c.step(true, false, false);
	double mid = c.cycle();
	CHECK(mid < 7 / 16.0);
	c.step(true, true, false);
	CHECK(c.cycle() == 7 / 16.0);
	CHECK(c.cycle() > mid);
}
TEST(pll_unplug_continues_internal) {
	CycleClock c;
	c.setSampleRate(48000);
	c.start();
	for (int i = 0; i < 6000 * 6; i++)
		c.step(true, i % 6000 == 0, false);
	c.setCps(0.25); // ignored while external, remembered
	CHECK_NEAR(c.cps(), 0.5, 1e-9);
	double at = c.cycle();
	for (int i = 0; i < 480; i++)
		c.step(false, false, false);
	CHECK_NEAR(c.cycle(), at + 480 * 0.25 / 48000, 1e-9);
	CHECK(c.cps() == 0.25);
}
TEST(pll_hold_before_two_edges) {
	CycleClock c;
	c.setSampleRate(48000);
	c.start();
	for (int i = 0; i < 1000; i++)
		c.step(true, i == 100, false);
	CHECK(c.cycle() == 1 / 16.0);
}
TEST(pll_reset_ignores_edge_within_1ms) {
	auto s = std::make_unique<Sequencer>();
	s->setSampleRate(48000);
	s->start();
	s->queue.push(ev(0.5, 0.6));
	Sequencer::Inputs in;
	Sequencer::Outputs out;
	in.clockConnected = true;
	for (int i = 0; i < 6000 * 6; i++) {
		in.clockEdge = i % 6000 == 0;
		s->process(in, out);
	}
	CHECK(s->clock.cycle() > 0.2);
	in.clockEdge = false, in.resetEdge = true;
	s->process(in, out);
	in.resetEdge = false;
	CHECK(s->clock.cycle() == 0);
	for (int i = 0; i < 10; i++)
		s->process(in, out);
	in.clockEdge = true; // 10 samples after reset: inside the 1 ms guard
	s->process(in, out);
	in.clockEdge = false;
	CHECK(s->clock.cycle() < 1 / 16.0);
	for (int i = 0; i < 100; i++)
		s->process(in, out);
	in.clockEdge = true;
	s->process(in, out);
	CHECK(s->clock.cycle() == 1 / 16.0);
}

// ---- VoiceAllocator ----
static int on(VoiceAllocator& v, uint32_t id, float n, int mc = -1) {
	return v.noteOn(id, n, 1.f, mc);
}

TEST(voices_rotate) {
	VoiceAllocator v;
	v.setChannels(3);
	CHECK(on(v, 1, 60) == 0);
	CHECK(on(v, 2, 61) == 1);
	v.noteOff(1);
	CHECK(on(v, 3, 62) == 2);
	CHECK(on(v, 4, 63) == 0); // next after last, free
}
TEST(voices_reuse) {
	VoiceAllocator v;
	v.setMode(VoiceAllocator::Reuse);
	v.setChannels(3);
	CHECK(on(v, 1, 60) == 0);
	CHECK(on(v, 2, 62) == 1);
	v.noteOff(1);
	v.noteOff(2);
	CHECK(on(v, 3, 60) == 0); // rotate would give 2
	v.noteOff(3);
	CHECK(on(v, 4, 62) == 1);
	v.noteOff(4);
	CHECK(on(v, 5, 64) == 2); // unknown note: rotate (last was 1)
}
TEST(voices_reset) {
	VoiceAllocator v;
	v.setMode(VoiceAllocator::Reset);
	v.setChannels(3);
	CHECK(on(v, 1, 60) == 0);
	CHECK(on(v, 2, 61) == 1);
	v.noteOff(1);
	CHECK(on(v, 3, 62) == 0);
	CHECK(on(v, 4, 63) == 2);
}
TEST(voices_steal_oldest) {
	VoiceAllocator v;
	v.setChannels(3);
	on(v, 1, 60), on(v, 2, 61), on(v, 3, 62);
	CHECK(on(v, 4, 63) == 0);
	CHECK(on(v, 5, 64) == 1);
	v.noteOff(1); // id 1 was stolen: no effect on the new owner of channel 0
	CHECK(v.gate(0));
	CHECK(v.pitchV(0) == 3.f / 12.f);
}
TEST(voices_retrigger_gap) {
	VoiceAllocator v;
	v.setMode(VoiceAllocator::Reset);
	v.setChannels(1);
	on(v, 1, 60);
	CHECK(v.gate(0));
	v.tick();
	CHECK(v.gate(0));
	on(v, 2, 62); // gated channel gets a new note
	CHECK(!v.gate(0));
	v.tick();
	CHECK(v.gate(0));
	v.tick();
	CHECK(v.gate(0));
	v.noteOff(2); // note-off and note-on within one sample also gap
	on(v, 3, 64);
	CHECK(!v.gate(0));
	v.tick();
	CHECK(v.gate(0));
}
TEST(voices_mpe) {
	VoiceAllocator v;
	v.setMode(VoiceAllocator::MPE);
	CHECK(on(v, 1, 60, 5) == 4);
	CHECK(on(v, 2, 61, 5) == 4); // one voice per channel
	CHECK(on(v, 3, 62, -1) == 0);
	CHECK(on(v, 4, 63, 16) == 15);
	CHECK(v.velV(4) == 10.f);
}
TEST(voices_all_off) {
	VoiceAllocator v;
	on(v, 1, 60), on(v, 2, 61);
	v.allOff();
	CHECK(!v.gate(0) && !v.gate(1));
}

// ---- StickyMap ----
TEST(sticky_defaults_and_unknown) {
	StickyMap m(StickyMap::trigDefaults());
	CHECK(m.channel("bd") == 0 && m.channel("sd") == 1 && m.channel("cb") == 11);
	CHECK(m.channel("zap") == 12);
	CHECK(m.channel("zap") == 12);
	CHECK(m.channel("zip") == 13);
	StickyMap cc(StickyMap::ccDefaults());
	CHECK(cc.channel("1") == 0 && cc.channel("16") == 15);
	// Every channel has a CC default: a non-default CC gets the highest unused one, a default always its own.
	CHECK(cc.channel("74") == 14);
	CHECK(cc.channel("2") == 1);
	CHECK(cc.channel("15") == 14 && cc.channel("74") == 13); // 74 moves off CC 15's channel
	// ccn(74) first, then ccn(1) and ccn(2): they still get channels 1 and 2 (a user's patch had 74 -> 1, 1 -> 2).
	StickyMap first(StickyMap::ccDefaults());
	CHECK(first.channel("74") == 15 && first.channel("1") == 0 && first.channel("2") == 1);
	// A map saved by an older version, with defaults off their channels, is put right on use.
	StickyMap old(StickyMap::ccDefaults());
	old.load({{"74", 0}, {"1", 1}, {"2", 2}});
	CHECK(old.channel("1") == 0 && old.channel("2") == 1 && old.channel("74") == 15);
}
TEST(sticky_persist_reset_full) {
	StickyMap m(StickyMap::trigDefaults());
	m.channel("a");
	auto saved = m.entries();
	StickyMap n(StickyMap::trigDefaults());
	n.load(saved);
	CHECK(n.channel("a") == 12 && n.channel("bd") == 0);
	m.reset();
	CHECK(m.channel("b") == 12);                    // "a" forgotten
	m.channel("c"), m.channel("d"), m.channel("e"); // 13, 14, 15: non-default channels are used up
	CHECK(m.channel("f") == 11);                    // then the highest unused default channel (cb not played yet)
	CHECK(m.channel("bd") == 0);                    // a default always gets its own
	for (int i = 0; i < 9; i++)
		m.channel("x" + std::to_string(i)); // fills 10 down to 2
	CHECK(m.channel("last") == 1);
	CHECK(m.channel("over") == -1);
	CHECK(m.channel("over") == -1);
	CHECK(m.channel("sd") == 1 && m.channel("last") == -1); // sd takes its channel back; nowhere left for "last"
}

// ---- ClockOut ----
TEST(clockout_n_per_cycle) {
	for (int N : {1, 2, 4, 8, 16, 24, 32, 48, 96}) {
		auto s = std::make_unique<Sequencer>();
		s->setSampleRate(1000);
		s->clock.setCps(0.37);
		Event t = ev(2.0, 2.0, Tempo); // tempo change mid-run
		t.value = 0.9f;
		s->queue.push(t);
		s->clockOut.setDivision(N);
		s->start();
		Sequencer::Inputs in;
		Sequencer::Outputs out;
		int n = 0;
		while (s->clock.cycle() < 4) {
			s->process(in, out);
			n += out.clk;
		}
		CHECK(n == 4 * N);
	}
}

// ---- Sequencer end-to-end ----
TEST(sequencer_clk_pulses_on_the_downbeat_at_start) {
	const int sr = 1000, N = 4;
	const double cps = 0.5;
	auto s = std::make_unique<Sequencer>();
	s->setSampleRate(sr);
	s->clock.setCps(cps);
	s->clockOut.setDivision(N);
	Sequencer::Inputs in;
	Sequencer::Outputs out;
	in.resetEdge = true; // PLAY: reset while stopped, the clock waits at 0
	s->process(in, out);
	in.resetEdge = false;
	int stopped = out.clk;
	for (int i = 0; i < 50; i++) {
		s->process(in, out);
		stopped += out.clk;
	}
	CHECK(stopped == 0);
	s->start();
	std::vector<int> pulses;
	for (int i = 0; i < 2 * sr; i++) {
		s->process(in, out);
		if (out.clk)
			pulses.push_back(i);
	}
	CHECK(pulses.size() >= 2 && pulses[0] == 0);
	CHECK(pulses.size() >= 2 && pulses[1] == (int)(sr / (cps * N)));
}

TEST(sequencer_notes_sample_exact) {
	auto s = std::make_unique<Sequencer>();
	s->setSampleRate(48000);
	s->clock.setCps(0.5);
	s->start();
	for (int i = 0; i < 4; i++)
		s->queue.push(ev(i * 0.25, i * 0.25 + 0.1, Note, 60 + i));
	Sequencer::Inputs in;
	Sequencer::Outputs out;
	std::vector<int> rise, fall;
	bool g = false;
	for (int i = 0; i < 96000; i++) {
		s->process(in, out);
		bool now = false;
		for (int c = 0; c < 4; c++)
			now |= out.gate[c] > 0;
		if (now && !g)
			rise.push_back(i);
		if (!now && g)
			fall.push_back(i);
		g = now;
	}
	CHECK((rise == std::vector<int>{0, 24000, 48000, 72000}));
	CHECK((fall == std::vector<int>{9600, 33600, 57600, 81600}));
	CHECK(out.channels == 16);
}
TEST(sequencer_trig_cc_stop) {
	auto s = std::make_unique<Sequencer>();
	s->setSampleRate(48000);
	s->start();
	Event t = ev(0.0001, 0.0002, Trig);
	t.channel = 3;
	Event c = ev(0.0001, 0.0002, CC);
	c.channel = 5, c.value = 0.5f;
	Event n = ev(0, 100, Note, 72);
	s->queue.push(t), s->queue.push(c), s->queue.push(n);
	s->queue.push(ev(5, 6, Note, 50)); // dropped by stop
	Sequencer::Inputs in;
	Sequencer::Outputs out;
	int trigs = 0;
	for (int i = 0; i < 200; i++) {
		s->process(in, out);
		trigs += out.trig[3];
		CHECK(!out.trig[4]);
	}
	CHECK(trigs == 1);
	CHECK_NEAR(out.cc[5], 5.0, 1e-6);
	CHECK(out.gate[0] == 10.f && out.voct[0] == 1.f && out.vel[0] == 5.f);
	s->stop();
	s->process(in, out);
	for (int c2 = 0; c2 < 16; c2++)
		CHECK(out.gate[c2] == 0.f);
	double at = s->clock.cycle();
	s->start();
	for (int i = 0; i < 10; i++)
		s->process(in, out);
	CHECK(s->clock.cycle() > at && out.gate[1] == 0.f);
}
TEST(sequencer_tempo_event) {
	auto s = std::make_unique<Sequencer>();
	s->setSampleRate(1000);
	s->clock.setCps(0.5);
	s->start();
	Event t = ev(0.5, 0.5, Tempo);
	t.value = 1;
	s->queue.push(t);
	s->queue.push(ev(1.0, 1.1, Note));
	Event stale = ev(0.75, 0.75, Tempo, 60, 7); // another epoch: dropped
	stale.value = 4;
	s->queue.push(stale);
	Sequencer::Inputs in;
	Sequencer::Outputs out;
	int rise = -1;
	for (int i = 0; i < 3000 && rise < 0; i++) {
		s->process(in, out);
		if (out.gate[0] > 0)
			rise = i;
	}
	// cycle 0.5 at sample 1000 (0.5 cps), from there 1 cps: cycle 1.0 at sample 1500.
	CHECK(rise == 1500);
	CHECK(s->clock.cps() == 1.0);
}
