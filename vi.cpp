#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include "motions.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ftxui;

// ============================================================================
// Forward declarations and enums
// ============================================================================

enum class Mode {
    NORMAL,
    INSERT,
    COMMAND,
    OPERATOR_PENDING,
    SEARCH_FORWARD,
    SEARCH_BACKWARD,
    VISUAL,
    VISUAL_LINE,
    VISUAL_BLOCK,
};

enum class OperatorType {
    NONE,
    DELETE_OP,
    CHANGE,
    YANK,
    INDENT,
    OUTDENT,
    TILDE_CASE,
};

enum class TextObjectType {
    NONE,
    INNER_WORD,
    A_WORD,
    INNER_WORD_BIG,
    A_WORD_BIG,
    INNER_QUOTE,
    A_QUOTE,
    INNER_DQUOTE,
    A_DQUOTE,
    INNER_PAREN,
    A_PAREN,
    INNER_BRACKET,
    A_BRACKET,
    INNER_BRACE,
    A_BRACE,
    INNER_ANGLE,
    A_ANGLE,
    INNER_BACKTICK,
    A_BACKTICK,
    INNER_PARAGRAPH,
    A_PARAGRAPH,
};

struct Range {
    int start_row, start_col;
    int end_row, end_col; // exclusive end
};

struct VisualSelection {
    int anchor_row, anchor_col;
    int cursor_row, cursor_col;
};

struct LastChange {
    OperatorType op = OperatorType::NONE;
    int count = 1;
    std::string motion_desc;   // "w", "j", "iw", etc.
    Range range;               // the affected range BEFORE the change
    std::string inserted_text; // for insert-mode changes
    bool was_linewise = false;
    std::vector<std::string> yanked_text; // for put/dot
};

struct UndoEntry {
    int cursor_row, cursor_col; // cursor position before the change
    int first_line;             // first affected line index
    std::vector<std::string>
        old_lines;      // original content of the affected lines
    int new_line_count = 0; // how many lines the change produced

    // Character-level granularity (for insert-mode word-boundary undo)
    bool is_char_level = false;
    int start_row = 0, start_col = 0;
    int end_row = 0, end_col = 0; // cursor position after the change
    std::string old_text;         // text that was there before
    std::string new_text;         // text that was inserted
};

// ============================================================================
// Syntax Highlighting — regex-based with per-line caching
// ============================================================================

struct TokenSpan {
    int start, end; // [start, end) column range
    Decorator deco;
};

struct SyntaxRule {
    std::string pattern;
    Decorator deco;
};

struct SyntaxDefinition {
    std::string name;
    std::vector<std::string> extensions;
    std::vector<SyntaxRule> rules;
};

class SyntaxHighlighter {
public:
    void SetLanguage(const std::string& filename);
    const std::vector<TokenSpan>&
    Highlight(int line_idx, const std::string& line) const;
    void InvalidateLine(int line_idx) {
        cache_.erase(line_idx);
        dirty_.erase(line_idx);
    }
    void InvalidateAll() const {
        cache_.clear();
        dirty_.clear();
    }
    bool HasLanguage() const { return lang_ != nullptr; }

private:
    const SyntaxDefinition* lang_ = nullptr;
    mutable std::map<int, std::vector<TokenSpan>> cache_;
    mutable std::map<int, bool> dirty_;
    void ParseRules();
};

// Built-in language definitions (rules compiled at startup)
static std::vector<SyntaxDefinition> kLanguages = {
    {"Python",
     {".py", ".pyw"},
     {
         {R"(#[^\n]*)", nothing},               // 0: comments (gray)
         {R"str("[^"]*"|'[^']*')str", nothing}, // 1: strings (green)
         {R"(\b(def|class|return|if|elif|else|for|while|import|from|as|try|except|finally|with|yield|lambda|pass|break|continue|and|or|not|in|is|True|False|None|async|await|raise|assert|del|global|nonlocal)\b)",
          nothing},                             // 2: keywords (blue)
         {R"(\b[0-9]+(\.[0-9]+)?\b)", nothing}, // 3: numbers (red)
         {R"(@[a-zA-Z_][a-zA-Z0-9_]*)",
          nothing}, // 4: decorators (green)
     }},
    {"C/C++",
     {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx"},
     {
         {R"(//[^\n]*)", nothing},      // 0: comments
         {R"str("[^"]*")str", nothing}, // 1: strings
         {R"(\b(auto|break|case|const|continue|default|do|else|enum|extern|for|goto|if|register|return|signed|sizeof|static|struct|switch|typedef|union|unsigned|void|volatile|while|class|namespace|template|typename|public|private|protected|virtual|override|final|new|delete|try|catch|throw|using|operator|bool|char|short|int|long|float|double|true|false|nullptr|include|define|ifdef|ifndef|endif|pragma)\b)",
          nothing}, // 2: keywords
         {R"(\b[0-9]+(\.[0-9]+)?(f|L|ll|u)?\b)", nothing}, // 3: numbers
         {R"(#.*)", nothing}, // 4: preprocessor
     }},
    {"Org-Mode",
     {".org"},
     {
         {R"(\*[^*]+\*)", nothing},       // 0: bold (gray)
         {R"(/[^/]+/)", nothing},         // 1: italic (green)
         {R"(^\*+\s.*)", nothing},        // 2: headers (light blue)
         {R"(:[a-zA-Z0-9_]+:)", nothing}, // 3: tags (red-orange)
         {R"(\bTODO\b)", nothing},        // 4: TODO (light green)
         {R"(\bDONE\b)", nothing},        // 5: DONE (light blue)
     }},
};

struct Buffer {
    std::string filename;
    std::vector<std::string> lines;
    bool modified = false;

    // ---- Yank register ----
    std::vector<std::string> yank_register;
    bool yank_linewise = false;
    bool yank_blockwise = false;

    // ---- Undo ----
    std::vector<UndoEntry> undo_stack;
    int undo_index =
        -1; // points to last applied entry; -1 = no undo available

    // ---- Word-boundary tracking for fine-grained undo
    bool insert_word_tracking = false;
    int insert_word_row = 0;
    int insert_word_col = 0;
    std::string insert_accumulated;

    // ---- Search ----
    std::string search_pattern;
    std::string last_search;
    bool search_highlight = true;
    std::vector<std::pair<int, int>> search_matches;
    int current_match_idx = -1;

    // ---- Syntax highlighting ----
    SyntaxHighlighter highlighter;
};

struct EditorView {
    std::shared_ptr<Buffer> buf;
    int cursor_row = 0;
    int cursor_col = 0;
    int top_row = 0;
    int left_col = 0;
    bool active = true;
};

// ============================================================================
// Syntax Highlighter Implementation
// ============================================================================

void SyntaxHighlighter::SetLanguage(const std::string& filename) {
    lang_ = nullptr;
    cache_.clear();
    dirty_.clear();
    // Find extension
    auto dot = filename.rfind('.');
    if (dot == std::string::npos)
        return;
    std::string ext = filename.substr(dot);
    for (auto& def : kLanguages) {
        for (auto& e : def.extensions) {
            if (e == ext) {
                lang_ = &def;
                ParseRules();
                return;
            }
        }
    }
}

void SyntaxHighlighter::ParseRules() {
    if (!lang_)
        return;
    auto& rules = const_cast<std::vector<SyntaxRule>&>(lang_->rules);
    // Wombat color scheme — order: comments, strings, keywords,
    // numbers, misc
    static const Decorator kWombat[] = {
        color(Color::RGB(0x99, 0x96, 0x8b)), // 0: comments (gray)
        color(Color::RGB(0x95, 0xe4, 0x54)), // 1: strings (green)
        color(Color::RGB(0x8a, 0xc6, 0xf2)), // 2: keywords (light blue)
        color(Color::RGB(0xe5, 0x78, 0x6d)), // 3: numbers (red-orange)
        color(Color::RGB(
            0xca, 0xe6, 0x82)), // 4: functions/decorators (light green)
        color(Color::RGB(0x8a, 0xc6, 0xf2)), // 5: misc (light blue)
        color(Color::RGB(0xe5, 0x78, 0x6d)), // 6: misc (red-orange)
    };
    for (size_t i = 0; i < rules.size(); ++i)
        rules[i].deco = kWombat[std::min(
            i, sizeof(kWombat) / sizeof(kWombat[0]) - 1)];
}

const std::vector<TokenSpan>&
SyntaxHighlighter::Highlight(int line_idx,
                             const std::string& line) const {
    // Simple caching: reuse spans from previous frame, clear when
    // scrolling
    auto& spans = cache_[line_idx];
    spans.clear();
    if (!lang_ || line.empty())
        return spans;

    for (auto& rule : lang_->rules) {
        try {
            std::regex re(rule.pattern);
            std::sregex_iterator re_it(line.begin(), line.end(), re);
            std::sregex_iterator re_end;
            for (; re_it != re_end; ++re_it) {
                TokenSpan ts;
                ts.start = static_cast<int>(re_it->position());
                ts.end = ts.start + static_cast<int>(re_it->length());
                ts.deco = rule.deco;
                spans.push_back(ts);
            }
        } catch (const std::regex_error&) {
        }
    }
    return spans;
}

// ============================================================================
// ViEditor class
// ============================================================================

class ViEditor {
public:
    explicit ViEditor(std::string filename);
    Component GetComponent();
    void Run();

private:
    // ---- Views (split support) ----
    std::vector<EditorView> views_;
    int active_view_ = 0;
    bool split_horizontal_ =
        true; // true=horizontal(top/bottom), false=vertical(left/right)

    EditorView& active() { return views_[active_view_]; }
    const EditorView& active() const { return views_[active_view_]; }

    ScreenInteractive* screen_ = nullptr;

    // ---- Mode ----
    Mode mode_ = Mode::NORMAL;
    Mode prev_mode_ =
        Mode::NORMAL; // for returning from operator-pending

    // ---- Command line ----
    std::string command_line_;

    // ---- Status ----
    std::string status_msg_;
    int status_timeout_ = 0;

    // ---- Operator pending ----
    OperatorType pending_op_ = OperatorType::NONE;
    int pending_count_ = 0; // 0 means "1" (no explicit count)
    int motion_count_ =
        0; // count preceding a motion while in op-pending
    bool operator_applied_ = false;

    // ---- Count accumulator ----
    int count_acc_ = 0; // accumulates digits before operator/motion

    // ---- Dot repeat ----
    LastChange last_change_;
    bool last_change_valid_ = false;
    // For recording insert-mode text for dot:
    std::string insert_recording_;
    bool recording_insert_ = false;

    // ---- Visual mode ----
    VisualSelection visual_sel_;
    bool visual_active_ = false;
    int visual_block_start_col_ = 0;

    // ---- Search ----
    bool search_forward_ = true;

    // ---- Key mappings ----
    struct KeyMapping {
        std::vector<std::string>
            from_seq; // sequence of Event strings to match
        std::vector<std::string> to_seq; // sequence to emit
        std::string from_str;            // concatenated for display
        std::string to_str;              // concatenated for display
    };
    std::vector<KeyMapping> key_mappings_;

    // ---- Mapping buffer ----
    std::vector<std::string> mapping_buffer_;
    bool replaying_ = false; // guard against mapping-buffer re-entrancy

    // ---- Last special motion (for ; and ,) ----
    char last_find_char_ = 0;
    bool last_find_forward_ = true;
    bool last_find_till_ = false; // 't' or 'T' vs 'f' or 'F'

    // ---- Multi-key state machines ----
    bool g_pending_ = false;
    bool find_pending_ = false;
    char find_pending_cmd_ = 0;
    bool text_obj_pending_ = false;
    bool text_obj_inner_ = false;
    bool ctrl_w_pending_ = false;
    bool z_pending_ = false;
    bool Z_pending_ = false;
    int ctrl_o_pending_ =
        0; // 0=off, 1=enter normal next, 2=return to insert after cmd

    // ---- Visual block change tracking ----
    bool block_change_pending_ = false;
    int block_change_start_row_ = 0;
    int block_change_end_row_ = 0;
    int block_change_start_col_ = 0;
    int block_change_end_col_ = 0;

    // ================================================================
    // File I/O
    // ================================================================
    void LoadFile(EditorView& view);
    void SaveFile(EditorView& view);

    // ================================================================
    // Status & Scrolling
    // ================================================================
    void SetStatus(std::string msg);
    void UpdateScroll(EditorView& view);
    void MoveCursor(EditorView& view, int drow, int dcol);

    // ================================================================
    // Editing primitives
    // ================================================================
    void InsertChar(EditorView& view, char c);
    void DeleteChar(EditorView& view); // x
    void DeleteRange(EditorView& view, const Range& r);
    std::vector<std::string> ExtractRange(EditorView& view,
                                          const Range& r);
    void PutAfter(EditorView& view,
                  const std::vector<std::string>& text, bool linewise,
                  bool blockwise);
    void PutBefore(EditorView& view,
                   const std::vector<std::string>& text, bool linewise,
                   bool blockwise);
    void IndentRange(EditorView& view, const Range& r, bool increase);
    void ToggleCaseRange(EditorView& view, const Range& r);

    // ================================================================
    // Undo
    // ================================================================
    void PushUndo(EditorView& view, int first_line, int line_count);
    void PushCharUndo(EditorView& view, int s_row, int s_col,
                      int e_row, int e_col,
                      const std::string& old_t,
                      const std::string& new_t);
    void PushInsertWordUndo(EditorView& view);
    void FinalizeInsertUndo(EditorView& view);
    void BeginInsertUndo(EditorView& view);
    void Undo(EditorView& view);
    void Redo(EditorView& view);

    // ================================================================
    // Visual block change
    // ================================================================
    void ApplyBlockChange(EditorView& view, OperatorType op);
    void FinalizeBlockChange(EditorView& view);

    // ================================================================
    // Motion functions – return the cursor position after motion
    // ================================================================
    // (CursorPos from motions.h)
    CursorPos MotionLeft(const EditorView& v, int count = 1) const;
    CursorPos MotionRight(const EditorView& v, int count = 1) const;
    CursorPos MotionDown(const EditorView& v, int count = 1) const;
    CursorPos MotionUp(const EditorView& v, int count = 1) const;
    CursorPos MotionWordForward(const EditorView& v,
                                int count = 1) const;
    CursorPos MotionWordBackward(const EditorView& v,
                                 int count = 1) const;
    CursorPos MotionWordEndForward(const EditorView& v,
                                   int count = 1) const;
    CursorPos MotionBigWordForward(const EditorView& v,
                                   int count = 1) const;
    CursorPos MotionBigWordBackward(const EditorView& v,
                                    int count = 1) const;
    CursorPos MotionBigWordEndForward(const EditorView& v,
                                      int count = 1) const;
    CursorPos MotionWordEndBackward(const EditorView& v,
                                    int count = 1) const;
    CursorPos MotionBigWordEndBackward(const EditorView& v,
                                       int count = 1) const;
    CursorPos MotionLineStart(const EditorView& v) const;
    CursorPos MotionLineEnd(const EditorView& v) const;
    CursorPos MotionFirstNonBlank(const EditorView& v) const;
    CursorPos MotionFileStart(const EditorView& v) const;
    CursorPos MotionFileEnd(const EditorView& v) const;
    CursorPos MotionFindForward(const EditorView& v, char c,
                                int count = 1, bool till = false) const;
    CursorPos MotionFindBackward(const EditorView& v, char c,
                                 int count = 1,
                                 bool till = false) const;
    CursorPos MotionParagraphForward(const EditorView& v,
                                     int count = 1) const;
    CursorPos MotionParagraphBackward(const EditorView& v,
                                      int count = 1) const;
    CursorPos MotionPercentMatch(const EditorView& v) const;

    // ================================================================
    // Text Objects
    // ================================================================
    Range TextObjectInnerWord(const EditorView& v) const;
    Range TextObjectAWord(const EditorView& v) const;
    Range TextObjectInnerBigWord(const EditorView& v) const;
    Range TextObjectABigWord(const EditorView& v) const;
    Range TextObjectQuoted(const EditorView& v, char quote) const;
    Range TextObjectBlock(const EditorView& v, char open,
                          char close) const;
    Range TextObjectInnerParagraph(const EditorView& v) const;
    Range TextObjectAParagraph(const EditorView& v) const;
    TextObjectType ParseTextObject(char c, bool inner) const;

    // ================================================================
    // Operator execution
    // ================================================================
    void ExecuteOperator(EditorView& view, OperatorType op,
                         const Range& r);
    Range GetRangeForTextObject(EditorView& view, TextObjectType tobj);
    bool ApplyOperatorMotion(EditorView& view, const CursorPos& target);

    // ================================================================
    // Dot repeat
    // ================================================================
    void RecordChange(OperatorType op, const Range& r, bool linewise);
    void RecordInsertChange(const std::string& text);
    void RepeatLastChange();

    // ================================================================
    // Visual mode
    // ================================================================
    void EnterVisual(bool linewise, bool blockwise);
    void ExitVisual();
    Range GetVisualRange(const EditorView& v) const;
    void ApplyOperatorToSelection(EditorView& view, OperatorType op);

    // ================================================================
    // Search
    // ================================================================
    void StartSearch(bool forward);
    void DoIncrementalSearch();
    void FinalizeSearch();
    void SearchNext(bool forward);
    void SearchWordUnderCursor(bool forward);
    void UpdateSearchMatches();
    void ClearSearchHighlight();

    // ================================================================
    // Command handling
    // ================================================================
    void ExecuteCommand();

    // ================================================================
    // Split views
    // ================================================================
    void SplitHorizontal();
    void SplitVertical();
    void CloseSplit();
    void NavigateSplit(char direction);
    void FocusNextSplit();
    void FocusPrevSplit();

    // ================================================================
    // Key mappings
    // ================================================================
    void AddMapping(const std::string& from, const std::string& to);
    std::vector<Event> ApplyMappings(const std::vector<Event>& events);

    // ================================================================
    // Rendering
    // ================================================================
    Element RenderView(const EditorView& view) const;
    Decorator CursorDecorator() const;
    Element RenderStatusBar() const;
    Element RenderCommandLine() const;
    Element RenderAllViews() const;

    // ================================================================
    // Event processing
    // ================================================================
    bool OnEvent(Event event);
    bool OnNormalEvent(Event event);
    bool OnInsertEvent(Event event);
    bool OnCommandEvent(Event event);
    bool OnOperatorPendingEvent(Event event);
    bool OnSearchEvent(Event event);
    bool OnVisualEvent(Event event);
    bool OnVisualLineEvent(Event event);
    bool OnVisualBlockEvent(Event event);

    // ================================================================
    // Utility
    // ================================================================
    static bool IsWordChar(char c);
    static bool IsBigWordChar(char c);
    static bool IsSpaceOrTab(char c);
    int EffectiveCount() const;
    std::string EventToString(const Event& e) const;
};

// ============================================================================
// Utility
// ============================================================================

bool ViEditor::IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool ViEditor::IsBigWordChar(char c) {
    return !std::isspace(static_cast<unsigned char>(c));
}

bool ViEditor::IsSpaceOrTab(char c) {
    return c == ' ' || c == '\t';
}

int ViEditor::EffectiveCount() const {
    int op = pending_count_ > 0 ? pending_count_ : 1;
    int mt = motion_count_ > 0 ? motion_count_ : 1;
    int acc = count_acc_ > 0 ? count_acc_ : 1;
    return op * mt * acc;
}

std::string ViEditor::EventToString(const Event& e) const {
    if (e == Event::Escape)
        return "<Esc>";
    if (e == Event::Return)
        return "<CR>";
    if (e == Event::Tab)
        return "<Tab>";
    if (e == Event::Backspace)
        return "<BS>";
    if (e == Event::Delete)
        return "<Del>";
    if (e == Event::ArrowUp)
        return "<Up>";
    if (e == Event::ArrowDown)
        return "<Down>";
    if (e == Event::ArrowLeft)
        return "<Left>";
    if (e == Event::ArrowRight)
        return "<Right>";
    if (e.is_character()) {
        std::string c = e.character();
        if (c.size() == 1) {
            if (c[0] < 32)
                return std::string("C-") + char(c[0] + 64);
            return c;
        }
    }
    return "<?>";
}

// ============================================================================
// Constructor & File I/O
// ============================================================================

ViEditor::ViEditor(std::string filename) {
    EditorView view;
    view.buf = std::make_shared<Buffer>();
    view.buf->filename = std::move(filename);
    LoadFile(view);
    views_.push_back(std::move(view));

    // Default key mappings
    AddMapping("<Space>/", ":nohl<CR>");
}

void ViEditor::LoadFile(EditorView& view) {
    std::ifstream file(view.buf->filename);
    view.buf->lines.clear();
    view.buf->highlighter.SetLanguage(view.buf->filename);
    if (!file.is_open()) {
        view.buf->lines.emplace_back("");
        SetStatus("New file: " + view.buf->filename);
        return;
    }
    std::string line;
    while (std::getline(file, line))
        view.buf->lines.push_back(std::move(line));
    if (view.buf->lines.empty())
        view.buf->lines.emplace_back("");
    view.buf->modified = false;
    view.cursor_row = 0;
    view.cursor_col = 0;
    view.top_row = 0;
    view.left_col = 0;
    view.buf->undo_stack.clear();
    view.buf->undo_index = -1;
    view.buf->insert_word_tracking = false;
    view.buf->insert_word_row = 0;
    view.buf->insert_word_col = 0;
    view.buf->insert_accumulated.clear();
    view.buf->yank_register.clear();
    view.buf->search_pattern.clear();
    view.buf->last_search.clear();
    view.buf->search_matches.clear();
    view.buf->current_match_idx = -1;
    SetStatus("Loaded " + view.buf->filename);
}

void ViEditor::SaveFile(EditorView& view) {
    std::ofstream file(view.buf->filename);
    if (!file.is_open()) {
        SetStatus("Cannot write to " + view.buf->filename);
        return;
    }
    for (size_t i = 0; i < view.buf->lines.size(); ++i) {
        file << view.buf->lines[i];
        if (i + 1 < view.buf->lines.size())
            file << '\n';
    }
    view.buf->modified = false;
    SetStatus("Saved " + view.buf->filename);
}

// ============================================================================
// Status & Scrolling
// ============================================================================

void ViEditor::SetStatus(std::string msg) {
    status_msg_ = std::move(msg);
    status_timeout_ = 5;
}

void ViEditor::UpdateScroll(EditorView& view) {
    // For horizontal splits: each view gets (dimy - 4 - (N-1)) / N rows
    int visible_rows = Terminal::Size().dimy - 3;
    if (split_horizontal_ && views_.size() > 1) {
        int avail = Terminal::Size().dimy - 3;
        int sep_count = static_cast<int>(views_.size()) - 1;
        visible_rows =
            (avail - sep_count) / static_cast<int>(views_.size());
    }

    if (view.cursor_row < view.top_row)
        view.top_row = view.cursor_row;
    else if (view.cursor_row >= view.top_row + visible_rows)
        view.top_row = view.cursor_row - visible_rows + 1;
    if (view.top_row < 0)
        view.top_row = 0;
    int max_top =
        std::max(0, static_cast<int>(view.buf->lines.size()) - visible_rows);
    if (view.top_row > max_top)
        view.top_row = max_top;

    // Horizontal scrolling: keep cursor visible within the editor area
    int visible_cols =
        Terminal::Size().dimx; // generous, works for splits too
    if (visible_cols < 20)
        visible_cols = 20; // minimum reasonable width
    if (view.cursor_col < view.left_col)
        view.left_col = view.cursor_col;
    else if (view.cursor_col >= view.left_col + visible_cols)
        view.left_col = view.cursor_col - visible_cols + 1;
    if (view.left_col < 0)
        view.left_col = 0;
}

void ViEditor::MoveCursor(EditorView& view, int drow, int dcol) {
    int new_row = view.cursor_row + drow;
    int new_col = view.cursor_col + dcol;
    new_row =
        std::clamp(new_row, 0, static_cast<int>(view.buf->lines.size()) - 1);
    const std::string& line = view.buf->lines[new_row];
    new_col = std::clamp(new_col, 0, static_cast<int>(line.size()));
    view.cursor_row = new_row;
    view.cursor_col = new_col;
    UpdateScroll(view);
}

// ============================================================================
// Editing primitives
// ============================================================================

void ViEditor::InsertChar(EditorView& view, char c) {
    std::string& line = view.buf->lines[view.cursor_row];
    if (view.cursor_col <= static_cast<int>(line.size())) {
        line.insert(line.begin() + view.cursor_col, c);
        ++view.cursor_col;
        view.buf->modified = true;
    }
}

void ViEditor::DeleteChar(EditorView& view) {
    std::string& line = view.buf->lines[view.cursor_row];
    if (!line.empty() &&
        view.cursor_col < static_cast<int>(line.size())) {
        line.erase(view.cursor_col, 1);
        view.buf->modified = true;
    } else if (!line.empty() &&
               view.cursor_col == static_cast<int>(line.size())) {
        // Join with next line
        if (view.cursor_row + 1 < static_cast<int>(view.buf->lines.size())) {
            line += view.buf->lines[view.cursor_row + 1];
            view.buf->lines.erase(view.buf->lines.begin() + view.cursor_row + 1,
                             view.buf->lines.begin() + view.cursor_row + 1 +
                                 1);
            view.buf->modified = true;
        }
    } else if (line.empty() && view.buf->lines.size() > 1) {
        view.buf->lines.erase(view.buf->lines.begin() + view.cursor_row,
                         view.buf->lines.begin() + view.cursor_row + 1);
        if (view.cursor_row >= static_cast<int>(view.buf->lines.size()))
            view.cursor_row = static_cast<int>(view.buf->lines.size()) - 1;
        view.cursor_col = 0;
        view.buf->modified = true;
        UpdateScroll(view);
    }
}

void ViEditor::DeleteRange(EditorView& view, const Range& r) {
    if (r.start_row == r.end_row) {
        int start = std::min(r.start_col, r.end_col);
        int end = std::max(r.start_col, r.end_col);
        int line_len = static_cast<int>(view.buf->lines[r.start_row].size());
        // If deleting the entire line, remove the line itself
        if (start == 0 && end >= line_len) {
            view.buf->lines.erase(view.buf->lines.begin() + r.start_row);
            if (view.buf->lines.empty())
                view.buf->lines.emplace_back("");
        } else {
            view.buf->lines[r.start_row].erase(start, end - start);
        }
    } else {
        int first_row = std::min(r.start_row, r.end_row);
        int last_row = std::max(r.start_row, r.end_row);
        int first_col =
            (r.start_row <= r.end_row) ? r.start_col : r.end_col;
        int last_col =
            (r.start_row <= r.end_row) ? r.end_col : r.start_col;

        view.buf->lines[first_row].erase(first_col);
        view.buf->lines[first_row] += view.buf->lines[last_row].substr(last_col);
        for (int i = last_row; i > first_row; --i)
            view.buf->lines.erase(view.buf->lines.begin() + i,
                             view.buf->lines.begin() + i + 1);
    }
    view.buf->modified = true;
}

std::vector<std::string> ViEditor::ExtractRange(EditorView& view,
                                                const Range& r) {
    std::vector<std::string> result;
    int first_row = std::min(r.start_row, r.end_row);
    int last_row = std::max(r.start_row, r.end_row);
    int first_col = std::min(r.start_col, r.end_col);
    int last_col = std::max(r.start_col, r.end_col);

    if (first_row == last_row) {
        result.push_back(view.buf->lines[first_row].substr(
            first_col, last_col - first_col));
    } else {
        // Determine direction
        if (r.start_row <= r.end_row) {
            result.push_back(
                view.buf->lines[r.start_row].substr(r.start_col));
            for (int i = r.start_row + 1; i < r.end_row; ++i)
                result.push_back(view.buf->lines[i]);
            result.push_back(
                view.buf->lines[r.end_row].substr(0, r.end_col));
        } else {
            result.push_back(view.buf->lines[r.end_row].substr(r.end_col));
            for (int i = r.end_row + 1; i < r.start_row; ++i)
                result.push_back(view.buf->lines[i]);
            result.push_back(
                view.buf->lines[r.start_row].substr(0, r.start_col));
        }
    }
    return result;
}

void ViEditor::PutAfter(EditorView& view,
                        const std::vector<std::string>& text,
                        bool linewise, bool /*blockwise*/) {
    if (text.empty()) {
        SetStatus("Nothing to paste — yank register is empty");
        return;
    }
    PushUndo(view, view.cursor_row, static_cast<int>(text.size()) + 1);
    if (linewise) {
        int ins_row = view.cursor_row + 1;
        for (const auto& t : text) {
            view.buf->lines.insert(view.buf->lines.begin() + ins_row, t);
            ++ins_row;
        }
        view.cursor_row = std::min(
            ins_row - 1, static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = 0;
    } else {
        std::string& line = view.buf->lines[view.cursor_row];
        if (text.size() == 1) {
            int ins = std::min(view.cursor_col + 1,
                               static_cast<int>(line.size()));
            line.insert(ins, text[0]);
            view.cursor_col =
                ins + static_cast<int>(text[0].size()) - 1;
        } else {
            // Multi-line paste
            std::string after = line.substr(view.cursor_col + 1);
            line.erase(view.cursor_col + 1);
            line += text[0];
            int ins_row = view.cursor_row + 1;
            for (size_t i = 1; i < text.size() - 1; ++i) {
                view.buf->lines.insert(view.buf->lines.begin() + ins_row,
                                  text[i]);
                ++ins_row;
            }
            if (text.size() > 1) {
                std::string last = text.back();
                if (ins_row < static_cast<int>(view.buf->lines.size())) {
                    last += view.buf->lines[ins_row];
                    view.buf->lines[ins_row] = last;
                } else {
                    view.buf->lines.push_back(last);
                }
            }
            view.cursor_row = ins_row;
            view.cursor_col = text.back().size() - 1;
        }
    }
    view.buf->modified = true;
    UpdateScroll(view);
}

void ViEditor::PutBefore(EditorView& view,
                         const std::vector<std::string>& text,
                         bool linewise, bool /*blockwise*/) {
    if (text.empty()) {
        SetStatus("Nothing to paste — yank register is empty");
        return;
    }
    PushUndo(view, view.cursor_row, static_cast<int>(text.size()) + 1);
    if (linewise) {
        int ins_row = view.cursor_row;
        for (const auto& t : text) {
            view.buf->lines.insert(view.buf->lines.begin() + ins_row, t);
            ++ins_row;
        }
        view.cursor_row = ins_row - static_cast<int>(text.size());
        view.cursor_col = 0;
    } else {
        std::string& line = view.buf->lines[view.cursor_row];
        if (text.size() == 1) {
            line.insert(view.cursor_col, text[0]);
            view.cursor_col += static_cast<int>(text[0].size()) - 1;
        } else {
            std::string after = line.substr(view.cursor_col);
            line.erase(view.cursor_col);
            line += text[0];
            int ins_row = view.cursor_row + 1;
            for (size_t i = 1; i < text.size() - 1; ++i) {
                view.buf->lines.insert(view.buf->lines.begin() + ins_row,
                                  text[i]);
                ++ins_row;
            }
            if (text.size() > 1) {
                view.buf->lines.insert(view.buf->lines.begin() + ins_row,
                                  text.back() + after);
            }
            view.cursor_row = ins_row;
            view.cursor_col = text.back().size() - 1;
        }
    }
    view.buf->modified = true;
    UpdateScroll(view);
}

void ViEditor::IndentRange(EditorView& view, const Range& r,
                           bool increase) {
    int first = std::min(r.start_row, r.end_row);
    int last = std::max(r.start_row, r.end_row);
    for (int i = first;
         i <= last && i < static_cast<int>(view.buf->lines.size()); ++i) {
        if (!view.buf->lines[i].empty()) {
            if (increase) {
                view.buf->lines[i].insert(0, "  ");
            } else {
                if (view.buf->lines[i].size() >= 2 &&
                    view.buf->lines[i][0] == ' ' && view.buf->lines[i][1] == ' ')
                    view.buf->lines[i].erase(0, 2);
                else if (!view.buf->lines[i].empty() &&
                         view.buf->lines[i][0] == ' ')
                    view.buf->lines[i].erase(0, 1);
            }
        }
    }
    view.buf->modified = true;
}

void ViEditor::ToggleCaseRange(EditorView& view, const Range& r) {
    if (r.start_row == r.end_row) {
        std::string& line = view.buf->lines[r.start_row];
        for (int i = r.start_col;
             i < r.end_col && i < static_cast<int>(line.size()); ++i) {
            if (std::islower(static_cast<unsigned char>(line[i])))
                line[i] = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(line[i])));
            else if (std::isupper(static_cast<unsigned char>(line[i])))
                line[i] = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(line[i])));
        }
    }
    view.buf->modified = true;
}

