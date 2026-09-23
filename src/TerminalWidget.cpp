#include "TerminalWidget.h"

#include <QClipboard>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

const QColor kDefaultBg(0x0C, 0x0C, 0x0C);
const QColor kDefaultFg(0xCC, 0xCC, 0xCC);
const QColor kSelection(0x58, 0x96, 0xFF, 0x60);

// Windows Terminal "Campbell" palette
const QColor kPalette16[16] = {
    QColor(0x0C, 0x0C, 0x0C), QColor(0xC5, 0x0F, 0x1F), QColor(0x13, 0xA1, 0x0E), QColor(0xC1, 0x9C, 0x00),
    QColor(0x00, 0x37, 0xDA), QColor(0x88, 0x17, 0x98), QColor(0x3A, 0x96, 0xDD), QColor(0xCC, 0xCC, 0xCC),
    QColor(0x76, 0x76, 0x76), QColor(0xE7, 0x48, 0x56), QColor(0x16, 0xC6, 0x0C), QColor(0xF9, 0xF1, 0xA5),
    QColor(0x3B, 0x78, 0xFF), QColor(0xB4, 0x00, 0x9E), QColor(0x61, 0xD6, 0xD6), QColor(0xF2, 0xF2, 0xF2),
};

QColor palette256(int i)
{
    i = std::clamp(i, 0, 255);
    if (i < 16)
        return kPalette16[i];
    if (i < 232) {
        const int idx = i - 16;
        const int r = idx / 36, g = (idx / 6) % 6, b = idx % 6;
        auto level = [](int v) { return v == 0 ? 0 : 55 + 40 * v; };
        return QColor(level(r), level(g), level(b));
    }
    const int gray = 8 + 10 * (i - 232);
    return QColor(gray, gray, gray);
}

QColor mix(const QColor& a, const QColor& b, qreal t)
{
    return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue() + (b.blue() - a.blue()) * t));
}

bool samePen(const vt::Pen& a, const vt::Pen& b)
{
    return a.fg == b.fg && a.bg == b.bg && (a.attrs & vt::StyleMask) == (b.attrs & vt::StyleMask);
}

} // namespace

TerminalWidget::TerminalWidget(QWidget* parent)
    : QWidget(parent)
    , m_emu(new vt::Emulator(120, 30, this))
    , m_scrollBar(new QScrollBar(Qt::Vertical, this))
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled);
    setCursor(Qt::IBeamCursor);

    m_scrollBar->setRange(0, 0);
    m_scrollBar->setFocusPolicy(Qt::NoFocus);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int v) {
        const int off = m_scrollBar->maximum() - v;
        if (off != m_scrollOffset) {
            m_scrollOffset = off;
            update();
        }
    });

    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(16);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this] {
        updateScrollBar();
        update();
    });
    m_blinkTimer.setInterval(530);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        update();
    });

    connect(m_emu, &vt::Emulator::screenChanged, this, &TerminalWidget::scheduleRepaint);
    connect(m_emu, &vt::Emulator::scrollbackGrew, this, [this](int n) {
        // keep the view anchored to the content while scrolled back
        if (m_scrollOffset > 0)
            m_scrollOffset = std::min(m_scrollOffset + n, m_emu->scrollbackSize());
    });

    QFont f(QStringLiteral("Cascadia Mono"));
    f.setPointSize(11);
    setTerminalFont(f);
}

// ---------------------------------------------------------------------------
// font & geometry

void TerminalWidget::setTerminalFont(const QFont& font)
{
    m_font = font;
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_font.setKerning(false);
    recomputeMetrics();
    updateGrid();
}

