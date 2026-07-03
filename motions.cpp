#include "motions.h"
CursorPos MotionLeft(const MotionView& v,
                                         int count) {
    int col = v.cursor_col;
    int row = v.cursor_row;
    for (int i = 0; i < count && (row > 0 || col > 0); ++i) {
        if (col > 0)
            --col;
        else if (row > 0) {
            --row;
            col = static_cast<int>((*v.lines)[row].size());
        }
    }
    return {row, col};
}

CursorPos MotionRight(const MotionView& v,
                                          int count) {
    int col = v.cursor_col;
    int row = v.cursor_row;
    int max_row = static_cast<int>(v.lines->size()) - 1;
    for (int i = 0;
         i < count &&
         (row < max_row || col < static_cast<int>((*v.lines)[row].size()));
         ++i) {
        if (col < static_cast<int>((*v.lines)[row].size()))
            ++col;
        else if (row < max_row) {
            ++row;
            col = 0;
        }
    }
    return {row, col};
}

CursorPos MotionDown(const MotionView& v,
                                         int count) {
    int row = std::min(v.cursor_row + count,
                       static_cast<int>(v.lines->size()) - 1);
    int col =
        std::min(v.cursor_col, static_cast<int>((*v.lines)[row].size()));
    return {row, col};
}

CursorPos MotionUp(const MotionView& v,
                                       int count) {
    int row = std::max(v.cursor_row - count, 0);
    int col =
        std::min(v.cursor_col, static_cast<int>((*v.lines)[row].size()));
    return {row, col};
}

CursorPos
MotionLineStart(const MotionView& v) {
    return {v.cursor_row, 0};
}

CursorPos MotionLineEnd(const MotionView& v) {
    return {v.cursor_row,
            static_cast<int>((*v.lines)[v.cursor_row].size())};
}

CursorPos
MotionFirstNonBlank(const MotionView& v) {
    const auto& line = (*v.lines)[v.cursor_row];
    int col = 0;
    while (col < static_cast<int>(line.size()) &&
           IsSpaceOrTab(line[col]))
        ++col;
    return {v.cursor_row, col};
}

CursorPos
MotionFileStart(const MotionView& /*v*/) {
    return {0, 0};
}

CursorPos MotionFileEnd(const MotionView& v) {
    return {static_cast<int>(v.lines->size()) - 1, 0};
}

CursorPos MotionWordForward(const MotionView& v,
                                                int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.lines->size()) - 1;

    for (int c = 0; c < count; ++c) {
        // Skip current word characters
        while (row <= max_row &&
               col < static_cast<int>((*v.lines)[row].size()) &&
               IsWordChar((*v.lines)[row][col]))
            ++col;
        // Skip non-word non-whitespace
        while (row <= max_row &&
               col < static_cast<int>((*v.lines)[row].size()) &&
               !IsWordChar((*v.lines)[row][col]) &&
               !IsSpaceOrTab((*v.lines)[row][col]))
            ++col;
        // Skip whitespace (but don't cross lines)
        while (col < static_cast<int>((*v.lines)[row].size()) &&
               IsSpaceOrTab((*v.lines)[row][col]))
            ++col;
        // If at end of line, go to next line
        if (col >= static_cast<int>((*v.lines)[row].size()) &&
            row < max_row) {
            ++row;
            col = 0;
            while (col < static_cast<int>((*v.lines)[row].size()) &&
                   IsSpaceOrTab((*v.lines)[row][col]))
                ++col;
        }
    }
    if (row > max_row) {
        row = max_row;
        col = static_cast<int>((*v.lines)[row].size());
    }
    return {row, col};
}

CursorPos MotionWordBackward(const MotionView& v,
                                                 int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        // Step 1: if at column 0, move to end of previous line
        if (col == 0 && row > 0) {
            --row;
            col = static_cast<int>((*v.lines)[row].size());
        }
        // Step 2: skip whitespace and non-word chars backward to find a
        // word First skip past any whitespace
        while (col > 0 && IsSpaceOrTab((*v.lines)[row][col - 1]))
            --col;
        // If we landed at column 0 after skipping whitespace, go up a
        // line
        if (col == 0 && row > 0) {
            --row;
            col = static_cast<int>((*v.lines)[row].size());
            while (col > 0 && IsSpaceOrTab((*v.lines)[row][col - 1]))
                --col;
        }
        // Step 3: if on a non-word char, step back one to get onto a
        // word boundary
        if (col > 0 && !IsWordChar((*v.lines)[row][col - 1]))
            --col;
        // Step 4: move to start of the word
        while (col > 0 && IsWordChar((*v.lines)[row][col - 1]))
            --col;
    }
    if (row < 0)
        row = 0;
    return {row, col};
}