// ============================================================================
// Undo
// ============================================================================

void ViEditor::PushUndo(EditorView& view, int first_line,
                        int line_count) {
    // Snapshots lines [first_line, first_line + line_count) before the
    // change. The change will replace these with new content; undo
    // restores the old content.
    if (line_count <= 0)
        return;

    UndoEntry entry;
    entry.is_char_level = false;
    entry.cursor_row = view.cursor_row;
    entry.cursor_col = view.cursor_col;
    entry.first_line = first_line;
    entry.new_line_count = 0; // will be set on undo by examining
                              // current state, or we compute it now

    int end = std::min(first_line + line_count,
                       static_cast<int>(view.buf->lines.size()));
    for (int i = first_line; i < end; ++i)
        entry.old_lines.push_back(view.buf->lines[i]);

    // Discard any redo entries beyond view.buf->undo_index
    if (view.buf->undo_index + 1 < static_cast<int>(view.buf->undo_stack.size()))
        view.buf->undo_stack.resize(view.buf->undo_index + 1);

    view.buf->undo_stack.push_back(std::move(entry));
    view.buf->undo_index = static_cast<int>(view.buf->undo_stack.size()) - 1;
}

void ViEditor::PushCharUndo(EditorView& view, int s_row, int s_col,
                            int e_row, int e_col,
                            const std::string& old_t,
                            const std::string& new_t) {
    UndoEntry entry;
    entry.is_char_level = true;
    entry.cursor_row = s_row;
    entry.cursor_col = s_col;
    entry.start_row = s_row;
    entry.start_col = s_col;
    entry.end_row = e_row;
    entry.end_col = e_col;
    entry.old_text = old_t;
    entry.new_text = new_t;

    if (view.buf->undo_index + 1 < static_cast<int>(view.buf->undo_stack.size()))
        view.buf->undo_stack.resize(view.buf->undo_index + 1);

    view.buf->undo_stack.push_back(std::move(entry));
    view.buf->undo_index = static_cast<int>(view.buf->undo_stack.size()) - 1;
}

void ViEditor::PushInsertWordUndo(EditorView& view) {
    if (!view.buf->insert_word_tracking || view.buf->insert_accumulated.empty())
        return;
    int s_row = view.buf->insert_word_row;
    int s_col = view.buf->insert_word_col;
    int e_row = view.cursor_row;
    int e_col = view.cursor_col;
    PushCharUndo(view, s_row, s_col, e_row, e_col, "",
                 view.buf->insert_accumulated);
    view.buf->insert_word_row = e_row;
    view.buf->insert_word_col = e_col;
    view.buf->insert_accumulated.clear();
}

void ViEditor::BeginInsertUndo(EditorView& view) {
    // Start word-boundary tracking for fine-grained undo
    view.buf->insert_word_tracking = false;
    view.buf->insert_word_row = view.cursor_row;
    view.buf->insert_word_col = view.cursor_col;
    view.buf->insert_accumulated.clear();
}

void ViEditor::FinalizeInsertUndo(EditorView& view) {
    // Push any remaining accumulated word text
    PushInsertWordUndo(view);
    view.buf->insert_word_tracking = false;
}

