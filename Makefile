RACK_DIR ?= ../Rack-SDK

FLAGS += -Isrc
# compile.mk sets -std=c++11 after our CXXFLAGS; EXTRA_CXXFLAGS is appended last.
EXTRA_CXXFLAGS += -std=c++17

SOURCES += $(wildcard src/*.cpp) src/editor/view.cpp
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

# hydra.cpp calls OpenGL 1.x directly: on Windows those come from opengl32.dll (not libRack).
ifdef ARCH_WIN
LDFLAGS += -lopengl32
endif

# Host-built unit tests: pure logic only, no Rack UI, sanitizers on.
TEST_SOURCES := $(wildcard test/test_*.cpp)
TEST_FLAGS := -std=c++17 -g -O1 -Wall -Wextra -Isrc -Itest -I$(RACK_DIR)/include -isystem $(QJS_DIR) -isystem vendor/dr_libs
# Web Audio graph + QuickJS binding, Filo's engine and downloads: linked into the tests; dep/include for simde (pulled in
# by Rack's dsp/ringbuffer.hpp) and ghc::filesystem.
ENGINE_SOURCES := src/engine.cpp src/fetch.cpp
TEST_SOURCES += $(AUDIO_SOURCES) $(ENGINE_SOURCES)
TEST_FLAGS += -I$(RACK_DIR)/dep/include
# MinGW: M_PI needs _USE_MATH_DEFINES and Rack's simd needs SSE 4.2 (SIMDe's fallback clashes with the intrinsics
# <windows.h> declares), as in Rack's compile.mk; no sanitizer runtimes: tests run without them.
ifdef ARCH_WIN
TEST_FLAGS += -D_USE_MATH_DEFINES -march=nehalem
else
ASAN := -fsanitize=address
ASAN_UBSAN := -fsanitize=address,undefined
endif

# Vendor C and the embedded webaudio.js for host builds, per flavour like the code that calls them: asan (tests,
# render), tsan, release (benchmarks, plugin-like optimization).
host_objects = $(patsubst %,build/test/$(1)/%.o,$(VENDOR_SOURCES)) build/test/$(1)/webaudio.js.o
HOST_OBJECTS := $(call host_objects,asan)
build/test/asan/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -g -O2 $(VENDOR_FLAGS) $(ASAN) -c -o $@ $<
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
	$(CXX) $(TEST_FLAGS) $(ASAN_UBSAN) -o $@ test/main.cpp $(TEST_SOURCES) $(HOST_OBJECTS)

test: build/test/run
	build/test/run

.PHONY: test

# Graph, worklet, engine and download tests (command/event queues, the worklet runtime on the render thread vs the JS
# thread, Filo's JS / render / audio threads, the download worker) under ThreadSanitizer.
TSAN_TESTS := test/main.cpp test/test_graph.cpp test/test_worklets.cpp test/test_engine.cpp test/test_fetch.cpp
build/test/run-tsan: $(TSAN_TESTS) $(wildcard test/*.hpp) $(AUDIO_SOURCES) $(ENGINE_SOURCES) $(call host_objects,tsan) $(wildcard src/*.hpp src/audio/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=thread -o $@ $(TSAN_TESTS) $(AUDIO_SOURCES) $(ENGINE_SOURCES) $(call host_objects,tsan)

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
	$(CXX) $(TEST_FLAGS) $(ASAN_UBSAN) -o $@ test/render/render.cpp $(AUDIO_SOURCES) $(HOST_OBJECTS)

render: build/test/render
	build/test/render

.PHONY: render

# Icing's shaders and passes on this machine's GL driver (test/glcheck.cpp). Not part of `make test`: CI runners may
# have no GL. The plugin's own objects minus its Rack UI, linked to libRack like the plugin.
GLCHECK_OBJECTS := $(filter-out build/src/plugin.cpp.o build/src/editor/%,$(OBJECTS))
GLCHECK_LIBS := -L$(RACK_DIR) -lRack
ifdef ARCH_MAC
GLCHECK_LIBS += -framework OpenGL -lobjc
endif
ifdef ARCH_LIN
GLCHECK_LIBS += -lGL
endif
ifdef ARCH_WIN
GLCHECK_LIBS += -lopengl32
endif
build/glcheck: test/glcheck.cpp $(GLCHECK_OBJECTS)
	$(CXX) $(filter-out -MMD -MP,$(CXXFLAGS)) -o $@ test/glcheck.cpp $(GLCHECK_OBJECTS) $(GLCHECK_LIBS)

glcheck: build/glcheck
	DYLD_LIBRARY_PATH=$(RACK_DIR) LD_LIBRARY_PATH=$(RACK_DIR) build/glcheck

.PHONY: glcheck

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