CursorPos MotionWordEndForward(const MotionView& v,
                                                   int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.lines->size()) - 1;

    for (int c = 0; c < count; ++c) {
        int line_len = static_cast<int>((*v.lines)[row].size());
        // Move forward one char so we don't stay on the same word end
        if (col < line_len)
            ++col;
        // Skip whitespace
        while (row <= max_row) {
            while (col < static_cast<int>((*v.lines)[row].size()) &&
                   IsSpaceOrTab((*v.lines)[row][col]))
                ++col;
            if (col >= static_cast<int>((*v.lines)[row].size()) &&
                row < max_row) {
                ++row;
                col = 0;
            } else {
                break;
            }
        }
        // Move to end of word-or-punctuation sequence
        if (row <= max_row &&
            col < static_cast<int>((*v.lines)[row].size())) {
            char cur = (*v.lines)[row][col];
            bool is_word = IsWordChar(cur);
            while (row <= max_row &&
                   col < static_cast<int>((*v.lines)[row].size())) {
                char ch = (*v.lines)[row][col];
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

CursorPos MotionBigWordForward(const MotionView& v,
                                                   int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.lines->size()) - 1;

    for (int c = 0; c < count; ++c) {
        while (row <= max_row &&
               col < static_cast<int>((*v.lines)[row].size()) &&
               IsBigWordChar((*v.lines)[row][col]))
            ++col;
        while (col < static_cast<int>((*v.lines)[row].size()) &&
               IsSpaceOrTab((*v.lines)[row][col]))
            ++col;
        if (col >= static_cast<int>((*v.lines)[row].size()) &&
            row < max_row) {
            ++row;
            col = 0;
            while (col < static_cast<int>((*v.lines)[row].size()) &&
                   IsSpaceOrTab((*v.lines)[row][col]))
                ++col;
        }
    }
    if (row > max_row) {
        row = max_row;
        col = static_cast<int>((*v.lines)[row].size());
    }
    return {row, col};
}

CursorPos MotionBigWordBackward(const MotionView& v,
                                                    int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;
    for (int c = 0; c < count; ++c) {
        while (row >= 0 && col > 0 &&
               IsSpaceOrTab((*v.lines)[row][col - 1]))
            --col;
        if (col == 0 && row > 0) {
            --row;
            col = static_cast<int>((*v.lines)[row].size());
        }
        while (row >= 0 && col > 0 &&
               IsBigWordChar((*v.lines)[row][col - 1]))
            --col;
    }
    if (row < 0)
        row = 0;
    return {row, col};
}

CursorPos
MotionBigWordEndForward(const MotionView& v,
                                  int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.lines->size()) - 1;
    for (int c = 0; c < count; ++c) {
        int line_len = static_cast<int>((*v.lines)[row].size());
        // Move forward one char so we don't stay on the same word end
        if (col < line_len)
            ++col;
        // Skip whitespace
        while (row <= max_row) {
            while (col < static_cast<int>((*v.lines)[row].size()) &&
                   IsSpaceOrTab((*v.lines)[row][col]))
                ++col;
            if (col >= static_cast<int>((*v.lines)[row].size()) &&
                row < max_row) {
                ++row;
                col = 0;
            } else {
                break;
            }
        }
        // Move to end of WORD
        while (row <= max_row &&
               col < static_cast<int>((*v.lines)[row].size()) &&
               IsBigWordChar((*v.lines)[row][col]))
            ++col;
        if (col > 0)
            --col;
    }
    return {row, col};
}

CursorPos MotionWordEndBackward(const MotionView& v,
                                                    int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        // Determine the type of character under the cursor
        if (row < 0 || row >= static_cast<int>(v.lines->size()))
            break;
        int line_len = static_cast<int>((*v.lines)[row].size());
        if (col >= line_len)
            col = line_len > 0 ? line_len - 1 : 0;
        char cur = (col < line_len) ? (*v.lines)[row][col] : ' ';
        bool is_word = IsWordChar(cur);
        bool is_space = IsSpaceOrTab(cur);

        // Step 1: skip backward past the current sequence
        if (!is_space) {
            while (true) {
                if (col <= 0) {
                    if (row > 0) {
                        --row;
                        col = static_cast<int>((*v.lines)[row].size());
                    } else {
                        break;
                    }
                }
                if (col > 0)
                    --col;
                else
                    break;
                char ch = (*v.lines)[row][col];
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
            if (col >= static_cast<int>((*v.lines)[row].size()))
                col = static_cast<int>((*v.lines)[row].size()) - 1;
            while (col >= 0 &&
                   col < static_cast<int>((*v.lines)[row].size()) &&
                   IsSpaceOrTab((*v.lines)[row][col]))
                --col;
            if (col < 0 && row > 0) {
                --row;
                col = static_cast<int>((*v.lines)[row].size()) - 1;
            } else {
                break;
            }
        }

        // Step 3: find the end of the previous sequence
        if (row >= 0 && col >= 0 &&
            col < static_cast<int>((*v.lines)[row].size()) &&
            !IsSpaceOrTab((*v.lines)[row][col])) {
            cur = (*v.lines)[row][col];
            is_word = IsWordChar(cur);
            // Find the start of this sequence
            int seq_start = col;
            while (seq_start > 0) {
                char prev = (*v.lines)[row][seq_start - 1];
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
            line_len = static_cast<int>((*v.lines)[row].size());
            while (col < line_len) {
                char ch = (*v.lines)[row][col];
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

CursorPos
MotionBigWordEndBackward(const MotionView& v,
                                   int count) {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        if (row < 0 || row >= static_cast<int>(v.lines->size()))
            break;
        int line_len = static_cast<int>((*v.lines)[row].size());
        if (col >= line_len)
            col = line_len > 0 ? line_len - 1 : 0;
        char cur = (col < line_len) ? (*v.lines)[row][col] : ' ';
        bool is_space = IsSpaceOrTab(cur);

        // Step 1: skip backward past the current BIG word
        if (!is_space) {
            while (true) {
                if (col <= 0) {
                    if (row > 0) {
                        --row;
                        col = static_cast<int>((*v.lines)[row].size());
                    } else {
                        break;
                    }
                }
                if (col > 0)
                    --col;
                else
                    break;
                if (IsSpaceOrTab((*v.lines)[row][col]))
                    break;
            }
        }

        // Step 2: skip backward past whitespace
        while (row >= 0) {
            if (col >= static_cast<int>((*v.lines)[row].size()))
                col = static_cast<int>((*v.lines)[row].size()) - 1;
            while (col >= 0 &&
                   col < static_cast<int>((*v.lines)[row].size()) &&
                   IsSpaceOrTab((*v.lines)[row][col]))
                --col;
            if (col < 0 && row > 0) {
                --row;
                col = static_cast<int>((*v.lines)[row].size()) - 1;
            } else {
                break;
            }
        }

        // Step 3: find the end of the previous BIG word
        if (row >= 0 && col >= 0 &&
            col < static_cast<int>((*v.lines)[row].size()) &&
            !IsSpaceOrTab((*v.lines)[row][col])) {
            int seq_start = col;
            while (seq_start > 0 &&
                   !IsSpaceOrTab((*v.lines)[row][seq_start - 1]))
                --seq_start;
            col = seq_start;
            line_len = static_cast<int>((*v.lines)[row].size());
            while (col < line_len && !IsSpaceOrTab((*v.lines)[row][col]))
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

CursorPos MotionFindForward(const MotionView& v,
                                                char target, int count,
                                                bool till) {
    int row = v.cursor_row;
    int col = v.cursor_col;
    int max_row = static_cast<int>(v.lines->size()) - 1;

    for (int c = 0; c < count; ++c) {
        ++col; // start after current position
        while (row <= max_row) {
            while (col < static_cast<int>((*v.lines)[row].size())) {
                if ((*v.lines)[row][col] == target) {
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

CursorPos MotionFindBackward(const MotionView& v,
                                                 char target, int count,
                                                 bool till) {
    int row = v.cursor_row;
    int col = v.cursor_col;

    for (int c = 0; c < count; ++c) {
        --col;
        while (row >= 0) {
            while (col >= 0) {
                if ((*v.lines)[row][col] == target) {
                    if (till) {
                        if (col + 1 <
                            static_cast<int>((*v.lines)[row].size()))
                            return {row, col + 1};
                        return {row, col};
                    }
                    return {row, col};
                }
                --col;
            }
            if (row > 0) {
                --row;
                col = static_cast<int>((*v.lines)[row].size()) - 1;
            } else
                break;
        }
    }
    return {v.cursor_row, v.cursor_col};
}

CursorPos
MotionParagraphForward(const MotionView& v, int count) {
    int row = v.cursor_row;
    int max_row = static_cast<int>(v.lines->size()) - 1;
    for (int c = 0; c < count && row < max_row; ++c) {
        // Skip non-empty lines
        while (row < max_row && !(*v.lines)[row].empty())
            ++row;
        // Skip empty lines
        while (row < max_row && (*v.lines)[row].empty())
            ++row;
    }
    return {row, 0};
}

CursorPos
MotionParagraphBackward(const MotionView& v,
                                  int count) {
    int row = v.cursor_row;
    for (int c = 0; c < count && row > 0; ++c) {
        while (row > 0 && !(*v.lines)[row].empty())
            --row;
        while (row > 0 && (*v.lines)[row].empty())
            --row;
    }
    return {row, 0};
}

CursorPos
MotionPercentMatch(const MotionView& v) {
    const auto& line = (*v.lines)[v.cursor_row];
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
        if (col < static_cast<int>((*v.lines)[row].size()) &&
            (*v.lines)[row][col] == self) {
            ++col;  // skip the character we are already on
        }
    }

    while (depth > 0) {
        if (forward) {
            while (col < static_cast<int>((*v.lines)[row].size())) {
                if ((*v.lines)[row][col] == self)
                    ++depth;
                else if ((*v.lines)[row][col] == match)
                    --depth;
                if (depth == 0)
                    return {row, col};
                ++col;
            }
            ++row;
            col = 0;
        } else {
            while (col >= 0) {
                if ((*v.lines)[row][col] == self)
                    ++depth;
                else if ((*v.lines)[row][col] == match)
                    --depth;
                if (depth == 0)
                    return {row, col};
                --col;
            }
            --row;
            if (row >= 0)
                col = static_cast<int>((*v.lines)[row].size()) - 1;
            else
                break;
        }
    }
    return {v.cursor_row, v.cursor_col};
}

// ============================================================================
// Text Objects
