# vipp — Makefile
# Builds a static, optimized binary with FTXUI baked in.

CXX      ?= g++
CXXFLAGS += -std=c++20 -Os -flto
STRIP    ?= strip

# --- Detect platform ---
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	LDFLAGS  += -framework Foundation -Wl,-dead_strip
	NPROC    := $(shell sysctl -n hw.ncpu)
else ifeq ($(UNAME_S),Linux)
	LDFLAGS  += -lpthread -Wl,--gc-sections
	NPROC    := $(shell nproc)
endif

# --- Paths ---
FTXUI_DIR   := .ftxui
FTXUI_BUILD := $(FTXUI_DIR)/build
FTXUI_STAMP := $(FTXUI_BUILD)/.built

# --- Targets ---
.PHONY: all static dynamic debug clean clean-all

all: vipp

vipp: vi.cpp $(FTXUI_STAMP)
	$(CXX) $(CXXFLAGS) \
		-I$(FTXUI_BUILD)/include -I$(FTXUI_DIR)/include \
		-o $@ vi.cpp \
		$(FTXUI_BUILD)/libftxui-component.a \
		$(FTXUI_BUILD)/libftxui-dom.a \
		$(FTXUI_BUILD)/libftxui-screen.a \
		$(LDFLAGS)
	$(STRIP) $@
	@echo "→ Built static vipp ($$(ls -lh vipp | awk '{print $$5}'))"

# Clone and build FTXUI static libraries (one-time)
$(FTXUI_STAMP):
	@if [ ! -d $(FTXUI_DIR) ]; then \
		echo "→ Cloning FTXUI v7.0.0 ..."; \
		git clone --depth 1 --branch v7.0.0 \
			https://github.com/ArthurSonzogni/FTXUI.git $(FTXUI_DIR); \
	fi
	cmake -B $(FTXUI_BUILD) -S $(FTXUI_DIR) \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_EXAMPLES=OFF \
		-DBUILD_TESTS=OFF \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(FTXUI_BUILD) -j$(NPROC)
	@touch $(FTXUI_STAMP)
	@echo "→ FTXUI static libraries built"

# Dynamic build (requires FTXUI installed system-wide via Homebrew/apt/etc)
dynamic: vi.cpp
	$(CXX) $(CXXFLAGS) \
		-o vipp vi.cpp \
		-lftxui-component -lftxui-dom -lftxui-screen $(LDFLAGS)
	$(STRIP) vipp
	@echo "→ Built dynamic vipp ($$(ls -lh vipp | awk '{print $$5}'))"

# Debug build (no optimisation, symbols kept)
debug: vi.cpp $(FTXUI_STAMP)
	$(CXX) -std=c++20 -g -O0 \
		-I$(FTXUI_BUILD)/include -I$(FTXUI_DIR)/include \
		-o vipp-debug vi.cpp \
		$(FTXUI_BUILD)/libftxui-component.a \
		$(FTXUI_BUILD)/libftxui-dom.a \
		$(FTXUI_BUILD)/libftxui-screen.a \
		$(LDFLAGS)
	@echo "→ Built debug vipp-debug"

clean:
	rm -f vipp vipp-debug

clean-all: clean
	rm -rf $(FTXUI_DIR)
