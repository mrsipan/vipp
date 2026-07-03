#include "../motions.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static int passed = 0;
static int failed = 0;

#define TEST(name)                                                        \
    do {                                                                  \
        std::cout << "  " << (name) << "... ";                            \
    } while (0)
#define OK()                                                              \
    do {                                                                  \
        std::cout << "OK" << std::endl;                                   \
        ++passed;                                                         \
    } while (0)
#define FAIL(msg)                                                         \
    do {                                                                  \
        std::cout << "FAIL: " << (msg) << std::endl;                      \
        ++failed;                                                         \
    } while (0)
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            FAIL(#cond);                                                  \
            return;                                                       \
        }                                                                 \
    } while (0)

// Helper: create a MotionView from a vector of strings and cursor pos
static MotionView mv(const std::vector<std::string>& lines, int row,
                     int col) {
    return {&lines, row, col};
}

// ============================================================================
// Basic motion tests
// ============================================================================

static void test_motion_left() {
    std::vector<std::string> L1 = {"hello"};
    TEST("left within line");
    auto v = mv(L1, 0, 3);
    auto p = MotionLeft(v);
    CHECK(p.row == 0 && p.col == 2);
    OK();

    TEST("left at start of line stays");
    v = mv(L1, 0, 0);
    p = MotionLeft(v);
    CHECK(p.row == 0 && p.col == 0);
    OK();

    std::vector<std::string> L2 = {"ab", "cd"};
    TEST("left wraps to previous line");
    v = mv(L2, 1, 0);
    p = MotionLeft(v);
    CHECK(p.row == 0 && p.col == 2);
    OK();
}

static void test_motion_right() {
    std::vector<std::string> L1 = {"hello"};
    TEST("right within line");
    auto v = mv(L1, 0, 1);
    auto p = MotionRight(v);
    CHECK(p.row == 0 && p.col == 2);
    OK();

    std::vector<std::string> L2 = {"ab", "cd"};
    TEST("right at end of line wraps to next");
    v = mv(L2, 0, 2);
    p = MotionRight(v);
    CHECK(p.row == 1 && p.col == 0);
    OK();

    std::vector<std::string> L3 = {"ab"};
    TEST("right at end of file stays");
    v = mv(L3, 0, 2);
    p = MotionRight(v);
    CHECK(p.row == 0 && p.col == 2);
    OK();
}

static void test_motion_down() {
    std::vector<std::string> L1 = {"a", "b", "c"};
    TEST("down one line");
    auto v = mv(L1, 0, 0);
    auto p = MotionDown(v);
    CHECK(p.row == 1);
    OK();

    std::vector<std::string> L2 = {"a"};
    TEST("down at last line stays");
    v = mv(L2, 0, 0);
    p = MotionDown(v);
    CHECK(p.row == 0);
    OK();
}

static void test_motion_line_end() {
    std::vector<std::string> L = {"hello world"};
    TEST("line end");
    auto v = mv(L, 0, 3);
    auto p = MotionLineEnd(v);
    CHECK(p.col == 11);
    OK();
}

static void test_motion_first_non_blank() {
    std::vector<std::string> L1 = {"   hello"};
    TEST("first non-blank");
    auto v = mv(L1, 0, 0);
    auto p = MotionFirstNonBlank(v);
    CHECK(p.col == 3);
    OK();

    std::vector<std::string> L2 = {"   "};
    TEST("first non-blank empty line");
    v = mv(L2, 0, 0);
    p = MotionFirstNonBlank(v);
    CHECK(p.col == 3);
    OK();
}

// ============================================================================
// Word motion tests
// ============================================================================

static void test_word_forward() {
    std::vector<std::string> L1 = {"hello world"};
    TEST("w from start of word");
    auto v = mv(L1, 0, 0);
    auto p = MotionWordForward(v);
    CHECK(p.row == 0 && p.col == 6);
    OK();

    TEST("w from middle of word");
    v = mv(L1, 0, 2);
    p = MotionWordForward(v);
    CHECK(p.row == 0 && p.col == 6);
    OK();

    TEST("w from space");
    v = mv(L1, 0, 5);
    p = MotionWordForward(v);
    CHECK(p.row == 0 && p.col == 6);
    OK();

    std::vector<std::string> L2 = {"foo/bar,baz"};
    TEST("w past punctuation");
    v = mv(L2, 0, 0);
    p = MotionWordForward(v);
    CHECK(p.row == 0 && p.col == 4);
    OK();

    std::vector<std::string> L3 = {"a b c d"};
    TEST("w count = 2");
    v = mv(L3, 0, 0);
    p = MotionWordForward(v, 2);
    CHECK(p.row == 0 && p.col == 4);
    OK();
}

static void test_word_backward() {
    std::vector<std::string> L = {"hello world"};
    TEST("b from middle of word");
    auto v = mv(L, 0, 8);
    auto p = MotionWordBackward(v);
    CHECK(p.row == 0 && p.col == 6);
    OK();

    TEST("b from start of word");
    v = mv(L, 0, 6);
    p = MotionWordBackward(v);
    CHECK(p.row == 0 && p.col == 0);
    OK();
}

