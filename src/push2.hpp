#pragma once
// Push 2 (Ableton): the MIDI event decoder, the LED message diff and the display frame format, as pure functions. The
// numbers come from Ableton's "Push 2 MIDI and Display Interface Manual"; sections are named in the comments. Nothing
// here touches MIDI ports, USB or GL, so the host tests cover it all.
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct NVGcontext;
struct NVGLUframebuffer;

namespace push2 {

// ---- Device ("USB Display Interface Access", "Display Interface Protocol")

constexpr uint16_t kVendorId = 0x2982, kProductId = 0x1967;
constexpr int kWidth = 960, kHeight = 160;
constexpr int kLineBytes = 2048; // 1920 pixel bytes + 128 filler bytes per line
constexpr int kFrameBytes = kHeight * kLineBytes;

inline constexpr uint8_t kFrameHeader[16] = {0xFF, 0xCC, 0xAA, 0x88};

// "Set MIDI Mode" (sysex 0x0A): 1 = User, 0 = Live.
inline constexpr uint8_t kUserMode[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x0A, 0x01, 0xF7};
inline constexpr uint8_t kLiveMode[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x0A, 0x00, 0xF7};

// ---- Controls ("MIDI Mapping", Appendix A)

namespace cc { // buttons (control change numbers; value 127 pressed, 0 released)
enum : uint8_t {
	TapTempo = 3,
	Metronome = 9,
	Track1Below = 20, // ...Track8Below = 27
	Track8Below = 27,
	Master = 28,
	StopClip = 29,
	Setup = 30,
	Layout = 31,
	Convert = 35,
	Scene8 = 36, // scene buttons count down: 36 = scene 8 ... 43 = scene 1
	Scene7 = 37,
	Scene6 = 38,
	Scene5 = 39,
	Scene4 = 40,
	Scene3 = 41,
	Scene2 = 42,
	Scene1 = 43,
	Left = 44,
	Right = 45,
	Up = 46,
	Down = 47,
	Select = 48,
	Shift = 49,
	Note = 50,
	Session = 51,
	AddDevice = 52,
	AddTrack = 53,
	OctaveDown = 54,
	OctaveUp = 55,
	Repeat = 56,
	Accent = 57,
	Scale = 58,
	User = 59,
	Mute = 60,
	Solo = 61,
	PageLeft = 62,
	PageRight = 63,
	Play = 85,
	Record = 86,
	New = 87,
	Duplicate = 88,
	Automate = 89,
	FixedLength = 90,
	Track1Above = 102, // ...Track8Above = 109
	Track8Above = 109,
	Device = 110,
	Browse = 111,
	Mix = 112,
	Clip = 113,
	Quantize = 116,
	DoubleLoop = 117,
	Delete = 118,
	Undo = 119,
};
// Encoders (control change; value is a relative delta)
constexpr uint8_t kTempo = 14, kSwing = 15, kTrack1 = 71 /* ...72..78 for tracks 2..8 */, kMasterVolume = 79;
} // namespace cc

// Encoder ids, one numbering for Encoder and Touch events. The touch notes are 0..7 for the track encoders, then 8, 9
// and 10 for the master volume, swing and tempo encoders; the ids are those note numbers. The CCs differ (see
// encoderOf). ponytail: notes 8..10 as Ableton's Live scripts use them; the manual's text only lists "0-10" (the
// mapping is a picture). Verify on hardware in the echo test.
enum Encoder : uint8_t { kTrack = 0, kMasterId = 8, kSwingId = 9, kTempoId = 10 };
constexpr int kEncoders = 11;
constexpr uint8_t kStripNote = 12;    // touch strip touch/release ("Touch Strip")
constexpr uint8_t kStrip = kEncoders; // Touch index of the touch strip (one past the last encoder id)

constexpr int kPadFirst = 36, kPadLast = 99; // "Pads": 8x8, bottom-left 36, top-right 99

// ---- Events

struct Event {
	enum Type : uint8_t { None, Pad, Button, Encoder, Touch, Bend } type = None;
	uint8_t index = 0; // Pad: row*8+col (row 0 = bottom); Button: CC; Encoder/Touch: encoder id or kStrip
	int value = 0;     // Pad: velocity (0 released); Button/Touch: 1/0; Encoder: delta; Bend: 0..16383
	uint8_t raw[3] = {};
	uint8_t size = 0; // bytes in raw (up to the first 3 of the message)
};

/** The two's complement 7-bit relative value of "Encoders": 1..63 right, 127..64 left. */
inline int relative(uint8_t v) {
	v &= 0x7F;
	return v >= 64 ? (int)v - 128 : (int)v;
}

inline uint8_t padNote(int row, int col) {
	return (uint8_t)(kPadFirst + row * 8 + col);
}

inline bool padOf(uint8_t note, int& row, int& col) {
	if (note < kPadFirst || note > kPadLast)
		return false;
	row = (note - kPadFirst) / 8, col = (note - kPadFirst) % 8;
	return true;
}

/** Button CCs the device sends (Appendix A, "Transmit" and "Recognize" both Yes). */
inline bool isButton(uint8_t c) {
	return c == cc::TapTempo || c == cc::Metronome || (c >= 20 && c <= 63) || (c >= 85 && c <= 90) ||
		   (c >= 102 && c <= 113) || (c >= 116 && c <= 119);
}

/** An encoder id from its CC, or -1. */
inline int encoderOf(uint8_t c) {
	if (c >= cc::kTrack1 && c < cc::kTrack1 + 8)
		return c - cc::kTrack1;
	return c == cc::kMasterVolume ? kMasterId : c == cc::kSwing ? kSwingId : c == cc::kTempo ? kTempoId : -1;
}

/** Decodes one MIDI message (channel is ignored). False, with type None, for anything the device doesn't send as a
 * control event: aftertouch, channel pressure, SysEx, realtime, the touch strip's mod-wheel mode, malformed data. */
inline bool decode(const uint8_t* b, size_t n, Event& e) {
	e = Event();
	e.size = (uint8_t)(n < 3 ? n : 3);
	if (e.size)
		std::memcpy(e.raw, b, e.size);
	if (n < 3 || !(b[0] & 0x80) || (b[1] & 0x80) || (b[2] & 0x80))
		return false;
	uint8_t kind = b[0] & 0xF0, d1 = b[1], d2 = b[2];
	int row, col;
	if (kind == 0x90 || kind == 0x80) {
		int vel = kind == 0x80 ? 0 : d2; // note off or note on with velocity 0: released
		if (padOf(d1, row, col)) {
			e.type = Event::Pad, e.index = (uint8_t)(row * 8 + col), e.value = vel;
		} else if (d1 == kStripNote) {
			e.type = Event::Touch, e.index = kStrip, e.value = vel > 0;
		} else if (d1 < kEncoders) {
			e.type = Event::Touch, e.index = d1, e.value = vel > 0;
		}
	} else if (kind == 0xB0) {
		if (isButton(d1)) {
			e.type = Event::Button, e.index = d1, e.value = d2 > 0;
		} else if (int id = encoderOf(d1); id >= 0) {
			e.type = Event::Encoder, e.index = (uint8_t)id, e.value = relative(d2);
		}
	} else if (kind == 0xE0) {
		e.type = Event::Bend, e.value = d2 << 7 | d1;
	}
	return e.type != Event::None;
}

/** Whether a MIDI port name is Push 2's User port (not the Live port), as Rack/RtMidi names it: macOS "... User Port",
 * Windows "MIDIIN2 (...)"/"MIDIOUT2 (...)", Linux ALSA "client:port" ending in ":1" (the Live port ends in ":0").
 * ponytail: substring and suffix rules; the module's menu has a manual override for any other naming. */
inline bool isUserPort(const std::string& name) {
	if (name.find("Live Port") != std::string::npos)
		return false;
	if (name.find("User Port") != std::string::npos || name.find("MIDIIN2") != std::string::npos ||
		name.find("MIDIOUT2") != std::string::npos)
		return true;
	size_t sp = name.rfind(' '), colon = name.rfind(':'); // ALSA: the last word is "<client>:<port>"
	return colon != std::string::npos && (sp == std::string::npos || colon > sp) && name.compare(colon, 2, ":1") == 0 &&
		   colon + 2 == name.size();
}

// ---- LEDs ("LEDs", "LED Animation", "Default Color Palettes")

// Palette indices that stay fixed across firmware (the RGB and white palettes).
constexpr uint8_t kBlack = 0, kRgbWhite = 122, kRgbLightGray = 123, kRgbDarkGray = 124, kBlue = 125, kGreen = 126,
				  kRed = 127;
constexpr uint8_t kWhiteDark = 16, kWhiteLight = 48,
				  kWhiteFull = 127; // white LEDs (the 0..127 index is the brightness)

// LED animation channels: 0 none, 1..5 one-shot, 6..10 pulsing, 11..15 blinking; within each, 24th, 16th, 8th, quarter,
// half note.
constexpr uint8_t kStatic = 0, kPulse24th = 6, kPulseHalf = 10, kBlinkQuarter = 14, kBlinkHalf = 15;

struct Led {
	uint8_t color = 0;   // palette index (RGB LEDs) or brightness (white LEDs)
	uint8_t channel = 0; // 0 static, 1..15 animation: `color` is the second colour, `base` the first
	// Wanted: the static colour an animation starts from. In `sent`: the static colour last sent (the device keeps it).
	uint8_t base = 0;
};
struct Leds {
	Led note[128]; // pads (note-on)
	Led cc[128];   // buttons (control change)
};

/** Writes up to `max` 3-byte messages (status | channel, number, colour) for the entries of `want` that differ from
 * `sent` (all of them when `all`), updates `sent` for those only and returns the count; the rest go out on the next
 * call. An animation starts from the colour last sent static (the manual: the second colour on channels 1..15
 * transitions from the one on channel 0), so when that is not its `base`, the base goes out first; both count toward
 * `max`, and with room for one only the base goes out now. */
inline int diff(const Leds& want, Leds& sent, bool all, uint8_t (*out)[3], int max) {
	int n = 0;
	for (int k = 0; k < 256 && n < max; k++) {
		const Led& w = k < 128 ? want.note[k] : want.cc[k - 128];
		Led& s = k < 128 ? sent.note[k] : sent.cc[k - 128];
		bool base = w.channel && (all || s.base != w.base);
		if (!all && !base && w.color == s.color && w.channel == s.channel)
			continue;
		uint8_t status = k < 128 ? 0x90 : 0xB0;
		if (base) {
			out[n][0] = status, out[n][1] = (uint8_t)(k & 127), out[n][2] = w.base;
			n++;
			s = {w.base, 0, w.base};
		}
		if (n < max) {
			out[n][0] = (uint8_t)(status | w.channel), out[n][1] = (uint8_t)(k & 127), out[n][2] = w.color;
			n++;
			s = {w.color, w.channel, w.channel ? w.base : w.color};
		}
	}
	return n;
}

// ---- Color palette ("RGB LED Color Processing": Set LED Color Palette Entry 0x03, Reapply Color Palette 0x05)

inline constexpr uint8_t kReapplyPalette[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x05, 0xF7};

/** The SysEx that sets palette entry `i` (0..127) to r, g, b and white w (0..255 each; w is the white LEDs' level for
 * the same index). Each value goes out as its low 7 bits, then its high bit. */
inline void paletteEntry(uint8_t i, uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t out[17]) {
	const uint8_t head[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x03, i};
	std::memcpy(out, head, sizeof head);
	const uint8_t v[4] = {r, g, b, w};
	for (int k = 0; k < 4; k++)
		out[8 + 2 * k] = v[k] & 0x7F, out[9 + 2 * k] = v[k] >> 7;
	out[16] = 0xF7;
}

// ---- Setup ("Global LED Brightness" 0x06, "Display Backlight" 0x08)

/** Set LED Brightness: 0..127 (a Push on USB power alone limits it to 8). */
inline void ledBrightness(uint8_t b, uint8_t out[9]) {
	const uint8_t m[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x06, (uint8_t)(b & 0x7F), 0xF7};
	std::memcpy(out, m, sizeof m);
}

/** Set Display Brightness: 0..255 as its low 7 bits, then the high bit (USB power alone limits it to 100). */
inline void displayBrightness(uint8_t b, uint8_t out[10]) {
	const uint8_t m[] = {0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x08, (uint8_t)(b & 0x7F), (uint8_t)(b >> 7), 0xF7};
	std::memcpy(out, m, sizeof m);
}

// ---- Display frame ("Pixel Data", "Pixel Color Encoding", "XORing Pixel Data")

/** `rgba`: kWidth*kHeight RGBA8, bottom row first (glReadPixels). `frame`: kFrameBytes, top row first, each pixel 16
 * bit little endian (b4..b0 g5..g0 r4..r0), 128 zero filler bytes closing each line, then every line XORed whole
 * (filler included) with the bytes E7 F3 E7 FF repeating from its first byte. */
inline void toFrame(const uint8_t* rgba, uint8_t* frame) {
	static const uint8_t kShape[4] = {0xE7, 0xF3, 0xE7, 0xFF};
	for (int y = 0; y < kHeight; y++) {
		const uint8_t* src = rgba + (size_t)(kHeight - 1 - y) * kWidth * 4;
		uint8_t* dst = frame + (size_t)y * kLineBytes;
		for (int x = 0; x < kWidth; x++, src += 4, dst += 2) {
			unsigned v = (unsigned)(src[2] >> 3) << 11 | (unsigned)(src[1] >> 2) << 5 | (unsigned)(src[0] >> 3);
			dst[0] = (uint8_t)(v & 0xFF), dst[1] = (uint8_t)(v >> 8);
		}
		std::memset(dst, 0, kLineBytes - kWidth * 2);
		dst = frame + (size_t)y * kLineBytes;
		for (int i = 0; i < kLineBytes; i++)
			dst[i] ^= kShape[i & 3];
	}
}

// ---- GL and USB (push2.cpp; the only code here that is not pure)

/** The 960x160 render target. UI thread only (GL). */
class Screen {
  public:
	/** Creates the FBO with vg on first use (so vg's drawMirror can show it), binds it (saving the previous binding
	 * and viewport), nvgBeginFrame on drawVg at kWidth x kHeight, ratio 1. drawVg is a context sharing vg's fonts and
	 * images, not vg itself: Rack measures text with vg between its frames, and a frame of ours (ratio 1, our state)
	 * would leave vg with the wrong metrics (Rack's APP->window->fbVg). False if the FBO can't be created (logged
	 * once). */
	bool begin(NVGcontext* vg, NVGcontext* drawVg);
	/** nvgEndFrame on drawVg; if rgba, glReadPixels the whole target into it (kWidth*kHeight*4, bottom row first, as
	 * toFrame expects); restores the binding and viewport. */
	void end(NVGcontext* drawVg, uint8_t* rgba);
	/** The last rendered image scaled into the rect (black if none). */
	void drawMirror(NVGcontext* vg, float x, float y, float w, float h) const;
	void destroy(); // the GL context is going away (onContextDestroy) or the widget is

