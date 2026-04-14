#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace ftxui;

class ViEditor {
public:
    explicit ViEditor(std::string filename);
    Component GetComponent();
    void Run();

private:
    // ---- Core data ----
    std::vector<std::string> lines_;
    int cursor_row_ = 0;
    int cursor_col_ = 0;
    int top_row_ = 0;
    std::string filename_;
    bool modified_ = false;
    std::string status_msg_;
    int status_timeout_ = 0;

    enum class Mode { NORMAL, INSERT, COMMAND };
    Mode mode_ = Mode::NORMAL;
    std::string command_line_;

    // ---- Helper methods ----
    void LoadFile();
    void SaveFile();
    void SetStatus(std::string msg);
    void UpdateScroll();
    void MoveCursor(int drow, int dcol);
    void InsertChar(char c);
    void DeleteChar();
    void DeleteWord();
    void DeleteLine();
    void OpenLineBelow();
    void OpenLineAbove();
    void AppendToEnd();
    void InsertAtBeginning();

    // ---- Command handling ----
    void ExecuteCommand();

    // ---- Rendering ----
    Element RenderEditorContent() const;
    Element RenderStatusBar() const;
    Element RenderCommandLine() const;

    // ---- Event processing ----
    bool OnEvent(Event event);
};

// ----------------------------------------------------------------------
// Constructor & File I/O
// ----------------------------------------------------------------------
ViEditor::ViEditor(std::string filename) : filename_(std::move(filename)) {
    LoadFile();
}

void ViEditor::LoadFile() {
    std::ifstream file(filename_);
    if (!file.is_open()) {
        lines_.emplace_back("");
        SetStatus("New file");
        return;
    }
    std::string line;
    while (std::getline(file, line))
        lines_.push_back(line);
    if (lines_.empty())
        lines_.emplace_back("");
    SetStatus("Loaded " + filename_);
    modified_ = false;
}

void ViEditor::SaveFile() {
    std::ofstream file(filename_);
    if (!file.is_open()) {
        SetStatus("Cannot write to " + filename_);
        return;
    }
    for (size_t i = 0; i < lines_.size(); ++i) {
        file << lines_[i];
        if (i + 1 < lines_.size())
            file << '\n';
    }
    modified_ = false;
    SetStatus("Saved " + filename_);
}

void ViEditor::SetStatus(std::string msg) {
    status_msg_ = std::move(msg);
    status_timeout_ = 5;
}

// ----------------------------------------------------------------------
// Cursor & Scrolling
// ----------------------------------------------------------------------
void ViEditor::UpdateScroll() {
    int visible_rows = Terminal::Size().dimy - 2;
    if (cursor_row_ < top_row_)
        top_row_ = cursor_row_;
    else if (cursor_row_ >= top_row_ + visible_rows)
        top_row_ = cursor_row_ - visible_rows + 1;
    if (top_row_ < 0)
        top_row_ = 0;
    if (top_row_ > static_cast<int>(lines_.size()) - visible_rows)
        top_row_ = std::max(0, static_cast<int>(lines_.size()) - visible_rows);
}

void ViEditor::MoveCursor(int drow, int dcol) {
    int new_row = cursor_row_ + drow;
    int new_col = cursor_col_ + dcol;

    new_row = std::clamp(new_row, 0, static_cast<int>(lines_.size()) - 1);
    const std::string& line = lines_[new_row];
    new_col = std::clamp(new_col, 0, static_cast<int>(line.size()));

    cursor_row_ = new_row;
    cursor_col_ = new_col;
    UpdateScroll();
}

// ----------------------------------------------------------------------
// Editing Actions
// ----------------------------------------------------------------------
void ViEditor::InsertChar(char c) {
    std::string& line = lines_[cursor_row_];
    if (cursor_col_ <= static_cast<int>(line.size())) {
        line.insert(line.begin() + cursor_col_, c);
        ++cursor_col_;
        modified_ = true;
    }
}

void ViEditor::DeleteChar() {
    std::string& line = lines_[cursor_row_];
    if (!line.empty() && cursor_col_ < static_cast<int>(line.size())) {
        line.erase(cursor_col_, 1);
        modified_ = true;
    } else if (!line.empty() && cursor_col_ == static_cast<int>(line.size())) {
        if (cursor_row_ + 1 < static_cast<int>(lines_.size())) {
            line += lines_[cursor_row_ + 1];
            lines_.erase(lines_.begin() + cursor_row_ + 1);
            modified_ = true;
        }
    } else if (line.empty() && lines_.size() > 1) {
        lines_.erase(lines_.begin() + cursor_row_);
        if (cursor_row_ >= static_cast<int>(lines_.size()))
            cursor_row_ = lines_.size() - 1;
        cursor_col_ = 0;
        modified_ = true;
    }
}

