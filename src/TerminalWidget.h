#pragma once

#include "VtEmulator.h"

#include <QFont>
#include <QMargins>
#include <QPoint>
#include <QTimer>
#include <QWidget>

class QScrollBar;

// Draws a vt::Emulator with QPainter and turns keyboard/mouse events into
// the byte sequences a VT terminal would send. Pure QWidget, no QML.
class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget* parent = nullptr);

    vt::Emulator* emulator() const { return m_emu; }

    void setTerminalFont(const QFont& font);
    QFont terminalFont() const { return m_font; }
    int columns() const { return m_cols; }
    int rows() const { return m_rows; }

    bool hasSelection() const { return m_hasSelection; }
    QString selectedText() const;
    void clearSelection();
    void copySelection();
    void paste();
    void sendText(const QString& text); // "types" text; honours bracketed-paste mode
    void sendRaw(const QByteArray& bytes) { emit inputReady(bytes); } // raw key sequences
    void scrollToBottom();
    void scrollBy(int lines);
    void zoom(int steps);

    QSize sizeHint() const override;

signals:
    void inputReady(const QByteArray& bytes); // bytes to write to the pty
    void gridResized(int cols, int rows);
    void userTyped();

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void inputMethodEvent(QInputMethodEvent* e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
    void recomputeMetrics();
    void updateGrid();
    void updateScrollBar();
    void scheduleRepaint();
    QByteArray encodeKey(const QKeyEvent* e) const;
    QPoint cellAt(const QPointF& pos) const; // x = column, y = absolute row
    int viewOffset() const;
    QColor resolveColor(const vt::Color& c, bool foreground, uint16_t attrs) const;

    vt::Emulator* m_emu;
    QScrollBar* m_scrollBar;
    QFont m_font;
    qreal m_cellW = 8;
    qreal m_cellH = 16;
    qreal m_ascent = 12;
    int m_cols = 120;
    int m_rows = 30;
    int m_scrollOffset = 0;
    QMargins m_padding{6, 4, 6, 4};

    QTimer m_repaintTimer;
    QTimer m_blinkTimer;
    bool m_blinkOn = true;

    bool m_selecting = false;
    bool m_hasSelection = false;
    QPoint m_selAnchor;
    QPoint m_selEnd;
};
