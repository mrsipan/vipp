#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// ============================================================================
// Core types needed by motions
// ============================================================================

struct CursorPos {
    int row, col;
};

// Lightweight view state used by motions (read-only)
struct MotionView {
    const std::vector<std::string>* lines;
    int cursor_row;
    int cursor_col;
};

// ============================================================================
// Character classification
// ============================================================================

inline bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

inline bool IsBigWordChar(char c) {
    return !(c == ' ' || c == '\t');
}

inline bool IsSpaceOrTab(char c) {
    return c == ' ' || c == '\t';
}

// ============================================================================
// Motion functions — all return a new CursorPos
// ============================================================================

CursorPos MotionLeft(const MotionView& v, int count = 1);
CursorPos MotionRight(const MotionView& v, int count = 1);
CursorPos MotionDown(const MotionView& v, int count = 1);
CursorPos MotionUp(const MotionView& v, int count = 1);

CursorPos MotionLineStart(const MotionView& v);
CursorPos MotionLineEnd(const MotionView& v);
CursorPos MotionFirstNonBlank(const MotionView& v);
CursorPos MotionFileStart(const MotionView& v);
CursorPos MotionFileEnd(const MotionView& v);

CursorPos MotionWordForward(const MotionView& v, int count = 1);
CursorPos MotionWordBackward(const MotionView& v, int count = 1);
CursorPos MotionWordEndForward(const MotionView& v, int count = 1);
CursorPos MotionWordEndBackward(const MotionView& v, int count = 1);

CursorPos MotionBigWordForward(const MotionView& v, int count = 1);
CursorPos MotionBigWordBackward(const MotionView& v, int count = 1);
CursorPos MotionBigWordEndForward(const MotionView& v, int count = 1);
CursorPos MotionBigWordEndBackward(const MotionView& v, int count = 1);

CursorPos MotionFindForward(const MotionView& v, char target,
                            int count = 1, bool till = false);
CursorPos MotionFindBackward(const MotionView& v, char target,
                             int count = 1, bool till = false);

CursorPos MotionPercentMatch(const MotionView& v);
CursorPos MotionParagraphForward(const MotionView& v, int count = 1);
CursorPos MotionParagraphBackward(const MotionView& v, int count = 1);
