# vipp — A Modern Vim Editor in C++20

Vipp is a feature-rich Vim implementation built in modern C++20 using [FTXUI](https://github.com/ArthurSonzogni/FTXUI) for the terminal UI.

## Build

### Prerequisites

- **C++20 compiler** (GCC 11+, Clang 14+, or Apple Clang 15+)
- **CMake 3.14+** (to build FTXUI from source)
- **Git** (to clone FTXUI)

### Option 1: Dynamic linking (macOS with Homebrew)

```sh
brew install ftxui
g++ -std=c++20 -I/opt/homebrew/include -L/opt/homebrew/lib \
    -o vipp vi.cpp -lftxui-component -lftxui-dom -lftxui-screen
```

### Option 2: Dynamic linking (Linux with system package manager)

```sh
# Debian/Ubuntu
sudo apt install libftxui-dev

# Fedora
sudo dnf install ftxui-devel

# Arch
sudo pacman -S ftxui

# Then compile:
g++ -std=c++20 -o vipp vi.cpp -lftxui-component -lftxui-dom -lftxui-screen
```

### Option 3: Static linking (no FTXUI runtime dependency — macOS & Linux)

This bakes FTXUI directly into the binary. The resulting `vipp` binary has **no** `libftxui*.so` or `libftxui*.dylib` dependency.

```sh
# 1. Clone and build FTXUI static libraries (one-time):
git clone --depth 1 --branch v7.0.0 https://github.com/ArthurSonzogni/FTXUI.git /tmp/ftxui-static
cmake -B /tmp/ftxui-static/build \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/ftxui-static/build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 2. Compile vipp statically:

# macOS:
g++ -std=c++20 -Os -flto \
    -I/tmp/ftxui-static/build/include -I/tmp/ftxui-static/include \
    -o vipp vi.cpp \
    /tmp/ftxui-static/build/libftxui-component.a \
    /tmp/ftxui-static/build/libftxui-dom.a \
    /tmp/ftxui-static/build/libftxui-screen.a \
    -framework Foundation
strip vipp

# Linux:
g++ -std=c++20 -Os -flto -Wl,--gc-sections \
    -I/tmp/ftxui-static/build/include -I/tmp/ftxui-static/include \
    -o vipp vi.cpp \
    /tmp/ftxui-static/build/libftxui-component.a \
    /tmp/ftxui-static/build/libftxui-dom.a \
    /tmp/ftxui-static/build/libftxui-screen.a \
    -lpthread
strip vipp
```

Verify no FTXUI shared library dependency:

```sh
# macOS
otool -L vipp | grep ftxui    # should produce no output

# Linux
ldd vipp | grep ftxui         # should produce no output
```

**Binary sizes** (macOS arm64, ~3000-line single source):

| Build | Flags | Size |
|-------|-------|------|
| Default static | `-std=c++20` | ~1.2 MB |
| Optimized static | `-Os -flto` + `strip` | **~330 KB** |
| Dynamic | `-Os` + `strip` | ~178 KB |

## Run

```sh
./vipp [filename]
```

If no filename is given, opens `untitled.txt`.

---

## Features

### Modes

| Mode | Enter | Color |
|------|-------|-------|
| **Normal** | `Esc` from any mode | White cursor block |
| **Insert** | `i`, `a`, `I`, `A`, `o`, `O` | Green cursor block |
| **Visual** (char) | `v` | Magenta cursor block |
| **Visual** (line) | `V` | Magenta cursor block |
| **Visual** (block) | `Ctrl-V` | Magenta cursor block |
| **Command** | `:` | Yellow prompt |
| **Search** | `/`, `?` | Yellow prompt |
| **Operator-Pending** | `d`, `c`, `y`, `>`, `<` | Yellow cursor block |

### Operators

| Key | Action | Example |
|-----|--------|---------|
| `d{motion}` | Delete | `dw` — delete word |
| `c{motion}` | Change (delete + insert) | `ciw` — change inner word |
| `y{motion}` | Yank (copy) | `y$` — yank to end of line |
| `dd` / `cc` / `yy` | Linewise delete / change / yank | `dd` — delete line |
| `p` / `P` | Put after / before cursor | `p` — paste |
| `>` / `<` | Indent / outdent | `>j` — indent this and next line |
| `~` | Toggle case | `~` — flip case of char under cursor |
| `x` | Delete character | `3x` — delete 3 chars |

### Motions

| Key | Motion |
|-----|--------|
| `h` `j` `k` `l` | Left, Down, Up, Right |
| `w` `b` `e` | Word forward, back, end |
| `W` `B` `E` | WORD forward, back, end |
| `0` `$` `^` | Line start, end, first non-blank |
| `gg` `G` | File start, end |
| `{` `}` | Paragraph back / forward |
| `%` | Jump to matching bracket |
| `f{char}` / `F{char}` | Find character forward / backward on line |
| `t{char}` / `T{char}` | Till character (stop before) |
| `;` / `,` | Repeat last `f`/`F`/`t`/`T` |
| `Ctrl-D` / `Ctrl-U` | Scroll half-page down / up |
| `zz` / `zt` / `zb` | Center / top / bottom current line on screen |

### Text Objects

| Key | Object |
|-----|--------|
| `iw` / `aw` | Inner / A word |
| `iW` / `aW` | Inner / A WORD |
| `i"` / `a"` | Inner / A double-quoted string |
| `i'` / `a'` | Inner / A single-quoted string |
| `i(` / `a(` `i)` / `a)` | Inner / A parentheses |
| `i[` / `a[` `i]` / `a]` | Inner / A brackets |
| `i{` / `a{` `i}` / `a}` | Inner / A braces |
| `i<` / `a<` `i>` / `a>` | Inner / A angle brackets |

Use with operators: `ciw` (change inner word), `da"` (delete around quotes), `yi(` (yank inside parens).

### Counts

Prefix any operator or motion with a number: `3dw` deletes 3 words, `5j` moves down 5 lines. Counts compose: `3d2w` deletes 2 words, 3 times.

### Dot Operator (`.`)

Repeats the last change. Works for insert-mode sessions, delete, change, and paste operations.

### Undo (`u`)

Linear undo stack. Each file/split has its own undo history.

### Search

| Key | Action |
|-----|--------|
| `/pattern` | Search forward (incremental, regex) |
| `?pattern` | Search backward (incremental, regex) |
| `n` / `N` | Next / previous match |
| `*` / `#` | Search word under cursor forward / backward |
| `:nohl` | Clear search highlighting |
| `:set hlsearch` / `:set nohlsearch` | Toggle persistent highlighting |

Search supports **incremental matching** (jumps as you type), **regex** via `std::regex::ECMAScript`, and **smart case** (uppercase in pattern = case-sensitive).

### Visual Mode

| Key | Mode |
|-----|------|
| `v` | Character-wise selection |
| `V` | Line-wise selection |
| `Ctrl-V` | Block (rectangular) selection |

In visual mode, operators apply to the selection:
- `d` — delete, `c` — change, `y` — yank, `>` / `<` — indent, `~` — toggle case

**Block change** (`Ctrl-V` + `c`): delete columns from all selected lines, type replacement, and the text is replicated across all lines on `Esc`.

### Insert Mode Shortcuts

| Key | Action |
|-----|--------|
| `Ctrl-W` | Delete word before cursor |
| `Backspace` | Delete character / join lines |
| `Enter` | Split line |

### Split Views

| Key | Action |
|-----|--------|
| `Ctrl-W v` | Vertical split |
| `Ctrl-W s` | Horizontal split |
| `Ctrl-W h` / `j` / `k` / `l` | Navigate splits |
| `Ctrl-W w` | Next split |
| `Ctrl-W q` | Close split (quits if last) |
| `:split [file]` | Horizontal split (optional file) |
| `:vsplit [file]` | Vertical split (optional file) |

Each split has its own **cursor**, **scroll position**, **undo history**, **yank register**, and **search state**.

### Key Mappings

Define custom key sequences with `:map`. Mappings apply in **normal mode** only.

**Syntax:**
```
:map <from-sequence> <to-sequence>
```

**Examples:**

```vim
" Save with <Space>fw (leader-key style)
:map <Space>fw :w<CR>

" Quit with <Space>qq
:map <Space>qq :q<CR>

" Compile and run (C++ file)
:map <Space>rr :!g++ -std=c++20 % -o a.out && ./a.out<CR>

" Move to beginning of line with <Space>h
:map <Space>h 0

" Delete line with <Space>d (no reaching for d)
:map <Space>d dd

" Yank line with <Space>y
:map <Space>y yy
```

**Supported tokens:**

| Token | Key |
|-------|-----|
| `a`–`z`, `A`–`Z`, `0`–`9` | Printable characters |
| `<Space>` | Space bar |
| `<CR>` | Enter / Return |
| `<Esc>` | Escape |
| `<Tab>` | Tab |
| `<BS>` | Backspace |
| `<Del>` | Delete |
| `<Up>`, `<Down>`, `<Left>`, `<Right>` | Arrow keys |
| `C-A` through `C-Z` | Ctrl+letter combinations |

Multi-key sequences are buffered until a full match (or mismatch) is detected.

### Quick Exit

| Keys | Action |
|------|--------|
| `ZZ` | Save and quit |
| `ZQ` | Quit without saving |

### Commands

| Command | Action |
|---------|--------|
| `:w` | Save file |
| `:q` | Quit (fails if modified) |
| `:wq` | Save and quit |
| `:q!` | Force quit (discard changes) |
| `:e filename` | Edit a different file |
| `:split [file]` | Horizontal split |
| `:vsplit [file]` | Vertical split |
| `:nohl` / `:nohlsearch` | Clear search highlight |
| `:set hlsearch` | Enable persistent search highlight |
| `:set nohlsearch` | Disable persistent search highlight |
| `:map from to` | Create a key mapping |

### Multiple Files

Each split can hold a different file. Use `:e` to switch the current view, or `:split file` / `:vsplit file` to open another file in a new split. Each view maintains independent undo, yank, and search state.