void ViEditor::Undo(EditorView& view) {
    if (view.buf->undo_index < 0 ||
        view.buf->undo_index >= static_cast<int>(view.buf->undo_stack.size())) {
        SetStatus("Already at oldest change");
        return;
    }

    UndoEntry& entry = view.buf->undo_stack[view.buf->undo_index];

    if (entry.is_char_level) {
        // Character-level undo: remove new_text, restore old_text
        std::string& line = view.buf->lines[entry.start_row];
        if (entry.start_col + static_cast<int>(entry.new_text.size()) <=
            static_cast<int>(line.size())) {
            line.erase(entry.start_col,
                       static_cast<int>(entry.new_text.size()));
        }
        if (!entry.old_text.empty())
            line.insert(entry.start_col, entry.old_text);

        // Swap texts so redo can restore the forward state
        std::swap(entry.old_text, entry.new_text);

        view.cursor_row = std::clamp(
            entry.cursor_row, 0, static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = std::clamp(
            entry.cursor_col, 0,
            static_cast<int>(view.buf->lines[view.cursor_row].size()));
        view.buf->modified = true;
        --view.buf->undo_index;
        UpdateScroll(view);
        SetStatus("Undo");
        return;
    }

    // Line-level undo (original behaviour)
    // Compute how many lines the change produced (current state from
    // first_line)
    int current_count = 0;
    if (entry.new_line_count > 0) {
        current_count = entry.new_line_count;
    } else {
        // For entries pushed via PushUndo (non-insert changes), we
        // don't know new_line_count. We need to infer it.  The entry
        // was pushed BEFORE the change. After the change, the lines
        // from first_line onward were modified. We can approximate:
        // compare old_lines size with what's there now. Simpler fix:
        // compute new_line_count at push time in PushUndo by deferring.
        // For now, use a heuristic: replace up to old_lines.size()
        // lines.
        current_count = static_cast<int>(entry.old_lines.size());
    }

    // Save current lines as the "new" state (for potential redo in the
    // future)
    std::vector<std::string> new_lines;
    int end_line = std::min(entry.first_line + current_count,
                            static_cast<int>(view.buf->lines.size()));
    for (int i = entry.first_line; i < end_line; ++i)
        new_lines.push_back(view.buf->lines[i]);

    // Restore old lines
    int remove_count = current_count;
    int old_size = static_cast<int>(entry.old_lines.size());

    // Remove current lines
    if (remove_count > 0 &&
        entry.first_line < static_cast<int>(view.buf->lines.size())) {
        int actual_remove =
            std::min(remove_count, static_cast<int>(view.buf->lines.size()) -
                                       entry.first_line);
        view.buf->lines.erase(view.buf->lines.begin() + entry.first_line,
                         view.buf->lines.begin() + entry.first_line +
                             actual_remove);
    }

    // Insert old lines
    for (int i = 0; i < old_size; ++i)
        view.buf->lines.insert(view.buf->lines.begin() + entry.first_line + i,
                          entry.old_lines[i]);

    // Update entry so re-undo (redo) would work
    entry.old_lines = std::move(new_lines);
    entry.new_line_count = old_size;

    // Restore cursor
    view.cursor_row = std::clamp(
        entry.cursor_row, 0, static_cast<int>(view.buf->lines.size()) - 1);
    view.cursor_col = std::clamp(
        entry.cursor_col, 0,
        static_cast<int>(view.buf->lines[view.cursor_row].size()));
    view.buf->modified = true;

    --view.buf->undo_index;
    UpdateScroll(view);
    SetStatus("Undo");
}

void ViEditor::Redo(EditorView& view) {
    int redo_idx = view.buf->undo_index + 1;
    if (redo_idx < 0 ||
        redo_idx >= static_cast<int>(view.buf->undo_stack.size())) {
        SetStatus("Already at newest change");
        return;
    }

    UndoEntry& entry = view.buf->undo_stack[redo_idx];

    if (entry.is_char_level) {
        // Character-level redo: same logic as undo (remove new_text,
        // insert old_text) since undo already swapped them
        std::string& line = view.buf->lines[entry.start_row];
        if (entry.start_col + static_cast<int>(entry.new_text.size()) <=
            static_cast<int>(line.size())) {
            line.erase(entry.start_col,
                       static_cast<int>(entry.new_text.size()));
        }
        if (!entry.old_text.empty())
            line.insert(entry.start_col, entry.old_text);

        // Swap texts back for potential re-undo
        std::swap(entry.old_text, entry.new_text);

        // Cursor goes to the position it was at after the original change
        view.cursor_row =
            std::clamp(entry.end_row, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col =
            std::clamp(entry.end_col, 0,
                       static_cast<int>(view.buf->lines[view.cursor_row].size()));
        view.buf->modified = true;
        view.buf->undo_index = redo_idx;
        UpdateScroll(view);
        SetStatus("Redo");
        return;
    }

    // Line-level redo (original behaviour)
    // Save current lines as the "old" state for potential undo
    int current_count =
        entry.new_line_count; // this was set during undo
    std::vector<std::string> current_lines;
    int end_line = std::min(entry.first_line + current_count,
                            static_cast<int>(view.buf->lines.size()));
    for (int i = entry.first_line; i < end_line; ++i)
        current_lines.push_back(view.buf->lines[i]);

    // Remove current lines
    if (current_count > 0 &&
        entry.first_line < static_cast<int>(view.buf->lines.size())) {
        int actual_remove = std::min(
            current_count,
            static_cast<int>(view.buf->lines.size()) - entry.first_line);
        view.buf->lines.erase(view.buf->lines.begin() + entry.first_line,
                         view.buf->lines.begin() + entry.first_line +
                             actual_remove);
    }

    // Insert old lines (which are the "forward" state)
    int old_size = static_cast<int>(entry.old_lines.size());
    for (int i = 0; i < old_size; ++i)
        view.buf->lines.insert(view.buf->lines.begin() + entry.first_line + i,
                          entry.old_lines[i]);

    // Update entry for potential re-undo
    entry.old_lines = std::move(current_lines);
    entry.new_line_count = old_size;

    // Restore cursor
    view.cursor_row = std::clamp(
        entry.cursor_row, 0, static_cast<int>(view.buf->lines.size()) - 1);
    view.cursor_col = std::clamp(
        entry.cursor_col, 0,
        static_cast<int>(view.buf->lines[view.cursor_row].size()));
    view.buf->modified = true;

    view.buf->undo_index = redo_idx; // advance past this entry
    UpdateScroll(view);
    SetStatus("Redo");
}

// ============================================================================
// Visual block change
// ============================================================================

void ViEditor::ApplyBlockChange(EditorView& view, OperatorType op) {
    Range r = GetVisualRange(view);
    int sr = std::min(r.start_row, r.end_row);
    int er = std::max(r.start_row, r.end_row);
    int sc = std::min(r.start_col, r.end_col);
    int ec = std::max(r.start_col, r.end_col);

    // Push undo for all affected lines
    PushUndo(view, sr, er - sr + 1);

    if (op == OperatorType::DELETE_OP) {
        // Yank the block text
        view.buf->yank_register.clear();
        for (int row = sr;
             row <= er && row < static_cast<int>(view.buf->lines.size());
             ++row) {
            std::string& line = view.buf->lines[row];
            int cs = std::min(sc, static_cast<int>(line.size()));
            int ce = std::min(ec, static_cast<int>(line.size()));
            if (cs < ce)
                view.buf->yank_register.push_back(line.substr(cs, ce - cs));
            else
                view.buf->yank_register.push_back("");
            if (cs < static_cast<int>(line.size()))
                line.erase(cs,
                           std::min(ce, static_cast<int>(line.size())) -
                               cs);
        }
        view.buf->yank_linewise = false;
        view.buf->yank_blockwise = true;
        view.cursor_row = sr;
        view.cursor_col =
            std::min(sc, static_cast<int>(view.buf->lines[sr].size()));
        view.buf->modified = true;
        ExitVisual();
    } else if (op == OperatorType::CHANGE) {
        // Delete the block and prepare for multi-line insert
        view.buf->yank_register.clear();
        for (int row = sr;
             row <= er && row < static_cast<int>(view.buf->lines.size());
             ++row) {
            std::string& line = view.buf->lines[row];
            int cs = std::min(sc, static_cast<int>(line.size()));
            int ce = std::min(ec, static_cast<int>(line.size()));
            if (cs < ce)
                line.erase(cs, ce - cs);
        }
        view.buf->yank_linewise = false;
        view.buf->yank_blockwise = true;
        // Remember the block for replication
        block_change_pending_ = true;
        block_change_start_row_ = sr;
        block_change_end_row_ = er;
        block_change_start_col_ = sc;
        block_change_end_col_ = ec;
        // Place cursor at start of deletion on first line
        view.cursor_row = sr;
        view.cursor_col = sc;
        view.buf->modified = true;
        ExitVisual();
        mode_ = Mode::INSERT;
        recording_insert_ = true;
        insert_recording_.clear();
    }
    UpdateScroll(view);
}

void ViEditor::FinalizeBlockChange(EditorView& view) {
    if (!block_change_pending_)
        return;
    block_change_pending_ = false;

    int sr = block_change_start_row_;
    int er = block_change_end_row_;
    int sc = block_change_start_col_;

    // Get the text that was inserted on the first line
    std::string& first_line = view.buf->lines[sr];
    std::string inserted = first_line.substr(sc);

    // Replicate to all other lines in the block
    for (int row = sr + 1;
         row <= er && row < static_cast<int>(view.buf->lines.size());
         ++row) {
        std::string& line = view.buf->lines[row];
        if (sc <= static_cast<int>(line.size()))
            line.insert(sc, inserted);
        else
            line.append(std::string(sc - line.size(), ' ') + inserted);
    }
    UpdateScroll(view);
}

// ============================================================================
// Motion functions
// ============================================================================

CursorPos ViEditor::MotionLeft(const EditorView& v, int count) const {
    int col = v.cursor_col;
    int row = v.cursor_row;
    for (int i = 0; i < count && (row > 0 || col > 0); ++i) {
        if (col > 0)
            --col;
        else if (row > 0) {
            --row;
            col = static_cast<int>(v.buf->lines[row].size());
        }
    }
    return {row, col};
}

CursorPos ViEditor::MotionRight(const EditorView& v, int count) const {
    int col = v.cursor_col;
    int row = v.cursor_row;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;
    for (int i = 0;
         i < count &&
         (row < max_row || col < static_cast<int>(v.buf->lines[row].size()));
         ++i) {
        if (col < static_cast<int>(v.buf->lines[row].size()))
            ++col;
        else if (row < max_row) {
            ++row;
            col = 0;
        }
    }
    return {row, col};
}

CursorPos ViEditor::MotionDown(const EditorView& v, int count) const {
    int row = std::min(v.cursor_row + count,
                       static_cast<int>(v.buf->lines.size()) - 1);
    int col =
        std::min(v.cursor_col, static_cast<int>(v.buf->lines[row].size()));
    return {row, col};
}

CursorPos ViEditor::MotionUp(const EditorView& v, int count) const {
    int row = std::max(v.cursor_row - count, 0);
    int col =
        std::min(v.cursor_col, static_cast<int>(v.buf->lines[row].size()));
    return {row, col};
}

CursorPos ViEditor::MotionLineStart(const EditorView& v) const {
    return {v.cursor_row, 0};
}

CursorPos ViEditor::MotionLineEnd(const EditorView& v) const {
    return {v.cursor_row,
            static_cast<int>(v.buf->lines[v.cursor_row].size())};
}

CursorPos ViEditor::MotionFirstNonBlank(const EditorView& v) const {
    const auto& line = v.buf->lines[v.cursor_row];
    int col = 0;
    while (col < static_cast<int>(line.size()) &&
           IsSpaceOrTab(line[col]))
        ++col;
    return {v.cursor_row, col};
}

CursorPos ViEditor::MotionFileStart(const EditorView& /*v*/) const {
    return {0, 0};
}

CursorPos ViEditor::MotionFileEnd(const EditorView& v) const {
    return {static_cast<int>(v.buf->lines.size()) - 1, 0};
}

CursorPos ViEditor::MotionWordForward(const EditorView& v,
                                      int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;

    for (int c = 0; c < count; ++c) {
        // Skip current word characters
        while (row <= max_row &&
               col < static_cast<int>(v.buf->lines[row].size()) &&
               IsWordChar(v.buf->lines[row][col]))
            ++col;
        // Skip non-word non-whitespace
        while (row <= max_row &&
               col < static_cast<int>(v.buf->lines[row].size()) &&
               !IsWordChar(v.buf->lines[row][col]) &&
               !IsSpaceOrTab(v.buf->lines[row][col]))
            ++col;
        // Skip whitespace (but don't cross lines)
        while (col < static_cast<int>(v.buf->lines[row].size()) &&
               IsSpaceOrTab(v.buf->lines[row][col]))
            ++col;
        // If at end of line, go to next line
        if (col >= static_cast<int>(v.buf->lines[row].size()) &&
            row < max_row) {
            ++row;
            col = 0;
            while (col < static_cast<int>(v.buf->lines[row].size()) &&
                   IsSpaceOrTab(v.buf->lines[row][col]))
                ++col;
        }
    }
    if (row > max_row) {
        row = max_row;
        col = static_cast<int>(v.buf->lines[row].size());
    }
    return {row, col};
}

CursorPos ViEditor::MotionWordBackward(const EditorView& v,
                                       int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        // Step 1: if at column 0, move to end of previous line
        if (col == 0 && row > 0) {
            --row;
            col = static_cast<int>(v.buf->lines[row].size());
        }
        // Step 2: skip whitespace and non-word chars backward to find a
        // word First skip past any whitespace
        while (col > 0 && IsSpaceOrTab(v.buf->lines[row][col - 1]))
            --col;
        // If we landed at column 0 after skipping whitespace, go up a
        // line
        if (col == 0 && row > 0) {
            --row;
            col = static_cast<int>(v.buf->lines[row].size());
            while (col > 0 && IsSpaceOrTab(v.buf->lines[row][col - 1]))
                --col;
        }
        // Step 3: if on a non-word char, step back one to get onto a
        // word boundary
        if (col > 0 && !IsWordChar(v.buf->lines[row][col - 1]))
            --col;
        // Step 4: move to start of the word
        while (col > 0 && IsWordChar(v.buf->lines[row][col - 1]))
            --col;
    }
    if (row < 0)
        row = 0;
    return {row, col};
}

CursorPos ViEditor::MotionWordEndForward(const EditorView& v,
                                         int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;

    for (int c = 0; c < count; ++c) {
        int line_len = static_cast<int>(v.buf->lines[row].size());
        // Move forward one char so we don't stay on the same word end
        if (col < line_len)
            ++col;
        // Skip whitespace
        while (row <= max_row) {
            while (col < static_cast<int>(v.buf->lines[row].size()) &&
                   IsSpaceOrTab(v.buf->lines[row][col]))
                ++col;
            if (col >= static_cast<int>(v.buf->lines[row].size()) &&
                row < max_row) {
                ++row;
                col = 0;
            } else {
                break;
            }
        }
        // Move to end of word-or-punctuation sequence
        if (row <= max_row &&
            col < static_cast<int>(v.buf->lines[row].size())) {
            char cur = v.buf->lines[row][col];
            bool is_word = IsWordChar(cur);
            while (row <= max_row &&
                   col < static_cast<int>(v.buf->lines[row].size())) {
                char ch = v.buf->lines[row][col];
                if (IsSpaceOrTab(ch))
                    break;
                if (is_word && !IsWordChar(ch))
                    break;
                if (!is_word && IsWordChar(ch))
                    break;
                ++col;
            }
        }
        // Position at last char of sequence
        if (col > 0)
            --col;
    }
    return {row, col};
}

CursorPos ViEditor::MotionBigWordForward(const EditorView& v,
                                         int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;

    for (int c = 0; c < count; ++c) {
        while (row <= max_row &&
               col < static_cast<int>(v.buf->lines[row].size()) &&
               IsBigWordChar(v.buf->lines[row][col]))
            ++col;
        while (col < static_cast<int>(v.buf->lines[row].size()) &&
               IsSpaceOrTab(v.buf->lines[row][col]))
            ++col;
        if (col >= static_cast<int>(v.buf->lines[row].size()) &&
            row < max_row) {
            ++row;
            col = 0;
            while (col < static_cast<int>(v.buf->lines[row].size()) &&
                   IsSpaceOrTab(v.buf->lines[row][col]))
                ++col;
        }
    }
    if (row > max_row) {
        row = max_row;
        col = static_cast<int>(v.buf->lines[row].size());
    }
    return {row, col};
}

CursorPos ViEditor::MotionBigWordBackward(const EditorView& v,
                                          int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    for (int c = 0; c < count; ++c) {
        while (row >= 0 && col > 0 &&
               IsSpaceOrTab(v.buf->lines[row][col - 1]))
            --col;
        if (col == 0 && row > 0) {
            --row;
            col = static_cast<int>(v.buf->lines[row].size());
        }
        while (row >= 0 && col > 0 &&
               IsBigWordChar(v.buf->lines[row][col - 1]))
            --col;
    }
    if (row < 0)
        row = 0;
    return {row, col};
}

CursorPos ViEditor::MotionBigWordEndForward(const EditorView& v,
                                            int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;
    for (int c = 0; c < count; ++c) {
        int line_len = static_cast<int>(v.buf->lines[row].size());
        // Move forward one char so we don't stay on the same word end
        if (col < line_len)
            ++col;
        // Skip whitespace
        while (row <= max_row) {
            while (col < static_cast<int>(v.buf->lines[row].size()) &&
                   IsSpaceOrTab(v.buf->lines[row][col]))
                ++col;
            if (col >= static_cast<int>(v.buf->lines[row].size()) &&
                row < max_row) {
                ++row;
                col = 0;
            } else {
                break;
            }
        }
        // Move to end of WORD
        while (row <= max_row &&
               col < static_cast<int>(v.buf->lines[row].size()) &&
               IsBigWordChar(v.buf->lines[row][col]))
            ++col;
        if (col > 0)
            --col;
    }
    return {row, col};
}

CursorPos ViEditor::MotionWordEndBackward(const EditorView& v,
                                          int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        // Determine the type of character under the cursor
        if (row < 0 || row >= static_cast<int>(v.buf->lines.size()))
            break;
        int line_len = static_cast<int>(v.buf->lines[row].size());
        if (col >= line_len)
            col = line_len > 0 ? line_len - 1 : 0;
        char cur = (col < line_len) ? v.buf->lines[row][col] : ' ';
        bool is_word = IsWordChar(cur);
        bool is_space = IsSpaceOrTab(cur);

        // Step 1: skip backward past the current sequence
        if (!is_space) {
            while (true) {
                if (col <= 0) {
                    if (row > 0) {
                        --row;
                        col = static_cast<int>(v.buf->lines[row].size());
                    } else {
                        break;
                    }
                }
                if (col > 0)
                    --col;
                else
                    break;
                char ch = v.buf->lines[row][col];
                if (IsSpaceOrTab(ch))
                    break;
                if (is_word && !IsWordChar(ch))
                    break;
                if (!is_word && IsWordChar(ch))
                    break;
            }
        }

        // Step 2: skip backward past whitespace
        while (row >= 0) {
            if (col >= static_cast<int>(v.buf->lines[row].size()))
                col = static_cast<int>(v.buf->lines[row].size()) - 1;
            while (col >= 0 &&
                   col < static_cast<int>(v.buf->lines[row].size()) &&
                   IsSpaceOrTab(v.buf->lines[row][col]))
                --col;
            if (col < 0 && row > 0) {
                --row;
                col = static_cast<int>(v.buf->lines[row].size()) - 1;
            } else {
                break;
            }
        }

        // Step 3: find the end of the previous sequence
        if (row >= 0 && col >= 0 &&
            col < static_cast<int>(v.buf->lines[row].size()) &&
            !IsSpaceOrTab(v.buf->lines[row][col])) {
            cur = v.buf->lines[row][col];
            is_word = IsWordChar(cur);
            // Find the start of this sequence
            int seq_start = col;
            while (seq_start > 0) {
                char prev = v.buf->lines[row][seq_start - 1];
                if (IsSpaceOrTab(prev))
                    break;
                if (is_word && !IsWordChar(prev))
                    break;
                if (!is_word && IsWordChar(prev))
                    break;
                --seq_start;
            }
            // Move from start to end
            col = seq_start;
            line_len = static_cast<int>(v.buf->lines[row].size());
            while (col < line_len) {
                char ch = v.buf->lines[row][col];
                if (IsSpaceOrTab(ch))
                    break;
                if (is_word && !IsWordChar(ch))
                    break;
                if (!is_word && IsWordChar(ch))
                    break;
                ++col;
            }
            if (col > 0)
                --col;
        }
    }
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    return {row, col};
}

CursorPos ViEditor::MotionBigWordEndBackward(const EditorView& v,
                                             int count) const {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        if (row < 0 || row >= static_cast<int>(v.buf->lines.size()))
            break;
        int line_len = static_cast<int>(v.buf->lines[row].size());
        if (col >= line_len)
            col = line_len > 0 ? line_len - 1 : 0;
        char cur = (col < line_len) ? v.buf->lines[row][col] : ' ';
        bool is_space = IsSpaceOrTab(cur);

        // Step 1: skip backward past the current BIG word
        if (!is_space) {
            while (true) {
                if (col <= 0) {
                    if (row > 0) {
                        --row;
                        col = static_cast<int>(v.buf->lines[row].size());
                    } else {
                        break;
                    }
                }
                if (col > 0)
                    --col;
                else
                    break;
                if (IsSpaceOrTab(v.buf->lines[row][col]))
                    break;
            }
        }

        // Step 2: skip backward past whitespace
        while (row >= 0) {
            if (col >= static_cast<int>(v.buf->lines[row].size()))
                col = static_cast<int>(v.buf->lines[row].size()) - 1;
            while (col >= 0 &&
                   col < static_cast<int>(v.buf->lines[row].size()) &&
                   IsSpaceOrTab(v.buf->lines[row][col]))
                --col;
            if (col < 0 && row > 0) {
                --row;
                col = static_cast<int>(v.buf->lines[row].size()) - 1;
            } else {
                break;
            }
        }

        // Step 3: find the end of the previous BIG word
        if (row >= 0 && col >= 0 &&
            col < static_cast<int>(v.buf->lines[row].size()) &&
            !IsSpaceOrTab(v.buf->lines[row][col])) {
            int seq_start = col;
            while (seq_start > 0 &&
                   !IsSpaceOrTab(v.buf->lines[row][seq_start - 1]))
                --seq_start;
            col = seq_start;
            line_len = static_cast<int>(v.buf->lines[row].size());
            while (col < line_len && !IsSpaceOrTab(v.buf->lines[row][col]))
                ++col;
            if (col > 0)
                --col;
        }
    }
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    return {row, col};
}

CursorPos ViEditor::MotionFindForward(const EditorView& v, char target,
                                      int count, bool till) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;

    for (int c = 0; c < count; ++c) {
        ++col; // start after current position
        while (row <= max_row) {
            while (col < static_cast<int>(v.buf->lines[row].size())) {
                if (v.buf->lines[row][col] == target) {
                    if (till) {
                        // For 't', stop one char before
                        if (col > 0)
                            return {row, col - 1};
                    }
                    return {row, col};
                }
                ++col;
            }
            if (row < max_row) {
                ++row;
                col = 0;
            } else
                break;
        }
    }
    return {v.cursor_row, v.cursor_col}; // not found, stay
}

CursorPos ViEditor::MotionFindBackward(const EditorView& v, char target,
                                       int count, bool till) const {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        --col;
        while (row >= 0) {
            while (col >= 0) {
                if (v.buf->lines[row][col] == target) {
                    if (till) {
                        if (col + 1 <
                            static_cast<int>(v.buf->lines[row].size()))
                            return {row, col + 1};
                        return {row, col};
                    }
                    return {row, col};
                }
                --col;
            }
            if (row > 0) {
                --row;
                col = static_cast<int>(v.buf->lines[row].size()) - 1;
            } else
                break;
        }
    }
    return {v.cursor_row, v.cursor_col};
}

CursorPos ViEditor::MotionParagraphForward(const EditorView& v,
                                           int count) const {
    int row = v.cursor_row;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;
    for (int c = 0; c < count && row < max_row; ++c) {
        // Skip non-empty lines
        while (row < max_row && !v.buf->lines[row].empty())
            ++row;
        // Skip empty lines
        while (row < max_row && v.buf->lines[row].empty())
            ++row;
    }
    return {row, 0};
}

CursorPos ViEditor::MotionParagraphBackward(const EditorView& v,
                                            int count) const {
    int row = v.cursor_row;
    for (int c = 0; c < count && row > 0; ++c) {
        while (row > 0 && !v.buf->lines[row].empty())
            --row;
        while (row > 0 && v.buf->lines[row].empty())
            --row;
    }
    return {row, 0};
}

