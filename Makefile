RACK_DIR ?= ../Rack-SDK

FLAGS += -Isrc
# compile.mk sets -std=c++11 after our CXXFLAGS; EXTRA_CXXFLAGS is appended last.
EXTRA_CXXFLAGS += -std=c++17

SOURCES += $(wildcard src/*.cpp)
AUDIO_SOURCES := $(wildcard src/audio/*.cpp)
SOURCES += $(AUDIO_SOURCES)

DISTRIBUTABLES += LICENSE res

include $(RACK_DIR)/plugin.mk

# Host-built unit tests: pure logic only, no Rack UI, sanitizers on.
TEST_SOURCES := $(wildcard test/test_*.cpp)
TEST_FLAGS := -std=c++17 -g -O1 -Wall -Wextra -Isrc -Itest -I$(RACK_DIR)/include
# Web Audio graph: linked into the tests; dep/include for simde, pulled in by Rack's dsp/ringbuffer.hpp.
TEST_SOURCES += $(AUDIO_SOURCES)
TEST_FLAGS += -I$(RACK_DIR)/dep/include

build/test/run: test/main.cpp test/check.hpp $(TEST_SOURCES) $(wildcard src/*.hpp src/*/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined -o $@ test/main.cpp $(TEST_SOURCES)

test: build/test/run
	build/test/run

.PHONY: test

# Graph tests (command/event queues, producer thread vs render thread) under ThreadSanitizer.
build/test/tsan: test/main.cpp test/check.hpp test/test_graph.cpp $(AUDIO_SOURCES) $(wildcard src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=thread -o $@ test/main.cpp test/test_graph.cpp $(AUDIO_SOURCES)

tsan: build/test/tsan
	build/test/tsan

# Rough CPU benchmark with plugin-like optimization (prints, asserts nothing).
build/test/bench: test/bench_audio.cpp $(AUDIO_SOURCES) $(wildcard src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -O3 -funsafe-math-optimizations -Wall -Wextra -Isrc -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include -o $@ test/bench_audio.cpp $(AUDIO_SOURCES)

bench: build/test/bench
	build/test/bench

.PHONY: tsan bench

# QuickJS CLI for headless engine tests. Vendor C: no warnings, strict IEEE math.
QJS_DIR := vendor/quickjs-ng
QJS_SOURCES := $(addprefix $(QJS_DIR)/,quickjs.c libregexp.c libunicode.c dtoa.c quickjs-libc.c qjs.c gen/repl.c gen/standalone.c)

build/qjs: $(QJS_SOURCES)
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -w -fno-unsafe-math-optimizations -funsigned-char -D_GNU_SOURCE -isystem $(QJS_DIR) -o $@ $(QJS_SOURCES) -lm -ldl -lpthread

# JS tests: host + upstream tune regression in Node, then the same engine under QuickJS.
test-js: build/qjs
	node --check js/host.mjs
	node --test test/*.test.mjs
	build/qjs test/qjs_smoke.js
	build/qjs test/qjs_tunes.mjs

.PHONY: test-js

# Audio regression (plan G1/G3) of build/render/<id>.wav against the Chrome references in test/render/reference.json;
# patterns without a render are skipped. References: scripts/fetch-test-samples && node scripts/render-reference.mjs
test-render:
	node --test test/render.test.mjs

.PHONY: test-render