static void test_word_end_forward() {
    std::vector<std::string> L1 = {"hello world"};
    TEST("e from start of word");
    auto v = mv(L1, 0, 0);
    auto p = MotionWordEndForward(v);
    CHECK(p.row == 0 && p.col == 4);
    OK();

    TEST("e twice should advance");
    v = mv(L1, 0, 0);
    p = MotionWordEndForward(v);
    CHECK(p.col == 4);
    p = MotionWordEndForward(mv(L1, p.row, p.col));
    CHECK(p.col == 10);
    OK();

    std::vector<std::string> L2 = {"foo/bar"};
    TEST("e handles punctuation — first e to end of word");
    v = mv(L2, 0, 0);
    p = MotionWordEndForward(v);
    CHECK(p.col == 2);  // end of "foo"
    OK();

    TEST("e handles punctuation — second e to end of /");
    v = mv(L2, p.row, p.col);
    p = MotionWordEndForward(v);
    CHECK(p.col == 3);  // end of "/"
    OK();

    TEST("e advances from end of word");
    v = mv(L1, 0, 4);
    p = MotionWordEndForward(v);
    CHECK(p.col == 10);
    OK();
}

static void test_word_end_backward() {
    std::vector<std::string> L = {"hello world"};
    TEST("ge from end of word");
    auto v = mv(L, 0, 10);
    auto p = MotionWordEndBackward(v);
    CHECK(p.row == 0 && p.col == 4);
    OK();

    TEST("ge from middle of word");
    v = mv(L, 0, 7);
    p = MotionWordEndBackward(v);
    CHECK(p.col == 4);
    OK();
}

// ============================================================================
// BIG word motion tests
// ============================================================================

static void test_big_word_forward() {
    std::vector<std::string> L = {"foo/bar.baz qux"};
    TEST("W skips punctuation");
    auto v = mv(L, 0, 0);
    auto p = MotionBigWordForward(v);
    CHECK(p.row == 0 && p.col == 12);
    OK();

    TEST("W from punctuation");
    v = mv(L, 0, 3);
    p = MotionBigWordForward(v);
    CHECK(p.col == 12);
    OK();
}

static void test_big_word_end_forward() {
    std::vector<std::string> L = {"foo/bar.baz"};
    TEST("E from start of big word");
    auto v = mv(L, 0, 0);
    auto p = MotionBigWordEndForward(v);
    CHECK(p.col == 10);
    OK();
}

// ============================================================================
// Find motion tests
// ============================================================================

static void test_find_forward() {
    std::vector<std::string> L = {"hello world"};
    TEST("f finds character");
    auto v = mv(L, 0, 0);
    auto p = MotionFindForward(v, 'o');
    CHECK(p.col == 4);
    OK();

    TEST("f finds next occurrence");
    v = mv(L, 0, 4);
    p = MotionFindForward(v, 'o');
    CHECK(p.col == 7);
    OK();

    TEST("t stops before");
    v = mv(L, 0, 0);
    p = MotionFindForward(v, 'o', 1, true);
    CHECK(p.col == 3);
    OK();

    TEST("f not found stays");
    v = mv(L, 0, 0);
    p = MotionFindForward(v, 'z');
    CHECK(p.row == 0 && p.col == 0);
    OK();
}

static void test_find_backward() {
    std::vector<std::string> L = {"hello world"};
    TEST("F finds character backwards");
    auto v = mv(L, 0, 10);
    auto p = MotionFindBackward(v, 'w');
    CHECK(p.col == 6);
    OK();

    TEST("T stops after");
    v = mv(L, 0, 10);
    p = MotionFindBackward(v, 'w', 1, true);
    CHECK(p.col == 7);
    OK();
}

// ============================================================================
// Paragraph motion tests
// ============================================================================

static void test_paragraph_forward() {
    std::vector<std::string> L1 = {"line1", "line2", "", "line3"};
    TEST("} skips to next paragraph");
    auto v = mv(L1, 0, 0);
    auto p = MotionParagraphForward(v);
    CHECK(p.row == 3);
    OK();

    std::vector<std::string> L2 = {"", "", "line1"};
    TEST("} from empty line");
    v = mv(L2, 0, 0);
    p = MotionParagraphForward(v);
    CHECK(p.row == 2);
    OK();
}

// ============================================================================
// Percent match tests
// ============================================================================

static void test_percent_match() {
    std::vector<std::string> L1 = {"foo(bar)baz"};
    TEST("% matches parens");
    auto v = mv(L1, 0, 3);
    auto p = MotionPercentMatch(v);
    CHECK(p.col == 7);
    OK();

    std::vector<std::string> L2 = {"{hello}"};
    TEST("% matches braces");
    v = mv(L2, 0, 0);
    p = MotionPercentMatch(v);
    CHECK(p.col == 6);
    OK();

    std::vector<std::string> L3 = {"[test]"};
    TEST("% matches brackets");
    v = mv(L3, 0, 0);
    p = MotionPercentMatch(v);
    CHECK(p.col == 5);
    OK();
}

// ============================================================================

int main() {
    std::cout << "Motion tests:" << std::endl;

    test_motion_left();
    test_motion_right();
    test_motion_down();
    test_motion_line_end();
    test_motion_first_non_blank();

    test_word_forward();
    test_word_backward();
    test_word_end_forward();
    test_word_end_backward();

    test_big_word_forward();
    test_big_word_end_forward();

    test_find_forward();
    test_find_backward();

    test_paragraph_forward();
    test_percent_match();

    std::cout << std::endl
              << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
