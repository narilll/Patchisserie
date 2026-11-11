#pragma once
// Minimal Web Audio graph: the subset superdough uses, spec-correct for that subset.
// Threading: the producer (JS) thread builds nodes and resources and posts POD commands; ONE render thread owns
// the graph and applies them at quantum boundaries. Nothing is allocated, freed or locked on the render thread:
// dead objects go back through a lock-free trash stack and are deleted by Graph::collect() on the producer.
// Pure logic: no Rack UI headers, testable on the host.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <dsp/ringbuffer.hpp>

namespace audio {

constexpr int kQuantum = 128;
// ponytail: mono/stereo only (destination.maxChannelCount = 2; superdough buses are all stereo). Raise it and add the
// spec's quad/5.1 mixing rules if surround output is ever needed.
constexpr int kMaxChannels = 2;

/** Context time -> first sample frame at or after it. The epsilon absorbs JS rounding of `frame / sampleRate`. */
inline int64_t timeToFrame(double t, double sampleRate) {
	return (int64_t)std::ceil(t * sampleRate - 1e-3);
}

class Graph;
struct Node;
struct Edge;

struct Disposable {
	Disposable* nextTrash = nullptr;
	virtual ~Disposable() = default;
};

/** Immutable data handed to the render thread (buffers, curves, wavetables, IR kernels). Refcounted: whoever drops
the last reference frees it, the render thread via Graph::drop() (deferred to the producer). */
struct Resource : Disposable {
	std::atomic<int> refs{1};
	template <class T> static T* retain(T* r) {
		if (r)
			r->refs.fetch_add(1, std::memory_order_relaxed);
		return r;
	}
};
/** Producer-side release. */
void release(Resource* r);

/** One render quantum of up to kMaxChannels. Invariant: silent => data is all zeros. */
struct Bus {
	int channels = 1;
	bool silent = true;
	alignas(16) float data[kMaxChannels][kQuantum] = {};
	/** Inactive nodes output a single channel of silence (spec "actively processing"). */
	void silence() {
		if (!silent)
			std::memset(data, 0, sizeof data);
		silent = true;
		channels = 1;
	}
	/** Caller writes all kQuantum frames of `ch` channels. */
	void begin(int ch) {
		channels = ch;
		silent = false;
	}
};

class AudioParam {
  public:
	enum Kind : uint8_t { Set, Linear, Exp };
	static constexpr int kMaxEvents = 64;

	AudioParam(Node* owner, float defaultValue, float minValue, float maxValue, bool aRate);

	Node* const owner;
	const float defaultValue, minValue, maxValue;
	const bool aRate;
	/** [[current value]]: intrinsic value at the start of the last rendered quantum. Producer may set it before
	the owner node is posted (constructor options); afterwards only through events. */
	double value;
	/** True when values[0] holds the whole quantum (only values[0] is written then). */
	bool constant = true;
	alignas(16) float values[kQuantum] = {};
	Edge* edges = nullptr;

	// Render thread.
	bool insert(Kind kind, double v, double time, int64_t nowFrame, double sampleRate);
	void cancel(double time);
	void compute(int64_t f0, double sampleRate, uint64_t quantum);
	float at(int i) const { return constant ? values[0] : values[i]; }

  private:
	struct Event {
		double time, value;
		int64_t frame;
		Kind kind;
	};
	Event ev[kMaxEvents];
	int n = 0;
	double intrinsic(int64_t frame, int next, double sampleRate) const;
};

enum class CountMode : uint8_t { Max, ClampedMax, Explicit };
enum class Interpretation : uint8_t { Speakers, Discrete };

/** SetAttr ids. Value in Command::a (enums as numbers), objects in Command::res. */
enum Attr : int {
	AttrChannelCount,
	AttrChannelCountMode,
	AttrChannelInterpretation,
	AttrType,
	AttrLoop,
	AttrLoopStart,
	AttrLoopEnd,
	AttrBuffer,
	AttrCurve,
	AttrOversample,
	AttrPeriodicWave,
	AttrFftSize,
};

struct Edge {
	Node* src;
	int out;
	Node* dst;         // owner of the input or param
	int in;            // node input index, -1 for a param
	AudioParam* param; // param target or nullptr
	Edge *prevIn, *nextIn, *prevOut, *nextOut;
};

struct Input {
	Edge* edges = nullptr;
	Bus bus;
};

struct DelayNode;

struct Node : Disposable {
	Node(double sampleRate, int numInputs, int numOutputs)
		: sampleRate(sampleRate), inputs(numInputs), outputs(numOutputs) {}

	uint64_t id = 0; // producer's handle, reported back in events
	const double sampleRate;
	std::vector<Input> inputs;
	std::vector<Bus> outputs;
	std::vector<AudioParam*> params; // members of subclasses, registered by AudioParam's constructor
	int channelCount = 2;
	CountMode countMode = CountMode::Max;
	Interpretation interpretation = Interpretation::Speakers;

	/** Inputs are mixed and params computed; write outputs (call Bus::silence() for silence). */
	virtual void process(int64_t f0) = 0;
	/** After JS dispose: true when the node can no longer produce sound. Default: no inputs and silent or unheard. */
	virtual bool reclaimable() const;
	virtual void setAttr(int attr, double v, Resource* r);
	virtual void start(double /*when*/, double /*offset*/, double /*duration*/) {}
	virtual void stop(double /*when*/) {}
	virtual DelayNode* asDelay() { return nullptr; }

