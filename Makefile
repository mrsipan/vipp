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
.PHONY: all debug clean clean-all format format-check test

all: vipp

OBJS = motions.o

vipp: vi.cpp $(OBJS) $(FTXUI_STAMP)
	$(CXX) $(CXXFLAGS) \
		-I$(FTXUI_BUILD)/include -I$(FTXUI_DIR)/include \
		-o $@ vi.cpp $(OBJS) \
		$(FTXUI_BUILD)/libftxui-component.a \
		$(FTXUI_BUILD)/libftxui-dom.a \
		$(FTXUI_BUILD)/libftxui-screen.a \
		$(LDFLAGS)
	$(STRIP) $@
	@echo "→ Built static vipp ($$(ls -lh vipp | awk '{print $$5}'))"

motions.o: motions.cpp motions.h
	$(CXX) $(CXXFLAGS) -c motions.cpp -o $@

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
	rm -f vipp vipp-debug motions.o test_motions test_motions.o

clean-all: clean
	rm -rf $(FTXUI_DIR)

# Format source code with clang-format
format:
	@which clang-format >/dev/null 2>&1 || { echo "clang-format not found. Install with: apt install clang-format / brew install clang-format"; exit 1; }
	clang-format -i vi.cpp
	@echo "→ Formatted vi.cpp"

# Check formatting without modifying (useful in CI)
format-check:
	@which clang-format >/dev/null 2>&1 || { echo "clang-format not found. Install with: apt install clang-format / brew install clang-format"; exit 1; }
	@clang-format --dry-run --Werror vi.cpp && echo "✓ Formatting OK" || { echo "✗ Formatting issues found — run 'make format' to fix"; exit 1; }

# Run tests
test: test_motions
	./test_motions

test_motions: tests/test_motions.cpp motions.o motions.h
	$(CXX) -std=c++20 -g -O0 -o $@ tests/test_motions.cpp motions.o
	@echo "→ Built test_motions"
