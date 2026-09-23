#include "VtEmulator.h"
#include "Wcwidth.h"

#include <algorithm>

namespace vt {

namespace {

constexpr size_t kMaxParams = 64;
constexpr int kMaxOscLen = 8192;

// DEC Special Graphics (ESC ( 0) mapping for 0x60..0x7E
const char32_t kDecSpecial[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518,
    0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524,
    0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

int clampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

Emulator::Emulator(int cols, int rows, QObject* parent)
    : QObject(parent)
    , m_cols(std::max(2, cols))
    , m_rows(std::max(1, rows))
{
    m_main.assign(size_t(m_rows), blankLine());
    m_alt.assign(size_t(m_rows), blankLine());
    m_scrollBottom = m_rows - 1;
    resetTabs();
}

// ---------------------------------------------------------------------------
// public API

void Emulator::feed(const QByteArray& data)
{
    if (data.isEmpty())
        return;
    for (char ch : data)
        processByte(uint8_t(ch));
    emit screenChanged();
}

void Emulator::setMaxScrollback(int lines)
{
    m_maxScrollback = std::max(0, lines);
    trimScrollback();
}

void Emulator::reset()
{
    m_pen = Pen();
    m_cx = m_cy = 0;
    m_wrapPending = false;
    m_useAlt = false;
    for (Line& l : m_main)
        l = blankLine();
    for (Line& l : m_alt)
        l = blankLine();
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_originMode = false;
    m_autoWrap = true;
    m_insertMode = false;
    m_lnm = false;
    m_cursorVisible = true;
    m_appCursorKeys = false;
    m_bracketedPaste = false;
    m_cursorStyle = 0;
    m_charsets[0] = m_charsets[1] = 0;
    m_charsetActive = 0;
    m_saved = SavedCursor();
    resetTabs();
    m_state = State::Ground;
    m_utf8Len = 0;
    emit screenChanged();
}

void Emulator::resize(int cols, int rows)
{
    cols = std::max(2, cols);
    rows = std::max(1, rows);
    if (cols == m_cols && rows == m_rows)
        return;

    // The main buffer exchanges rows with the scrollback so that the content
    // the user is looking at stays put (no reflow of long lines, like conhost).
    int& mainCy = m_useAlt ? m_saved.y : m_cy;
    if (rows < m_rows) {
        int toRemove = m_rows - rows;
        // drop blank lines below the cursor first
        while (toRemove > 0 && int(m_main.size()) - 1 > mainCy && isBlankLine(m_main.back())) {
            m_main.pop_back();
            --toRemove;
        }
        while (toRemove > 0 && !m_main.empty()) {
            pushScrollback(std::move(m_main.front()));
            m_main.erase(m_main.begin());
            --toRemove;
            if (mainCy > 0)
                --mainCy;
        }
        trimScrollback();
    } else if (rows > m_rows) {
        int toAdd = rows - m_rows;
        while (toAdd > 0 && !m_scrollback.empty()) {
            m_main.insert(m_main.begin(), std::move(m_scrollback.back()));
            m_scrollback.pop_back();
            --toAdd;
            ++mainCy;
        }
        while (toAdd > 0) {
            m_main.push_back(blankLine());
            --toAdd;
        }
    }
    m_main.resize(size_t(rows), blankLine());
    m_alt.resize(size_t(rows), blankLine());

    auto fitLine = [cols](Line& l) {
        l.cells.resize(size_t(cols), Cell());
        if (l.cells.back().pen.attrs & Wide) // tail was cut off
            l.cells.back() = Cell();
    };
    for (Line& l : m_main)
        fitLine(l);
    for (Line& l : m_alt)
        fitLine(l);

    m_cols = cols;
    m_rows = rows;
    m_scrollTop = 0;
    m_scrollBottom = rows - 1;
    m_cx = clampInt(m_cx, 0, cols - 1);
    m_cy = clampInt(m_cy, 0, rows - 1);
    m_saved.x = clampInt(m_saved.x, 0, cols - 1);
    m_saved.y = clampInt(m_saved.y, 0, rows - 1);
    m_wrapPending = false;
    resetTabs();
    emit screenChanged();
}

const Line& Emulator::viewLine(int row, int offset) const
{
    const std::vector<Line>& s = screen();
    if (m_useAlt)
        offset = 0;
    const int sb = int(m_scrollback.size());
    offset = clampInt(offset, 0, sb);
    row = clampInt(row, 0, m_rows - 1);
    if (row < offset)
        return m_scrollback[size_t(sb - offset + row)];
    return s[size_t(row - offset)];
}

const Line& Emulator::absoluteLine(int absRow) const
{
    const int sb = int(m_scrollback.size());
    absRow = clampInt(absRow, 0, sb + m_rows - 1);
    if (absRow < sb)
        return m_scrollback[size_t(absRow)];
    return screen()[size_t(absRow - sb)];
}

QString Emulator::lineText(const Line& line, bool trimRight)
{
    QString out;
    out.reserve(int(line.cells.size()));
    for (const Cell& c : line.cells) {
        if (c.pen.attrs & WideTail)
            continue;
        if (c.text.isEmpty())
            out.append(QLatin1Char(' '));
        else
            out.append(c.text);
    }
    if (trimRight) {
        int n = out.size();
        while (n > 0 && out.at(n - 1) == QLatin1Char(' '))
            --n;
        out.truncate(n);
    }
    return out;
}

QString Emulator::screenText() const
{
    QString out;
    for (int r = 0; r < m_rows; ++r) {
        if (r)
            out.append(QLatin1Char('\n'));
        out.append(lineText(viewLine(r, 0)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// parser

void Emulator::processByte(uint8_t b)
{
    switch (m_state) {
    case State::Ground:
        if (b == 0x1B) {
            m_utf8Len = 0;
            m_state = State::Escape;
        } else if (b < 0x20) {
            m_utf8Len = 0;
            handleC0(b);
        } else if (b == 0x7F) {
            // DEL: ignored
        } else {
            decodeUtf8(b);
        }
        break;
    case State::Escape:
        handleEscape(b);
        break;
    case State::EscInter:
        handleEscInter(b);
        break;
    case State::Csi:
        handleCsi(b);
        break;
    case State::Osc:
        handleOsc(b);
        break;
    case State::OscEsc:
        if (b == '\\') {
            dispatchOsc();
            m_state = State::Ground;
        } else {
            // ESC not followed by ST: abandon the OSC, treat as a new escape
            m_state = State::Escape;
            handleEscape(b);
        }
        break;
    case State::Dcs:
        if (b == 0x1B)
            m_state = State::DcsEsc;
        break;
    case State::DcsEsc:
        if (b == '\\')
            m_state = State::Ground;
        else if (b != 0x1B)
            m_state = State::Dcs;
        break;
    }
}

void Emulator::decodeUtf8(uint8_t b)
{
    if (m_utf8Len == 0) {
        if (b < 0x80) {
            print(b);
        } else if ((b & 0xE0) == 0xC0) {
            m_utf8Cp = b & 0x1F;
            m_utf8Len = 1;
        } else if ((b & 0xF0) == 0xE0) {
            m_utf8Cp = b & 0x0F;
            m_utf8Len = 2;
        } else if ((b & 0xF8) == 0xF0) {
            m_utf8Cp = b & 0x07;
            m_utf8Len = 3;
        } else {
            print(0xFFFD);
        }
        return;
    }
    if ((b & 0xC0) != 0x80) {
        // broken sequence: emit replacement and reprocess this byte
        m_utf8Len = 0;
        print(0xFFFD);
        processByte(b);
        return;
    }
    m_utf8Cp = (m_utf8Cp << 6) | (b & 0x3F);
    if (--m_utf8Len == 0) {
        char32_t cp = m_utf8Cp;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            cp = 0xFFFD;
        print(cp);
    }
}

void Emulator::handleC0(uint8_t b)
{
    switch (b) {
    case 0x07:
        emit bell();
        break;
    case 0x08:
        m_wrapPending = false;
        if (m_cx > 0)
            --m_cx;
        break;
    case 0x09:
        tabForward(1);
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        linefeed();
        if (m_lnm)
            m_cx = 0;
        break;
    case 0x0D:
        m_cx = 0;
        m_wrapPending = false;
        break;
    case 0x0E:
        m_charsetActive = 1;
        break;
    case 0x0F:
        m_charsetActive = 0;
        break;
    default:
        break;
    }
}

void Emulator::handleEscape(uint8_t b)
{
    if (b < 0x20 && b != 0x1B) {
        handleC0(b); // C0 controls execute inside escape sequences
        return;
    }
    m_state = State::Ground;
    switch (b) {
    case '[':
        m_params.clear();
        m_paramValue = 0;
        m_paramHasDigit = false;
        m_paramSub = false;
        m_csiPrivate = 0;
        m_csiInter = 0;
        m_state = State::Csi;
        break;
    case ']':
        m_osc.clear();
        m_state = State::Osc;
        break;
    case 'P': // DCS
    case 'X': // SOS
    case '^': // PM
    case '_': // APC
        m_state = State::Dcs;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '#':
    case '%':
    case ' ':
        m_escInter = b;
        m_state = State::EscInter;
        break;
    case '7':
        saveCursor();
        break;
    case '8':
        restoreCursor();
        break;
    case 'D': // IND
        linefeed();
        break;
    case 'E': // NEL
        m_cx = 0;
        linefeed();
        break;
    case 'H': // HTS
        if (m_cx < int(m_tabs.size()))
            m_tabs[size_t(m_cx)] = true;
        break;
    case 'M': // RI
        reverseIndex();
        break;
    case 'c': // RIS
        reset();
        break;
    case 0x1B:
        m_state = State::Escape;
        break;
    default: // '=', '>', '\\', 'N', 'O', ... ignored
        break;
    }
}

void Emulator::handleEscInter(uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        m_escInter = b; // further intermediate
        return;
    }
    m_state = State::Ground;
    switch (m_escInter) {
    case '(':
        m_charsets[0] = (b == '0') ? 1 : 0;
        break;
    case ')':
        m_charsets[1] = (b == '0') ? 1 : 0;
        break;
    case '#':
        if (b == '8') { // DECALN
            for (Line& l : screen())
                for (Cell& c : l.cells) {
                    c = Cell();
                    c.text = QStringLiteral("E");
                }
            setCursorPos(0, 0);
        }
        break;
    default:
        break;
    }
}

void Emulator::pushParam()
{
    Param p;
    p.value = m_paramValue;
    p.sub = m_paramSub;
    if (m_params.size() < kMaxParams)
        m_params.push_back(p);
    m_paramValue = 0;
    m_paramHasDigit = false;
    m_paramSub = false;
}

void Emulator::handleCsi(uint8_t b)
{
    if (b >= '0' && b <= '9') {
        m_paramValue = std::min(m_paramValue * 10 + (b - '0'), 1000000);
        m_paramHasDigit = true;
    } else if (b == ';' || b == ':') {
        pushParam();
        m_paramSub = (b == ':');
    } else if (b >= 0x3C && b <= 0x3F) { // < = > ?
        if (m_params.empty() && !m_paramHasDigit)
            m_csiPrivate = b;
        else
            m_csiPrivate = 0xFF; // malformed: ignore at dispatch
    } else if (b >= 0x20 && b <= 0x2F) {
        m_csiInter = b;
    } else if (b >= 0x40 && b <= 0x7E) {
        pushParam();
        if (m_csiPrivate != 0xFF)
            dispatchCsi(b);
        m_state = State::Ground;
    } else if (b == 0x1B) {
        m_state = State::Escape;
    } else if (b == 0x18 || b == 0x1A) {
        m_state = State::Ground;
    } else if (b < 0x20) {
        handleC0(b);
    } else {
        m_state = State::Ground; // 0x7F or 8-bit byte: abort
    }
}

void Emulator::dispatchCsi(uint8_t final)
{
    const int n = int(m_params.size());
    auto P = [&](int i, int def) {
        const int v = (i < n) ? m_params[size_t(i)].value : 0;
        return v > 0 ? v : def;
    };
    auto P0 = [&](int i) { return (i < n) ? m_params[size_t(i)].value : 0; };

    if (m_csiInter == ' ' && final == 'q') { // DECSCUSR
        m_cursorStyle = P0(0);
        return;
    }
    if (m_csiInter != 0)
        return;

    if (m_csiPrivate == '?') {
        switch (final) {
        case 'h':
            setMode(true);
            break;
        case 'l':
            setMode(false);
            break;
        case 'J':
            eraseDisplay(P0(0));
            break;
        case 'K':
            eraseLine(P0(0));
            break;
        default:
            break;
        }
        return;
    }
    if (m_csiPrivate != 0)
        return; // '>' '=' '<' forms (XTMODKEYS, DA2, ...) are not needed

    switch (final) {
    case '@':
        insertChars(P(0, 1));
        break;
    case 'A':
        cursorUp(P(0, 1));
        break;
    case 'B':
        cursorDown(P(0, 1));
        break;
    case 'C':
        cursorRight(P(0, 1));
        break;
    case 'D':
        cursorLeft(P(0, 1));
        break;
    case 'E':
        cursorDown(P(0, 1));
        m_cx = 0;
        break;
    case 'F':
        cursorUp(P(0, 1));
        m_cx = 0;
        break;
    case 'G':
    case '`':
        setCursorX(P(0, 1) - 1);
        break;
    case 'H':
    case 'f':
        setCursorPos(P(1, 1) - 1, P(0, 1) - 1);
        break;
    case 'I':
        tabForward(P(0, 1));
        break;
    case 'J':
        eraseDisplay(P0(0));
        break;
    case 'K':
        eraseLine(P0(0));
        break;
    case 'L':
        insertLines(P(0, 1));
        break;
    case 'M':
        deleteLines(P(0, 1));
        break;
    case 'P':
        deleteChars(P(0, 1));
        break;
    case 'S':
        scrollUp(P(0, 1), m_scrollTop, m_scrollBottom, false);
        break;
    case 'T':
        scrollDown(P(0, 1), m_scrollTop, m_scrollBottom);
        break;
    case 'X':
        eraseChars(P(0, 1));
        break;
    case 'Z':
        tabBackward(P(0, 1));
        break;
    case 'a':
        cursorRight(P(0, 1));
        break;
    case 'd':
        setCursorY(P(0, 1) - 1);
        break;
    case 'e':
        cursorDown(P(0, 1));
        break;
    case 'g':
        if (P0(0) == 3)
            std::fill(m_tabs.begin(), m_tabs.end(), false);
        else if (P0(0) == 0 && m_cx < int(m_tabs.size()))
            m_tabs[size_t(m_cx)] = false;
        break;
    case 'h':
        setMode(true);
        break;
    case 'l':
        setMode(false);
        break;
    case 'm':
        sgr();
        break;
    case 'r': { // DECSTBM
        int top = P(0, 1) - 1;
        int bottom = P(1, m_rows) - 1;
        top = clampInt(top, 0, m_rows - 1);
        bottom = clampInt(bottom, 0, m_rows - 1);
        if (top < bottom) {
            m_scrollTop = top;
            m_scrollBottom = bottom;
            setCursorPos(0, 0);
        }
        break;
    }
    case 's':
        saveCursor();
        break;
    case 'u':
        restoreCursor();
        break;
    default: // 'c' DA, 'n' DSR, 't' window ops, ...: conhost answers those itself
        break;
    }
}

void Emulator::setMode(bool on)
{
    for (const Param& p : m_params) {
        if (p.sub)
            continue;
        if (m_csiPrivate == '?') {
            switch (p.value) {
            case 1:
                m_appCursorKeys = on;
                break;
            case 6:
                m_originMode = on;
                setCursorPos(0, 0);
                break;
            case 7:
                m_autoWrap = on;
                if (!on)
                    m_wrapPending = false;
                break;
            case 25:
                m_cursorVisible = on;
                break;
            case 47:
            case 1047:
                switchAlt(on, false);
                break;
            case 1048:
                if (on)
                    saveCursor();
                else
                    restoreCursor();
                break;
            case 1049:
                switchAlt(on, true);
                break;
            case 2004:
                m_bracketedPaste = on;
                break;
            default: // 12 (blink), 1004 (focus), 9001 (win32-input), 2026 (sync), mouse modes...
                break;
            }
        } else {
            switch (p.value) {
            case 4:
                m_insertMode = on;
                break;
            case 20:
                m_lnm = on;
                break;
            default:
                break;
            }
        }
    }
}

void Emulator::sgr()
{
    const int n = int(m_params.size());
    auto val = [&](int i) { return m_params[size_t(i)].value; };
    for (int i = 0; i < n; ++i) {
        if (m_params[size_t(i)].sub)
            continue; // stray sub-parameters (e.g. 4:3) are ignored
        const int v = val(i);
        switch (v) {
        case 0:
            m_pen = Pen();
            break;
        case 1:
            m_pen.attrs |= Bold;
            break;
        case 2:
            m_pen.attrs |= Dim;
            break;
        case 3:
            m_pen.attrs |= Italic;
            break;
        case 4:
        case 21:
            m_pen.attrs |= Underline;
            break;
        case 5:
        case 6:
            m_pen.attrs |= Blink;
            break;
        case 7:
            m_pen.attrs |= Inverse;
            break;
        case 8:
            m_pen.attrs |= Hidden;
            break;
        case 9:
            m_pen.attrs |= Strike;
            break;
        case 22:
            m_pen.attrs &= uint16_t(~(Bold | Dim));
            break;
        case 23:
            m_pen.attrs &= uint16_t(~Italic);
            break;
        case 24:
            m_pen.attrs &= uint16_t(~Underline);
            break;
        case 25:
            m_pen.attrs &= uint16_t(~Blink);
            break;
        case 27:
            m_pen.attrs &= uint16_t(~Inverse);
            break;
        case 28:
            m_pen.attrs &= uint16_t(~Hidden);
            break;
        case 29:
            m_pen.attrs &= uint16_t(~Strike);
            break;
        case 38:
        case 48:
        case 58: {
            // 38;5;n  38;2;r;g;b  38:5:n  38:2:r:g:b  38:2::r:g:b
            std::vector<int> args;
            int j = i + 1;
            const bool colon = (j < n) && m_params[size_t(j)].sub;
            if (colon) {
                while (j < n && m_params[size_t(j)].sub) {
                    args.push_back(val(j));
                    ++j;
                }
            } else if (j < n) {
                const int mode = val(j);
                args.push_back(mode);
                ++j;
                const int need = (mode == 5) ? 1 : (mode == 2) ? 3 : 0;
                for (int k = 0; k < need && j < n; ++k, ++j)
                    args.push_back(val(j));
            }
            Color c;
            bool ok = false;
            if (!args.empty()) {
                if (args[0] == 5 && args.size() >= 2) {
                    c = Color::indexed(clampInt(args[1], 0, 255));
                    ok = true;
                } else if (args[0] == 2 && args.size() >= 5) { // with colour-space id
                    c = Color::rgb(clampInt(args[2], 0, 255), clampInt(args[3], 0, 255), clampInt(args[4], 0, 255));
                    ok = true;
                } else if (args[0] == 2 && args.size() >= 4) {
                    c = Color::rgb(clampInt(args[1], 0, 255), clampInt(args[2], 0, 255), clampInt(args[3], 0, 255));
                    ok = true;
                }
            }
            if (ok) {
                if (v == 38)
                    m_pen.fg = c;
                else if (v == 48)
                    m_pen.bg = c;
            }
            i = j - 1;
            break;
        }
        case 39:
            m_pen.fg = Color();
            break;
        case 49:
            m_pen.bg = Color();
            break;
        default:
            if (v >= 30 && v <= 37)
                m_pen.fg = Color::indexed(v - 30);
            else if (v >= 40 && v <= 47)
                m_pen.bg = Color::indexed(v - 40);
            else if (v >= 90 && v <= 97)
                m_pen.fg = Color::indexed(v - 90 + 8);
            else if (v >= 100 && v <= 107)
                m_pen.bg = Color::indexed(v - 100 + 8);
            break;
        }
    }
}

void Emulator::handleOsc(uint8_t b)
{
    if (b == 0x07) {
        dispatchOsc();
        m_state = State::Ground;
    } else if (b == 0x1B) {
        m_state = State::OscEsc;
    } else if (b == 0x18 || b == 0x1A) {
        m_state = State::Ground;
    } else if (b >= 0x20 || b == 0x09) {
        if (m_osc.size() < kMaxOscLen)
            m_osc.append(char(b));
    }
}

void Emulator::dispatchOsc()
{
    const int semi = int(m_osc.indexOf(';'));
    const QByteArray cmd = semi < 0 ? m_osc : m_osc.left(semi);
    const QByteArray arg = semi < 0 ? QByteArray() : m_osc.mid(semi + 1);
    bool ok = false;
    const int code = cmd.toInt(&ok);
    if (!ok)
        return;
    if (code == 0 || code == 2) {
        const QString t = QString::fromUtf8(arg);
        if (t != m_title) {
            m_title = t;
            emit titleChanged(t);
        }
    }
    // 4/10/11 (colours), 8 (hyperlinks), 9;4 (progress), 52 (clipboard): ignored
}

// ---------------------------------------------------------------------------
// screen model

Cell Emulator::eraseCell() const
{
    Cell c;
    c.pen.bg = m_pen.bg; // back-colour-erase, like xterm/Windows Terminal
    return c;
}

Line Emulator::blankLine() const
{
    Line l;
    l.cells.assign(size_t(m_cols), eraseCell());
    return l;
}

bool Emulator::isBlankLine(const Line& line)
{
    for (const Cell& c : line.cells)
        if (!c.isBlank())
            return false;
    return true;
}

void Emulator::clearWideAt(Line& line, int x)
{
    if (x < 0 || x >= int(line.cells.size()))
        return;
    const uint16_t a = line.cells[size_t(x)].pen.attrs;
    if ((a & WideTail) && x > 0) {
        Cell& head = line.cells[size_t(x - 1)];
        Cell e = eraseCell();
        e.pen.bg = head.pen.bg;
        head = e;
    } else if ((a & Wide) && x + 1 < int(line.cells.size())) {
        Cell& tail = line.cells[size_t(x + 1)];
        Cell e = eraseCell();
        e.pen.bg = tail.pen.bg;
        tail = e;
    }
}

void Emulator::print(char32_t cp)
{
    if (m_charsets[m_charsetActive] == 1 && cp >= 0x60 && cp <= 0x7E)
        cp = kDecSpecial[cp - 0x60];

    const int w = charWidth(cp);
    if (w == 0) {
        // combining mark: attach to the previous cell
        Line& line = lineAt(m_cy);
        const int x = m_wrapPending ? m_cx : m_cx - 1;
        if (x >= 0 && x < m_cols) {
            Cell& c = line.cells[size_t(x)];
            if ((c.pen.attrs & WideTail) && x > 0) {
                Cell& head = line.cells[size_t(x - 1)];
                if (!head.text.isEmpty())
                    head.text.append(QString::fromUcs4(&cp, 1));
            } else if (!c.text.isEmpty()) {
                c.text.append(QString::fromUcs4(&cp, 1));
            }
        }
        return;
    }

    if (m_wrapPending) {
        m_wrapPending = false;
        if (m_autoWrap) {
            lineAt(m_cy).wrapped = true;
            m_cx = 0;
            linefeed();
        }
    }
    if (w == 2 && m_cx >= m_cols - 1) {
        if (m_autoWrap) {
            Line& line = lineAt(m_cy);
            clearWideAt(line, m_cx);
            line.cells[size_t(m_cx)] = eraseCell();
            line.wrapped = true;
            m_cx = 0;
            linefeed();
        } else {
            m_cx = std::max(0, m_cols - 2);
        }
    }

    Line& line = lineAt(m_cy);
    if (m_insertMode) {
        line.cells.insert(line.cells.begin() + m_cx, size_t(w), eraseCell());
        line.cells.resize(size_t(m_cols));
    }
    clearWideAt(line, m_cx);

    Cell c;
    c.text = QString::fromUcs4(&cp, 1);
    c.pen = m_pen;
    if (w == 2) {
        c.pen.attrs |= Wide;
        line.cells[size_t(m_cx)] = c;
        if (m_cx + 1 < m_cols) {
            clearWideAt(line, m_cx + 1);
            Cell tail;
            tail.pen = m_pen;
            tail.pen.attrs |= WideTail;
            line.cells[size_t(m_cx + 1)] = tail;
        }
    } else {
        line.cells[size_t(m_cx)] = c;
    }

    m_cx += w;
    if (m_cx >= m_cols) {
        m_cx = m_cols - 1;
        if (m_autoWrap)
            m_wrapPending = true;
    }
}

void Emulator::linefeed()
{
    m_wrapPending = false;
    if (m_cy == m_scrollBottom)
        scrollUp(1, m_scrollTop, m_scrollBottom, true);
    else if (m_cy < m_rows - 1)
        ++m_cy;
}

void Emulator::reverseIndex()
{
    m_wrapPending = false;
    if (m_cy == m_scrollTop)
        scrollDown(1, m_scrollTop, m_scrollBottom);
    else if (m_cy > 0)
        --m_cy;
}

void Emulator::scrollUp(int n, int top, int bottom, bool toScrollback)
{
    if (top < 0 || bottom >= m_rows || top > bottom)
        return;
    n = clampInt(n, 0, bottom - top + 1);
    if (n == 0)
        return;
    std::vector<Line>& s = screen();
    int grew = 0;
    for (int i = 0; i < n; ++i) {
        if (toScrollback && top == 0 && !m_useAlt) {
            pushScrollback(std::move(s[size_t(top)]));
            ++grew;
        }
        s.erase(s.begin() + top);
        s.insert(s.begin() + bottom, blankLine());
    }
    if (grew) {
        trimScrollback();
        emit scrollbackGrew(grew);
    }
}

void Emulator::scrollDown(int n, int top, int bottom)
{
    if (top < 0 || bottom >= m_rows || top > bottom)
        return;
    n = clampInt(n, 0, bottom - top + 1);
    std::vector<Line>& s = screen();
    for (int i = 0; i < n; ++i) {
        s.erase(s.begin() + bottom);
        s.insert(s.begin() + top, blankLine());
    }
}

void Emulator::pushScrollback(Line&& line)
{
    if (m_maxScrollback <= 0)
        return;
    m_scrollback.push_back(std::move(line));
}

void Emulator::trimScrollback()
{
    while (int(m_scrollback.size()) > m_maxScrollback)
        m_scrollback.pop_front();
}

void Emulator::cursorUp(int n)
{
    m_wrapPending = false;
    const int minY = (m_cy >= m_scrollTop) ? m_scrollTop : 0;
    m_cy = std::max(minY, m_cy - n);
}

void Emulator::cursorDown(int n)
{
    m_wrapPending = false;
    const int maxY = (m_cy <= m_scrollBottom) ? m_scrollBottom : m_rows - 1;
    m_cy = std::min(maxY, m_cy + n);
}

void Emulator::cursorLeft(int n)
{
    m_wrapPending = false;
    m_cx = std::max(0, m_cx - n);
}

void Emulator::cursorRight(int n)
{
    m_wrapPending = false;
    m_cx = std::min(m_cols - 1, m_cx + n);
}

void Emulator::setCursorX(int x)
{
    m_wrapPending = false;
    m_cx = clampInt(x, 0, m_cols - 1);
}

void Emulator::setCursorY(int y)
{
    m_wrapPending = false;
    if (m_originMode) {
        y += m_scrollTop;
        m_cy = clampInt(y, m_scrollTop, m_scrollBottom);
    } else {
        m_cy = clampInt(y, 0, m_rows - 1);
    }
}

void Emulator::setCursorPos(int x, int y)
{
    setCursorY(y);
    setCursorX(x);
}

void Emulator::eraseLine(int mode)
{
    Line& line = lineAt(m_cy);
    int from = 0, to = m_cols - 1;
    if (mode == 0)
        from = m_cx;
    else if (mode == 1)
        to = m_cx;
    else if (mode != 2)
        return;
    clearWideAt(line, from);
    clearWideAt(line, to);
    for (int x = from; x <= to; ++x)
        line.cells[size_t(x)] = eraseCell();
    if (mode == 2)
        line.wrapped = false;
}

void Emulator::eraseDisplay(int mode)
{
    std::vector<Line>& s = screen();
    switch (mode) {
    case 0:
        eraseLine(0);
        for (int y = m_cy + 1; y < m_rows; ++y)
            s[size_t(y)] = blankLine();
        break;
    case 1:
        eraseLine(1);
        for (int y = 0; y < m_cy; ++y)
            s[size_t(y)] = blankLine();
        break;
    case 2:
        for (Line& l : s)
            l = blankLine();
        break;
    case 3:
        m_scrollback.clear();
        break;
    default:
        break;
    }
}

void Emulator::insertChars(int n)
{
    Line& line = lineAt(m_cy);
    n = clampInt(n, 0, m_cols - m_cx);
    if (n == 0)
        return;
    clearWideAt(line, m_cx);
    line.cells.insert(line.cells.begin() + m_cx, size_t(n), eraseCell());
    line.cells.resize(size_t(m_cols));
    if (line.cells.back().pen.attrs & Wide)
        line.cells.back() = eraseCell();
}

void Emulator::deleteChars(int n)
{
    Line& line = lineAt(m_cy);
    n = clampInt(n, 0, m_cols - m_cx);
    if (n == 0)
        return;
    clearWideAt(line, m_cx);
    clearWideAt(line, m_cx + n - 1);
    line.cells.erase(line.cells.begin() + m_cx, line.cells.begin() + m_cx + n);
    line.cells.resize(size_t(m_cols), eraseCell());
}

void Emulator::eraseChars(int n)
{
    Line& line = lineAt(m_cy);
    n = clampInt(n, 0, m_cols - m_cx);
    if (n == 0)
        return;
    clearWideAt(line, m_cx);
    clearWideAt(line, m_cx + n - 1);
    for (int x = m_cx; x < m_cx + n; ++x)
        line.cells[size_t(x)] = eraseCell();
}

void Emulator::insertLines(int n)
{
    if (m_cy < m_scrollTop || m_cy > m_scrollBottom)
        return;
    scrollDown(n, m_cy, m_scrollBottom);
    m_cx = 0;
    m_wrapPending = false;
}

void Emulator::deleteLines(int n)
{
    if (m_cy < m_scrollTop || m_cy > m_scrollBottom)
        return;
    scrollUp(n, m_cy, m_scrollBottom, false);
    m_cx = 0;
    m_wrapPending = false;
}

void Emulator::tabForward(int n)
{
    m_wrapPending = false;
    while (n-- > 0) {
        int x = m_cx + 1;
        while (x < m_cols - 1 && !m_tabs[size_t(x)])
            ++x;
        m_cx = std::min(x, m_cols - 1);
    }
}

void Emulator::tabBackward(int n)
{
    m_wrapPending = false;
    while (n-- > 0) {
        int x = m_cx - 1;
        while (x > 0 && !m_tabs[size_t(x)])
            --x;
        m_cx = std::max(x, 0);
    }
}

void Emulator::resetTabs()
{
    m_tabs.assign(size_t(m_cols), false);
    for (int x = 8; x < m_cols; x += 8)
        m_tabs[size_t(x)] = true;
}

void Emulator::saveCursor()
{
    m_saved.x = m_cx;
    m_saved.y = m_cy;
    m_saved.pen = m_pen;
    m_saved.originMode = m_originMode;
    m_saved.autoWrap = m_autoWrap;
    m_saved.charsets[0] = m_charsets[0];
    m_saved.charsets[1] = m_charsets[1];
    m_saved.charsetActive = m_charsetActive;
    m_saved.valid = true;
}

void Emulator::restoreCursor()
{
    m_wrapPending = false;
    if (!m_saved.valid) {
        setCursorPos(0, 0);
        m_pen = Pen();
        return;
    }
    m_pen = m_saved.pen;
    m_originMode = m_saved.originMode;
    m_autoWrap = m_saved.autoWrap;
    m_charsets[0] = m_saved.charsets[0];
    m_charsets[1] = m_saved.charsets[1];
    m_charsetActive = m_saved.charsetActive;
    m_cx = clampInt(m_saved.x, 0, m_cols - 1);
    m_cy = clampInt(m_saved.y, 0, m_rows - 1);
}

void Emulator::switchAlt(bool on, bool withCursor)
{
    if (on == m_useAlt)
        return;
    if (on) {
        if (withCursor)
            saveCursor();
        m_useAlt = true;
        for (Line& l : m_alt)
            l = blankLine();
    } else {
        m_useAlt = false;
        if (withCursor)
            restoreCursor();
    }
    m_wrapPending = false;
}

} // namespace vt
