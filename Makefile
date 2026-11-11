RACK_DIR ?= ../Rack-SDK

FLAGS += -Isrc
# compile.mk sets -std=c++11 after our CXXFLAGS; EXTRA_CXXFLAGS is appended last.
EXTRA_CXXFLAGS += -std=c++17

SOURCES += $(wildcard src/*.cpp)
AUDIO_SOURCES := $(wildcard src/audio/*.cpp)
SOURCES += $(AUDIO_SOURCES)
# Web Audio classes for QuickJS, embedded as a C array (Rack's xxd rule): src_audio_webaudio_js[].
BINARIES += src/audio/webaudio.js

# Vendor C (quickjs-ng, dr_libs implementations): no warnings, strict IEEE math, headers as system headers.
QJS_DIR := vendor/quickjs-ng
VENDOR_SOURCES := $(addprefix $(QJS_DIR)/,quickjs.c libregexp.c libunicode.c dtoa.c) vendor/dr_libs.c
VENDOR_FLAGS := -w -fno-unsafe-math-optimizations -funsigned-char -D_GNU_SOURCE
SOURCES += $(VENDOR_SOURCES)
FLAGS += -isystem $(QJS_DIR) -isystem vendor/dr_libs
build/vendor/%.o: FLAGS += $(VENDOR_FLAGS)

DISTRIBUTABLES += LICENSE res

include $(RACK_DIR)/plugin.mk

# Host-built unit tests: pure logic only, no Rack UI, sanitizers on.
TEST_SOURCES := $(wildcard test/test_*.cpp)
TEST_FLAGS := -std=c++17 -g -O1 -Wall -Wextra -Isrc -Itest -I$(RACK_DIR)/include -isystem $(QJS_DIR) -isystem vendor/dr_libs
# Web Audio graph + QuickJS binding: linked into the tests; dep/include for simde, pulled in by Rack's dsp/ringbuffer.hpp.
TEST_SOURCES += $(AUDIO_SOURCES)
TEST_FLAGS += -I$(RACK_DIR)/dep/include

# Vendor C and the embedded webaudio.js for host builds, per flavour like the code that calls them: asan (tests,
# render), tsan, release (benchmarks, plugin-like optimization).
host_objects = $(patsubst %,build/test/$(1)/%.o,$(VENDOR_SOURCES)) build/test/$(1)/webaudio.js.o
HOST_OBJECTS := $(call host_objects,asan)
build/test/asan/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -g -O2 $(VENDOR_FLAGS) -fsanitize=address -c -o $@ $<
build/test/tsan/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -g -O2 $(VENDOR_FLAGS) -fsanitize=thread -c -o $@ $<
build/test/release/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O3 $(VENDOR_FLAGS) -c -o $@ $<
build/test/%/webaudio.js.o: src/audio/webaudio.js
	@mkdir -p $(@D)
	xxd -i $< | $(CC) -c -o $@ -xc -

build/test/run: test/main.cpp test/check.hpp $(TEST_SOURCES) $(HOST_OBJECTS) $(wildcard src/*.hpp src/*/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined -o $@ test/main.cpp $(TEST_SOURCES) $(HOST_OBJECTS)

test: build/test/run
	build/test/run

.PHONY: test

# Graph and worklet tests (command/event queues, the worklet runtime on the render thread vs the JS thread) under
# ThreadSanitizer.
TSAN_TESTS := test/main.cpp test/test_graph.cpp test/test_worklets.cpp
build/test/run-tsan: $(TSAN_TESTS) $(wildcard test/*.hpp) $(AUDIO_SOURCES) $(call host_objects,tsan) $(wildcard src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=thread -o $@ $(TSAN_TESTS) $(AUDIO_SOURCES) $(call host_objects,tsan)

tsan: build/test/run-tsan
	build/test/run-tsan

# CPU benchmarks with plugin-like optimization, no sanitizers: graph nodes and every worklet processor per voice
# (bench, prints only), the whole render set + all patterns stacked (render-bench, plan G2/G4: fails on a leak or
# a render thread above 25 % of one core).
RELEASE_FLAGS := -std=c++17 -O3 -funsafe-math-optimizations -Wall -Wextra -Isrc -Itest -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -isystem $(QJS_DIR) -isystem vendor/dr_libs
build/test/bench: test/bench_audio.cpp $(AUDIO_SOURCES) $(call host_objects,release) $(wildcard src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(RELEASE_FLAGS) -o $@ test/bench_audio.cpp $(AUDIO_SOURCES) $(call host_objects,release)

bench: build/test/bench
	build/test/bench

build/test/render-bench: test/render/render.cpp $(AUDIO_SOURCES) $(call host_objects,release) $(wildcard src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(RELEASE_FLAGS) -o $@ test/render/render.cpp $(AUDIO_SOURCES) $(call host_objects,release)

render-bench: build/test/render-bench
	build/test/render-bench --bench --loop 20

.PHONY: tsan bench render-bench

# Offline render harness (plan gates G1-G3): res/engine.js + superdough on an offline-driven graph, under ASan.
# Writes build/render/<id>.wav; `node --test test/render.test.mjs` compares them with the Chrome references.
build/test/render: test/render/render.cpp $(AUDIO_SOURCES) $(HOST_OBJECTS) $(wildcard src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined -o $@ test/render/render.cpp $(AUDIO_SOURCES) $(HOST_OBJECTS)

render: build/test/render
	build/test/render

.PHONY: render

# QuickJS CLI for headless engine tests. Vendor C: no warnings, strict IEEE math.
QJS_SOURCES := $(addprefix $(QJS_DIR)/,quickjs.c libregexp.c libunicode.c dtoa.c quickjs-libc.c qjs.c gen/repl.c gen/standalone.c)

build/qjs: $(QJS_SOURCES)
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -w -fno-unsafe-math-optimizations -funsigned-char -D_GNU_SOURCE -isystem $(QJS_DIR) -o $@ $(QJS_SOURCES) -lm -ldl -lpthread

# JS tests: host + upstream tune regression in Node, then the same engine under QuickJS.
test-js: build/qjs
	node --check js/host.mjs
	node --check src/audio/webaudio.js
	node --test test/*.test.mjs
	build/qjs test/qjs_smoke.js
	build/qjs test/qjs_tunes.mjs

.PHONY: test-js

# Audio regression (plan G1/G3) of build/render/<id>.wav against the Chrome references in test/render/reference.json;
# patterns without a render are skipped. References: scripts/fetch-test-samples && node scripts/render-reference.mjs
test-render:
	node --test test/render.test.mjs

.PHONY: test-render
