# Makefile — USB camera unplug watchdog
#
# Linux-only (libudev + ALSA). Build on the RK3588 board:
#   make            -> produces ./watchdog
#   make clean      -> remove build/ and the binary

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
LDLIBS   := -ludev -lasound -lpthread

TARGET := watchdog
SRC    := $(wildcard src/*.cpp)
OBJ    := $(patsubst src/%.cpp,build/%.o,$(SRC))
DEPS   := $(OBJ:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

clean:
	rm -rf build $(TARGET)