  private:
	NVGLUframebuffer* fb_ = nullptr;
	bool failed_ = false;
	int fbo_ = 0, viewport_[4] = {};
	float clear_[4] = {};
};

/** Sends frames to the display over USB on its own thread (libusb). */
class Display {
  public:
	enum Status : int { Off, NoDevice, NoAccess, NoDriver, Busy, Error, Connected };
	~Display() { stop(); }
	void start(); // idempotent
	void stop();  // idempotent; joins the thread, releases and closes the device
	/** UI thread: copies a kWidth*kHeight*4 RGBA frame (bottom row first); the thread converts it (toFrame) and sends
	 * it. */
	void submit(const uint8_t* rgba);
	Status status() const { return (Status)status_.load(std::memory_order_relaxed); }
	uint32_t connections() const {
		return connections_.load(std::memory_order_relaxed);
	} // bumped on every open: the UI's replug signal

  private:
	void run();
	void setStatus(Status s);

	std::thread thread_;
	std::mutex mutex_; // guards next_, frame_, fresh_, quit_
	std::condition_variable cv_;
	std::vector<uint8_t> next_, frame_; // the submitted RGBA; the converted frame
	bool fresh_ = false, quit_ = false;
	std::atomic<int> status_{Off};
	std::atomic<uint32_t> connections_{0};
};

} // namespace push2