void TerminalWidget::recomputeMetrics()
{
    const QFontMetricsF fm(m_font);
    m_cellW = std::max<qreal>(1.0, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellH = std::max<qreal>(1.0, std::ceil(fm.height()));
    m_ascent = fm.ascent();
}

void TerminalWidget::updateGrid()
{
    const int sbw = m_scrollBar->sizeHint().width();
    m_scrollBar->setGeometry(width() - sbw, 0, sbw, height());
    const int availW = width() - sbw - m_padding.left() - m_padding.right();
    const int availH = height() - m_padding.top() - m_padding.bottom();
    const int cols = std::max(2, int(availW / m_cellW));
    const int rows = std::max(1, int(availH / m_cellH));
    if (cols != m_cols || rows != m_rows) {
        m_cols = cols;
        m_rows = rows;
        m_emu->resize(cols, rows);
        emit gridResized(cols, rows);
    }
    updateScrollBar();
    update();
}

int TerminalWidget::viewOffset() const
{
    if (m_emu->altScreen())
        return 0;
    return std::clamp(m_scrollOffset, 0, m_emu->scrollbackSize());
}

void TerminalWidget::updateScrollBar()
{
    const int sb = m_emu->altScreen() ? 0 : m_emu->scrollbackSize();
    m_scrollOffset = std::clamp(m_scrollOffset, 0, sb);
    const QSignalBlocker blocker(m_scrollBar);
    m_scrollBar->setRange(0, sb);
    m_scrollBar->setPageStep(m_rows);
    m_scrollBar->setSingleStep(1);
    m_scrollBar->setValue(sb - m_scrollOffset);
}

void TerminalWidget::scheduleRepaint()
{
    if (!m_repaintTimer.isActive())
        m_repaintTimer.start();
}

QSize TerminalWidget::sizeHint() const
{
    return QSize(int(100 * m_cellW) + m_padding.left() + m_padding.right() + m_scrollBar->sizeHint().width(),
                 int(30 * m_cellH) + m_padding.top() + m_padding.bottom());
}

void TerminalWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    updateGrid();
}

void TerminalWidget::scrollToBottom()
{
    if (m_scrollOffset != 0) {
        m_scrollOffset = 0;
        updateScrollBar();
        update();
    }
}

void TerminalWidget::scrollBy(int lines)
{
    const int sb = m_emu->altScreen() ? 0 : m_emu->scrollbackSize();
    const int off = std::clamp(m_scrollOffset + lines, 0, sb);
    if (off != m_scrollOffset) {
        m_scrollOffset = off;
        updateScrollBar();
        update();
    }
}

void TerminalWidget::zoom(int steps)
{
    QFont f = m_font;
    f.setPointSize(std::clamp(f.pointSize() + steps, 6, 40));
    setTerminalFont(f);
}

// ---------------------------------------------------------------------------
// painting

QColor TerminalWidget::resolveColor(const vt::Color& c, bool foreground, uint16_t attrs) const
{
    if (c.kind == vt::Color::Rgb)
        return QColor(c.r, c.g, c.b);
    if (c.kind == vt::Color::Indexed) {
        int i = c.idx;
        if (foreground && (attrs & vt::Bold) && i < 8)
            i += 8; // bold = bright, like most terminals
        return palette256(i);
    }
    return foreground ? kDefaultFg : kDefaultBg;
}

void TerminalWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kDefaultBg);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont fonts[4];
    for (int i = 0; i < 4; ++i) {
        fonts[i] = m_font;
        fonts[i].setBold(i & 1);
        fonts[i].setItalic(i & 2);
    }

    const qreal ox = m_padding.left();
    const qreal oy = m_padding.top();
    const int offset = viewOffset();
    const int sb = m_emu->altScreen() ? 0 : m_emu->scrollbackSize();
    const int baseAbs = sb - offset; // absolute row shown at view row 0

    // selection bounds (absolute rows)
    QPoint selA = m_selAnchor, selB = m_selEnd;
    if (selA.y() > selB.y() || (selA.y() == selB.y() && selA.x() > selB.x()))
        std::swap(selA, selB);

    for (int r = 0; r < m_rows; ++r) {
        const vt::Line& line = m_emu->viewLine(r, offset);
        const int n = int(line.cells.size());
        const qreal y = oy + r * m_cellH;

        // pass 1: cell backgrounds
        for (int c = 0; c < m_cols && c < n; ++c) {
            const vt::Cell& cell = line.cells[size_t(c)];
            const uint16_t a = cell.pen.attrs;
            const bool inverse = a & vt::Inverse;
            if (!inverse && cell.pen.bg.kind == vt::Color::Default)
                continue;
            const QColor bg = inverse ? resolveColor(cell.pen.fg, true, a) : resolveColor(cell.pen.bg, false, a);
            p.fillRect(QRectF(ox + c * m_cellW, y, m_cellW, m_cellH), bg);
        }

        // pass 2: text, in runs of identical style
        int c = 0;
        while (c < m_cols && c < n) {
            const vt::Cell& cell = line.cells[size_t(c)];
            if (cell.text.isEmpty() || (cell.pen.attrs & (vt::WideTail | vt::Hidden))) {
                ++c;
                continue;
            }
            const vt::Pen pen = cell.pen;
            const int start = c;
            QString run;
            if ((pen.attrs & vt::Wide) || cell.text.size() > 1) {
                run = cell.text;
                c += (pen.attrs & vt::Wide) ? 2 : 1;
            } else {
                while (c < m_cols && c < n) {
                    const vt::Cell& cc = line.cells[size_t(c)];
                    if ((cc.pen.attrs & (vt::Wide | vt::WideTail)) || cc.text.size() > 1 || !samePen(cc.pen, pen))
                        break;
                    run.append(cc.text.isEmpty() ? QStringLiteral(" ") : cc.text);
                    ++c;
                }
            }
            const uint16_t a = pen.attrs;
            QColor fg = (a & vt::Inverse) ? resolveColor(pen.bg, false, a) : resolveColor(pen.fg, true, a);
            if (a & vt::Dim)
                fg = mix(fg, kDefaultBg, 0.45);
            p.setFont(fonts[((a & vt::Bold) ? 1 : 0) | ((a & vt::Italic) ? 2 : 0)]);
            p.setPen(fg);
            const qreal x = ox + start * m_cellW;
            p.drawText(QPointF(x, y + m_ascent), run);
            const qreal runW = (c - start) * m_cellW;
            if (a & vt::Underline)
                p.drawLine(QPointF(x, y + m_cellH - 1.5), QPointF(x + runW, y + m_cellH - 1.5));
            if (a & vt::Strike)
                p.drawLine(QPointF(x, y + m_cellH * 0.55), QPointF(x + runW, y + m_cellH * 0.55));
        }

        // selection overlay
        if (m_hasSelection) {
            const int abs = baseAbs + r;
            if (abs >= selA.y() && abs <= selB.y()) {
                const int c0 = (abs == selA.y()) ? selA.x() : 0;
                const int c1 = (abs == selB.y()) ? selB.x() : m_cols - 1;
                if (c1 >= c0)
                    p.fillRect(QRectF(ox + c0 * m_cellW, y, (c1 - c0 + 1) * m_cellW, m_cellH), kSelection);
            }
        }
    }

    // cursor
    if (offset == 0 && m_emu->cursorVisible()) {
        const int cx = m_emu->cursorX();
        const int cy = m_emu->cursorY();
        if (cx >= 0 && cx < m_cols && cy >= 0 && cy < m_rows) {
            const vt::Line& line = m_emu->viewLine(cy, 0);
            const vt::Cell* cell = cx < int(line.cells.size()) ? &line.cells[size_t(cx)] : nullptr;
            const bool wide = cell && (cell->pen.attrs & vt::Wide);
            const QRectF rc(ox + cx * m_cellW, oy + cy * m_cellH, m_cellW * (wide ? 2 : 1), m_cellH);
            const int style = m_emu->cursorStyle();
            const bool bar = (style == 5 || style == 6);
            const bool underline = (style == 3 || style == 4);
            if (hasFocus() && m_blinkOn) {
                if (bar) {
                    p.fillRect(QRectF(rc.left(), rc.top(), 2.0, rc.height()), kDefaultFg);
                } else if (underline) {
                    p.fillRect(QRectF(rc.left(), rc.bottom() - 2.0, rc.width(), 2.0), kDefaultFg);
                } else {
                    p.fillRect(rc, kDefaultFg);
                    if (cell && !cell->text.isEmpty()) {
                        p.setFont(fonts[0]);
                        p.setPen(kDefaultBg);
                        p.drawText(QPointF(rc.left(), rc.top() + m_ascent), cell->text);
                    }
                }
            } else if (!hasFocus()) {
                p.setPen(QPen(kDefaultFg, 1.0));
                p.setBrush(Qt::NoBrush);
                p.drawRect(rc.adjusted(0.5, 0.5, -0.5, -0.5));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// keyboard

bool TerminalWidget::event(QEvent* e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const int k = ke->key();
        const Qt::KeyboardModifiers m = ke->modifiers();
        const bool plainOrShift = !(m & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
        const bool ctrlOnly = (m & Qt::ControlModifier) && !(m & (Qt::ShiftModifier | Qt::AltModifier));
        const bool altOnly = (m & Qt::AltModifier) && !(m & Qt::ControlModifier);
        if ((k == Qt::Key_Tab || k == Qt::Key_Backtab || k == Qt::Key_Escape) && plainOrShift) {
            e->accept();
            return true;
        }
        if (ctrlOnly && ((k >= Qt::Key_A && k <= Qt::Key_Z) || k == Qt::Key_Space || k == Qt::Key_BracketLeft ||
                         k == Qt::Key_BracketRight || k == Qt::Key_Backslash)) {
            e->accept(); // Ctrl+letter belongs to the shell (Ctrl+C, Ctrl+R, Ctrl+B, ...)
            return true;
        }
        if (altOnly && k != Qt::Key_Alt && ((k >= Qt::Key_A && k <= Qt::Key_Z) || k == Qt::Key_Return ||
                                             k == Qt::Key_Enter || k == Qt::Key_Backspace)) {
            e->accept();
            return true;
        }
    } else if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const bool plainOrShift = !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) && plainOrShift) {
            keyPressEvent(ke); // focus navigation would eat Tab otherwise
            return true;
        }
    }
    return QWidget::event(e);
}

