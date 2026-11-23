#pragma once
// Tart: the Push 2 as a Rack controller. E1 is the hardware echo test: the state of every pad, button, encoder and
// the touch strip, and the LEDs that echo it back. Machine hands it between the audio thread (MIDI events in, wanted
// LEDs out) and the UI thread (display). Audio-thread paths never allocate, wait on a lock, throw or log.
#include <atomic>
#include <cstdint>

#include "push2.hpp"

namespace tart {

struct State {
	uint8_t pad[64] = {};               // held pads' velocity, 0 = up
	bool button[128] = {};              // held buttons by CC
	int encoder[push2::kEncoders] = {}; // accumulated deltas per encoder id
	uint32_t touched = 0;               // bit per encoder id, push2::kStrip for the touch strip
	int bend = 8192;                    // touch strip, 0..16383
	uint8_t last[3] = {}, lastSize = 0; // last decoded message, for the display
	uint32_t events = 0;                // decoded events so far
};

void handle(State& s, const push2::Event& e);
/** The LEDs that show `s`: held pads lit by velocity, held buttons lit, Play blinking while not held. */
void leds(const State& s, push2::Leds& out);

class Machine {
  public:
	// --- Audio thread
	/** Applies n events under the lock and renders the wanted LEDs; false when the UI thread holds the lock (the caller
	 * keeps the events and retries next sample). */
	bool apply(const push2::Event* ev, int n, push2::Leds& want);

	// --- UI thread
	State snapshot(); // a copy under the lock

  private:
	bool tryLock() { return !busy_.exchange(true, std::memory_order_acquire); }
	void unlock() { busy_.store(false, std::memory_order_release); }
	void lock();

	// ponytail: one lock, as lattice::Machine; the audio side only try-locks and never waits.
	std::atomic<bool> busy_{false};
	State state_; // under busy_
};

} // namespace tart
