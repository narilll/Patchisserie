#pragma once
// Pure logic for the Filo module's Rack outputs: event queue, cycle clock/PLL, voices, sticky maps, CLK divider.
// No Rack UI headers. Audio-thread paths (EventQueue consumer side, CycleClock, VoiceAllocator, ClockOut,
// Sequencer::process) use fixed-size storage only: no allocation, locks or exceptions.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace filo {

enum Kind : uint8_t { Note, Trig, CC, Tempo };

struct Event {
	uint32_t epoch;
	double begin, end; // cycles
	uint8_t kind;
	float note;      // MIDI number, fractional allowed
	float vel;       // 0..1
	int8_t midichan; // 1..16, -1 unset
	int8_t channel;  // 0..15, pre-resolved for Trig/CC
	float value;     // CC value 0..1; Tempo: cps from `begin` on
};
static_assert(std::is_trivially_copyable<Event>::value, "Event must be POD-like");

// SPSC: producer thread (JS) push(), audio thread everything else.
class EventQueue {
  public:
	static constexpr uint32_t kRing = 4096, kPending = 1024;

	// Producer side. Returns false when full; the caller must check.
	bool push(const Event& e) {
		uint32_t h = head_.load(std::memory_order_relaxed);
		if (h - tail_.load(std::memory_order_acquire) >= kRing)
			return false;
		ring_[h % kRing] = e;
		head_.store(h + 1, std::memory_order_release);
		return true;
	}

	// Consumer side. Events of another epoch are dropped (pending now, ring contents when seen).
	void setEpoch(uint32_t epoch) {
		epoch_ = epoch;
		int k = 0;
		for (int i = 0; i < n_; i++)
			if (pending_[i].epoch == epoch_)
				pending_[k++] = pending_[i];
		n_ = k;
	}

	// Drops everything pending, including what is still in the ring.
	void clear() {
		n_ = 0;
		tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
	}

	// Writes up to `max` events with begin <= cycle to out, in begin order; returns the count.
	// Events that already ended (end <= cycle, end > begin) are dropped instead; zero-length events always fire.
	int popDue(double cycle, Event* out, int max) {
		drain();
		int i = 0, k = 0;
		while (i < n_ && pending_[i].begin <= cycle) {
			const Event& e = pending_[i];
			if (!(e.end <= cycle && e.end > e.begin)) {
				if (k == max)
					break;
				out[k++] = e;
			}
			i++;
		}
		std::memmove(pending_, pending_ + i, (n_ - i) * sizeof(Event));
		n_ -= i;
		return k;
	}

  private:
	void drain() {
		uint32_t t = tail_.load(std::memory_order_relaxed), h = head_.load(std::memory_order_acquire);
		// Pending full: leave the rest in the ring (backpressure shows up as push() == false).
		while (t != h && n_ < (int)kPending) {
			const Event& e = ring_[t++ % kRing];
			if (e.epoch != epoch_)
				continue;
			int i = n_++;
			while (i > 0 && pending_[i - 1].begin > e.begin) {
				pending_[i] = pending_[i - 1];
				i--;
			}
			pending_[i] = e;
		}
		tail_.store(t, std::memory_order_release);
	}

	Event ring_[kRing];
	std::atomic<uint32_t> head_{0}, tail_{0};
	Event pending_[kPending];
	int n_ = 0;
	uint32_t epoch_ = 0;
};

class CycleClock {
  public:
	void setSampleRate(float sr) { sr_ = sr, anchor(); }
	void setCps(double cps) { cps_ = cps, anchor(); } // internal tempo; remembered while external
	void setPulsesPerCycle(int ppc) {
		ppc = std::max(1, ppc);
		if (ppc == ppc_) // called every sample
			return;
		// pulses_ counts in the old unit: re-count the cycle in the new one, the estimate restarts.
		ppc_ = ppc, pulses_ = (int64_t)std::floor(cycle_ * ppc), np_ = 0, hasLast_ = false;
	}
	double cycle() const { return cycle_; }
	double cps() const { return ext_ ? (np_ ? cpsEst_ : 0.0) : cps_; }
	bool started() const { return running_; }
	void start() {
		running_ = true;
		hasLast_ = false;
		sinceEdge_ = 0;
	}
	void stop() { running_ = false; }