void ViEditor::DeleteWord() {
    std::string& line = lines_[cursor_row_];
    if (line.empty())
        return;
    size_t start = cursor_col_;
    size_t end = start;
    while (end < line.size() && std::isalnum(line[end]))
        ++end;
    if (end == start)
        ++end;
    line.erase(start, end - start);
    modified_ = true;
}

void ViEditor::DeleteLine() {
    if (lines_.size() == 1) {
        lines_[0].clear();
        cursor_col_ = 0;
    } else {
        lines_.erase(lines_.begin() + cursor_row_);
        if (cursor_row_ >= static_cast<int>(lines_.size()))
            cursor_row_ = lines_.size() - 1;
        cursor_col_ = 0;
    }
    modified_ = true;
}

void ViEditor::OpenLineBelow() {
    lines_.insert(lines_.begin() + cursor_row_ + 1, "");
    ++cursor_row_;
    cursor_col_ = 0;
    mode_ = Mode::INSERT;
    modified_ = true;
}

void ViEditor::OpenLineAbove() {
    lines_.insert(lines_.begin() + cursor_row_, "");
    cursor_col_ = 0;
    mode_ = Mode::INSERT;
    modified_ = true;
}

void ViEditor::AppendToEnd() {
    cursor_col_ = static_cast<int>(lines_[cursor_row_].size());
    mode_ = Mode::INSERT;
}

void ViEditor::InsertAtBeginning() {
    cursor_col_ = 0;
    mode_ = Mode::INSERT;
}

// ----------------------------------------------------------------------
// Command Execution
// ----------------------------------------------------------------------
void ViEditor::ExecuteCommand() {
    if (command_line_ == "w") {
        SaveFile();
    } else if (command_line_ == "q") {
        if (!modified_)
            throw std::runtime_error("exit");
        else
            SetStatus("File modified (use :q! to discard)");
    } else if (command_line_ == "wq") {
        SaveFile();
        throw std::runtime_error("exit");
    } else if (command_line_ == "q!") {
        throw std::runtime_error("exit");
    } else if (command_line_.rfind("e ", 0) == 0) {
        filename_ = command_line_.substr(2);
        LoadFile();
        cursor_row_ = cursor_col_ = top_row_ = 0;
        modified_ = false;
    } else {
        SetStatus("Unknown command: " + command_line_);
    }
    command_line_.clear();
    mode_ = Mode::NORMAL;
}

// ----------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------
Element ViEditor::RenderEditorContent() const {
    Elements lines_elements;
    int visible_rows = Terminal::Size().dimy - 2;
    for (int i = 0; i < visible_rows; ++i) {
        int file_row = top_row_ + i;
        if (file_row >= 0 && file_row < static_cast<int>(lines_.size())) {
            std::string line = lines_[file_row];
            if (file_row == cursor_row_ && mode_ != Mode::COMMAND) {
                line.insert(cursor_col_, "█");
                lines_elements.push_back(hbox(text(line)));
            } else {
                lines_elements.push_back(text(line));
            }
        } else {
            lines_elements.push_back(text("~"));
        }
    }
    return vbox(std::move(lines_elements));
}

Element ViEditor::RenderStatusBar() const {
    std::string mode_str;
    switch (mode_) {
        case Mode::NORMAL:  mode_str = "NORMAL"; break;
        case Mode::INSERT:  mode_str = "INSERT"; break;
        case Mode::COMMAND: mode_str = "COMMAND"; break;
    }
    std::string mod_str = modified_ ? "+" : " ";
    std::string left = mode_str + mod_str + " " + filename_ +
                       "  lines:" + std::to_string(lines_.size());
    std::string right = "Ln " + std::to_string(cursor_row_ + 1) +
                        ", Col " + std::to_string(cursor_col_ + 1);
    int width = Terminal::Size().dimx;
    std::string status = left + std::string(width - left.size() - right.size(), ' ') + right;
    return text(status) | bold;
}

Element ViEditor::RenderCommandLine() const {
    if (mode_ == Mode::COMMAND)
        return text(":" + command_line_ + "█") | color(Color::Yellow);
    return emptyElement();
}