CursorPos ViEditor::MotionPercentMatch(const EditorView& v) const {
    const auto& line = v.buf->lines[v.cursor_row];
    char target = 0;
    char under = (v.cursor_col < static_cast<int>(line.size()))
                     ? line[v.cursor_col]
                     : 0;
    char prev = (v.cursor_col > 0) ? line[v.cursor_col - 1] : 0;

    char open_c = 0, close_c = 0;

    if (under == '(' || under == ')' || prev == '(' || prev == ')') {
        open_c = '(';
        close_c = ')';
        target = (under == '(' || prev == '(') ? open_c : close_c;
    } else if (under == '[' || under == ']' || prev == '[' ||
               prev == ']') {
        open_c = '[';
        close_c = ']';
        target = (under == '[' || prev == '[') ? open_c : close_c;
    } else if (under == '{' || under == '}' || prev == '{' ||
               prev == '}') {
        open_c = '{';
        close_c = '}';
        target = (under == '{' || prev == '{') ? open_c : close_c;
    }
    if (!open_c)
        return {v.cursor_row, v.cursor_col};

    bool forward = (target == open_c);
    char match = forward ? close_c : open_c;
    char self = forward ? open_c : close_c;
    int depth = 1;
    int row = v.cursor_row;
    int col = (prev == self || under == self) ? v.cursor_col
                                              : v.cursor_col + 1;

    if (forward) {
        if (col < static_cast<int>(v.buf->lines[row].size()) &&
            v.buf->lines[row][col] == self) {
            ++col; // skip the character we are already on
        }
    }

    while (depth > 0) {
        if (forward) {
            while (col < static_cast<int>(v.buf->lines[row].size())) {
                if (v.buf->lines[row][col] == self)
                    ++depth;
                else if (v.buf->lines[row][col] == match)
                    --depth;
                if (depth == 0)
                    return {row, col};
                ++col;
            }
            ++row;
            col = 0;
        } else {
            while (col >= 0) {
                if (v.buf->lines[row][col] == self)
                    ++depth;
                else if (v.buf->lines[row][col] == match)
                    --depth;
                if (depth == 0)
                    return {row, col};
                --col;
            }
            --row;
            if (row >= 0)
                col = static_cast<int>(v.buf->lines[row].size()) - 1;
            else
                break;
        }
    }
    return {v.cursor_row, v.cursor_col};
}

// ============================================================================
// Text Objects
// ============================================================================

Range ViEditor::TextObjectInnerWord(const EditorView& v) const {
    const auto& line = v.buf->lines[v.cursor_row];
    int start = v.cursor_col;
    int end = v.cursor_col;

    if (start >= static_cast<int>(line.size()) ||
        !IsWordChar(line[start]))
        return {v.cursor_row, start, v.cursor_row,
                std::min(start + 1, static_cast<int>(line.size()))};

    while (start > 0 && IsWordChar(line[start - 1]))
        --start;
    while (end < static_cast<int>(line.size()) && IsWordChar(line[end]))
        ++end;
    return {v.cursor_row, start, v.cursor_row, end};
}

Range ViEditor::TextObjectAWord(const EditorView& v) const {
    const auto& line = v.buf->lines[v.cursor_row];
    int start = v.cursor_col;
    int end = v.cursor_col;

    // Include trailing/leading whitespace
    if (start < static_cast<int>(line.size()) &&
        IsWordChar(line[start])) {
        while (start > 0 && IsWordChar(line[start - 1]))
            --start;
        while (end < static_cast<int>(line.size()) &&
               IsWordChar(line[end]))
            ++end;
        // Include trailing whitespace (one space)
        if (end < static_cast<int>(line.size()) &&
            IsSpaceOrTab(line[end]))
            ++end;
    } else if (start < static_cast<int>(line.size()) &&
               IsSpaceOrTab(line[start])) {
        while (start > 0 && IsSpaceOrTab(line[start - 1]))
            --start;
        while (end < static_cast<int>(line.size()) &&
               IsSpaceOrTab(line[end]))
            ++end;
        // If followed by word, include it
        while (end < static_cast<int>(line.size()) &&
               IsWordChar(line[end]))
            ++end;
    }
    return {v.cursor_row, start, v.cursor_row, end};
}

Range ViEditor::TextObjectInnerBigWord(const EditorView& v) const {
    const auto& line = v.buf->lines[v.cursor_row];
    int start = v.cursor_col;
    int end = v.cursor_col;
    if (start >= static_cast<int>(line.size()) ||
        !IsBigWordChar(line[start]))
        return {v.cursor_row, start, v.cursor_row,
                std::min(start + 1, static_cast<int>(line.size()))};
    while (start > 0 && IsBigWordChar(line[start - 1]))
        --start;
    while (end < static_cast<int>(line.size()) &&
           IsBigWordChar(line[end]))
        ++end;
    return {v.cursor_row, start, v.cursor_row, end};
}

Range ViEditor::TextObjectABigWord(const EditorView& v) const {
    const auto& line = v.buf->lines[v.cursor_row];
    int start = v.cursor_col;
    int end = v.cursor_col;
    if (start < static_cast<int>(line.size()) &&
        IsBigWordChar(line[start])) {
        while (start > 0 && IsBigWordChar(line[start - 1]))
            --start;
        while (end < static_cast<int>(line.size()) &&
               IsBigWordChar(line[end]))
            ++end;
        if (end < static_cast<int>(line.size()) &&
            IsSpaceOrTab(line[end]))
            ++end;
    } else if (start < static_cast<int>(line.size()) &&
               IsSpaceOrTab(line[start])) {
        while (start > 0 && IsSpaceOrTab(line[start - 1]))
            --start;
        while (end < static_cast<int>(line.size()) &&
               IsSpaceOrTab(line[end]))
            ++end;
        while (end < static_cast<int>(line.size()) &&
               IsBigWordChar(line[end]))
            ++end;
    }
    return {v.cursor_row, start, v.cursor_row, end};
}

Range ViEditor::TextObjectQuoted(const EditorView& v,
                                 char quote) const {
    const auto& line = v.buf->lines[v.cursor_row];
    // Find surrounding quotes
    int start = -1;
    for (int i = 0;
         i <= v.cursor_col && i < static_cast<int>(line.size()); ++i) {
        if (line[i] == quote)
            start = i;
    }
    if (start == -1)
        return {v.cursor_row, v.cursor_col, v.cursor_row, v.cursor_col};

    int end = -1;
    for (int i = start + 1; i < static_cast<int>(line.size()); ++i) {
        if (line[i] == quote) {
            end = i;
            break;
        }
    }
    if (end == -1)
        return {v.cursor_row, start + 1, v.cursor_row,
                static_cast<int>(line.size())};

    return {v.cursor_row, start + 1, v.cursor_row, end}; // inner
}

Range ViEditor::TextObjectBlock(const EditorView& v, char open,
                                char close) const {
    int row = v.cursor_row;
    int col = v.cursor_col;
    // Find opening bracket by searching backward
    int depth = 0;
    int start_row = row, start_col = col;
    int end_row = row, end_col = col;
    bool found_open = false, found_close = false;

    // Search backward for opening bracket
    while (row >= 0) {
        while (col >= 0) {
            if (v.buf->lines[row][col] == close)
                ++depth;
            else if (v.buf->lines[row][col] == open) {
                if (depth == 0) {
                    start_row = row;
                    start_col = col;
                    found_open = true;
                    break;
                }
                --depth;
            }
            --col;
        }
        if (found_open)
            break;
        --row;
        if (row >= 0)
            col = static_cast<int>(v.buf->lines[row].size()) - 1;
    }
    if (!found_open)
        return {v.cursor_row, v.cursor_col, v.cursor_row, v.cursor_col};

    // Search forward for closing bracket
    row = start_row;
    col = start_col + 1;
    depth = 0;
    while (row < static_cast<int>(v.buf->lines.size())) {
        while (col < static_cast<int>(v.buf->lines[row].size())) {
            if (v.buf->lines[row][col] == open)
                ++depth;
            else if (v.buf->lines[row][col] == close) {
                if (depth == 0) {
                    end_row = row;
                    end_col = col;
                    found_close = true;
                    break;
                }
                --depth;
            }
            ++col;
        }
        if (found_close)
            break;
        ++row;
        col = 0;
    }
    if (!found_close)
        return {start_row, start_col + 1, v.cursor_row, v.cursor_col};

    return {start_row, start_col + 1, end_row, end_col}; // inner
}

Range ViEditor::TextObjectInnerParagraph(const EditorView& v) const {
    int start = v.cursor_row;
    int end = v.cursor_row;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;

    // Search upward for paragraph boundary (empty line or file start)
    while (start > 0 && !v.buf->lines[start - 1].empty())
        --start;

    // Search downward for paragraph boundary
    while (end < max_row && !v.buf->lines[end + 1].empty())
        ++end;

    // end_row with end_col = 0 signals linewise (covers start..end
    // inclusive)
    return {start, 0, end, 0};
}

Range ViEditor::TextObjectAParagraph(const EditorView& v) const {
    int start = v.cursor_row;
    int end = v.cursor_row;
    int max_row = static_cast<int>(v.buf->lines.size()) - 1;

    // Search upward: skip blank lines to find paragraph start
    while (start > 0 && v.buf->lines[start - 1].empty())
        --start;
    while (start > 0 && !v.buf->lines[start - 1].empty())
        --start;

    // Search downward: skip blank lines to find paragraph end
    while (end < max_row && v.buf->lines[end + 1].empty())
        ++end;
    while (end < max_row && !v.buf->lines[end + 1].empty())
        ++end;

    // Include trailing blank lines
    while (end < max_row && v.buf->lines[end + 1].empty())
        ++end;

    return {start, 0, end, 0};
}

TextObjectType ViEditor::ParseTextObject(char c, bool inner) const {
    if (inner) {
        if (c == 'w')
            return TextObjectType::INNER_WORD;
        if (c == 'W')
            return TextObjectType::INNER_WORD_BIG;
        if (c == '\'')
            return TextObjectType::INNER_QUOTE;
        if (c == '"')
            return TextObjectType::INNER_DQUOTE;
        if (c == '`')
            return TextObjectType::INNER_BACKTICK;
        if (c == '(' || c == ')')
            return TextObjectType::INNER_PAREN;
        if (c == '[' || c == ']')
            return TextObjectType::INNER_BRACKET;
        if (c == '{' || c == '}')
            return TextObjectType::INNER_BRACE;
        if (c == '<' || c == '>')
            return TextObjectType::INNER_ANGLE;
        if (c == 'p')
            return TextObjectType::INNER_PARAGRAPH;
    } else {
        if (c == 'w')
            return TextObjectType::A_WORD;
        if (c == 'W')
            return TextObjectType::A_WORD_BIG;
        if (c == '\'')
            return TextObjectType::A_QUOTE;
        if (c == '"')
            return TextObjectType::A_DQUOTE;
        if (c == '`')
            return TextObjectType::A_BACKTICK;
        if (c == '(' || c == ')')
            return TextObjectType::A_PAREN;
        if (c == '[' || c == ']')
            return TextObjectType::A_BRACKET;
        if (c == '{' || c == '}')
            return TextObjectType::A_BRACE;
        if (c == '<' || c == '>')
            return TextObjectType::A_ANGLE;
        if (c == 'p')
            return TextObjectType::A_PARAGRAPH;
    }
    return TextObjectType::NONE;
}