QByteArray TerminalWidget::encodeKey(const QKeyEvent* e) const
{
    const int key = e->key();
    const Qt::KeyboardModifiers mods = e->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool alt = mods & Qt::AltModifier;
    const bool meta = mods & Qt::MetaModifier;

    auto modParam = [&] {
        int m = 1;
        if (shift)
            m += 1;
        if (alt)
            m += 2;
        if (ctrl)
            m += 4;
        if (meta)
            m += 8;
        return m;
    };
    auto cursorKey = [&](char final) -> QByteArray {
        const int m = modParam();
        if (m == 1)
            return (m_emu->appCursorKeys() ? QByteArray("\x1bO") : QByteArray("\x1b[")) + final;
        return QByteArray("\x1b[1;") + QByteArray::number(m) + final;
    };
    auto tildeKey = [&](int num) -> QByteArray {
        const int m = modParam();
        QByteArray s = QByteArray("\x1b[") + QByteArray::number(num);
        if (m != 1)
            s += ";" + QByteArray::number(m);
        return s + "~";
    };
    auto ssKey = [&](char final) -> QByteArray { // F1-F4
        const int m = modParam();
        if (m == 1)
            return QByteArray("\x1bO") + final;
        return QByteArray("\x1b[1;") + QByteArray::number(m) + final;
    };

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (shift && !ctrl && !alt)
            return QByteArray("\\\r"); // Claude Code: "\ + Enter" inserts a newline (what /terminal-setup configures)
        if (alt && !ctrl)
            return QByteArray("\x1b\r"); // Meta+Return, as macOS terminals send it
        return QByteArray("\r");
    case Qt::Key_Backspace:
        if (ctrl)
            return QByteArray("\x17"); // ^W: delete word
        if (alt)
            return QByteArray("\x1b\x7f");
        return QByteArray("\x7f");
    case Qt::Key_Tab:
        return QByteArray("\t");
    case Qt::Key_Backtab:
        return QByteArray("\x1b[Z");
    case Qt::Key_Escape:
        return QByteArray("\x1b");
    case Qt::Key_Up:
        return cursorKey('A');
    case Qt::Key_Down:
        return cursorKey('B');
    case Qt::Key_Right:
        return cursorKey('C');
    case Qt::Key_Left:
        return cursorKey('D');
    case Qt::Key_Home:
        return cursorKey('H');
    case Qt::Key_End:
        return cursorKey('F');
    case Qt::Key_Insert:
        return tildeKey(2);
    case Qt::Key_Delete:
        return tildeKey(3);
    case Qt::Key_PageUp:
        return tildeKey(5);
    case Qt::Key_PageDown:
        return tildeKey(6);
    case Qt::Key_F1:
        return ssKey('P');
    case Qt::Key_F2:
        return ssKey('Q');
    case Qt::Key_F3:
        return ssKey('R');
    case Qt::Key_F4:
        return ssKey('S');
    case Qt::Key_F5:
        return tildeKey(15);
    case Qt::Key_F6:
        return tildeKey(17);
    case Qt::Key_F7:
        return tildeKey(18);
    case Qt::Key_F8:
        return tildeKey(19);
    case Qt::Key_F9:
        return tildeKey(20);
    case Qt::Key_F10:
        return tildeKey(21);
    case Qt::Key_F11:
        return tildeKey(23);
    case Qt::Key_F12:
        return tildeKey(24);
    default:
        break;
    }

    // Ctrl combinations → C0 control codes (Ctrl+Alt is AltGr on Italian keyboards: leave it to text())
    if (ctrl && !alt) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            return QByteArray(1, char(key - Qt::Key_A + 1));
        switch (key) {
        case Qt::Key_Space:
        case Qt::Key_At:
            return QByteArray(1, '\0');
        case Qt::Key_BracketLeft:
            return QByteArray("\x1b");
        case Qt::Key_Backslash:
            return QByteArray("\x1c");
        case Qt::Key_BracketRight:
            return QByteArray("\x1d");
        case Qt::Key_AsciiCircum:
            return QByteArray("\x1e");
        case Qt::Key_Underscore:
        case Qt::Key_Minus:
            return QByteArray("\x1f");
        case Qt::Key_Question:
            return QByteArray("\x7f");
        default:
            break;
        }
    }

    const QString text = e->text();
    if (text.isEmpty())
        return QByteArray();
    if (text.at(0).unicode() < 0x20)
        return QByteArray(); // control char we did not map: ignore
    QByteArray utf8 = text.toUtf8();
    if (alt && !ctrl)
        utf8.prepend('\x1b');
    return utf8;
}

