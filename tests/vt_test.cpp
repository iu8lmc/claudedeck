// Minimal self-contained tests for vt::Emulator (no QtTest dependency).
#include "VtEmulator.h"

#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_fails;                                                                             \
        }                                                                                          \
    } while (0)

static QString row(const vt::Emulator& e, int r, int offset = 0)
{
    return vt::Emulator::lineText(e.viewLine(r, offset));
}

static const vt::Cell& cell(const vt::Emulator& e, int x, int y)
{
    return e.viewLine(y, 0).cells[size_t(x)];
}

int main()
{
    { // plain text
        vt::Emulator e(10, 3);
        e.feed("hello");
        CHECK(row(e, 0) == "hello");
        CHECK(e.cursorX() == 5 && e.cursorY() == 0);
    }
    { // autowrap
        vt::Emulator e(5, 3);
        e.feed("abcdefg");
        CHECK(row(e, 0) == "abcde");
        CHECK(row(e, 1) == "fg");
        CHECK(e.cursorX() == 2 && e.cursorY() == 1);
        CHECK(e.viewLine(0, 0).wrapped);
    }
    { // pending wrap: CR LF after a full line must not leave an empty line
        vt::Emulator e(5, 3);
        e.feed("abcde");
        CHECK(e.cursorX() == 4);
        e.feed("\r\nX");
        CHECK(row(e, 0) == "abcde");
        CHECK(row(e, 1) == "X");
    }
    { // scrolling into the scrollback
        vt::Emulator e(5, 2);
        e.feed("1\r\n2\r\n3");
        CHECK(row(e, 0) == "2");
        CHECK(row(e, 1) == "3");
        CHECK(e.scrollbackSize() == 1);
        CHECK(row(e, 0, 1) == "1");
        CHECK(row(e, 1, 1) == "2");
        CHECK(vt::Emulator::lineText(e.absoluteLine(0)) == "1");
    }
    { // CUP / CHA / VPA
        vt::Emulator e(10, 3);
        e.feed("\x1b[2;3HX");
        CHECK(row(e, 1) == "  X");
        e.feed("\x1b[1G\x1b[3dY");
        CHECK(row(e, 2) == "Y");
    }
    { // SGR basic + bright + reset
        vt::Emulator e(10, 1);
        e.feed("\x1b[1;31mA\x1b[0mB\x1b[92mC");
        CHECK(cell(e, 0, 0).pen.attrs & vt::Bold);
        CHECK(cell(e, 0, 0).pen.fg == vt::Color::indexed(1));
        CHECK(cell(e, 1, 0).pen == vt::Pen());
        CHECK(cell(e, 2, 0).pen.fg == vt::Color::indexed(10));
    }
    { // extended colours: semicolon and colon forms
        vt::Emulator e(10, 1);
        e.feed("\x1b[38;2;10;20;30mA\x1b[0m\x1b[38:2::1:2:3mB\x1b[0m\x1b[48:5:200mC\x1b[0m\x1b[38;5;7mD");
        CHECK(cell(e, 0, 0).pen.fg == vt::Color::rgb(10, 20, 30));
        CHECK(cell(e, 1, 0).pen.fg == vt::Color::rgb(1, 2, 3));
        CHECK(cell(e, 2, 0).pen.bg == vt::Color::indexed(200));
        CHECK(cell(e, 3, 0).pen.fg == vt::Color::indexed(7));
    }
    { // EL / ED / ECH
        vt::Emulator e(10, 3);
        e.feed("abcdefghij\x1b[5G\x1b[K");
        CHECK(row(e, 0) == "abcd");
        e.feed("\x1b[2;1Hxyz\x1b[1G\x1b[2X");
        CHECK(row(e, 1) == "  z");
        e.feed("\x1b[2J");
        CHECK(row(e, 0).isEmpty() && row(e, 1).isEmpty());
    }
    { // back-colour erase
        vt::Emulator e(4, 1);
        e.feed("\x1b[44m\x1b[K");
        CHECK(cell(e, 3, 0).pen.bg == vt::Color::indexed(4));
        CHECK(cell(e, 3, 0).isBlank());
    }
    { // alternate screen
        vt::Emulator e(10, 2);
        e.feed("main\x1b[?1049h\x1b[HALT\x1b[?1049l");
        CHECK(row(e, 0) == "main");
        CHECK(!e.altScreen());
        CHECK(e.cursorX() == 4);
    }
    { // scroll region
        vt::Emulator e(5, 4);
        e.feed("\x1b[2;3r\x1b[2;1Ha\r\nb\r\nc");
        CHECK(row(e, 0).isEmpty());
        CHECK(row(e, 1) == "b");
        CHECK(row(e, 2) == "c");
        CHECK(row(e, 3).isEmpty());
        CHECK(e.scrollbackSize() == 0);
        e.feed("\x1b[r\x1b[4;1Hend");
        CHECK(row(e, 3) == "end");
    }
    { // UTF-8 split across feeds + accents
        vt::Emulator e(10, 1);
        e.feed("\xC3");
        e.feed("\xA8 perch\xC3\xA9");
        CHECK(row(e, 0) == QString::fromUtf8("è perché"));
    }
    { // wide characters occupy two cells
        vt::Emulator e(6, 2);
        e.feed("\xE6\x97\xA5\xE6\x9C\xAC"); // 日本
        CHECK(e.cursorX() == 4);
        CHECK(cell(e, 0, 0).pen.attrs & vt::Wide);
        CHECK(cell(e, 1, 0).pen.attrs & vt::WideTail);
        CHECK(row(e, 0) == QString::fromUtf8("日本"));
        e.feed("\x1b[2GX"); // overwrite the tail → head must be cleared
        CHECK(cell(e, 0, 0).isBlank());
        CHECK(row(e, 0) == QString::fromUtf8(" X本"));
    }
    { // combining mark attaches to the previous cell
        vt::Emulator e(6, 1);
        e.feed("e\xCC\x81x");
        CHECK(cell(e, 0, 0).text == QString::fromUtf8("e\xCC\x81"));
        CHECK(e.cursorX() == 2);
    }
    { // DECSC / DECRC
        vt::Emulator e(6, 3);
        e.feed("\x1b[3;3H\x1b" "7\x1b[H\x1b" "8Z");
        CHECK(row(e, 2) == "  Z");
    }
    { // ICH / DCH / IL / DL
        vt::Emulator e(8, 3);
        e.feed("abcdef\x1b[1G\x1b[2@");
        CHECK(row(e, 0) == "  abcdef");
        e.feed("\x1b[2P");
        CHECK(row(e, 0) == "abcdef");
        e.feed("\x1b[2;1Hsecond\x1b[1;1H\x1b[L");
        CHECK(row(e, 0).isEmpty());
        CHECK(row(e, 1) == "abcdef");
        CHECK(row(e, 2) == "second");
        e.feed("\x1b[M");
        CHECK(row(e, 0) == "abcdef");
        CHECK(row(e, 1) == "second");
    }
    { // resize: growing pulls lines back from the scrollback
        vt::Emulator e(5, 2);
        e.feed("1\r\n2\r\n3");
        CHECK(e.scrollbackSize() == 1);
        e.resize(5, 3);
        CHECK(e.scrollbackSize() == 0);
        CHECK(row(e, 0) == "1" && row(e, 1) == "2" && row(e, 2) == "3");
        CHECK(e.cursorY() == 2);
        e.resize(5, 1);
        CHECK(row(e, 0) == "3");
        CHECK(e.cursorY() == 0);
        CHECK(e.scrollbackSize() == 2);
    }
    { // title + bell
        vt::Emulator e(10, 1);
        int bells = 0;
        QObject::connect(&e, &vt::Emulator::bell, [&] { ++bells; });
        e.feed("\x1b]0;Hello\x07\x07\x1b]2;World\x1b\\");
        CHECK(e.title() == "World");
        CHECK(bells == 1);
    }
    { // unknown / query sequences are ignored without side effects
        vt::Emulator e(10, 1);
        e.feed("\x1b[?9001h\x1b[?1004h\x1b[?2026h\x1b[c\x1b[>0;1;0c\x1b[6n\x1b[=1u\x1b[>4;2m\x1bPq#0;2;0;0;0\x1b\\ok");
        CHECK(row(e, 0) == "ok");
    }
    { // modes we track
        vt::Emulator e(10, 1);
        e.feed("\x1b[?1h\x1b[?2004h\x1b[?25l\x1b[3 q");
        CHECK(e.appCursorKeys());
        CHECK(e.bracketedPaste());
        CHECK(!e.cursorVisible());
        CHECK(e.cursorStyle() == 3);
        e.feed("\x1b[?25h\x1b[?1l");
        CHECK(e.cursorVisible() && !e.appCursorKeys());
    }
    { // DEC line drawing
        vt::Emulator e(10, 1);
        e.feed("\x1b(0qx\x1b(B");
        CHECK(row(e, 0) == QString::fromUtf8("─│"));
    }
    { // Ink-style redraw: cursor up + erase line + rewrite
        vt::Emulator e(20, 5);
        e.feed("line one\r\nline two\r\n");
        e.feed("\x1b[2A\x1b[2K\x1b[1Gfirst\x1b[1B\x1b[2K\x1b[1Gsecond\r\n");
        CHECK(row(e, 0) == "first");
        CHECK(row(e, 1) == "second");
        CHECK(e.cursorY() == 2);
    }

    if (g_fails == 0) {
        std::printf("vt_test: all tests passed\n");
        return 0;
    }
    std::printf("vt_test: %d failure(s)\n", g_fails);
    return 1;
}