Range ViEditor::GetRangeForTextObject(EditorView& view,
                                      TextObjectType tobj) {
    switch (tobj) {
    case TextObjectType::INNER_WORD:
        return TextObjectInnerWord(view);
    case TextObjectType::A_WORD:
        return TextObjectAWord(view);
    case TextObjectType::INNER_WORD_BIG:
        return TextObjectInnerBigWord(view);
    case TextObjectType::A_WORD_BIG:
        return TextObjectABigWord(view);
    case TextObjectType::INNER_QUOTE:
        return TextObjectQuoted(view, '\'');
    case TextObjectType::A_QUOTE: {
        auto inner = TextObjectQuoted(view, '\'');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_DQUOTE:
        return TextObjectQuoted(view, '"');
    case TextObjectType::A_DQUOTE: {
        auto inner = TextObjectQuoted(view, '"');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_BACKTICK:
        return TextObjectQuoted(view, '`');
    case TextObjectType::A_BACKTICK: {
        auto inner = TextObjectQuoted(view, '`');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_PAREN:
        return TextObjectBlock(view, '(', ')');
    case TextObjectType::A_PAREN: {
        auto inner = TextObjectBlock(view, '(', ')');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_BRACKET:
        return TextObjectBlock(view, '[', ']');
    case TextObjectType::A_BRACKET: {
        auto inner = TextObjectBlock(view, '[', ']');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_BRACE:
        return TextObjectBlock(view, '{', '}');
    case TextObjectType::A_BRACE: {
        auto inner = TextObjectBlock(view, '{', '}');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_ANGLE:
        return TextObjectBlock(view, '<', '>');
    case TextObjectType::A_ANGLE: {
        auto inner = TextObjectBlock(view, '<', '>');
        if (inner.start_col > 0)
            inner.start_col--;
        if (inner.end_col <
            static_cast<int>(view.buf->lines[inner.end_row].size()))
            inner.end_col++;
        return inner;
    }
    case TextObjectType::INNER_PARAGRAPH:
        return TextObjectInnerParagraph(view);
    case TextObjectType::A_PARAGRAPH:
        return TextObjectAParagraph(view);
    default:
        return {view.cursor_row, view.cursor_col, view.cursor_row,
                view.cursor_col};
    }
}

// ============================================================================
// Operator execution
// ============================================================================

void ViEditor::ExecuteOperator(EditorView& view, OperatorType op,
                               const Range& r) {
    Range adjusted = r;
    // For linewise operations, ensure range covers full lines
    bool linewise = false;

    switch (op) {
    case OperatorType::DELETE_OP: {
        auto yanked = ExtractRange(view, adjusted);
        // Check if this is a linewise delete (covers full lines)
        if (adjusted.start_col == 0 && adjusted.end_col == 0) {
            linewise = true;
            // adjust range to cover full lines
            int sr = std::min(adjusted.start_row, adjusted.end_row);
            int er = std::max(adjusted.start_row, adjusted.end_row);
            adjusted = {sr, 0, er,
                        static_cast<int>(view.buf->lines[er].size())};
            yanked = ExtractRange(view, adjusted);
            PushUndo(view, sr, er - sr + 1);
            DeleteRange(view, adjusted);
            if (sr < static_cast<int>(view.buf->lines.size())) {
                view.cursor_row = sr;
                view.cursor_col = 0;
            }
        } else {
            int sr = std::min(adjusted.start_row, adjusted.end_row);
            int er = std::max(adjusted.start_row, adjusted.end_row);
            PushUndo(view, sr, er - sr + 1);
            DeleteRange(view, adjusted);
            if (adjusted.start_row <= adjusted.end_row) {
                view.cursor_row = adjusted.start_row;
                view.cursor_col = adjusted.start_col;
            } else {
                view.cursor_row = adjusted.end_row;
                view.cursor_col = adjusted.end_col;
            }
        }
        view.buf->yank_register = yanked;
        view.buf->yank_linewise = linewise;
        view.buf->yank_blockwise = false;
        RecordChange(op, adjusted, linewise);
        break;
    }
    case OperatorType::CHANGE: {
        // Push line-level undo for the deletion part
        int change_sr = std::min(adjusted.start_row, adjusted.end_row);
        int change_er = std::max(adjusted.start_row, adjusted.end_row);
        PushUndo(view, change_sr, change_er - change_sr + 1);

        auto yanked = ExtractRange(view, adjusted);
        if (adjusted.start_col == 0 && adjusted.end_col == 0) {
            linewise = true;
            adjusted = {change_sr, 0, change_er,
                        static_cast<int>(view.buf->lines[change_er].size())};
            yanked = ExtractRange(view, adjusted);
            // Re-push undo with expanded range for linewise
            view.buf->undo_stack.pop_back();
            --view.buf->undo_index;
            PushUndo(view, change_sr, change_er - change_sr + 1);
        }
        // The typing part will use char-level word-boundary undo entries
        DeleteRange(view, adjusted);
        view.buf->yank_register = yanked;
        view.buf->yank_linewise = linewise;
        view.buf->yank_blockwise = false;
        RecordChange(op, adjusted, linewise);
        if (linewise) {
            view.cursor_col = 0;
        } else {
            if (adjusted.start_row <= adjusted.end_row) {
                view.cursor_row = adjusted.start_row;
                view.cursor_col = adjusted.start_col;
            } else {
                view.cursor_row = adjusted.end_row;
                view.cursor_col = adjusted.end_col;
            }
        }
        mode_ = Mode::INSERT;
        recording_insert_ = true;
        insert_recording_.clear();
        break;
    }
    case OperatorType::YANK: {
        auto yanked = ExtractRange(view, adjusted);
        if (adjusted.start_col == 0 && adjusted.end_col == 0) {
            // Linewise: expand range to cover full lines
            int sr = std::min(adjusted.start_row, adjusted.end_row);
            int er = std::max(adjusted.start_row, adjusted.end_row);
            adjusted = {sr, 0, er,
                        static_cast<int>(view.buf->lines[er].size())};
            yanked = ExtractRange(view, adjusted);
            view.buf->yank_linewise = true;
        } else {
            view.buf->yank_linewise = false;
        }
        view.buf->yank_blockwise = false;
        view.buf->yank_register = yanked;
        // Cursor doesn't move for yank
        SetStatus("Yanked " + std::to_string(yanked.size()) +
                  " line(s)");
        break;
    }
    case OperatorType::INDENT: {
        int sr = std::min(adjusted.start_row, adjusted.end_row);
        int er = std::max(adjusted.start_row, adjusted.end_row);
        PushUndo(view, sr, er - sr + 1);
        IndentRange(view, adjusted, true);
        break;
    }
    case OperatorType::OUTDENT: {
        int sr = std::min(adjusted.start_row, adjusted.end_row);
        int er = std::max(adjusted.start_row, adjusted.end_row);
        PushUndo(view, sr, er - sr + 1);
        IndentRange(view, adjusted, false);
        break;
    }
    case OperatorType::TILDE_CASE: {
        int sr = std::min(adjusted.start_row, adjusted.end_row);
        int er = std::max(adjusted.start_row, adjusted.end_row);
        PushUndo(view, sr, er - sr + 1);
        ToggleCaseRange(view, adjusted);
        break;
    }
    default:
        break;
    }
    UpdateScroll(view);
    // Change operator stays in INSERT; all others exit operator-pending
    // back to NORMAL
    if (op != OperatorType::CHANGE && mode_ == Mode::OPERATOR_PENDING)
        mode_ = Mode::NORMAL;
}

bool ViEditor::ApplyOperatorMotion(EditorView& view,
                                   const CursorPos& target) {
    Range r;
    r.start_row = view.cursor_row;
    r.start_col = view.cursor_col;
    r.end_row = target.row;
    r.end_col = target.col;
    ExecuteOperator(view, pending_op_, r);
    pending_op_ = OperatorType::NONE;
    pending_count_ = 0;
    motion_count_ = 0;
    operator_applied_ = true;
    return true;
}

// ============================================================================
// Dot repeat
// ============================================================================

void ViEditor::RecordChange(OperatorType op, const Range& r,
                            bool linewise) {
    last_change_.op = op;
    last_change_.range = r;
    last_change_.was_linewise = linewise;
    last_change_.yanked_text = active().buf->yank_register;
    last_change_valid_ = true;
}

void ViEditor::RecordInsertChange(const std::string& text) {
    last_change_.op = OperatorType::CHANGE;
    last_change_.inserted_text = text;
    last_change_valid_ = true;
}

void ViEditor::RepeatLastChange() {
    if (!last_change_valid_) {
        SetStatus("No previous change to repeat");
        return;
    }
    EditorView& view = active();
    if (last_change_.op == OperatorType::CHANGE &&
        !last_change_.inserted_text.empty()) {
        // Push line-level undo for the entire dot repeat
        PushUndo(view, view.cursor_row, 1);
        mode_ = Mode::INSERT;
        recording_insert_ = false;
        for (char c : last_change_.inserted_text) {
            if (c == '\n') {
                // Handle newline in insert
                std::string& line = view.buf->lines[view.cursor_row];
                std::string rest = line.substr(view.cursor_col);
                line.erase(view.cursor_col);
                view.buf->lines.insert(
                    view.buf->lines.begin() + view.cursor_row + 1, rest);
                ++view.cursor_row;
                view.cursor_col = 0;
            } else {
                InsertChar(view, c);
            }
        }
        view.buf->modified = true;
        mode_ = Mode::NORMAL;
        // Move cursor left by 1 (Vim convention: dot leaves cursor on
        // last char)
        if (view.cursor_col > 0)
            --view.cursor_col;
    } else if (last_change_.op == OperatorType::DELETE_OP) {
        // Re-execute delete on the same range shape, starting from
        // cursor
        PushUndo(view, last_change_.range.start_row, 1);
        DeleteRange(view, last_change_.range);
    } else if (last_change_.op == OperatorType::CHANGE) {
        PushUndo(view, last_change_.range.start_row, 1);
        DeleteRange(view, last_change_.range);
        mode_ = Mode::INSERT;
    }
    UpdateScroll(view);
}

// ============================================================================
// Visual mode
// ============================================================================

void ViEditor::EnterVisual(bool linewise, bool blockwise) {
    if (linewise)
        mode_ = Mode::VISUAL_LINE;
    else if (blockwise)
        mode_ = Mode::VISUAL_BLOCK;
    else
        mode_ = Mode::VISUAL;

    visual_active_ = true;
    visual_sel_.anchor_row = active().cursor_row;
    visual_sel_.anchor_col = active().cursor_col;
    visual_sel_.cursor_row = active().cursor_row;
    visual_sel_.cursor_col = active().cursor_col;
    visual_block_start_col_ = active().cursor_col;
}

void ViEditor::ExitVisual() {
    mode_ = Mode::NORMAL;
    visual_active_ = false;
}

Range ViEditor::GetVisualRange(const EditorView& v) const {
    Range r;
    if (mode_ == Mode::VISUAL) {
        r.start_row = visual_sel_.anchor_row;
        r.start_col = visual_sel_.anchor_col;
        r.end_row = v.cursor_row;
        r.end_col = v.cursor_col;
        // Character visual: include the character under cursor
        if (r.end_col < static_cast<int>(v.buf->lines[r.end_row].size())) {
            if (r.start_row < r.end_row ||
                (r.start_row == r.end_row && r.start_col <= r.end_col))
                r.end_col++;
            else
                r.start_col++;
        }
    } else if (mode_ == Mode::VISUAL_LINE) {
        r.start_row = std::min(visual_sel_.anchor_row, v.cursor_row);
        r.end_row = std::max(visual_sel_.anchor_row, v.cursor_row);
        r.start_col = 0;
        r.end_col = 0; // signal linewise
    } else {           // VISUAL_BLOCK
        int col_start = std::min(visual_sel_.anchor_col, v.cursor_col);
        int col_end =
            std::max(visual_sel_.anchor_col, v.cursor_col) + 1;
        r.start_row = std::min(visual_sel_.anchor_row, v.cursor_row);
        r.end_row = std::max(visual_sel_.anchor_row, v.cursor_row);
        r.start_col = col_start;
        r.end_col = col_end;
    }
    return r;
}

void ViEditor::ApplyOperatorToSelection(EditorView& view,
                                        OperatorType op) {
    Range r = GetVisualRange(view);
    ExecuteOperator(view, op, r);
    ExitVisual();
}

// ============================================================================
// Search
// ============================================================================

void ViEditor::StartSearch(bool forward) {
    search_forward_ = forward;
    active().buf->search_pattern.clear();
    active().buf->search_matches.clear();
    active().buf->current_match_idx = -1;
    if (forward)
        mode_ = Mode::SEARCH_FORWARD;
    else
        mode_ = Mode::SEARCH_BACKWARD;
    command_line_.clear();
}

void ViEditor::DoIncrementalSearch() {
    if (active().buf->search_pattern.empty()) {
        active().buf->search_matches.clear();
        active().buf->current_match_idx = -1;
        return;
    }

    EditorView& view = active();
    std::string pat = active().buf->search_pattern;

    // Compile regex
    std::regex re;
    try {
        bool icase = true; // smartcase would be nice
        auto flags = std::regex::ECMAScript;
        if (icase) {
            // if pattern has uppercase, make it case-sensitive
            bool has_upper =
                std::any_of(pat.begin(), pat.end(), [](char c) {
                    return std::isupper(static_cast<unsigned char>(c));
                });
            if (!has_upper)
                flags |= std::regex::icase;
        }
        re = std::regex(pat, flags);
    } catch (const std::regex_error&) {
        active().buf->search_matches.clear();
        active().buf->current_match_idx = -1;
        return;
    }

    active().buf->search_matches.clear();
    for (int row = 0; row < static_cast<int>(view.buf->lines.size());
         ++row) {
        std::sregex_iterator it(view.buf->lines[row].begin(),
                                view.buf->lines[row].end(), re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            active().buf->search_matches.push_back(
                {row, static_cast<int>(it->position())});
        }
    }

    if (active().buf->search_matches.empty()) {
        active().buf->current_match_idx = -1;
        return;
    }

    // Find the best match for cursor position
    if (search_forward_) {
        int start_row = view.cursor_row;
        int start_col = view.cursor_col + 1; // start after cursor
        // Find next match
        active().buf->current_match_idx = -1;
        for (size_t i = 0; i < active().buf->search_matches.size(); ++i) {
            auto& m = active().buf->search_matches[i];
            if (m.first > start_row ||
                (m.first == start_row && m.second >= start_col)) {
                active().buf->current_match_idx = static_cast<int>(i);
                break;
            }
        }
        if (active().buf->current_match_idx == -1 &&
            !active().buf->search_matches.empty())
            active().buf->current_match_idx = 0; // wrap
    } else {
        int start_row = view.cursor_row;
        int start_col = view.cursor_col;
        active().buf->current_match_idx = -1;
        for (int i =
                 static_cast<int>(active().buf->search_matches.size()) - 1;
             i >= 0; --i) {
            auto& m = active().buf->search_matches[i];
            if (m.first < start_row ||
                (m.first == start_row && m.second < start_col)) {
                active().buf->current_match_idx = i;
                break;
            }
        }
        if (active().buf->current_match_idx == -1 &&
            !active().buf->search_matches.empty())
            active().buf->current_match_idx =
                static_cast<int>(active().buf->search_matches.size()) - 1;
    }

    // Jump to current match
    if (active().buf->current_match_idx >= 0 &&
        active().buf->current_match_idx <
            static_cast<int>(active().buf->search_matches.size())) {
        auto& m = active().buf->search_matches[active().buf->current_match_idx];
        view.cursor_row = m.first;
        view.cursor_col = m.second;
        UpdateScroll(view);
    }
}

void ViEditor::FinalizeSearch() {
    active().buf->last_search = active().buf->search_pattern;
    if (!active().buf->search_pattern.empty())
        active().buf->search_highlight = true;
    mode_ = Mode::NORMAL;
    command_line_.clear();
}

void ViEditor::SearchNext(bool forward) {
    if (active().buf->last_search.empty()) {
        SetStatus("No previous search pattern");
        return;
    }
    active().buf->search_pattern = active().buf->last_search;
    search_forward_ = forward;
    DoIncrementalSearch();
    FinalizeSearch();
}

void ViEditor::SearchWordUnderCursor(bool forward) {
    EditorView& view = active();
    const auto& line = view.buf->lines[view.cursor_row];
    // Find word boundaries at cursor
    int start = view.cursor_col;
    int end = view.cursor_col;
    if (start < static_cast<int>(line.size()) &&
        IsWordChar(line[start])) {
        while (start > 0 && IsWordChar(line[start - 1]))
            --start;
        while (end < static_cast<int>(line.size()) &&
               IsWordChar(line[end]))
            ++end;
    } else {
        // Just search for the next word
        while (end < static_cast<int>(line.size()) &&
               !IsWordChar(line[end]))
            ++end;
        if (end >= static_cast<int>(line.size()))
            return;
        start = end;
        while (end < static_cast<int>(line.size()) &&
               IsWordChar(line[end]))
            ++end;
    }
    std::string word = line.substr(start, end - start);
    if (word.empty())
        return;

    // Escape regex special characters
    std::string escaped;
    for (char c : word) {
        if (c == '.' || c == '*' || c == '+' || c == '?' || c == '[' ||
            c == ']' || c == '(' || c == ')' || c == '{' || c == '}' ||
            c == '^' || c == '$' || c == '|' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    active().buf->search_pattern = "\\b" + escaped + "\\b";
    active().buf->last_search = active().buf->search_pattern;
    active().buf->search_highlight = true;
    search_forward_ = forward;
    DoIncrementalSearch();
    mode_ = Mode::NORMAL;
}

void ViEditor::UpdateSearchMatches() {
    if (active().buf->last_search.empty())
        return;
    EditorView& view = active();
    active().buf->search_pattern = active().buf->last_search;
    search_forward_ = true;
    std::string pat = active().buf->search_pattern;

    std::regex re;
    try {
        bool has_upper =
            std::any_of(pat.begin(), pat.end(), [](char c) {
                return std::isupper(static_cast<unsigned char>(c));
            });
        auto flags = std::regex::ECMAScript;
        if (!has_upper)
            flags |= std::regex::icase;
        re = std::regex(pat, flags);
    } catch (...) {
        return;
    }

    active().buf->search_matches.clear();
    for (int row = 0; row < static_cast<int>(view.buf->lines.size());
         ++row) {
        std::sregex_iterator it(view.buf->lines[row].begin(),
                                view.buf->lines[row].end(), re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            active().buf->search_matches.push_back(
                {row, static_cast<int>(it->position())});
            // Also store length for highlighting (we can recompute)
        }
    }
    // Don't change cursor
}

void ViEditor::ClearSearchHighlight() {
    active().buf->search_matches.clear();
    active().buf->current_match_idx = -1;
    active().buf->last_search.clear();
    active().buf->search_highlight = false;
}

// ============================================================================
// Command execution (expanded)
// ============================================================================

void ViEditor::ExecuteCommand() {
    EditorView& view = active();

    if (command_line_ == "w") {
        SaveFile(view);
    } else if (command_line_ == "q") {
        if (!view.buf->modified) {
            if (screen_)
                screen_->Exit();
            return;
        } else
            SetStatus("File modified (use :q! to discard)");
    } else if (command_line_ == "wq") {
        SaveFile(view);
        if (screen_)
            screen_->Exit();
        return;
    } else if (command_line_ == "q!") {
        if (screen_)
            screen_->Exit();
        return;
    } else if (command_line_.rfind("e ", 0) == 0) {
        view.buf->filename = command_line_.substr(2);
        LoadFile(view);
        view.cursor_row = view.cursor_col = view.top_row = 0;
        view.buf->modified = false;
    } else if (command_line_ == "e" || command_line_ == "e!") {
        // :e or :e! — reload current file from disk
        LoadFile(view);
        view.cursor_row = view.cursor_col = view.top_row = 0;
        view.buf->modified = false;
    } else if (command_line_ == "nohl" ||
               command_line_ == "nohlsearch") {
        ClearSearchHighlight();
    } else if (command_line_.rfind("map ", 0) == 0) {
        // :map from to
        std::string rest = command_line_.substr(4);
        size_t space = rest.find(' ');
        if (space != std::string::npos) {
            std::string from = rest.substr(0, space);
            std::string to = rest.substr(space + 1);
            AddMapping(from, to);
            SetStatus("Mapped " + from + " -> " + to);
        } else {
            SetStatus("Usage: :map <from> <to>");
        }
    } else if (command_line_.rfind("split ", 0) == 0 ||
               command_line_ == "sp") {
        if (command_line_.size() > 6) {
            // :split <filename> — create new view with its own buffer
            EditorView new_view;
            new_view.buf = std::make_shared<Buffer>();
            new_view.buf->filename = command_line_.substr(6);
            new_view.active = false;
            new_view.top_row = active().top_row;
            views_.push_back(std::move(new_view));
            active_view_ = static_cast<int>(views_.size()) - 1;
            split_horizontal_ = true;
            for (auto& v : views_)
                v.active = false;
            views_[active_view_].active = true;
            LoadFile(views_.back());
            SetStatus("Horizontal split created");
        } else {
            SplitHorizontal();
        }
    } else if (command_line_.rfind("vsplit ", 0) == 0 ||
               command_line_ == "vsp") {
        if (command_line_.size() > 7) {
            // :vsplit <filename> — create new view with its own buffer
            EditorView new_view;
            new_view.buf = std::make_shared<Buffer>();
            new_view.buf->filename = command_line_.substr(7);
            new_view.active = false;
            new_view.top_row = active().top_row;
            views_.push_back(std::move(new_view));
            active_view_ = static_cast<int>(views_.size()) - 1;
            split_horizontal_ = false;
            for (auto& v : views_)
                v.active = false;
            views_[active_view_].active = true;
            LoadFile(views_.back());
            SetStatus("Vertical split created");
        } else {
            SplitVertical();
        }
    } else if (command_line_.rfind("set ", 0) == 0) {
        std::string setting = command_line_.substr(4);
        if (setting == "hlsearch")
            active().buf->search_highlight = true;
        else if (setting == "nohlsearch")
            active().buf->search_highlight = false;
        else
            SetStatus("Unknown option: " + setting);
    } else {
        SetStatus("Unknown command: " + command_line_);
    }
    command_line_.clear();
    mode_ = Mode::NORMAL;
}

// ============================================================================
// Split views
// ============================================================================

void ViEditor::SplitHorizontal() {
    EditorView new_view = active();
    new_view.active = false;
    new_view.top_row = active().top_row;
    views_.push_back(std::move(new_view));
    active_view_ = static_cast<int>(views_.size()) - 1;
    split_horizontal_ = true;
    for (auto& v : views_)
        v.active = false;
    views_[active_view_].active = true;
    SetStatus("Horizontal split created");
}

void ViEditor::SplitVertical() {
    EditorView new_view = active();
    new_view.active = false;
    new_view.top_row = active().top_row;
    views_.push_back(std::move(new_view));
    active_view_ = static_cast<int>(views_.size()) - 1;
    split_horizontal_ = false;
    for (auto& v : views_)
        v.active = false;
    views_[active_view_].active = true;
    SetStatus("Vertical split created");
}

void ViEditor::CloseSplit() {
    if (views_.size() <= 1) {
        // Close the last view = quit
        if (!active().buf->modified) {
            if (screen_)
                screen_->Exit();
            return;
        } else
            SetStatus("File modified (use :q! to discard)");
        return;
    }
    views_.erase(views_.begin() + active_view_);
    if (active_view_ >= static_cast<int>(views_.size()))
        active_view_ = static_cast<int>(views_.size()) - 1;
    for (auto& v : views_)
        v.active = false;
    views_[active_view_].active = true;
    SetStatus("Split closed");
}

void ViEditor::NavigateSplit(char direction) {
    if (views_.size() <= 1)
        return;
    if (split_horizontal_) {
        if (direction == 'j' || direction == 'k') {
            FocusNextSplit();
        }
    } else {
        if (direction == 'l' || direction == 'h') {
            FocusNextSplit();
        }
    }
}

void ViEditor::FocusNextSplit() {
    if (views_.size() <= 1)
        return;
    views_[active_view_].active = false;
    active_view_ = (active_view_ + 1) % views_.size();
    views_[active_view_].active = true;
}

void ViEditor::FocusPrevSplit() {
    if (views_.size() <= 1)
        return;
    views_[active_view_].active = false;
    active_view_ = (active_view_ - 1 + views_.size()) % views_.size();
    views_[active_view_].active = true;
}

// ============================================================================
// Key mappings
// ============================================================================

void ViEditor::AddMapping(const std::string& from,
                          const std::string& to) {
    KeyMapping km;
    km.from_str = from;
    km.to_str = to;
    // Parse from string into event sequence
    for (size_t i = 0; i < from.size(); ++i) {
        if (from[i] == '<') {
            size_t end = from.find('>', i);
            if (end != std::string::npos) {
                km.from_seq.push_back(from.substr(i, end - i + 1));
                i = end;
            } else {
                km.from_seq.push_back(std::string(1, from[i]));
            }
        } else {
            km.from_seq.push_back(std::string(1, from[i]));
        }
    }
    for (size_t i = 0; i < to.size(); ++i) {
        if (to[i] == '<') {
            size_t end = to.find('>', i);
            if (end != std::string::npos) {
                km.to_seq.push_back(to.substr(i, end - i + 1));
                i = end;
            } else {
                km.to_seq.push_back(std::string(1, to[i]));
            }
        } else {
            km.to_seq.push_back(std::string(1, to[i]));
        }
    }
    key_mappings_.push_back(km);
}

std::vector<Event>
ViEditor::ApplyMappings(const std::vector<Event>& events) {
    // Simple placeholder – full mapping application would need
    // buffering
    return events;
}

// ============================================================================
// Rendering
// ============================================================================

Decorator ViEditor::CursorDecorator() const {
    switch (mode_) {
    case Mode::INSERT:
        return bgcolor(Color::Green) | color(Color::Black) | bold;
    case Mode::VISUAL:
    case Mode::VISUAL_LINE:
    case Mode::VISUAL_BLOCK:
        return bgcolor(Color::Magenta) | color(Color::White) | bold;
    case Mode::OPERATOR_PENDING:
        return bgcolor(Color::Yellow) | color(Color::Black) | bold;
    case Mode::COMMAND:
    case Mode::SEARCH_FORWARD:
    case Mode::SEARCH_BACKWARD:
        return bgcolor(Color::GrayDark) | color(Color::White);
    default: // NORMAL
        return bgcolor(Color::RGB(160, 82, 45)) | color(Color::White) |
               bold;
    }
}

Element ViEditor::RenderView(const EditorView& view) const {
    Elements lines_elements;
    // For horizontal splits: each view gets (dimy - 4 - (N-1)) / N rows
    int visible_rows = Terminal::Size().dimy - 3;
    if (split_horizontal_ && views_.size() > 1) {
        int avail = Terminal::Size().dimy - 3;
        int sep_count = static_cast<int>(views_.size()) - 1;
        visible_rows =
            (avail - sep_count) / static_cast<int>(views_.size());
    }
    int visible_cols = Terminal::Size().dimx;
    if (visible_cols < 20)
        visible_cols = 20;

    // Build a set of highlighted positions for search
    std::map<std::pair<int, int>, int>
        hl_lengths; // (row,col) -> match length
    if (view.buf->search_highlight && !view.buf->last_search.empty()) {
        std::string pat = view.buf->last_search;
        try {
            bool has_upper =
                std::any_of(pat.begin(), pat.end(), [](char c) {
                    return std::isupper(static_cast<unsigned char>(c));
                });
            auto flags = std::regex::ECMAScript;
            if (!has_upper)
                flags |= std::regex::icase;
            std::regex re(pat, flags);
            for (int row = 0; row < static_cast<int>(view.buf->lines.size());
                 ++row) {
                std::sregex_iterator it(view.buf->lines[row].begin(),
                                        view.buf->lines[row].end(), re);
                std::sregex_iterator end;
                for (; it != end; ++it) {
                    hl_lengths[{row,
                                static_cast<int>(it->position())}] =
                        static_cast<int>(it->length());
                }
            }
        } catch (...) {
        }
    }

    Decorator cursor_deco = CursorDecorator();
    bool show_cursor = (view.active && mode_ != Mode::COMMAND &&
                        mode_ != Mode::SEARCH_FORWARD &&
                        mode_ != Mode::SEARCH_BACKWARD);

    for (int i = 0; i < visible_rows; ++i) {
        int file_row = view.top_row + i;
        if (file_row >= 0 &&
            file_row < static_cast<int>(view.buf->lines.size())) {
            const std::string& line = view.buf->lines[file_row];
            bool is_cursor_row = (file_row == view.cursor_row);
            bool in_visual = visual_active_ && view.active;

            // ---- Visual character-wise ----
            if (in_visual && mode_ == Mode::VISUAL) {
                int vis_start =
                    std::min(visual_sel_.anchor_col, view.cursor_col);
                int vis_end =
                    std::max(visual_sel_.anchor_col, view.cursor_col);
                int anchor_row = visual_sel_.anchor_row;
                int cursor_row_v = view.cursor_row;
                int sr = std::min(anchor_row, cursor_row_v);
                int er = std::max(anchor_row, cursor_row_v);
                int line_len = static_cast<int>(line.size());
                int start_col = view.left_col;
                int end_col = start_col + visible_cols;

                Elements parts;
                for (int c = start_col; c < end_col; ++c) {
                    bool in_sel = false;
                    if (file_row > sr && file_row < er)
                        in_sel = true;
                    else if (file_row == sr && file_row == er)
                        in_sel = (c >= vis_start && c <= vis_end);
                    else if (file_row == sr)
                        in_sel = (c >= (anchor_row <= cursor_row_v
                                            ? visual_sel_.anchor_col
                                            : view.cursor_col));
                    else if (file_row == er)
                        in_sel = (c <= (anchor_row <= cursor_row_v
                                            ? view.cursor_col
                                            : visual_sel_.anchor_col));

                    bool at_cursor =
                        (is_cursor_row && c == view.cursor_col &&
                         show_cursor);
                    std::string ch_str =
                        (c < line_len) ? std::string(1, line[c]) : " ";

                    if (at_cursor)
                        parts.push_back(text(ch_str) | cursor_deco);
                    else if (in_sel)
                        parts.push_back(text(ch_str) | inverted);
                    else
                        parts.push_back(text(ch_str));
                }
                lines_elements.push_back(hbox(std::move(parts)));

                // ---- Visual line-wise ----
            } else if (in_visual && mode_ == Mode::VISUAL_LINE) {
                int sr =
                    std::min(visual_sel_.anchor_row, view.cursor_row);
                int er =
                    std::max(visual_sel_.anchor_row, view.cursor_row);
                bool in_sel = (file_row >= sr && file_row <= er);
                int line_len = static_cast<int>(line.size());
                int start_col = view.left_col;
                int end_col = start_col + visible_cols;

                Elements parts;
                for (int c = start_col; c < end_col; ++c) {
                    bool at_cursor =
                        (is_cursor_row && c == view.cursor_col &&
                         show_cursor);
                    std::string ch_str =
                        (c < line_len) ? std::string(1, line[c]) : " ";

                    if (at_cursor)
                        parts.push_back(text(ch_str) | cursor_deco);
                    else if (in_sel)
                        parts.push_back(text(ch_str) | inverted);
                    else
                        parts.push_back(text(ch_str));
                }
                lines_elements.push_back(hbox(std::move(parts)));

                // ---- Visual block-wise ----
            } else if (in_visual && mode_ == Mode::VISUAL_BLOCK) {
                int col_start =
                    std::min(visual_sel_.anchor_col, view.cursor_col);
                int col_end =
                    std::max(visual_sel_.anchor_col, view.cursor_col);
                int sr =
                    std::min(visual_sel_.anchor_row, view.cursor_row);
                int er =
                    std::max(visual_sel_.anchor_row, view.cursor_row);
                bool row_in_sel = (file_row >= sr && file_row <= er);
                int line_len = static_cast<int>(line.size());
                int start_col = view.left_col;
                int end_col = start_col + visible_cols;

                Elements parts;
                for (int c = start_col; c < end_col; ++c) {
                    bool in_sel =
                        (row_in_sel && c >= col_start && c <= col_end);
                    bool at_cursor =
                        (is_cursor_row && c == view.cursor_col &&
                         show_cursor);
                    std::string ch_str =
                        (c < line_len) ? std::string(1, line[c]) : " ";

                    if (at_cursor)
                        parts.push_back(text(ch_str) | cursor_deco);
                    else if (in_sel)
                        parts.push_back(text(ch_str) | inverted);
                    else
                        parts.push_back(text(ch_str));
                }
                lines_elements.push_back(hbox(std::move(parts)));

                // ---- Normal / Insert / Operator-pending ----
            } else {
                int line_len = static_cast<int>(line.size());
                int start_col = view.left_col;
                int end_col = start_col + visible_cols;

                // Get syntax highlighting spans for this line (cached)
                const auto& syntax_spans =
                    view.buf->highlighter.Highlight(file_row, line);

                Elements parts;
                for (int c = start_col; c < end_col; ++c) {
                    bool hl = false;
                    for (const auto& [pos, len] : hl_lengths) {
                        if (pos.first == file_row && c >= pos.second &&
                            c < pos.second + len) {
                            hl = true;
                            break;
                        }
                    }

                    // Check syntax highlight (lower priority than
                    // search)
                    Decorator syn_deco = nothing;
                    if (!hl) {
                        for (const auto& span : syntax_spans) {
                            if (c >= span.start && c < span.end) {
                                syn_deco = span.deco;
                                break;
                            }
                        }
                    }

                    bool at_cursor =
                        (is_cursor_row && c == view.cursor_col &&
                         show_cursor);
                    std::string ch_str =
                        (c < line_len) ? std::string(1, line[c]) : " ";

                    if (at_cursor)
                        parts.push_back(text(ch_str) | cursor_deco);
                    else if (hl && !ch_str.empty())
                        parts.push_back(
                            text(ch_str) | color(Color::White) |
                            bgcolor(Color::RGB(255, 105, 180)));
                    else if (!ch_str.empty())
                        parts.push_back(text(ch_str) | syn_deco);
                }
                lines_elements.push_back(hbox(std::move(parts)));
            }
        } else {
            lines_elements.push_back(text("~") | dim);
        }
    }

    auto result = vbox(std::move(lines_elements));
    return result;
}

Element ViEditor::RenderStatusBar() const {
    const EditorView& view = active();

    std::string mode_str;
    switch (mode_) {
    case Mode::NORMAL:
        mode_str = "NORMAL";
        break;
    case Mode::INSERT:
        mode_str = "INSERT";
        break;
    case Mode::COMMAND:
        mode_str = "COMMAND";
        break;
    case Mode::OPERATOR_PENDING:
        mode_str = "OP-PENDING";
        break;
    case Mode::SEARCH_FORWARD:
        mode_str = "SEARCH /";
        break;
    case Mode::SEARCH_BACKWARD:
        mode_str = "SEARCH ?";
        break;
    case Mode::VISUAL:
        mode_str = "VISUAL";
        break;
    case Mode::VISUAL_LINE:
        mode_str = "VISUAL LINE";
        break;
    case Mode::VISUAL_BLOCK:
        mode_str = "VISUAL BLOCK";
        break;
    }

    std::string mod_str = view.buf->modified ? "+" : " ";
    std::string pending =
        (pending_count_ > 0
             ? " [" + std::to_string(pending_count_) + "]"
             : "");
    std::string count_str =
        (count_acc_ > 0 ? " count:" + std::to_string(count_acc_) : "");

    std::string left = mode_str + mod_str + pending + count_str + " " +
                       view.buf->filename +
                       "  lines:" + std::to_string(view.buf->lines.size());

    if (!status_msg_.empty() && status_timeout_ > 0)
        left = status_msg_;

    std::string right = "Ln " + std::to_string(view.cursor_row + 1) +
                        ", Col " + std::to_string(view.cursor_col + 1);

    // Position indicator: Top / Bot / xx%
    int total = static_cast<int>(view.buf->lines.size());
    int cur = view.cursor_row + 1;
    if (cur == 1)
        right += "  Top";
    else if (cur == total)
        right += "  Bot";
    else if (total > 0)
        right += "  " + std::to_string((cur * 100) / total) + "%";

    if (views_.size() > 1)
        right += " [" + std::to_string(active_view_ + 1) + "/" +
                 std::to_string(views_.size()) + "]";

    int width = Terminal::Size().dimx;
    int padding = width - static_cast<int>(left.size()) -
                  static_cast<int>(right.size());
    if (padding < 0)
        padding = 0;
    std::string status = left + std::string(padding, ' ') + right;
    return text(status) | bold | bgcolor(Color::RGB(160, 82, 45)) |
           color(Color::White);
}

Element ViEditor::RenderCommandLine() const {
    if (mode_ == Mode::COMMAND)
        return text(":" + command_line_ + "█") | color(Color::Yellow);
    if (mode_ == Mode::SEARCH_FORWARD)
        return text("/" + active().buf->search_pattern + "█") |
               color(Color::Yellow);
    if (mode_ == Mode::SEARCH_BACKWARD)
        return text("?" + active().buf->search_pattern + "█") |
               color(Color::Yellow);
    // Always occupy a row so the layout doesn't shift when entering
    // command mode
    return text("") | size(HEIGHT, EQUAL, 1);
}

Element ViEditor::RenderAllViews() const {
    auto cmd_line = RenderCommandLine();
    if (views_.size() <= 1) {
        return vbox({
            RenderView(active()) | flex_grow,
            separator(),
            RenderStatusBar() | size(HEIGHT, EQUAL, 1),
            cmd_line | size(HEIGHT, EQUAL, 1),
        });
    }

    // For splits, compute exact heights so nothing gets pushed
    // off-screen
    int total_height = Terminal::Size().dimy;
    int fixed_rows = 3; // main separator(1) + status(1) + cmd(1)
    int avail = total_height - fixed_rows;

    Elements view_elements;
    if (split_horizontal_) {
        int sep_count = static_cast<int>(views_.size()) - 1;
        int view_rows =
            (avail - sep_count) / static_cast<int>(views_.size());
        for (size_t i = 0; i < views_.size(); ++i) {
            view_elements.push_back(RenderView(views_[i]) |
                                    size(HEIGHT, EQUAL, view_rows));
            if (i + 1 < views_.size())
                view_elements.push_back(separator() |
                                        color(Color::RGB(160, 82, 45)));
        }
    } else {
        for (size_t i = 0; i < views_.size(); ++i)
            view_elements.push_back(RenderView(views_[i]) | flex_grow);
    }

    auto views_box = split_horizontal_ ? vbox(std::move(view_elements))
                                       : hbox(std::move(view_elements));

    return vbox({
        views_box | size(HEIGHT, EQUAL, avail),
        separator(),
        RenderStatusBar() | size(HEIGHT, EQUAL, 1),
        cmd_line | size(HEIGHT, EQUAL, 1),
    });
}

// ============================================================================
// Event processing
// ============================================================================

// Convert a key description string (like "w", "<CR>", "<Space>") to an
// Event
static Event EventFromString(const std::string& s) {
    if (s.size() == 1 && s[0] >= 32 && s[0] <= 126)
        return Event::Character(s[0]);
    if (s == "<CR>")
        return Event::Return;
    if (s == "<Esc>")
        return Event::Escape;
    if (s == "<Tab>")
        return Event::Tab;
    if (s == "<BS>")
        return Event::Backspace;
    if (s == "<Del>")
        return Event::Delete;
    if (s == "<Space>")
        return Event::Character(' ');
    if (s == "<Up>")
        return Event::ArrowUp;
    if (s == "<Down>")
        return Event::ArrowDown;
    if (s == "<Left>")
        return Event::ArrowLeft;
    if (s == "<Right>")
        return Event::ArrowRight;
    if (s.size() == 3 && s[0] == 'C' && s[1] == '-') {
        char c = s[2];
        if (c >= 'A' && c <= 'Z')
            return Event::Special({static_cast<char>(c - 'A' + 1)});
        if (c >= 'a' && c <= 'z')
            return Event::Special({static_cast<char>(c - 'a' + 1)});
    }
    // Fallback: treat as character
    if (s.size() == 1)
        return Event::Character(s[0]);
    return Event::Character('?');
}

// Convert an Event to a string key description for mapping matching
static std::string EventToMappingStr(const Event& e) {
    if (e == Event::Return)
        return "<CR>";
    if (e == Event::Escape)
        return "<Esc>";
    if (e == Event::Tab)
        return "<Tab>";
    if (e == Event::Backspace)
        return "<BS>";
    if (e == Event::Delete)
        return "<Del>";
    if (e == Event::ArrowUp)
        return "<Up>";
    if (e == Event::ArrowDown)
        return "<Down>";
    if (e == Event::ArrowLeft)
        return "<Left>";
    if (e == Event::ArrowRight)
        return "<Right>";
    if (e.is_character()) {
        std::string c = e.character();
        if (c.size() == 1) {
            unsigned char uc = static_cast<unsigned char>(c[0]);
            if (uc == ' ')
                return "<Space>";
            if (uc < 32) {
                char name = 'A' + uc - 1;
                return std::string("C-") + name;
            }
            if (uc == 127)
                return "<BS>";
            return c;
        }
    }
    if (e == Event::Special({23}))
        return "C-W";
    if (e == Event::Special({22}))
        return "C-V";
    if (e == Event::Special({4}))
        return "C-D";
    if (e == Event::Special({21}))
        return "C-U";
    if (e == Event::Special({18}))
        return "C-R";
    if (e == Event::Special({15}))
        return "C-O";
    return "";
}

bool ViEditor::OnEvent(Event event) {
    // Tick status timeout
    if (status_timeout_ > 0)
        --status_timeout_;

    // Global Ctrl-C: behaves like Vim — aborts pending operations and
    // returns to Normal mode (unlike Escape, does not trigger
    // InsertLeave finalization or cursor adjustment)
    if (event == Event::Special({3})) {
        // Cancel all pending multi-key state machines
        g_pending_ = false;
        z_pending_ = false;
        Z_pending_ = false;
        find_pending_ = false;
        text_obj_pending_ = false;
        ctrl_w_pending_ = false;
        ctrl_o_pending_ = 0;
        pending_op_ = OperatorType::NONE;
        pending_count_ = 0;
        motion_count_ = 0;
        count_acc_ = 0;
        mapping_buffer_.clear();
        recording_insert_ = false;
        block_change_pending_ = false;

        if (mode_ == Mode::INSERT) {
            // Exit insert mode: finalize word-boundary undo but
            // cancel block changes, don't record for dot, don't
            // move cursor left (unlike Escape)
            FinalizeInsertUndo(active());
            mode_ = Mode::NORMAL;
            SetStatus("Interrupted");
            return true;
        }
        if (mode_ == Mode::COMMAND) {
            command_line_.clear();
            mode_ = Mode::NORMAL;
            SetStatus("Interrupted");
            return true;
        }
        if (mode_ == Mode::SEARCH_FORWARD ||
            mode_ == Mode::SEARCH_BACKWARD) {
            active().buf->search_matches.clear();
            active().buf->search_pattern.clear();
            active().buf->current_match_idx = -1;
            command_line_.clear();
            mode_ = Mode::NORMAL;
            SetStatus("Interrupted");
            return true;
        }
        if (mode_ == Mode::VISUAL || mode_ == Mode::VISUAL_LINE ||
            mode_ == Mode::VISUAL_BLOCK) {
            ExitVisual();
            SetStatus("Interrupted");
            return true;
        }
        if (mode_ == Mode::OPERATOR_PENDING) {
            mode_ = Mode::NORMAL;
            SetStatus("Interrupted");
            return true;
        }
        // Normal mode: just clear any pending state (already done above)
        if (mode_ == Mode::NORMAL) {
            SetStatus("Interrupted");
            return true;
        }
        return true;
    }

    // Global Escape to return to normal mode
    if (event == Event::Escape) {
        if (mode_ != Mode::NORMAL && mode_ != Mode::INSERT) {
            if (mode_ == Mode::SEARCH_FORWARD ||
                mode_ == Mode::SEARCH_BACKWARD) {
                // On escape from search, go back to original position
                active().buf->search_matches.clear();
                active().buf->search_pattern.clear();
                active().buf->current_match_idx = -1;
            }
            if (mode_ == Mode::VISUAL || mode_ == Mode::VISUAL_LINE ||
                mode_ == Mode::VISUAL_BLOCK) {
                ExitVisual();
            }
            mode_ = Mode::NORMAL;
            pending_op_ = OperatorType::NONE;
            pending_count_ = 0;
            motion_count_ = 0;
            count_acc_ = 0;
            return true;
        }
        // In insert mode, escape goes to normal (already handled in
        // OnInsertEvent)
    }

    // ---- Key mapping buffer (normal mode only) ----
    // Skip when replaying injected events, or when a multi-key command
    // is pending
    bool pending = g_pending_ || z_pending_ || Z_pending_ ||
                   find_pending_ || text_obj_pending_ ||
                   ctrl_w_pending_ || (ctrl_o_pending_ != 0);
    if (mode_ == Mode::NORMAL && !key_mappings_.empty() && !pending &&
        !replaying_) {
        std::string key_str = EventToMappingStr(event);
        if (!key_str.empty()) {
            mapping_buffer_.push_back(key_str);

            // Check against all mappings
            bool prefix_match = false;
            for (const auto& km : key_mappings_) {
                if (km.from_seq.size() < mapping_buffer_.size())
                    continue;
                bool match = true;
                for (size_t i = 0; i < mapping_buffer_.size(); ++i) {
                    if (i >= km.from_seq.size() ||
                        mapping_buffer_[i] != km.from_seq[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    if (km.from_seq.size() == mapping_buffer_.size()) {
                        // Exact match: replay mapped events inline
                        std::vector<std::string> to_seq = km.to_seq;
                        mapping_buffer_.clear();
                        replaying_ = true;
                        for (const auto& s : to_seq)
                            OnEvent(EventFromString(s));
                        replaying_ = false;
                        return true;
                    }
                    prefix_match = true;
                }
            }
            if (prefix_match)
                return true; // keep buffering

            // No match: replay buffered events inline
            std::vector<std::string> saved = mapping_buffer_;
            mapping_buffer_.clear();
            replaying_ = true;
            for (const auto& s : saved)
                OnEvent(EventFromString(s));
            replaying_ = false;
            return true;
        }
    }

    // Ctrl-O state machine: 1 = enter NORMAL now, 2 = return to INSERT
    // after command
    if (ctrl_o_pending_ == 1) {
        ctrl_o_pending_ = 2;
        mode_ = Mode::NORMAL;
    }

    // Dispatch based on mode
    bool handled = false;
    switch (mode_) {
    case Mode::NORMAL:
        handled = OnNormalEvent(event);
        break;
    case Mode::INSERT:
        handled = OnInsertEvent(event);
        break;
    case Mode::COMMAND:
        handled = OnCommandEvent(event);
        break;
    case Mode::OPERATOR_PENDING:
        handled = OnOperatorPendingEvent(event);
        break;
    case Mode::SEARCH_FORWARD:
    case Mode::SEARCH_BACKWARD:
        handled = OnSearchEvent(event);
        break;
    case Mode::VISUAL:
        handled = OnVisualEvent(event);
        break;
    case Mode::VISUAL_LINE:
        handled = OnVisualLineEvent(event);
        break;
    case Mode::VISUAL_BLOCK:
        handled = OnVisualBlockEvent(event);
        break;
    }

    // Ctrl-O: after one normal-mode command completes, return to insert
    // mode. Only switch back if no pending state (gg, f, etc.) is
    // waiting.
    if (ctrl_o_pending_ == 2 && mode_ == Mode::NORMAL && !g_pending_ &&
        !z_pending_ && !Z_pending_ && !find_pending_ &&
        !text_obj_pending_ && !ctrl_w_pending_ &&
        !block_change_pending_) {
        ctrl_o_pending_ = 0;
        mode_ = Mode::INSERT;
    }

    return handled;
}

// ---- Normal mode ----
bool ViEditor::OnNormalEvent(Event event) {
    EditorView& view = active();

    // Accumulate digits for count
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1 &&
            std::isdigit(static_cast<unsigned char>(ch[0])) &&
            ch[0] != '0') {
            count_acc_ = count_acc_ * 10 + (ch[0] - '0');
            return true;
        }
        // '0' is special: if no count accumulated, it goes to start of
        // line
        if (ch.size() == 1 && ch[0] == '0' && count_acc_ == 0) {
            view.cursor_col = 0;
            UpdateScroll(view);
            return true;
        }
    }

    int count = count_acc_ > 0 ? count_acc_ : 1;

    // Ctrl-W prefix for split navigation
    if (ctrl_w_pending_) {
        ctrl_w_pending_ = false;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char c = ch[0];
                if (c == 'h' || c == 'j' || c == 'k' || c == 'l') {
                    NavigateSplit(c);
                    count_acc_ = 0;
                    return true;
                } else if (c == 'w') {
                    FocusNextSplit();
                    count_acc_ = 0;
                    return true;
                } else if (c == 'W') {
                    FocusPrevSplit();
                    count_acc_ = 0;
                    return true;
                } else if (c == 'v') {
                    SplitVertical();
                    count_acc_ = 0;
                    return true;
                } else if (c == 's') {
                    SplitHorizontal();
                    count_acc_ = 0;
                    return true;
                } else if (c == 'q' || c == 'c') {
                    CloseSplit();
                    count_acc_ = 0;
                    return true;
                }
            }
        }
        // Invalid follow-up key: cancel Ctrl-W, let it fall through to
        // normal processing
        count_acc_ = 0;
    }
    if (event == Event::Special({23})) { // Ctrl-W
        ctrl_w_pending_ = true;
        count_acc_ = 0;
        return true;
    }

    // g pending for gg
    if (g_pending_) {
        g_pending_ = false;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char c = ch[0];
                if (c == 'g') {
                    // gg: go to first line (or line specified by count)
                    if (count_acc_ > 0) {
                        view.cursor_row = std::min(
                            count_acc_ - 1,
                            static_cast<int>(view.buf->lines.size()) - 1);
                    } else {
                        view.cursor_row = 0;
                    }
                    view.cursor_col = 0;
                    UpdateScroll(view);
                    count_acc_ = 0;
                    return true;
                }
                if (c == 'e') {
                    // ge: go to end of previous word
                    auto pos = MotionWordEndBackward(view, count);
                    view.cursor_row = pos.row;
                    view.cursor_col = pos.col;
                    UpdateScroll(view);
                    count_acc_ = 0;
                    return true;
                }
                if (c == 'E') {
                    // gE: go to end of previous WORD
                    auto pos = MotionBigWordEndBackward(view, count);
                    view.cursor_row = pos.row;
                    view.cursor_col = pos.col;
                    UpdateScroll(view);
                    count_acc_ = 0;
                    return true;
                }
                if (c == 'R') {
                    // gR: reload current file from disk
                    LoadFile(view);
                    view.cursor_row = view.cursor_col = view.top_row =
                        0;
                    view.buf->modified = false;
                    SetStatus("Reloaded " + view.buf->filename);
                    count_acc_ = 0;
                    return true;
                }
            }
        }
        count_acc_ = 0;
        return true;
    }

    // z pending for zz, zt, zb
    if (z_pending_) {
        z_pending_ = false;
        count_acc_ = 0;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char c = ch[0];
                int visible = Terminal::Size().dimy - 3;
                if (split_horizontal_ && views_.size() > 1) {
                    int avail = Terminal::Size().dimy - 3;
                    int sep_count = static_cast<int>(views_.size()) - 1;
                    visible = (avail - sep_count) /
                              static_cast<int>(views_.size());
                }
                if (c == 'z') {
                    // zz: center cursor
                    view.top_row =
                        std::max(0, view.cursor_row - visible / 2);
                } else if (c == 't') {
                    // zt: cursor at top
                    view.top_row = view.cursor_row;
                } else if (c == 'b') {
                    // zb: cursor at bottom
                    view.top_row =
                        std::max(0, view.cursor_row - visible + 1);
                }
                int max_top = std::max(
                    0, static_cast<int>(view.buf->lines.size()) - visible);
                if (view.top_row > max_top)
                    view.top_row = max_top;
                UpdateScroll(view);
                return true;
            }
        }
        return true;
    }

    // Z pending for ZZ, ZQ
    if (Z_pending_) {
        Z_pending_ = false;
        count_acc_ = 0;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char c = ch[0];
                if (c == 'Z') {
                    // ZZ: save and quit
                    SaveFile(view);
                    if (screen_)
                        screen_->Exit();
                    return true;
                } else if (c == 'Q') {
                    // ZQ: quit without saving
                    if (screen_)
                        screen_->Exit();
                    return true;
                }
            }
        }
        return true;
    }

    // find/till pending (f, F, t, T)
    if (find_pending_) {
        find_pending_ = false;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char target = ch[0];
                last_find_char_ = target;
                CursorPos pos;
                if (last_find_forward_) {
                    pos = MotionFindForward(view, target, count,
                                            last_find_till_);
                } else {
                    pos = MotionFindBackward(view, target, count,
                                             last_find_till_);
                }
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
        }
        count_acc_ = 0;
        return true;
    }

    // Operators (enter operator-pending mode)
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1) {
            char c = ch[0];
            switch (c) {
            case 'd':
                pending_op_ = OperatorType::DELETE_OP;
                pending_count_ = count;
                mode_ = Mode::OPERATOR_PENDING;
                prev_mode_ = Mode::NORMAL;
                count_acc_ = 0;
                return true;
            case 'c':
                pending_op_ = OperatorType::CHANGE;
                pending_count_ = count;
                mode_ = Mode::OPERATOR_PENDING;
                prev_mode_ = Mode::NORMAL;
                count_acc_ = 0;
                return true;
            case 'y':
                pending_op_ = OperatorType::YANK;
                pending_count_ = count;
                mode_ = Mode::OPERATOR_PENDING;
                prev_mode_ = Mode::NORMAL;
                count_acc_ = 0;
                return true;
            case '>':
                pending_op_ = OperatorType::INDENT;
                pending_count_ = count;
                mode_ = Mode::OPERATOR_PENDING;
                prev_mode_ = Mode::NORMAL;
                count_acc_ = 0;
                return true;
            case '<':
                pending_op_ = OperatorType::OUTDENT;
                pending_count_ = count;
                mode_ = Mode::OPERATOR_PENDING;
                prev_mode_ = Mode::NORMAL;
                count_acc_ = 0;
                return true;
            case '~': {
                Range r = {view.cursor_row, view.cursor_col,
                           view.cursor_row, view.cursor_col + count};
                ExecuteOperator(view, OperatorType::TILDE_CASE, r);
                MoveCursor(view, 0, count);
                count_acc_ = 0;
                return true;
            }
            case 'D': {
                // Delete to end of line (like d$)
                int end_col = static_cast<int>(
                    view.buf->lines[view.cursor_row].size());
                Range r = {view.cursor_row, view.cursor_col,
                           view.cursor_row, end_col};
                ExecuteOperator(view, OperatorType::DELETE_OP, r);
                count_acc_ = 0;
                return true;
            }
            case 'C': {
                // Change to end of line (like c$)
                int end_col = static_cast<int>(
                    view.buf->lines[view.cursor_row].size());
                Range r = {view.cursor_row, view.cursor_col,
                           view.cursor_row, end_col};
                ExecuteOperator(view, OperatorType::CHANGE, r);
                mode_ = Mode::INSERT;
                BeginInsertUndo(view);
                count_acc_ = 0;
                return true;
            }
            case 'Y': {
                // Yank to end of line (like y$)
                int end_col = static_cast<int>(
                    view.buf->lines[view.cursor_row].size());
                Range r = {view.cursor_row, view.cursor_col,
                           view.cursor_row, end_col};
                ExecuteOperator(view, OperatorType::YANK, r);
                count_acc_ = 0;
                return true;
            }
            // Special: dd, cc, yy (double-tap = linewise)
            default:
                break;
            }
        }
    }

    // Handle immediate keys (not operators)
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1) {
            char c = ch[0];

            // Basic motions
            if (c == 'h') {
                for (int i = 0; i < count; ++i)
                    MoveCursor(view, 0, -1);
                count_acc_ = 0;
                return true;
            }
            if (c == 'j') {
                for (int i = 0; i < count; ++i)
                    MoveCursor(view, 1, 0);
                count_acc_ = 0;
                return true;
            }
            if (c == 'k') {
                for (int i = 0; i < count; ++i)
                    MoveCursor(view, -1, 0);
                count_acc_ = 0;
                return true;
            }
            if (c == 'l') {
                for (int i = 0; i < count; ++i)
                    MoveCursor(view, 0, 1);
                count_acc_ = 0;
                return true;
            }
            if (c == '$') {
                view.cursor_col = static_cast<int>(
                    view.buf->lines[view.cursor_row].size());
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == '^') {
                auto pos = MotionFirstNonBlank(view);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }

            // Word motions
            if (c == 'w') {
                auto pos = MotionWordForward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == 'b') {
                auto pos = MotionWordBackward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == 'e') {
                auto pos = MotionWordEndForward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == 'W') {
                auto pos = MotionBigWordForward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == 'B') {
                auto pos = MotionBigWordBackward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == 'E') {
                auto pos = MotionBigWordEndForward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }

            // g prefix
            if (c == 'g') {
                g_pending_ = true;
                // count_acc_ preserved for gg line number
                return true;
            }

            // z prefix for zz, zt, zb
            if (c == 'z') {
                z_pending_ = true;
                count_acc_ = 0;
                return true;
            }

            // Z prefix for ZZ, ZQ
            if (c == 'Z') {
                Z_pending_ = true;
                count_acc_ = 0;
                return true;
            }

            if (c == 'G') {
                if (count_acc_ > 0) {
                    view.cursor_row = std::min(
                        count - 1,
                        static_cast<int>(view.buf->lines.size()) - 1);
                } else {
                    view.cursor_row =
                        static_cast<int>(view.buf->lines.size()) - 1;
                }
                view.cursor_col = 0;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }

            // f, F, t, T
            if (c == 'f' || c == 'F' || c == 't' || c == 'T') {
                find_pending_ = true;
                find_pending_cmd_ = c;
                last_find_forward_ = (c == 'f' || c == 't');
                last_find_till_ = (c == 't' || c == 'T');
                count_acc_ = count; // preserve count for the motion
                return true;
            }

            // ; and , repeat last f/t/F/T
            if (c == ';' || c == ',') {
                if (last_find_char_ != 0) {
                    bool forward = (c == ';') ? last_find_forward_
                                              : !last_find_forward_;
                    CursorPos pos;
                    if (forward)
                        pos = MotionFindForward(view, last_find_char_,
                                                count, last_find_till_);
                    else
                        pos =
                            MotionFindBackward(view, last_find_char_,
                                               count, last_find_till_);
                    view.cursor_row = pos.row;
                    view.cursor_col = pos.col;
                }
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }

            // % - match bracket
            if (c == '%') {
                auto pos = MotionPercentMatch(view);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }

            // { and } paragraph motions
            if (c == '}') {
                auto pos = MotionParagraphForward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }
            if (c == '{') {
                auto pos = MotionParagraphBackward(view, count);
                view.cursor_row = pos.row;
                view.cursor_col = pos.col;
                UpdateScroll(view);
                count_acc_ = 0;
                return true;
            }

            // x - delete char
            if (c == 'x') {
                Range r = {view.cursor_row, view.cursor_col,
                           view.cursor_row, view.cursor_col + count};
                PushUndo(view, view.cursor_row, 1);
                // Before deleting, yank
                active().buf->yank_register = ExtractRange(view, r);
                active().buf->yank_linewise = false;
                for (int i = 0; i < count; ++i)
                    DeleteChar(view);
                UpdateScroll(view);
                RecordChange(OperatorType::DELETE_OP, r, false);
                count_acc_ = 0;
                return true;
            }

            // i, a, I, A, o, O - enter insert mode
            if (c == 'i') {
                BeginInsertUndo(view);
                mode_ = Mode::INSERT;
                recording_insert_ = true;
                insert_recording_.clear();
                count_acc_ = 0;
                return true;
            }
            if (c == 'a') {
                BeginInsertUndo(view);
                MoveCursor(view, 0, 1);
                count_acc_ = 0;
                mode_ = Mode::INSERT;
                recording_insert_ = true;
                insert_recording_.clear();
                return true;
            }
            if (c == 'I') {
                BeginInsertUndo(view);
                view.cursor_col = MotionFirstNonBlank(view).col;
                UpdateScroll(view);
                count_acc_ = 0;
                mode_ = Mode::INSERT;
                recording_insert_ = true;
                insert_recording_.clear();
                return true;
            }
            if (c == 'A') {
                BeginInsertUndo(view);
                view.cursor_col = static_cast<int>(
                    view.buf->lines[view.cursor_row].size());
                UpdateScroll(view);
                count_acc_ = 0;
                mode_ = Mode::INSERT;
                recording_insert_ = true;
                insert_recording_.clear();
                return true;
            }
            if (c == 'o') {
                BeginInsertUndo(view);
                PushUndo(view, view.cursor_row, 1);
                view.buf->lines.insert(
                    view.buf->lines.begin() + view.cursor_row + 1, "");
                ++view.cursor_row;
                view.cursor_col = 0;
                view.buf->modified = true;
                UpdateScroll(view);
                count_acc_ = 0;
                mode_ = Mode::INSERT;
                recording_insert_ = true;
                insert_recording_.clear();
                return true;
            }
            if (c == 'O') {
                BeginInsertUndo(view);
                PushUndo(view, view.cursor_row, 1);
                view.buf->lines.insert(view.buf->lines.begin() + view.cursor_row,
                                  "");
                view.cursor_col = 0;
                view.buf->modified = true;
                UpdateScroll(view);
                count_acc_ = 0;
                mode_ = Mode::INSERT;
                recording_insert_ = true;
                insert_recording_.clear();
                return true;
            }

            // p, P - put/paste
            if (c == 'p') {
                for (int i = 0; i < count; ++i)
                    PutAfter(view, active().buf->yank_register,
                             active().buf->yank_linewise,
                             active().buf->yank_blockwise);
                count_acc_ = 0;
                return true;
            }
            if (c == 'P') {
                for (int i = 0; i < count; ++i)
                    PutBefore(view, active().buf->yank_register,
                              active().buf->yank_linewise,
                              active().buf->yank_blockwise);
                count_acc_ = 0;
                return true;
            }

            // . - dot repeat
            if (c == '.') {
                for (int i = 0; i < count; ++i)
                    RepeatLastChange();
                count_acc_ = 0;
                return true;
            }

            // u - undo
            if (c == 'u') {
                for (int i = 0; i < count; ++i)
                    Undo(view);
                count_acc_ = 0;
                return true;
            }

            // Visual modes
            if (c == 'v') {
                EnterVisual(false, false);
                count_acc_ = 0;
                return true;
            }
            if (c == 'V') {
                EnterVisual(true, false);
                count_acc_ = 0;
                return true;
            }

            // Search
            if (c == '/') {
                StartSearch(true);
                count_acc_ = 0;
                return true;
            }
            if (c == '?') {
                StartSearch(false);
                count_acc_ = 0;
                return true;
            }
            if (c == '*') {
                SearchWordUnderCursor(true);
                count_acc_ = 0;
                return true;
            }
            if (c == '#') {
                SearchWordUnderCursor(false);
                count_acc_ = 0;
                return true;
            }
            if (c == 'n') {
                SearchNext(true);
                count_acc_ = 0;
                return true;
            }
            if (c == 'N') {
                SearchNext(false);
                count_acc_ = 0;
                return true;
            }

            // Command mode
            if (c == ':') {
                mode_ = Mode::COMMAND;
                command_line_.clear();
                count_acc_ = 0;
                return true;
            }
        }
    }

    // Ctrl-V for visual block (FTXUI sends Ctrl+key as Event::Special)
    if (event == Event::Special({22})) {
        EnterVisual(false, true);
        count_acc_ = 0;
        return true;
    }

    // Ctrl-D / Ctrl-U: scroll half-page down/up
    if (event == Event::Special({4}) || event == Event::Special({21})) {
        int half = std::max(1, Terminal::Size().dimy / 2);
        int dir = (event == Event::Special({4}))
                      ? 1
                      : -1; // 4=Ctrl-D(down), 21=Ctrl-U(up)
        int amount = half * count;
        view.cursor_row =
            std::clamp(view.cursor_row + dir * amount, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = std::min(
            view.cursor_col,
            static_cast<int>(view.buf->lines[view.cursor_row].size()));
        // Scroll the view so cursor stays centered-ish
        view.top_row = std::clamp(
            view.cursor_row - half / 2, 0,
            std::max(0, static_cast<int>(view.buf->lines.size()) - half));
        UpdateScroll(view);
        count_acc_ = 0;
        return true;
    }

    // Ctrl-R: redo
    if (event == Event::Special({18})) {
        for (int i = 0; i < count; ++i)
            Redo(view);
        count_acc_ = 0;
        return true;
    }

    // Enter: scroll half-page down (like Ctrl-D)
    if (event == Event::Return) {
        int half = std::max(1, Terminal::Size().dimy / 2);
        int amount = half * count;
        view.cursor_row =
            std::clamp(view.cursor_row + amount, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = std::min(
            view.cursor_col,
            static_cast<int>(view.buf->lines[view.cursor_row].size()));
        view.top_row = std::clamp(
            view.cursor_row - half / 2, 0,
            std::max(0, static_cast<int>(view.buf->lines.size()) - half));
        UpdateScroll(view);
        count_acc_ = 0;
        return true;
    }

    // Backspace: scroll half-page up (like Ctrl-U)
    if (event == Event::Backspace) {
        int half = std::max(1, Terminal::Size().dimy / 2);
        int amount = half * count;
        view.cursor_row =
            std::clamp(view.cursor_row - amount, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = std::min(
            view.cursor_col,
            static_cast<int>(view.buf->lines[view.cursor_row].size()));
        view.top_row = std::clamp(
            view.cursor_row - half / 2, 0,
            std::max(0, static_cast<int>(view.buf->lines.size()) - half));
        UpdateScroll(view);
        count_acc_ = 0;
        return true;
    }

    // Arrow keys
    if (event == Event::ArrowLeft) {
        MoveCursor(view, 0, -count);
        count_acc_ = 0;
        return true;
    }
    if (event == Event::ArrowDown) {
        MoveCursor(view, count, 0);
        count_acc_ = 0;
        return true;
    }
    if (event == Event::ArrowUp) {
        MoveCursor(view, -count, 0);
        count_acc_ = 0;
        return true;
    }
    if (event == Event::ArrowRight) {
        MoveCursor(view, 0, count);
        count_acc_ = 0;
        return true;
    }

    count_acc_ = 0;
    return false;
}

// ---- Insert mode ----
bool ViEditor::OnInsertEvent(Event event) {
    EditorView& view = active();

    if (event == Event::Escape) {
        FinalizeInsertUndo(view);
        FinalizeBlockChange(view);
        mode_ = Mode::NORMAL;
        if (recording_insert_) {
            RecordInsertChange(insert_recording_);
            recording_insert_ = false;
        }
        // Move cursor left by 1 (Vim behavior: exit insert moves left)
        if (view.cursor_col > 0)
            --view.cursor_col;
        return true;
    }

    if (event == Event::Return) {
        // Push accumulated word text before the newline
        PushInsertWordUndo(view);
        // Push line-level undo for the line split
        PushUndo(view, view.cursor_row, 2);
        std::string& line = view.buf->lines[view.cursor_row];
        // Compute leading whitespace of current line for auto-indent
        int indent = 0;
        while (indent < static_cast<int>(line.size()) &&
               (line[indent] == ' ' || line[indent] == '\t'))
            ++indent;
        std::string rest = line.substr(view.cursor_col);
        line.erase(view.cursor_col);
        // Prepend the indentation to the new line
        std::string ws = line.substr(0, indent);
        view.buf->lines.insert(view.buf->lines.begin() + view.cursor_row + 1,
                          ws + rest);
        ++view.cursor_row;
        view.cursor_col = indent;
        view.buf->modified = true;
        UpdateScroll(view);
        // Reset word tracking after structural change
        view.buf->insert_word_row = view.cursor_row;
        view.buf->insert_word_col = view.cursor_col;
        view.buf->insert_accumulated.clear();
        if (recording_insert_)
            insert_recording_ += '\n';
        return true;
    }

    if (event == Event::Backspace || event == Event::Delete) {
        if (view.cursor_col > 0) {
            // Push accumulated word text before the deletion
            PushInsertWordUndo(view);
            // Record the character being deleted for undo
            char deleted =
                view.buf->lines[view.cursor_row][view.cursor_col - 1];
            int pre_col = view.cursor_col; // position before deletion
            if (recording_insert_ && !insert_recording_.empty())
                insert_recording_.pop_back();
            view.buf->lines[view.cursor_row].erase(view.cursor_col - 1, 1);
            --view.cursor_col;
            view.buf->modified = true;
            // Push char-level undo for the deletion
            // cursor before change = (row, pre_col); after = (row, pre_col-1)
            PushCharUndo(view, view.cursor_row, view.cursor_col,
                         view.cursor_row, view.cursor_col,
                         std::string(1, deleted), "");
            // Override cursor pos in the entry to pre-deletion position
            view.buf->undo_stack.back().cursor_row = view.cursor_row;
            view.buf->undo_stack.back().cursor_col = pre_col;
            // Reset word tracking to current position
            view.buf->insert_word_row = view.cursor_row;
            view.buf->insert_word_col = view.cursor_col;
            view.buf->insert_accumulated.clear();
        } else if (view.cursor_row > 0) {
            PushInsertWordUndo(view);
            // Joining lines: use line-level undo
            PushUndo(view, view.cursor_row - 1, 2);
            if (recording_insert_ && !insert_recording_.empty())
                insert_recording_.pop_back();
            std::string& prev = view.buf->lines[view.cursor_row - 1];
            std::string& curr = view.buf->lines[view.cursor_row];
            view.cursor_col = static_cast<int>(prev.size());
            prev += curr;
            view.buf->lines.erase(view.buf->lines.begin() + view.cursor_row,
                             view.buf->lines.begin() + view.cursor_row + 1);
            --view.cursor_row;
            view.buf->modified = true;
            UpdateScroll(view);
            view.buf->insert_word_row = view.cursor_row;
            view.buf->insert_word_col = view.cursor_col;
            view.buf->insert_accumulated.clear();
        }
        return true;
    }

    // Ctrl-O: execute one normal-mode command, then return to insert
    // mode. Set to 1 = enter NORMAL on next event; 2 = return to INSERT
    // after command.
    if (event == Event::Special({15})) {
        ctrl_o_pending_ = 1;
        return true;
    }

    // Ctrl-W: delete word before cursor (FTXUI sends Ctrl+key as
    // Event::Special)
    if (event == Event::Special({23})) {
        // Push accumulated word text before Ctrl-W deletion
        PushInsertWordUndo(view);
        if (view.cursor_col == 0) {
            // At start of line: join with previous line
            if (view.cursor_row > 0) {
                PushUndo(view, view.cursor_row - 1, 2);
                std::string& prev = view.buf->lines[view.cursor_row - 1];
                std::string& curr = view.buf->lines[view.cursor_row];
                view.cursor_col = static_cast<int>(prev.size());
                prev += curr;
                view.buf->lines.erase(view.buf->lines.begin() + view.cursor_row,
                                 view.buf->lines.begin() + view.cursor_row +
                                     1);
                --view.cursor_row;
                view.buf->modified = true;
                UpdateScroll(view);
            }
        } else {
            PushUndo(view, view.cursor_row, 1);
            std::string& line = view.buf->lines[view.cursor_row];
            int pos = view.cursor_col;
            // Skip whitespace before cursor
            while (pos > 0 && IsSpaceOrTab(line[pos - 1]))
                --pos;
            // Delete word characters (letters, digits, underscore)
            int word_start = pos;
            while (word_start > 0 && IsWordChar(line[word_start - 1]))
                --word_start;
            if (word_start == pos) {
                // No word chars found: delete the single non-word,
                // non-space character before cursor
                if (pos > 0 && !IsSpaceOrTab(line[pos - 1]))
                    --pos;
            } else {
                pos = word_start;
            }
            int count = view.cursor_col - pos;
            if (count > 0) {
                line.erase(pos, count);
                view.cursor_col = pos;
                view.buf->modified = true;
            }
        }
        // Reset word tracking after Ctrl-W deletion
        view.buf->insert_word_row = view.cursor_row;
        view.buf->insert_word_col = view.cursor_col;
        view.buf->insert_accumulated.clear();
        return true;
    }

    if (event == Event::Tab) {
        // Expand tab to 4 spaces so it appears correctly in the
        // terminal
        for (int t = 0; t < 4; ++t) {
            InsertChar(view, ' ');
            if (recording_insert_)
                insert_recording_ += ' ';
        }
        UpdateScroll(view);
        return true;
    }

    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1 && ch[0] >= 32 && ch[0] <= 126) {
            char c = ch[0];
            bool is_word = IsWordChar(static_cast<unsigned char>(c));
            bool is_space = IsSpaceOrTab(c);
            bool is_punct = !is_word && !is_space;

            // Check if the new char belongs to a different chunk class
            if (!view.buf->insert_accumulated.empty()) {
                char first = view.buf->insert_accumulated[0];
                bool acc_is_word =
                    IsWordChar(static_cast<unsigned char>(first));
                bool acc_is_space = IsSpaceOrTab(first);
                if (is_word != acc_is_word || is_space != acc_is_space) {
                    // Class changed: push accumulated text as undo entry
                    PushInsertWordUndo(view);
                }
            }

            InsertChar(view, c);

            // Track for word-boundary undo
            if (!view.buf->insert_word_tracking) {
                view.buf->insert_word_tracking = true;
                view.buf->insert_word_row = view.cursor_row;
                view.buf->insert_word_col = view.cursor_col - 1;
                view.buf->insert_accumulated.clear();
            }
            view.buf->insert_accumulated += c;

            // Each punctuation character is its own undo entry
            if (is_punct)
                PushInsertWordUndo(view);

            UpdateScroll(view);
            if (recording_insert_)
                insert_recording_ += c;
        }
        return true;
    }

    return false;
}