	// Render-thread bookkeeping, owned by Graph.
	Graph* graph = nullptr;
	Node *prev = nullptr, *next = nullptr;
	Edge* outEdges = nullptr;
	uint64_t visited = 0, dfsMark = 0;
	bool inProgress = false, disposed = false, inCycle = false;
};

struct Command {
	enum Type : uint8_t { Create, Dispose, Connect, Disconnect, ParamEvent, ParamCancel, SetAttr, Start, Stop };
	Type type;
	AudioParam::Kind kind = AudioParam::Set;
	int out = 0, in = 0, attr = 0; // out/in = -1: any (Disconnect)
	Node* node = nullptr;          // subject; the source for Connect/Disconnect
	Node* dst = nullptr;
	AudioParam* param = nullptr;
	double a = 0, b = 0, c = 0; // value/time, start(when, offset, duration), attr value
	Resource* res = nullptr;    // ownership of one reference passes with the command
};

struct Event {
	enum Type : uint8_t { Ended, Error } type;
	uint64_t id; // node id
	int code;    // Error: ErrorCode
};
enum ErrorCode { EdgePoolFull = 1, ParamEventsFull };

/** Graph objects are large (inline queues): allocate on the heap. */
class Graph {
  public:
	explicit Graph(double sampleRate, int64_t startFrame = 0, int maxEdges = 1 << 16);
	~Graph();
	Graph(const Graph&) = delete;
	Graph& operator=(const Graph&) = delete;

	// --- Producer thread ---
	Node* destination() const { return dest_; }
	double sampleRate() const { return sr_; }
	double currentTime() const { return (double)frame_.load() / sr_; }
	/** Live nodes on the render side, destination included. */
	int nodeCount() const { return nodeCount_.load(); }
	/** False when the command queue is full (nothing posted). */
	bool post(const Command& c);
	bool create(Node* n) { return post({Command::Create, AudioParam::Set, 0, 0, 0, n}); }
	bool dispose(Node* n) { return post({Command::Dispose, AudioParam::Set, 0, 0, 0, n}); }
	bool connect(Node* src, Node* dst, int out = 0, int in = 0) {
		return post({Command::Connect, AudioParam::Set, out, in, 0, src, dst});
	}
	bool connect(Node* src, AudioParam* p, int out = 0) {
		return post({Command::Connect, AudioParam::Set, out, -1, 0, src, nullptr, p});
	}
	/** Spec variants: disconnect(), disconnect(out), disconnect(dst[, out[, in]]). */
	bool disconnect(Node* src, Node* dst = nullptr, int out = -1, int in = -1) {
		return post({Command::Disconnect, AudioParam::Set, out, in, 0, src, dst});
	}
	/** disconnect(param[, out]). */
	bool disconnect(Node* src, AudioParam* p, int out = -1) {
		return post({Command::Disconnect, AudioParam::Set, out, -1, 0, src, nullptr, p});
	}
	bool setValueAtTime(AudioParam* p, double v, double t) { return paramEvent(p, AudioParam::Set, v, t); }
	bool linearRampToValueAtTime(AudioParam* p, double v, double t) { return paramEvent(p, AudioParam::Linear, v, t); }
	bool exponentialRampToValueAtTime(AudioParam* p, double v, double t) {
		return paramEvent(p, AudioParam::Exp, v, t);
	}
	bool cancelScheduledValues(AudioParam* p, double t) {
		return post({Command::ParamCancel, AudioParam::Set, 0, 0, 0, nullptr, nullptr, p, 0, t});
	}
	bool setAttr(Node* n, int attr, double v, Resource* r = nullptr) {
		return post({Command::SetAttr, AudioParam::Set, 0, 0, attr, n, nullptr, nullptr, 0, 0, v, r});
	}
	/** duration < 0: play to the end. */
	bool start(Node* n, double when = 0, double offset = 0, double duration = -1) {
		return post({Command::Start, AudioParam::Set, 0, 0, 0, n, nullptr, nullptr, when, offset, duration});
	}
	bool stop(Node* n, double when = 0) {
		return post({Command::Stop, AudioParam::Set, 0, 0, 0, n, nullptr, nullptr, when});
	}
	/** ponytail: events are dropped if 16k pile up unpolled; poll every JS tick. */
	bool pollEvent(Event& e);
	/** Frees everything the render thread retired. Call regularly. */
	void collect();

	// --- Render thread ---
	/** Any frame count; partial quanta are buffered so the timeline is identical for any chunking. Null outputs
	render and drop (underflow policy: the audio thread's missing frames are rendered, never skipped). */
	void render(float* left, float* right, int frames);
	void discard(int frames) { render(nullptr, nullptr, frames); }

	// --- Used by nodes on the render thread ---
	void drop(Resource* r);
	void emit(Event e);

  private:
	bool paramEvent(AudioParam* p, AudioParam::Kind k, double v, double t) {
		return post({Command::ParamEvent, k, 0, 0, 0, nullptr, nullptr, p, v, t});
	}
	void apply(const Command& c);
	void renderQuantum();
	void pull(Node* n);
	void mix(Input& in, const Node& n);
	void markCycles();
	bool reaches(Node* from, Node* target, uint64_t mark);
	void link(Node* src, int out, Node* dst, int in, AudioParam* p);
	void unlink(Edge* e);
	void remove(Node* n);
	void trash(Disposable* d);

	const double sr_;
	int64_t f0_;                 // render thread: first frame of the next quantum
	std::atomic<int64_t> frame_; // published copy of f0_
	std::atomic<int> nodeCount_{0};
	uint64_t quantum_ = 0, dfsMark_ = 0;
	bool topologyDirty_ = true;
	Node* head_ = nullptr;
	Node* dest_;
	int pos_ = kQuantum; // read position in the last rendered quantum
	std::vector<Edge> edgePool_;
	Edge* freeEdges_ = nullptr;
	std::atomic<Disposable*> trash_{nullptr};
	rack::dsp::RingBuffer<Command, 1 << 15> commands_;
	rack::dsp::RingBuffer<Event, 1 << 14> events_;
};

} // namespace audio
