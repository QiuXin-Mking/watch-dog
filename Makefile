# Makefile — USB camera unplug watchdog
#
# Linux-only (libudev + ALSA). Build on the RK3588 board:
#   make            -> produces ./watchdog
#   make clean      -> remove build/ and the binary
#
# Host unit tests compile only pure sources (no ALSA / udev headers):
#   make test

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
LDLIBS   := -ludev -lasound -lpthread

TARGET := watchdog
SRC    := $(wildcard src/*.cpp)
OBJ    := $(patsubst src/%.cpp,build/%.o,$(SRC))
DEPS   := $(OBJ:.o=.d)

TEST_BUILD := build/tests
TEST_CXXFLAGS := $(CXXFLAGS) -Isrc
# Pure sources: no <alsa/asoundlib.h>, no <libudev.h>.
PURE_SRC := src/json_lite.cpp src/capture_client.cpp src/phase_voice_policy.cpp \
            src/playback_arbiter.cpp src/sound_bank.cpp src/wav.cpp src/state.cpp
PURE_OBJ := $(patsubst src/%.cpp,$(TEST_BUILD)/%.o,$(PURE_SRC))
WATCHER_OBJ := $(TEST_BUILD)/phase_watcher.o

TESTS    := test_json test_phase_policy test_playback_arbiter test_phase_watcher
TEST_BIN := $(addprefix $(TEST_BUILD)/,$(TESTS))

.PHONY: all clean test $(TESTS)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(TEST_BUILD)/%.o: src/%.cpp
	@mkdir -p $(TEST_BUILD)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP -c -o $@ $<

$(TEST_BUILD)/%.o: tests/%.cpp
	@mkdir -p $(TEST_BUILD)
	$(CXX) $(TEST_CXXFLAGS) -MMD -MP -c -o $@ $<

$(TEST_BUILD)/test_json: $(TEST_BUILD)/test_json.o $(TEST_BUILD)/json_lite.o
	$(CXX) $(TEST_CXXFLAGS) -o $@ $^

$(TEST_BUILD)/test_phase_policy: $(TEST_BUILD)/test_phase_policy.o \
                                 $(TEST_BUILD)/phase_voice_policy.o \
                                 $(TEST_BUILD)/json_lite.o
	$(CXX) $(TEST_CXXFLAGS) -o $@ $^

$(TEST_BUILD)/test_playback_arbiter: $(TEST_BUILD)/test_playback_arbiter.o \
                                     $(TEST_BUILD)/playback_arbiter.o
	$(CXX) $(TEST_CXXFLAGS) -o $@ $^

$(TEST_BUILD)/test_phase_watcher: $(TEST_BUILD)/test_phase_watcher.o \
                                  $(WATCHER_OBJ) $(PURE_OBJ)
	$(CXX) $(TEST_CXXFLAGS) -o $@ $^

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "== $$t =="; $$t || exit 1; done

test_json: $(TEST_BUILD)/test_json
test_phase_policy: $(TEST_BUILD)/test_phase_policy
test_playback_arbiter: $(TEST_BUILD)/test_playback_arbiter
test_phase_watcher: $(TEST_BUILD)/test_phase_watcher

-include $(DEPS) $(TEST_BUILD)/*.d

clean:
	rm -rf build $(TARGET)