// ---- Command mode ----
bool ViEditor::OnCommandEvent(Event event) {
    if (event == Event::Return) {
        ExecuteCommand();
        return true;
    }
    if (event == Event::Escape) {
        command_line_.clear();
        mode_ = Mode::NORMAL;
        return true;
    }
    if (event == Event::Backspace || event == Event::Delete) {
        if (!command_line_.empty())
            command_line_.pop_back();
        return true;
    }
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1 && ch[0] >= 32 && ch[0] <= 126) {
            command_line_.push_back(ch[0]);
        }
        return true;
    }
    return false;
}

// ---- Operator-pending mode ----
bool ViEditor::OnOperatorPendingEvent(Event event) {
    EditorView& view = active();

    // Text object pending: we already captured 'i' or 'a', now get the
    // object char
    if (text_obj_pending_) {
        text_obj_pending_ = false;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char obj_char = ch[0];
                TextObjectType tobj =
                    ParseTextObject(obj_char, text_obj_inner_);
                if (tobj != TextObjectType::NONE) {
                    int total_count =
                        (pending_count_ > 0 ? pending_count_ : 1) *
                        (motion_count_ > 0 ? motion_count_ : 1);
                    for (int i = 0; i < total_count; ++i) {
                        Range r = GetRangeForTextObject(view, tobj);
                        ExecuteOperator(view, pending_op_, r);
                    }
                    pending_op_ = OperatorType::NONE;
                    pending_count_ = 0;
                    motion_count_ = 0;
                    return true;
                }
            }
        }
        // Invalid text object: cancel operator
        pending_op_ = OperatorType::NONE;
        pending_count_ = 0;
        motion_count_ = 0;
        mode_ = Mode::NORMAL;
        return true;
    }

    // Find pending (f/F/t/T): wait for target character
    if (find_pending_) {
        find_pending_ = false;
        if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1) {
                char target = ch[0];
                last_find_char_ = target;
                int total_count =
                    (pending_count_ > 0 ? pending_count_ : 1) *
                    (motion_count_ > 0 ? motion_count_ : 1);
                CursorPos pos;
                if (last_find_forward_)
                    pos = MotionFindForward(view, target, total_count,
                                            last_find_till_);
                else
                    pos = MotionFindBackward(view, target, total_count,
                                             last_find_till_);
                ApplyOperatorMotion(view, pos);
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
        }
        pending_op_ = OperatorType::NONE;
        pending_count_ = 0;
        motion_count_ = 0;
        mode_ = Mode::NORMAL;
        return true;
    }

    // Accumulate digits (motion count)
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1 &&
            std::isdigit(static_cast<unsigned char>(ch[0])) &&
            ch[0] != '0') {
            motion_count_ = motion_count_ * 10 + (ch[0] - '0');
            return true;
        }
    }

    int count = motion_count_ > 0 ? motion_count_ : 1;
    int total_count = pending_count_ > 0 ? pending_count_ : 1;
    int final_count = count * total_count;

    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1) {
            char c = ch[0];

            // Handle dd, cc, yy (linewise double-tap)
            if ((c == 'd' && pending_op_ == OperatorType::DELETE_OP) ||
                (c == 'c' && pending_op_ == OperatorType::CHANGE) ||
                (c == 'y' && pending_op_ == OperatorType::YANK)) {
                // Linewise operation
                int start_row = view.cursor_row;
                int end_row =
                    std::min(start_row + final_count - 1,
                             static_cast<int>(view.buf->lines.size()) - 1);
                Range r = {start_row, 0, end_row,
                           0}; // 0 end_col signals linewise
                ExecuteOperator(view, pending_op_, r);
                pending_op_ = OperatorType::NONE;
                pending_count_ = 0;
                motion_count_ = 0;
                return true;
            }

            // Text objects: i or a followed by a character
            if (c == 'i' || c == 'a') {
                text_obj_pending_ = true;
                text_obj_inner_ = (c == 'i');
                return true;
            }

            // Motions
            if (c == 'h') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionLeft(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'j') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionDown(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'k') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionUp(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'l') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionRight(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'w') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionWordForward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'b') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionWordBackward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'e') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionWordEndForward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'W') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionBigWordForward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'B') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionBigWordBackward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'E') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionBigWordEndForward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == '0') {
                CursorPos pos = MotionLineStart(view);
                ApplyOperatorMotion(view, pos);
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == '$') {
                CursorPos pos = MotionLineEnd(view);
                ApplyOperatorMotion(view, pos);
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == '^') {
                CursorPos pos = MotionFirstNonBlank(view);
                ApplyOperatorMotion(view, pos);
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == 'G') {
                CursorPos pos;
                if (motion_count_ > 0)
                    pos = {std::min(
                               final_count - 1,
                               static_cast<int>(view.buf->lines.size()) - 1),
                           0};
                else
                    pos = MotionFileEnd(view);
                ApplyOperatorMotion(view, pos);
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == '%') {
                CursorPos pos = MotionPercentMatch(view);
                ApplyOperatorMotion(view, pos);
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == '}') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionParagraphForward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }
            if (c == '{') {
                for (int i = 0; i < final_count; ++i) {
                    CursorPos pos = MotionParagraphBackward(view);
                    ApplyOperatorMotion(view, pos);
                    if (pending_op_ == OperatorType::NONE)
                        break;
                }
                motion_count_ = 0;
                pending_count_ = 0;
                return true;
            }

            // f, F, t, T need next char
            if (c == 'f' || c == 'F' || c == 't' || c == 'T') {
                find_pending_ = true;
                find_pending_cmd_ = c;
                last_find_forward_ = (c == 'f' || c == 't');
                last_find_till_ = (c == 't' || c == 'T');
                return true;
            }
        }
    }

    // Escape cancels operator
    if (event == Event::Escape) {
        pending_op_ = OperatorType::NONE;
        pending_count_ = 0;
        motion_count_ = 0;
        mode_ = Mode::NORMAL;
        return true;
    }

    return false;
}

