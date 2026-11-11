#pragma once
// Shared rig for test_graph.cpp / test_nodes.cpp: one graph, driven single-threaded (producer == render thread).
#include <memory>

#include "audio/nodes.hpp"

namespace audiotest {

using namespace audio;

constexpr double kSr = 48000;

struct Rig {
	std::unique_ptr<Graph> g;
	std::vector<float> L, R;
	explicit Rig(int64_t startFrame = 0) : g(new Graph(kSr, startFrame)) {}
	template <class T, class... A> T* make(A... args) {
		T* n = new T(kSr, args...);
		g->create(n);
		return n;
	}
	Node* dest() const { return g->destination(); }
	/** Renders `frames` more frames into L/R (replacing their contents). */
	void render(int frames) {
		L.assign(frames, 0.f);
		R.assign(frames, 0.f);
		g->render(L.data(), R.data(), frames);
		g->collect();
	}
	/** Context time of frame `f` relative to the graph's start frame. */
	double at(int64_t f, int64_t start = 0) const { return (double)(start + f) / kSr; }
	ConstantSourceNode* constant(float v) {
		auto* c = make<ConstantSourceNode>();
		c->offset.value = v;
		g->start(c);
		return c;
	}
	AudioBufferSourceNode* play(AudioBufferData* b, double when = 0, double offset = 0, double duration = -1) {
		auto* s = make<AudioBufferSourceNode>();
		g->setAttr(s, AttrBuffer, 0, b);
		g->start(s, when, offset, duration);
		return s;
	}
};

inline AudioBufferData* buffer(std::vector<float> mono, double sr = kSr) {
	auto* b = new AudioBufferData(1, (int64_t)mono.size(), sr);
	b->data[0] = std::move(mono);
	return b;
}

inline AudioBufferData* click(int len = 1) {
	std::vector<float> d(len, 0.f);
	d[0] = 1;
	return buffer(d);
}

} // namespace audiotest
