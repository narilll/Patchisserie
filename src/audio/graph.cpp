#include "audio/graph.hpp"

#include <algorithm>

#include "audio/nodes.hpp"

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace audio {

#if defined(__aarch64__)
DenormalsOff::DenormalsOff() {
	asm volatile("mrs %0, fpcr" : "=r"(saved));
	asm volatile("msr fpcr, %0" ::"r"(saved | (UINT64_C(1) << 24))); // FZ: flush denormal inputs and outputs
}
DenormalsOff::~DenormalsOff() {
	asm volatile("msr fpcr, %0" ::"r"(saved));
}
#elif defined(__x86_64__) || defined(__i386__)
DenormalsOff::DenormalsOff() : saved(_mm_getcsr()) {
	_mm_setcsr((unsigned)saved | 0x8040); // FTZ | DAZ
}
DenormalsOff::~DenormalsOff() {
	_mm_setcsr((unsigned)saved);
}
#else
DenormalsOff::DenormalsOff() {}
DenormalsOff::~DenormalsOff() {}
#endif

void release(Resource* r) {
	if (r && r->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
		delete r;
}

// --- AudioParam ---

AudioParam::AudioParam(Node* owner, float defaultValue, float minValue, float maxValue, bool aRate)
	: owner(owner), defaultValue(defaultValue), minValue(minValue), maxValue(maxValue), aRate(aRate),
	  value(defaultValue) {
	owner->params.push_back(this);
}

bool AudioParam::insert(Kind kind, double v, double time, int64_t nowFrame, double sampleRate) {
	int pos = n;
	while (pos > 0 && ev[pos - 1].time > time)
		pos--;
	// Spec: a ramp with no preceding event behaves as if setValueAtTime(value, currentTime) came first; one that
	// already ended by then (T1 <= T0) holds V1 from T1.
	if (kind != Set && pos == 0 && time <= (double)nowFrame / sampleRate)
		kind = Set;
	if (kind != Set && pos == 0) {
		if (!insert(Set, value, (double)nowFrame / sampleRate, nowFrame, sampleRate))
			return false;
		return insert(kind, v, time, nowFrame, sampleRate);
	}
	if (n == kMaxEvents)
		return false;
	std::memmove(&ev[pos + 1], &ev[pos], (n - pos) * sizeof(Event));
	ev[pos] = {time, v, timeToFrame(time, sampleRate), kind};
	n++;
	return true;
}

void AudioParam::cancel(double time) {
	int i = 0;
	while (i < n && ev[i].time < time)
		i++;
	n = i;
}

// `next` = index of the first event after `frame`; ev[next - 1] is the one in effect.
double AudioParam::intrinsic(int64_t frame, int next, double sampleRate) const {
	if (next == 0)
		return value; // no event yet, or all removed: hold
	const Event& e0 = ev[next - 1];
	if (next == n || ev[next].kind == Set)
		return e0.value;
	const Event& e1 = ev[next];
	double x = std::max(0.0, ((double)frame / sampleRate - e0.time) / (e1.time - e0.time));
	if (e1.kind == Linear)
		return e0.value + (e1.value - e0.value) * x;
	if (e0.value * e1.value <= 0)
		return e0.value; // opposite signs or zero: hold V0 (spec)
	return e0.value * std::pow(e1.value / e0.value, x);
}

void AudioParam::compute(int64_t f0, double sampleRate, uint64_t quantum) {
	// Drop events wholly in the past, keeping the one in effect as the next ramp's start point.
	int keep = 0;
	while (keep + 1 < n && ev[keep + 1].frame <= f0)
		keep++;
	if (keep) {
		std::memmove(ev, ev + keep, (n - keep) * sizeof(Event));
		n -= keep;
	}
	int next = 0;
	while (next < n && ev[next].frame <= f0)
		next++;
	bool steady = next == n || (ev[next].kind == Set && ev[next].frame >= f0 + kQuantum);
	if (!aRate || steady) {
		values[0] = (float)intrinsic(f0, next, sampleRate);
		constant = true;
	} else {
		for (int i = 0; i < kQuantum; i++) {
			while (next < n && ev[next].frame <= f0 + i)
				next++;
			values[i] = (float)intrinsic(f0 + i, next, sampleRate);
		}
		constant = false;
	}
	value = values[0];

	// Connected nodes are summed into the value after down-mixing to mono (speakers).
	bool connected = false;
	for (Edge* e = edges; e; e = e->nextIn) {
		const Bus& b = e->src->outputs[e->out];
		if (e->src->visited != quantum || b.silent)
			continue;
		if (constant) {
			std::fill(values + 1, values + kQuantum, values[0]);
			constant = false;
		}
		connected = true;
		int len = aRate ? kQuantum : 1; // k-rate samples the input at the first frame
		if (b.channels == 1)
			for (int i = 0; i < len; i++)
				values[i] += b.data[0][i];
		else
			for (int i = 0; i < len; i++)
				values[i] += 0.5f * (b.data[0][i] + b.data[1][i]);
	}
	if (connected && !aRate)
		constant = true;
	int len = constant ? 1 : kQuantum;
	for (int i = 0; i < len; i++) {
		float v = values[i];
		values[i] = std::isnan(v) ? defaultValue : std::min(std::max(v, minValue), maxValue);
	}
}

// --- Node defaults ---

bool Node::reclaimable() const {
	if (!outEdges)
		return true; // unheard: whatever still feeds it can't reach the output (Chrome collects it too)
	for (const Input& in : inputs)
		if (in.edges)
			return false;
	for (const Bus& b : outputs)
		if (!b.silent)
			return false;
	return true;
}

void Node::setAttr(int attr, double v, Resource* r) {
	if (attr == AttrChannelCount)
		channelCount = std::min(std::max((int)v, 1), kMaxChannels);
	else if (attr == AttrChannelCountMode)
		countMode = (CountMode)(int)v;
	else if (attr == AttrChannelInterpretation)
		interpretation = (Interpretation)(int)v;
	if (r)
		graph->drop(r); // not an object attribute of this node
}

// --- Graph ---

Graph::Graph(double sampleRate, int64_t startFrame, int maxEdges)
	: sr_(sampleRate), f0_(startFrame), frame_(startFrame), edgePool_(maxEdges) {
	for (Edge& e : edgePool_) {
		e.nextOut = freeEdges_;
		freeEdges_ = &e;
	}
	dest_ = new DestinationNode(sampleRate);
	dest_->graph = this;
	head_ = dest_;
	nodeCount_ = 1;
}

Graph::~Graph() {
	while (!commands_.empty()) {
		Command c = commands_.shift();
		if (c.type == Command::Create)
			delete c.node;
		release(c.res);
	}
	while (!events_.empty())
		release(events_.shift().data);
	for (Node* n = head_; n;) {
		Node* next = n->next;
		delete n;
		n = next;
	}
	collect();
}

bool Graph::post(const Command& c) {
	if (commands_.full())
		return false;
	commands_.push(c);
	return true;
}

bool Graph::pollEvent(Event& e) {
	if (events_.empty())
		return false;
	e = events_.shift();
	return true;
}

void Graph::collect() {
	for (Disposable* d = trash_.exchange(nullptr, std::memory_order_acquire); d;) {
		Disposable* next = d->nextTrash;
		delete d;
		d = next;
	}
}

void Graph::trash(Disposable* d) {
	d->nextTrash = trash_.load(std::memory_order_relaxed);
	while (!trash_.compare_exchange_weak(d->nextTrash, d, std::memory_order_release, std::memory_order_relaxed)) {
	}
}

void Graph::drop(Resource* r) {
	if (r && r->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
		trash(r);
}

void Graph::emit(Event e) {
	if (!events_.full())
		events_.push(e);
	else
		drop(e.data);
}

void Graph::render(float* left, float* right, int frames) {
	const Bus& out = dest_->inputs[0].bus; // explicit stereo
	for (int done = 0; done < frames;) {
		if (pos_ == kQuantum) {
			renderQuantum();
			pos_ = 0;
		}
		int len = std::min(frames - done, kQuantum - pos_);
		if (left)
			std::memcpy(left + done, out.data[0] + pos_, len * sizeof(float));
		if (right)
			std::memcpy(right + done, out.data[1] + pos_, len * sizeof(float));
		pos_ += len;
		done += len;
	}
}

void Graph::renderQuantum() {
	while (!commands_.empty())
		apply(commands_.shift());
	if (topologyDirty_)
		markCycles();
	quantum_++;
	// Spec: every live node is processed each quantum (sources advance even when unheard), in pull order.
	for (Node* n = head_; n; n = n->next)
		pull(n);
	// DelayWriter half of cycle breakers: all their inputs are rendered by now.
	for (Node* n = head_; n; n = n->next) {
		if (!n->inCycle)
			continue;
		mix(n->inputs[0], *n); // its inputs were all pulled by the loop above
		n->asDelay()->write(f0_);
	}
	// Boundary guard: a NaN/Inf that slipped through never reaches the audio output.
	Bus& out = dest_->inputs[0].bus;
	for (auto& ch : out.data)
		for (float& v : ch)
			if (!std::isfinite(v))
				v = 0;
	for (Node* n = head_; n;) {
		Node* next = n->next;
		if (n->disposed && n->reclaimable())
			remove(n);
		n = next;
	}
	f0_ += kQuantum;
	frame_.store(f0_);
}

void Graph::pull(Node* n) {
	if (n->visited == quantum_ || n->inProgress)
		return; // in progress = cycle without a delay: that edge reads as silence (muted)
	n->inProgress = true;
	for (AudioParam* p : n->params) {
		for (Edge* e = p->edges; e; e = e->nextIn)
			pull(e->src);
		p->compute(f0_, sr_, quantum_);
	}
	if (n->inCycle) {
		n->asDelay()->read(f0_); // DelayReader: a source, its inputs are written after everything else
	} else {
		for (Input& in : n->inputs) {
			for (Edge* e = in.edges; e; e = e->nextIn)
				pull(e->src);
			mix(in, *n);
		}
		n->process(f0_);
	}
	n->inProgress = false;
	n->visited = quantum_;
}

// Spec "Channel up-mixing and down-mixing" for mono <-> stereo.
void Graph::mix(Input& in, const Node& n) {
	int maxChannels = 1;
	bool active = false;
	for (Edge* e = in.edges; e; e = e->nextIn) {
		const Bus& b = e->src->outputs[e->out];
		if (e->src->visited == quantum_ && !b.silent) {
			active = true;
			maxChannels = std::max(maxChannels, b.channels);
		}
	}
	int ch = n.countMode == CountMode::Explicit ? n.channelCount : maxChannels;
	if (n.countMode == CountMode::ClampedMax)
		ch = std::min(ch, n.channelCount);
	Bus& dst = in.bus;
	dst.silence();
	dst.channels = ch;
	if (!active)
		return;
	dst.silent = false;
	bool speakers = n.interpretation == Interpretation::Speakers;
	for (Edge* e = in.edges; e; e = e->nextIn) {
		const Bus& b = e->src->outputs[e->out];
		if (e->src->visited != quantum_ || b.silent)
			continue;
		if (b.channels == ch) {
			for (int c = 0; c < ch; c++)
				for (int i = 0; i < kQuantum; i++)
					dst.data[c][i] += b.data[c][i];
		} else if (ch == 2) { // up-mix mono
			for (int i = 0; i < kQuantum; i++)
				dst.data[0][i] += b.data[0][i];
			if (speakers)
				for (int i = 0; i < kQuantum; i++)
					dst.data[1][i] += b.data[0][i];
		} else { // down-mix stereo
			for (int i = 0; i < kQuantum; i++)
				dst.data[0][i] += speakers ? 0.5f * (b.data[0][i] + b.data[1][i]) : b.data[0][i];
		}
	}
}

// Cycle breakers (spec rendering loop): a DelayNode that can reach itself is split into writer and reader.
void Graph::markCycles() {
	for (Node* n = head_; n; n = n->next)
		if (n->asDelay())
			n->inCycle = reaches(n, n, ++dfsMark_);
	topologyDirty_ = false;
}

bool Graph::reaches(Node* from, Node* target, uint64_t mark) {
	for (Edge* e = from->outEdges; e; e = e->nextOut) {
		if (e->dst == target)
			return true;
		if (e->dst->dfsMark == mark)
			continue;
		e->dst->dfsMark = mark;
		if (reaches(e->dst, target, mark))
			return true;
	}
	return false;
}

void Graph::link(Node* src, int out, Node* dst, int in, AudioParam* p) {
	if (out < 0 || out >= (int)src->outputs.size() || (!p && (in < 0 || in >= (int)dst->inputs.size())))
		return;
	Edge*& head = p ? p->edges : dst->inputs[in].edges;
	for (Edge* e = head; e; e = e->nextIn)
		if (e->src == src && e->out == out)
			return; // spec: repeated connections with the same termini are ignored
	Edge* e = freeEdges_;
	if (!e) {
		emit({Event::Error, src->id, EdgePoolFull});
		return;
	}
	freeEdges_ = e->nextOut;
	*e = {src, out, p ? p->owner : dst, p ? -1 : in, p, nullptr, head, nullptr, src->outEdges};
	if (head)
		head->prevIn = e;
	head = e;
	if (src->outEdges)
		src->outEdges->prevOut = e;
	src->outEdges = e;
	topologyDirty_ = true;
}

void Graph::unlink(Edge* e) {
	Edge*& head = e->param ? e->param->edges : e->dst->inputs[e->in].edges;
	(e->prevIn ? e->prevIn->nextIn : head) = e->nextIn;
	if (e->nextIn)
		e->nextIn->prevIn = e->prevIn;
	(e->prevOut ? e->prevOut->nextOut : e->src->outEdges) = e->nextOut;
	if (e->nextOut)
		e->nextOut->prevOut = e->prevOut;
	e->nextOut = freeEdges_;
	freeEdges_ = e;
	topologyDirty_ = true;
}

void Graph::remove(Node* n) {
	while (n->outEdges)
		unlink(n->outEdges);
	for (Input& in : n->inputs)
		while (in.edges)
			unlink(in.edges);
	for (AudioParam* p : n->params)
		while (p->edges)
			unlink(p->edges);
	(n->prev ? n->prev->next : head_) = n->next;
	if (n->next)
		n->next->prev = n->prev;
	nodeCount_--;
	n->retire();
	trash(n);
}

void Graph::apply(const Command& c) {
	Node* n = c.node;
	switch (c.type) {
	case Command::Create:
		n->graph = this;
		n->next = head_;
		head_->prev = n;
		head_ = n;
		nodeCount_++;
		topologyDirty_ = true;
		break;
	case Command::Dispose:
		n->disposed = true;
		break;
	case Command::Connect:
		link(n, c.out, c.dst, c.in, c.param);
		break;
	case Command::Disconnect:
		for (Edge* e = n->outEdges; e;) {
			Edge* next = e->nextOut;
			bool match = (c.out < 0 || e->out == c.out) &&
						 (c.param ? e->param == c.param
								  : !c.dst || (!e->param && e->dst == c.dst && (c.in < 0 || e->in == c.in)));
			if (match)
				unlink(e);
			e = next;
		}
		break;
	case Command::ParamEvent:
		if (!c.param->insert(c.kind, c.a, c.b, f0_, sr_))
			emit({Event::Error, c.param->owner->id, ParamEventsFull});
		break;
	case Command::ParamCancel:
		c.param->cancel(c.b);
		break;
	case Command::SetAttr:
		n->setAttr(c.attr, c.c, c.res);
		break;
	case Command::Start:
		n->start(c.a, c.b, c.c);
		break;
	case Command::Stop:
		n->stop(c.a);
		break;
	}
}

} // namespace audio