	void step(bool clockConnected, bool clockEdge, bool resetEdge) {
		if (clockConnected && !ext_)
			hasLast_ = false, np_ = 0, sinceEdge_ = 0, pulses_ = (int64_t)std::floor(cycle_ * ppc_);
		if (!clockConnected && ext_)
			anchor();
		ext_ = clockConnected;
		bool edge = clockEdge;
		if (guard_ > 0)
			guard_--, edge = false;
		if (resetEdge) {
			cycle_ = 0, pulses_ = 0, sinceEdge_ = 0, hasLast_ = false;
			anchor();
			guard_ = (int)std::ceil(sr_ * 0.001); // ignore clock edges for 1 ms, like Fundamental SEQ3
			return;
		}
		if (!running_)
			return;
		if (!ext_) {
			// From the last tempo change, not summed per sample: the JS host predicts sample n's cycle from a
			// snapshot with the same formula, so audio and .midi() events stay on one sample after hours.
			cycle_ = anchorCycle_ + (double)++sinceAnchor_ * cps_ / sr_;
			return;
		}
		sinceEdge_++;
		if (edge) {
			if (hasLast_) {
				// Average of the last 4 periods = (t_k - t_k-4)/4: jitter on a pulse is divided by 4.
				per_[pi_++ & 3] = (double)sinceEdge_;
				np_ = std::min(np_ + 1, 4);
				double sum = 0;
				for (int i = 0; i < np_; i++)
					sum += per_[i];
				cpsEst_ = sr_ / (sum / np_ * ppc_);
			}
			hasLast_ = true;
			sinceEdge_ = 0;
			pulses_++;
		}
		double floorC = (double)pulses_ / ppc_;
		double c = floorC; // no estimate yet: hold at the pulse position
		if (np_) {
			// Never runs past the next pulse.
			double next = (double)(pulses_ + 1) / ppc_;
			c = std::min(floorC + sinceEdge_ * cpsEst_ / sr_, std::nextafter(next, 0.0));
		}
		cycle_ = std::max(cycle_, std::max(c, floorC)); // monotonic; an early pulse jumps forward
	}

  private:
	void anchor() { anchorCycle_ = cycle_, sinceAnchor_ = 0; }
	double sr_ = 48000, cps_ = 0.5, cycle_ = 0, cpsEst_ = 0;
	int ppc_ = 16, guard_ = 0, np_ = 0, pi_ = 0;
	int64_t pulses_ = 0, sinceEdge_ = 0, sinceAnchor_ = 0;
	double per_[4] = {}, anchorCycle_ = 0;
	bool running_ = false, ext_ = false, hasLast_ = false;
};

class VoiceAllocator {
  public:
	enum Mode { Rotate, Reuse, Reset, MPE };

	void setChannels(int n) {
		n = std::min(std::max(n, 1), 16);
		if (n != n_)
			n_ = n, allOff();
	}
	int channels() const { return n_; }
	void setMode(Mode m) { mode_ = m; }

	int noteOn(uint32_t id, float note, float vel, int midichan) {
		int ch = pick(note, midichan);
		if (high_[ch])
			gap_[ch] = true; // retrigger: gate low for this one sample
		v_[ch] = {true, id, note, vel, ++age_};
		last_ = ch;
		return ch;
	}
	void noteOff(uint32_t id) {
		for (int c = 0; c < 16; c++)
			if (v_[c].active && v_[c].id == id)
				v_[c].active = false;
	}
	void allOff() {
		for (int c = 0; c < 16; c++)
			v_[c].active = false, gap_[c] = false;
	}
	// Call once per sample after reading the outputs.
	void tick() {
		for (int c = 0; c < 16; c++)
			high_[c] = gate(c), gap_[c] = false;
	}
	bool gate(int c) const { return v_[c].active && !gap_[c]; }
	float pitchV(int c) const { return (v_[c].note - 60.f) / 12.f; }
	float velV(int c) const { return v_[c].vel * 10.f; }