// ----------------------------------------------------------------------
// Event Handling (fixed for FTXUI's character() returning string)
// ----------------------------------------------------------------------
bool ViEditor::OnEvent(Event event) {
    // Global exit
    if (event == Event::Escape && mode_ != Mode::COMMAND) {
        mode_ = Mode::NORMAL;
        return true;
    }

    // Command mode
    if (mode_ == Mode::COMMAND) {
        if (event == Event::Return) {
            ExecuteCommand();
            return true;
        } else if (event == Event::Backspace || event == Event::Delete) {
            if (!command_line_.empty())
                command_line_.pop_back();
            return true;
        } else if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1 && ch[0] >= 32 && ch[0] <= 126) {
                command_line_.push_back(ch[0]);
            }
            return true;
        }
        return false;
    }

    // Insert mode
    if (mode_ == Mode::INSERT) {
        if (event == Event::Return) {
            std::string& line = lines_[cursor_row_];
            std::string rest = line.substr(cursor_col_);
            line.erase(cursor_col_);
            lines_.insert(lines_.begin() + cursor_row_ + 1, rest);
            ++cursor_row_;
            cursor_col_ = 0;
            modified_ = true;
            return true;
        } else if (event == Event::Backspace || event == Event::Delete) {
            if (cursor_col_ > 0) {
                lines_[cursor_row_].erase(cursor_col_ - 1, 1);
                --cursor_col_;
                modified_ = true;
            } else if (cursor_row_ > 0) {
                std::string& prev = lines_[cursor_row_ - 1];
                std::string& curr = lines_[cursor_row_];
                cursor_col_ = static_cast<int>(prev.size());
                prev += curr;
                lines_.erase(lines_.begin() + cursor_row_);
                --cursor_row_;
                modified_ = true;
            }
            return true;
        } else if (event.is_character()) {
            std::string ch = event.character();
            if (ch.size() == 1 && ch[0] >= 32 && ch[0] <= 126) {
                InsertChar(ch[0]);
            }
            return true;
        }
        return false;
    }

    // Normal mode
    // Motions
    if (event == Event::Character('h'))      MoveCursor(0, -1);
    else if (event == Event::Character('j')) MoveCursor(1, 0);
    else if (event == Event::Character('k')) MoveCursor(-1, 0);
    else if (event == Event::Character('l')) MoveCursor(0, 1);
    else if (event == Event::Character('0')) cursor_col_ = 0;
    else if (event == Event::Character('$')) cursor_col_ = static_cast<int>(lines_[cursor_row_].size());
    else if (event == Event::Character('g')) {
        cursor_row_ = 0;
        cursor_col_ = 0;
    } else if (event == Event::Character('G')) {
        cursor_row_ = static_cast<int>(lines_.size()) - 1;
        cursor_col_ = 0;
    }
    // Editing commands
    else if (event == Event::Character('i'))      mode_ = Mode::INSERT;
    else if (event == Event::Character('a')) {    MoveCursor(0, 1); mode_ = Mode::INSERT; }
    else if (event == Event::Character('A'))      AppendToEnd();
    else if (event == Event::Character('I'))      InsertAtBeginning();
    else if (event == Event::Character('o'))      OpenLineBelow();
    else if (event == Event::Character('O'))      OpenLineAbove();
    else if (event == Event::Character('x'))      DeleteChar();
    else if (event == Event::Character('d')) {
        // For simplicity, delete line immediately (single 'd')
        // In a full vi, you'd wait for a second 'd'
        DeleteLine();
    } else if (event == Event::Character(':')) {
        mode_ = Mode::COMMAND;
        command_line_.clear();
    }
    // Arrow keys
    else if (event == Event::ArrowUp)    MoveCursor(-1, 0);
    else if (event == Event::ArrowDown)  MoveCursor(1, 0);
    else if (event == Event::ArrowLeft)  MoveCursor(0, -1);
    else if (event == Event::ArrowRight) MoveCursor(0, 1);
    else {
        SetStatus("Not in insert mode");
        return false;
    }

    UpdateScroll();
    return true;
}

// ----------------------------------------------------------------------
// FTXUI Component & Main Loop
// ----------------------------------------------------------------------
Component ViEditor::GetComponent() {
    return Renderer([this] {
        return vbox({
            RenderEditorContent(),
            separator(),
            RenderStatusBar(),
            RenderCommandLine(),
        });
    }) | CatchEvent([this](Event event) {
        try {
            return OnEvent(event);
        } catch (const std::runtime_error&) {
            return true;  // Exit gracefully
        }
    });
}

void ViEditor::Run() {
    auto screen = ScreenInteractive::Fullscreen();
    screen.Loop(GetComponent());
}

// ----------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------
int main(int argc, const char* argv[]) {
    std::string filename = (argc > 1) ? argv[1] : "untitled.txt";
    ViEditor editor(filename);
    editor.Run();
    return 0;
}