void TerminalWidget::keyPressEvent(QKeyEvent* e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    if (shift && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && !m_emu->altScreen()) {
        if (e->key() == Qt::Key_PageUp) {
            scrollBy(m_rows);
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_PageDown) {
            scrollBy(-m_rows);
            e->accept();
            return;
        }
    }
    const bool ctrlOnly = (e->modifiers() & Qt::ControlModifier) && !(e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier));
    if (ctrlOnly && e->key() == Qt::Key_C && m_hasSelection) {
        copySelection(); // like Windows Terminal: Ctrl+C copies a selection, otherwise it is ^C
        clearSelection();
        e->accept();
        return;
    }
    if (ctrlOnly && e->key() == Qt::Key_V) {
        const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
        const bool imageOnly = mime && mime->hasImage() && !mime->hasText();
        if (!imageOnly) { // an image-only clipboard is left to Claude Code (^V = paste image)
            paste();
            e->accept();
            return;
        }
    }
    const QByteArray bytes = encodeKey(e);
    if (bytes.isEmpty()) {
        e->ignore();
        return;
    }
    scrollToBottom();
    clearSelection();
    emit inputReady(bytes);
    emit userTyped();
    e->accept();
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent* e)
{
    const QString commit = e->commitString();
    if (!commit.isEmpty()) {
        scrollToBottom();
        emit inputReady(commit.toUtf8());
        emit userTyped();
    }
    e->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle) {
        return QRectF(m_padding.left() + m_emu->cursorX() * m_cellW, m_padding.top() + m_emu->cursorY() * m_cellH,
                      m_cellW, m_cellH);
    }
    if (query == Qt::ImEnabled)
        return true;
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::focusInEvent(QFocusEvent* e)
{
    QWidget::focusInEvent(e);
    m_blinkOn = true;
    m_blinkTimer.start();
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent* e)
{
    QWidget::focusOutEvent(e);
    m_blinkTimer.stop();
    m_blinkOn = true;
    update();
}

// ---------------------------------------------------------------------------
// text in / out

void TerminalWidget::sendText(const QString& text)
{
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    QByteArray bytes = t.toUtf8();
    if (m_emu->bracketedPaste())
        bytes = "\x1b[200~" + bytes + "\x1b[201~";
    scrollToBottom();
    emit inputReady(bytes);
}

