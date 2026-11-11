# MiniVectorDB build.
#
# Zero third-party dependencies: only a C++17 compiler is required.
# Works with GNU make on Linux (used inside Docker) and with mingw32-make on
# MSYS2/Windows. The only platform difference that leaks in is how a directory
# gets created, because mingw32-make runs recipes through cmd.exe.

ifeq ($(OS),Windows_NT)
  EXE   := .exe
  MKDIR  = if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
else
  EXE   :=
  MKDIR  = mkdir -p "$1"
endif

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc
LDFLAGS  ?=
BUILD    ?= build

BIN  := $(BUILD)/minivectordb$(EXE)
SRCS := $(wildcard src/*.cpp src/*/*.cpp)
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

all: $(BIN)

$(BIN): $(OBJS)
	@$(call MKDIR,$(BUILD))
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.cpp
	@$(call MKDIR,$(BUILD)/$(patsubst %/,%,$(dir $*)))
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILD)

-include $(DEPS)

.PHONY: all clean