// ---- Search mode ----
bool ViEditor::OnSearchEvent(Event event) {
    if (event == Event::Return) {
        FinalizeSearch();
        return true;
    }
    if (event == Event::Escape) {
        active().buf->search_pattern.clear();
        active().buf->search_matches.clear();
        active().buf->current_match_idx = -1;
        mode_ = Mode::NORMAL;
        return true;
    }
    if (event == Event::Backspace || event == Event::Delete) {
        if (!active().buf->search_pattern.empty()) {
            active().buf->search_pattern.pop_back();
            DoIncrementalSearch();
        }
        return true;
    }
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1 && ch[0] >= 32 && ch[0] <= 126) {
            active().buf->search_pattern.push_back(ch[0]);
            DoIncrementalSearch();
        }
        return true;
    }
    return false;
}

// ---- Visual mode (character-wise) ----
bool ViEditor::OnVisualEvent(Event event) {
    EditorView& view = active();

    if (event == Event::Escape) {
        ExitVisual();
        return true;
    }

    // Ctrl-D / Ctrl-U: scroll half-page
    if (event == Event::Special({4}) || event == Event::Special({21})) {
        int half = std::max(1, Terminal::Size().dimy / 2);
        int dir = (event == Event::Special({4})) ? 1 : -1;
        view.cursor_row =
            std::clamp(view.cursor_row + dir * half, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = std::min(
            view.cursor_col,
            static_cast<int>(view.buf->lines[view.cursor_row].size()));
        view.top_row = std::clamp(
            view.cursor_row - half / 2, 0,
            std::max(0, static_cast<int>(view.buf->lines.size()) - half));
        UpdateScroll(view);
        return true;
    }

    // Operators on selection
    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1) {
            char c = ch[0];
            if (c == 'd' || c == 'x') {
                ApplyOperatorToSelection(view, OperatorType::DELETE_OP);
                return true;
            }
            if (c == 'c') {
                ApplyOperatorToSelection(view, OperatorType::CHANGE);
                return true;
            }
            if (c == 'y') {
                ApplyOperatorToSelection(view, OperatorType::YANK);
                return true;
            }
            if (c == '>') {
                ApplyOperatorToSelection(view, OperatorType::INDENT);
                return true;
            }
            if (c == '<') {
                ApplyOperatorToSelection(view, OperatorType::OUTDENT);
                return true;
            }
            if (c == '~') {
                ApplyOperatorToSelection(view,
                                         OperatorType::TILDE_CASE);
                return true;
            }
        }
    }

    // Motions extend selection
    if (event == Event::Character('h')) {
        MoveCursor(view, 0, -1);
        visual_sel_.cursor_row = view.cursor_row;
        visual_sel_.cursor_col = view.cursor_col;
        return true;
    }
    if (event == Event::Character('j')) {
        MoveCursor(view, 1, 0);
        visual_sel_.cursor_row = view.cursor_row;
        visual_sel_.cursor_col = view.cursor_col;
        return true;
    }
    if (event == Event::Character('k')) {
        MoveCursor(view, -1, 0);
        visual_sel_.cursor_row = view.cursor_row;
        visual_sel_.cursor_col = view.cursor_col;
        return true;
    }
    if (event == Event::Character('l')) {
        MoveCursor(view, 0, 1);
        visual_sel_.cursor_row = view.cursor_row;
        visual_sel_.cursor_col = view.cursor_col;
        return true;
    }
    if (event == Event::Character('w')) {
        auto pos = MotionWordForward(view);
        view.cursor_row = pos.row;
        view.cursor_col = pos.col;
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('b')) {
        auto pos = MotionWordBackward(view);
        view.cursor_row = pos.row;
        view.cursor_col = pos.col;
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('e')) {
        auto pos = MotionWordEndForward(view);
        view.cursor_row = pos.row;
        view.cursor_col = pos.col;
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('$')) {
        view.cursor_col =
            static_cast<int>(view.buf->lines[view.cursor_row].size());
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('0')) {
        view.cursor_col = 0;
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('^')) {
        auto pos = MotionFirstNonBlank(view);
        view.cursor_row = pos.row;
        view.cursor_col = pos.col;
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('G')) {
        view.cursor_row = static_cast<int>(view.buf->lines.size()) - 1;
        view.cursor_col = 0;
        UpdateScroll(view);
        return true;
    }

    if (event == Event::ArrowLeft) {
        MoveCursor(view, 0, -1);
        return true;
    }
    if (event == Event::ArrowDown) {
        MoveCursor(view, 1, 0);
        return true;
    }
    if (event == Event::ArrowUp) {
        MoveCursor(view, -1, 0);
        return true;
    }
    if (event == Event::ArrowRight) {
        MoveCursor(view, 0, 1);
        return true;
    }

    return false;
}