  private:
	struct Voice {
		bool active;
		uint32_t id;
		float note, vel;
		uint64_t age;
	};
	int oldest() const {
		int best = 0;
		for (int c = 1; c < n_; c++)
			if (v_[c].age < v_[best].age)
				best = c;
		return best;
	}
	int rotate() const {
		for (int i = 1; i <= n_; i++) {
			int c = (last_ + i) % n_;
			if (!v_[c].active)
				return c;
		}
		return oldest();
	}
	int pick(float note, int midichan) const {
		switch (mode_) {
		case MPE: // one voice per channel; out-of-range clamps to the last channel, unset -> 0
			return midichan < 1 ? 0 : std::min(midichan - 1, n_ - 1);
		case Reset:
			for (int c = 0; c < n_; c++)
				if (!v_[c].active)
					return c;
			return oldest();
		case Reuse:
			for (int c = 0; c < n_; c++)
				if (!v_[c].active && v_[c].note == note)
					return c;
			return rotate();
		default:
			return rotate();
		}
	}
	Voice v_[16] = {};
	bool gap_[16] = {}, high_[16] = {};
	int n_ = 16, last_ = -1;
	uint64_t age_ = 0;
	Mode mode_ = Rotate;
};

// Key string -> channel 0..15. A default key (CC 1..16, bd, sd, ...) always gets its default channel: a key that sits
// there moves (defaults win). Other keys are assigned on first use and then sticky: the lowest channel no default
// points at, else the highest default channel whose key hasn't been used (CC 1..16 all have defaults; from the top so
// the low, common numbers stay free).
class StickyMap {
  public:
	explicit StickyMap(std::vector<std::pair<std::string, int>> defaults) : defaults_(std::move(defaults)) {}

	static std::vector<std::pair<std::string, int>> trigDefaults() {
		return {{"bd", 0}, {"sd", 1}, {"hh", 2}, {"oh", 3}, {"cp", 4},  {"rim", 5},
				{"lt", 6}, {"mt", 7}, {"ht", 8}, {"cr", 9}, {"rd", 10}, {"cb", 11}};
	}
	static std::vector<std::pair<std::string, int>> ccDefaults() {
		std::vector<std::pair<std::string, int>> d;
		for (int i = 1; i <= 16; i++)
			d.push_back({std::to_string(i), i - 1});
		return d;
	}

	// -1 when all 16 channels are taken (not remembered); the caller ignores the event and logs once.
	int channel(const std::string& key) {
		int def = defaultOf(key);
		auto it = map_.find(key); // no allocation for known keys
		if (it != map_.end() && (def < 0 || it->second == def))
			return it->second;
		if (def < 0)
			return assign(key);
		std::string moved; // what sits on the default's channel (an older map may have put the key elsewhere)
		for (auto& kv : map_)
			if (kv.second == def && kv.first != key)
				moved = kv.first;
		map_[key] = def;
		if (!moved.empty()) {
			map_.erase(moved);
			assign(moved);
		}
		return def;
	}
	void reset() { map_.clear(); }
	std::vector<std::pair<std::string, int>> entries() const { return {map_.begin(), map_.end()}; }
	void load(const std::vector<std::pair<std::string, int>>& e) {
		map_.clear();
		for (auto& kv : e)
			if (kv.second >= 0 && kv.second < 16) // patch data is a real boundary
				map_.emplace(kv);
	}

  private:
	int defaultOf(const std::string& key) const {
		for (auto& kv : defaults_)
			if (kv.first == key)
				return kv.second;
		return -1;
	}
	int assign(const std::string& key) { // a key without a default
		bool used[16] = {}, reserved[16] = {};
		for (auto& kv : map_)
			used[kv.second] = true;
		for (auto& kv : defaults_)
			reserved[kv.second] = true;
		for (int c = 0; c < 16; c++)
			if (!used[c] && !reserved[c])
				return map_.emplace(key, c).first->second;
		for (int c = 15; c >= 0; c--)
			if (!used[c])
				return map_.emplace(key, c).first->second;
		return -1;
	}

