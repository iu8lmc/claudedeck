#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <deque>
#include <vector>

namespace vt {

struct Color {
    enum Kind : uint8_t { Default = 0, Indexed = 1, Rgb = 2 };
    uint8_t kind = Default;
    uint8_t idx = 0;             // 0..255 when Indexed
    uint8_t r = 0, g = 0, b = 0; // when Rgb

    static Color indexed(int i)
    {
        Color c;
        c.kind = Indexed;
        c.idx = uint8_t(i & 0xFF);
        return c;
    }
    static Color rgb(int r, int g, int b)
    {
        Color c;
        c.kind = Rgb;
        c.r = uint8_t(r);
        c.g = uint8_t(g);
        c.b = uint8_t(b);
        return c;
    }
    bool operator==(const Color& o) const
    {
        return kind == o.kind && idx == o.idx && r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

enum Attr : uint16_t {
    Bold = 1 << 0,
    Dim = 1 << 1,
    Italic = 1 << 2,
    Underline = 1 << 3,
    Blink = 1 << 4,
    Inverse = 1 << 5,
    Hidden = 1 << 6,
    Strike = 1 << 7,
    Wide = 1 << 8,     // first half of a double-width glyph
    WideTail = 1 << 9, // second half of a double-width glyph (draws nothing)
    StyleMask = 0x00FF,
};

struct Pen {
    Color fg;
    Color bg;
    uint16_t attrs = 0;
    bool operator==(const Pen& o) const { return fg == o.fg && bg == o.bg && attrs == o.attrs; }
    bool operator!=(const Pen& o) const { return !(*this == o); }
};

struct Cell {
    QString text; // one grapheme (base char + combining marks); empty = blank
    Pen pen;
    bool isBlank() const { return text.isEmpty(); }
};

struct Line {
    std::vector<Cell> cells;
    bool wrapped = false; // soft-wrapped into the following line
};

// A compact xterm-style terminal emulator: enough of the VT100/VT220/xterm
// repertoire to render what ConPTY (and Ink-based TUIs such as Claude Code)
// produce, with a scrollback buffer and an alternate screen.
class Emulator : public QObject {
    Q_OBJECT
public:
    explicit Emulator(int cols = 80, int rows = 24, QObject* parent = nullptr);

    void feed(const QByteArray& data);
    void resize(int cols, int rows);
    void reset();

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int cursorX() const { return m_cx; }
    int cursorY() const { return m_cy; }
    bool cursorVisible() const { return m_cursorVisible; }
    int cursorStyle() const { return m_cursorStyle; } // DECSCUSR value, 0 = default
    bool altScreen() const { return m_useAlt; }
    bool appCursorKeys() const { return m_appCursorKeys; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    QString title() const { return m_title; }

    int scrollbackSize() const { return int(m_scrollback.size()); }
    int totalLines() const { return scrollbackSize() + m_rows; }
    void setMaxScrollback(int lines);

    // Line shown at view row `row` when the view is scrolled back by `offset`
    // lines (0 = live screen). Scrollback lines may be shorter or longer than
    // cols() when the terminal was resized; screen lines always have cols().
    const Line& viewLine(int row, int offset) const;
    // Absolute addressing: 0..totalLines()-1, scrollback first, then screen.
    const Line& absoluteLine(int absRow) const;

    static QString lineText(const Line& line, bool trimRight = true);
    QString screenText() const;

signals:
    void screenChanged();
    void scrollbackGrew(int lines);
    void bell();
    void titleChanged(const QString& title);

private:
    enum class State { Ground, Escape, EscInter, Csi, Osc, OscEsc, Dcs, DcsEsc };

    struct Param {
        int value = 0;
        bool sub = false; // introduced by ':' (sub-parameter of the previous one)
    };

    struct SavedCursor {
        int x = 0, y = 0;
        Pen pen;
        bool originMode = false;
        bool autoWrap = true;
        int charsets[2] = {0, 0};
        int charsetActive = 0;
        bool valid = false;
    };

    // parser
    void processByte(uint8_t b);
    void decodeUtf8(uint8_t b);
    void handleC0(uint8_t b);
    void handleEscape(uint8_t b);
    void handleEscInter(uint8_t b);
    void handleCsi(uint8_t b);
    void pushParam();
    void dispatchCsi(uint8_t final);
    void handleOsc(uint8_t b);
    void dispatchOsc();
    void setMode(bool on);
    void sgr();

    // screen model
    std::vector<Line>& screen() { return m_useAlt ? m_alt : m_main; }
    const std::vector<Line>& screen() const { return m_useAlt ? m_alt : m_main; }
    Line& lineAt(int y) { return screen()[size_t(y)]; }
    Cell eraseCell() const;
    Line blankLine() const;
    static bool isBlankLine(const Line& line);
    void clearWideAt(Line& line, int x);

    void print(char32_t cp);
    void linefeed();
    void reverseIndex();
    void scrollUp(int n, int top, int bottom, bool toScrollback);
    void scrollDown(int n, int top, int bottom);
    void pushScrollback(Line&& line);
    void trimScrollback();

    void cursorUp(int n);
    void cursorDown(int n);
    void cursorLeft(int n);
    void cursorRight(int n);
    void setCursorX(int x);
    void setCursorY(int y);
    void setCursorPos(int x, int y);

    void eraseLine(int mode);
    void eraseDisplay(int mode);
    void insertChars(int n);
    void deleteChars(int n);
    void eraseChars(int n);
    void insertLines(int n);
    void deleteLines(int n);
    void tabForward(int n);
    void tabBackward(int n);
    void resetTabs();
    void saveCursor();
    void restoreCursor();
    void switchAlt(bool on, bool withCursor);

    // geometry & buffers
    int m_cols;
    int m_rows;
    int m_cx = 0;
    int m_cy = 0;
    bool m_wrapPending = false;
    std::vector<Line> m_main;
    std::vector<Line> m_alt;
    std::deque<Line> m_scrollback;
    int m_maxScrollback = 5000;
    bool m_useAlt = false;

    // attributes & modes
    Pen m_pen;
    SavedCursor m_saved;
    int m_scrollTop = 0;
    int m_scrollBottom = 0;
    bool m_originMode = false;
    bool m_autoWrap = true;
    bool m_insertMode = false;
    bool m_lnm = false;
    bool m_cursorVisible = true;
    bool m_appCursorKeys = false;
    bool m_bracketedPaste = false;
    int m_cursorStyle = 0;
    int m_charsets[2] = {0, 0}; // 0 = ASCII, 1 = DEC special graphics
    int m_charsetActive = 0;
    std::vector<bool> m_tabs;
    QString m_title;

    // parser state
    State m_state = State::Ground;
    uint32_t m_utf8Cp = 0;
    int m_utf8Len = 0;
    uint8_t m_escInter = 0;
    uint8_t m_csiPrivate = 0;
    uint8_t m_csiInter = 0;
    std::vector<Param> m_params;
    int m_paramValue = 0;
    bool m_paramHasDigit = false;
    bool m_paramSub = false;
    QByteArray m_osc;
};

} // namespace vt
