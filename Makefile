RACK_DIR ?= ../Rack-SDK

FLAGS += -Isrc
# compile.mk sets -std=c++11 after our CXXFLAGS; EXTRA_CXXFLAGS is appended last.
EXTRA_CXXFLAGS += -std=c++17

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += LICENSE res

include $(RACK_DIR)/plugin.mk

# Host-built unit tests: pure logic only, no Rack UI, sanitizers on.
TEST_SOURCES := $(wildcard test/test_*.cpp)
TEST_FLAGS := -std=c++17 -g -O1 -Wall -Wextra -Isrc -Itest -I$(RACK_DIR)/include

build/test/run: test/main.cpp test/check.hpp $(TEST_SOURCES) $(wildcard src/*.hpp src/*/*.hpp)
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined -o $@ test/main.cpp $(TEST_SOURCES)

test: build/test/run
	build/test/run

.PHONY: test

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