	std::vector<std::pair<std::string, int>> defaults_;
	std::map<std::string, int> map_;
};

// CLK divider: true when floor(cycle*N) changed. The caller turns it into a 1 ms 10 V pulse.
class ClockOut {
  public:
	void setDivision(int n) { n_ = std::max(1, n); } // N in 1,2,4,8,16,24,32,48,96 (menu-enforced)
	bool clk(double prevCycle, double cycle) const { return std::floor(cycle * n_) != std::floor(prevCycle * n_); }

  private:
	int n_ = 16;
};

class Sequencer {
  public:
	struct Inputs {
		bool clockConnected = false, clockEdge = false, resetEdge = false;
	};
	struct Outputs {
		float voct[16], gate[16], vel[16], cc[16]; // volts; gate 0/10
		bool trig[16], clk;
		int channels;
	};

	// Owned parts are public: the host configures them directly.
	CycleClock clock;
	EventQueue queue;
	VoiceAllocator voices;
	ClockOut clockOut;

	void setSampleRate(float sr) { clock.setSampleRate(sr); }
	bool started() const { return clock.started(); }
	void start() { clock.start(); }
	// STOP: gates down, queue emptied. The caller bumps the epoch so events still in flight are dropped.
	void stop() {
		clock.stop();
		voices.allOff();
		queue.clear();
		nAct_ = 0;
	}

	void process(const Inputs& in, Outputs& out) {
		// Events fire at the cycle at the start of this sample, so begin 0 fires on sample 0, and a Tempo event sets
		// the cps this sample advances with.
		if (in.resetEdge) { // pending events belong to the old timeline
			clock.step(in.clockConnected, in.clockEdge, true);
			voices.allOff();
			queue.clear();
			nAct_ = 0;
			lastAt_ = -1;
		}
		double at = clock.cycle() + kEps;
		std::memset(out.trig, 0, sizeof out.trig);
		if (clock.started()) {
			for (int i = 0; i < nAct_;)
				if (act_[i].end <= at) {
					voices.noteOff(act_[i].id);
					act_[i] = act_[--nAct_];
				} else
					i++;
			Event ev[16];
			int n;
			do {
				n = queue.popDue(at, ev, 16);
				for (int i = 0; i < n; i++)
					fire(ev[i], out);
			} while (n == 16);
		}
		out.clk = clock.started() && clockOut.clk(lastAt_, at);
		if (clock.started()) // held while stopped (-1 after a reset), so the first pulse lands on cycle 0 at start
			lastAt_ = at;
		if (!in.resetEdge)
			clock.step(in.clockConnected, in.clockEdge, false);
		for (int c = 0; c < 16; c++) {
			out.voct[c] = voices.pitchV(c);
			out.gate[c] = voices.gate(c) ? 10.f : 0.f;
			out.vel[c] = voices.velV(c);
			out.cc[c] = cc_[c] * 10.f;
		}
		out.channels = voices.channels();
		voices.tick();
	}

  private:
	static constexpr double kEps = 1e-9; // absorbs float summation error so begin 0.25 lands on the exact sample
	static constexpr int kActive = 128;
	struct Active {
		uint32_t id;
		double end;
	};
	void fire(const Event& e, Outputs& out) {
		if (e.kind == Note) {
			if (nAct_ == kActive) // ponytail: >128 simultaneous notes drop the new one; raise kActive if needed
				return;
			uint32_t id = ++nextId_;
			voices.noteOn(id, e.note, e.vel, e.midichan);
			act_[nAct_++] = {id, e.end};
		} else if (e.kind == Tempo) {
			if (e.value > 0)
				clock.setCps(e.value);
		} else if (e.channel >= 0 && e.channel < 16) {
			if (e.kind == Trig)
				out.trig[e.channel] = true;
			else
				cc_[e.channel] = std::min(std::max(e.value, 0.f), 1.f);
		}
	}

	Active act_[kActive];
	int nAct_ = 0;
	uint32_t nextId_ = 0;
	float cc_[16] = {};
	double lastAt_ = -1;
};

} // namespace filo