// ---- Visual Line mode ----
bool ViEditor::OnVisualLineEvent(Event event) {
    EditorView& view = active();

    if (event == Event::Escape) {
        ExitVisual();
        return true;
    }

    // Ctrl-D / Ctrl-U: scroll half-page
    if (event == Event::Special({4}) || event == Event::Special({21})) {
        int half = std::max(1, Terminal::Size().dimy / 2);
        int dir = (event == Event::Special({4})) ? 1 : -1;
        view.cursor_row =
            std::clamp(view.cursor_row + dir * half, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = 0;
        view.top_row = std::clamp(
            view.cursor_row - half / 2, 0,
            std::max(0, static_cast<int>(view.buf->lines.size()) - half));
        UpdateScroll(view);
        return true;
    }

    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1) {
            char c = ch[0];
            if (c == 'd' || c == 'x') {
                ApplyOperatorToSelection(view, OperatorType::DELETE_OP);
                return true;
            }
            if (c == 'c') {
                ApplyOperatorToSelection(view, OperatorType::CHANGE);
                return true;
            }
            if (c == 'y') {
                ApplyOperatorToSelection(view, OperatorType::YANK);
                return true;
            }
            if (c == '>') {
                ApplyOperatorToSelection(view, OperatorType::INDENT);
                return true;
            }
            if (c == '<') {
                ApplyOperatorToSelection(view, OperatorType::OUTDENT);
                return true;
            }
            if (c == 'V') {
                ExitVisual();
                return true;
            } // toggle off
        }
    }

    if (event == Event::Character('j')) {
        MoveCursor(view, 1, 0);
        return true;
    }
    if (event == Event::Character('k')) {
        MoveCursor(view, -1, 0);
        return true;
    }
    if (event == Event::Character('G')) {
        view.cursor_row = static_cast<int>(view.buf->lines.size()) - 1;
        view.cursor_col = 0;
        UpdateScroll(view);
        return true;
    }

    if (event == Event::ArrowDown) {
        MoveCursor(view, 1, 0);
        return true;
    }
    if (event == Event::ArrowUp) {
        MoveCursor(view, -1, 0);
        return true;
    }

    return false;
}

// ---- Visual Block mode ----
bool ViEditor::OnVisualBlockEvent(Event event) {
    EditorView& view = active();

    if (event == Event::Escape) {
        ExitVisual();
        return true;
    }

    // Ctrl-D / Ctrl-U: scroll half-page
    if (event == Event::Special({4}) || event == Event::Special({21})) {
        int half = std::max(1, Terminal::Size().dimy / 2);
        int dir = (event == Event::Special({4})) ? 1 : -1;
        view.cursor_row =
            std::clamp(view.cursor_row + dir * half, 0,
                       static_cast<int>(view.buf->lines.size()) - 1);
        view.cursor_col = std::min(
            view.cursor_col,
            static_cast<int>(view.buf->lines[view.cursor_row].size()));
        view.top_row = std::clamp(
            view.cursor_row - half / 2, 0,
            std::max(0, static_cast<int>(view.buf->lines.size()) - half));
        UpdateScroll(view);
        return true;
    }

    if (event.is_character()) {
        std::string ch = event.character();
        if (ch.size() == 1) {
            char c = ch[0];
            if (c == 'd' || c == 'x') {
                ApplyBlockChange(view, OperatorType::DELETE_OP);
                return true;
            }
            if (c == 'c') {
                ApplyBlockChange(view, OperatorType::CHANGE);
                return true;
            }
            if (c == 'y') {
                ApplyOperatorToSelection(view, OperatorType::YANK);
                return true;
            }
            if (c == '>') {
                ApplyOperatorToSelection(view, OperatorType::INDENT);
                return true;
            }
            if (c == '<') {
                ApplyOperatorToSelection(view, OperatorType::OUTDENT);
                return true;
            }
            if (c == '~') {
                ApplyOperatorToSelection(view,
                                         OperatorType::TILDE_CASE);
                return true;
            }
        }
    }

    if (event == Event::Character('h')) {
        MoveCursor(view, 0, -1);
        return true;
    }
    if (event == Event::Character('j')) {
        MoveCursor(view, 1, 0);
        return true;
    }
    if (event == Event::Character('k')) {
        MoveCursor(view, -1, 0);
        return true;
    }
    if (event == Event::Character('l')) {
        MoveCursor(view, 0, 1);
        return true;
    }
    if (event == Event::Character('$')) {
        view.cursor_col =
            static_cast<int>(view.buf->lines[view.cursor_row].size());
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('0')) {
        view.cursor_col = 0;
        UpdateScroll(view);
        return true;
    }
    if (event == Event::Character('G')) {
        view.cursor_row = static_cast<int>(view.buf->lines.size()) - 1;
        view.cursor_col = visual_block_start_col_;
        UpdateScroll(view);
        return true;
    }

    if (event == Event::ArrowLeft) {
        MoveCursor(view, 0, -1);
        return true;
    }
    if (event == Event::ArrowDown) {
        MoveCursor(view, 1, 0);
        return true;
    }
    if (event == Event::ArrowUp) {
        MoveCursor(view, -1, 0);
        return true;
    }
    if (event == Event::ArrowRight) {
        MoveCursor(view, 0, 1);
        return true;
    }

    return false;
}

// ============================================================================
// FTXUI Component & Main Loop
// ============================================================================

Component ViEditor::GetComponent() {
    return Renderer([this] { return RenderAllViews(); }) |
           CatchEvent([this](Event event) { return OnEvent(event); });
}

void ViEditor::Run() {
    auto screen = ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false); // let us handle Ctrl-C ourselves
    screen_ = &screen;
    screen.Loop(GetComponent());
}

// ============================================================================
// Entry point
// ============================================================================

int main(int argc, const char* argv[]) {
    std::string filename = (argc > 1) ? argv[1] : "untitled.txt";
    ViEditor editor(filename);
    editor.Run();
    return 0;
}