void TerminalWidget::paste()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    clearSelection();
    sendText(text);
    emit userTyped();
}

void TerminalWidget::copySelection()
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void TerminalWidget::clearSelection()
{
    if (m_hasSelection || m_selecting) {
        m_hasSelection = false;
        m_selecting = false;
        update();
    }
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection)
        return QString();
    QPoint a = m_selAnchor, b = m_selEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    QString out;
    for (int abs = a.y(); abs <= b.y(); ++abs) {
        if (abs < 0 || abs >= m_emu->totalLines())
            continue;
        const vt::Line& line = m_emu->absoluteLine(abs);
        const int c0 = (abs == a.y()) ? a.x() : 0;
        const int c1 = (abs == b.y()) ? b.x() : m_cols - 1;
        QString row;
        for (int c = c0; c <= c1 && c < int(line.cells.size()); ++c) {
            const vt::Cell& cell = line.cells[size_t(c)];
            if (cell.pen.attrs & vt::WideTail)
                continue;
            row.append(cell.text.isEmpty() ? QStringLiteral(" ") : cell.text);
        }
        if (abs != b.y()) {
            int n = row.size();
            while (n > 0 && row.at(n - 1) == QLatin1Char(' '))
                --n;
            row.truncate(n);
            if (!line.wrapped)
                row.append(QLatin1Char('\n'));
        }
        out.append(row);
    }
    return out;
}

// ---------------------------------------------------------------------------
// mouse

QPoint TerminalWidget::cellAt(const QPointF& pos) const
{
    const int col = std::clamp(int((pos.x() - m_padding.left()) / m_cellW), 0, m_cols - 1);
    const int row = std::clamp(int((pos.y() - m_padding.top()) / m_cellH), 0, m_rows - 1);
    const int sb = m_emu->altScreen() ? 0 : m_emu->scrollbackSize();
    return QPoint(col, sb - viewOffset() + row);
}

void TerminalWidget::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    if (e->button() == Qt::LeftButton) {
        m_selAnchor = m_selEnd = cellAt(e->position());
        m_selecting = true;
        m_hasSelection = false;
        update();
    } else if (e->button() == Qt::RightButton) {
        if (m_hasSelection) {
            copySelection();
            clearSelection();
        } else {
            paste();
        }
    }
    e->accept();
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (m_selecting) {
        const QPoint c = cellAt(e->position());
        if (c != m_selEnd) {
            m_selEnd = c;
            m_hasSelection = (m_selEnd != m_selAnchor);
            update();
        }
    }
    e->accept();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        m_selecting = false;
    e->accept();
}

void TerminalWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const QPoint c = cellAt(e->position());
    if (c.y() < 0 || c.y() >= m_emu->totalLines())
        return;
    const vt::Line& line = m_emu->absoluteLine(c.y());
    auto isWordCell = [&](int x) {
        if (x < 0 || x >= int(line.cells.size()))
            return false;
        const QString& t = line.cells[size_t(x)].text;
        return !t.isEmpty() && !t.at(0).isSpace();
    };
    if (!isWordCell(c.x()))
        return;
    int x0 = c.x(), x1 = c.x();
    while (isWordCell(x0 - 1))
        --x0;
    while (isWordCell(x1 + 1))
        ++x1;
    m_selAnchor = QPoint(x0, c.y());
    m_selEnd = QPoint(x1, c.y());
    m_hasSelection = true;
    m_selecting = false;
    update();
    e->accept();
}

void TerminalWidget::wheelEvent(QWheelEvent* e)
{
    const int notches = e->angleDelta().y() / 120;
    if (notches == 0) {
        e->ignore();
        return;
    }
    if (e->modifiers() & Qt::ControlModifier) {
        zoom(notches > 0 ? 1 : -1);
    } else if (m_emu->altScreen()) {
        // full-screen apps get arrow keys instead of scrollback
        const QByteArray key = notches > 0 ? QByteArray("\x1b[A") : QByteArray("\x1b[B");
        for (int i = 0; i < std::abs(notches) * 3; ++i)
            emit inputReady(key);
    } else {
        scrollBy(notches * 3);
    }
    e->accept();
}
