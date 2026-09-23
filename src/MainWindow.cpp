#include "MainWindow.h"

#include "NewSessionDialog.h"
#include "TerminalWidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kMaxRecent = 12;

QString glyphFor(SessionTab::Status s)
{
    switch (s) {
    case SessionTab::Status::Starting:
        return QStringLiteral("…");
    case SessionTab::Status::Working:
        return QStringLiteral("◐");
    case SessionTab::Status::Waiting:
        return QStringLiteral("○");
    case SessionTab::Status::Attention:
        return QStringLiteral("●");
    case SessionTab::Status::Exited:
        return QStringLiteral("✕");
    case SessionTab::Status::Failed:
        return QStringLiteral("!");
    }
    return QString();
}

QColor colorFor(SessionTab::Status s)
{
    switch (s) {
    case SessionTab::Status::Starting:
        return QColor(0x90, 0x90, 0x90);
    case SessionTab::Status::Working:
        return QColor(0x3B, 0x8F, 0xF0);
    case SessionTab::Status::Waiting:
        return QColor(0x2E, 0xA0, 0x4A);
    case SessionTab::Status::Attention:
        return QColor(0xF0, 0x8A, 0x1E);
    case SessionTab::Status::Exited:
        return QColor(0x80, 0x80, 0x80);
    case SessionTab::Status::Failed:
        return QColor(0xD6, 0x2C, 0x2C);
    }
    return QColor();
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ClaudeDeck"));
    resize(1100, 720);

    m_claudeCommand = m_settings.value(QStringLiteral("claude/command"), QStringLiteral("claude")).toString();
    m_font = QFont(m_settings.value(QStringLiteral("terminal/fontFamily"), QStringLiteral("Cascadia Mono")).toString());
    m_font.setPointSize(m_settings.value(QStringLiteral("terminal/fontSize"), 11).toInt());
    m_scrollback = m_settings.value(QStringLiteral("terminal/scrollback"), 5000).toInt();
    m_recentDirs = m_settings.value(QStringLiteral("session/recentDirs")).toStringList();
    restoreGeometry(m_settings.value(QStringLiteral("window/geometry")).toByteArray());

    auto* central = new QWidget(this);
    auto* vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_tabs = new QTabWidget(central);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    m_tabs->setElideMode(Qt::ElideRight);
    auto* newBtn = new QToolButton(m_tabs);
    newBtn->setText(QStringLiteral(" + "));
    newBtn->setToolTip(tr("Nuova sessione (Ctrl+Shift+T)"));
    newBtn->setAutoRaise(true);
    connect(newBtn, &QToolButton::clicked, this, &MainWindow::newSessionDialog);
    m_tabs->setCornerWidget(newBtn, Qt::TopLeftCorner);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentChanged);
    vbox->addWidget(m_tabs, 1);

    auto* bar = new QWidget(central);
    auto* hbox = new QHBoxLayout(bar);
    hbox->setContentsMargins(8, 4, 8, 4);
    auto* label = new QLabel(tr("Broadcast:"), bar);
    m_broadcastEdit = new QLineEdit(bar);
    m_broadcastEdit->setPlaceholderText(
        tr("Prompt per tutte le sessioni — Invio per inviare, Esc per tornare al terminale (Ctrl+Shift+B)"));
    m_onlyWaiting = new QCheckBox(tr("Solo alle sessioni in attesa"), bar);
    m_onlyWaiting->setChecked(m_settings.value(QStringLiteral("broadcast/onlyWaiting"), false).toBool());
    auto* sendBtn = new QPushButton(tr("Invia a tutte"), bar);
    hbox->addWidget(label);
    hbox->addWidget(m_broadcastEdit, 1);
    hbox->addWidget(m_onlyWaiting);
    hbox->addWidget(sendBtn);
    connect(m_broadcastEdit, &QLineEdit::returnPressed, this, &MainWindow::broadcast);
    connect(sendBtn, &QPushButton::clicked, this, &MainWindow::broadcast);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_broadcastEdit);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, &MainWindow::focusCurrentTerminal);
    vbox->addWidget(bar);

    setCentralWidget(central);

    m_statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusLabel);

    buildMenus();
    updateStatusBar();
    updateWindowTitle();

    QTimer::singleShot(0, this, &MainWindow::newSessionDialog);
}

void MainWindow::buildMenus()
{
    auto add = [this](QMenu* menu, const QString& text, const QList<QKeySequence>& keys, auto slot) {
        QAction* a = menu->addAction(text);
        if (!keys.isEmpty())
            a->setShortcuts(keys);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };

    QMenu* session = menuBar()->addMenu(tr("&Sessione"));
    add(session, tr("&Nuova sessione…"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)}, &MainWindow::newSessionDialog);
    add(session, tr("Nuova sessione nell'&ultima cartella"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
        &MainWindow::quickNewSession);
    add(session, tr("&Riavvia con --continue"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R)},
        &MainWindow::restartCurrent);
    add(session, tr("&Chiudi scheda"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)}, &MainWindow::closeCurrentTab);
    session->addSeparator();
    add(session, tr("&Impostazioni…"), {}, &MainWindow::showSettings);
    session->addSeparator();
    add(session, tr("&Esci"), {}, &MainWindow::close);

    QMenu* edit = menuBar()->addMenu(tr("&Modifica"));
    add(edit, tr("&Copia"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)}, [this] {
        if (SessionTab* t = currentTab())
            t->terminal()->copySelection();
    });
    add(edit, tr("&Incolla"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)}, [this] {
        if (SessionTab* t = currentTab())
            t->terminal()->paste();
    });
    edit->addSeparator();
    add(edit, tr("Vai al &broadcast"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)}, [this] {
        m_broadcastEdit->setFocus(Qt::ShortcutFocusReason);
        m_broadcastEdit->selectAll();
    });

    QMenu* view = menuBar()->addMenu(tr("&Visualizza"));
    add(view, tr("Zoom &avanti"), {QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal)},
        [this] {
            if (SessionTab* t = currentTab())
                t->terminal()->zoom(1);
        });
    add(view, tr("Zoom &indietro"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)}, [this] {
        if (SessionTab* t = currentTab())
            t->terminal()->zoom(-1);
    });
    add(view, tr("&Reimposta zoom"), {QKeySequence(Qt::CTRL | Qt::Key_0)}, [this] {
        if (SessionTab* t = currentTab())
            t->terminal()->setTerminalFont(m_font);
    });
    view->addSeparator();
    add(view, tr("Scheda &successiva"),
        {QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)}, [this] {
            const int n = m_tabs->count();
            if (n > 1)
                m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % n);
        });
    add(view, tr("Scheda &precedente"),
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab),
         QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
        [this] {
            const int n = m_tabs->count();
            if (n > 1)
                m_tabs->setCurrentIndex((m_tabs->currentIndex() + n - 1) % n);
        });
    for (int i = 1; i <= 9; ++i) {
        add(view, tr("Vai alla scheda %1").arg(i), {QKeySequence(int(Qt::CTRL) | (int(Qt::Key_0) + i))}, [this, i] {
            if (i <= m_tabs->count())
                m_tabs->setCurrentIndex(i - 1);
        });
    }
}

// ---------------------------------------------------------------------------
// sessions

SessionTab* MainWindow::tabAt(int index) const
{
    return qobject_cast<SessionTab*>(m_tabs->widget(index));
}

SessionTab* MainWindow::currentTab() const
{
    return qobject_cast<SessionTab*>(m_tabs->currentWidget());
}

int MainWindow::indexOf(SessionTab* tab) const
{
    return m_tabs->indexOf(tab);
}

int MainWindow::runningCount() const
{
    int n = 0;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (SessionTab* t = tabAt(i))
            if (t->isRunning())
                ++n;
    return n;
}

void MainWindow::newSessionDialog()
{
    NewSessionDialog dlg(m_recentDirs, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    SessionTab::Launch launch;
    launch.claudeCommand = m_claudeCommand;
    launch.workingDirectory = dlg.workingDirectory();
    launch.arguments = dlg.arguments();
    openSession(launch);
}

void MainWindow::quickNewSession()
{
    if (m_recentDirs.isEmpty()) {
        newSessionDialog();
        return;
    }
    SessionTab::Launch launch;
    launch.claudeCommand = m_claudeCommand;
    launch.workingDirectory = QDir::fromNativeSeparators(m_recentDirs.first());
    openSession(launch);
}

void MainWindow::openSession(const SessionTab::Launch& launch)
{
    auto* tab = new SessionTab(launch, m_font, m_scrollback, m_tabs);
    connect(tab, &SessionTab::statusChanged, this, &MainWindow::updateTabAppearance);
    connect(tab, &SessionTab::titleChanged, this, &MainWindow::updateTabAppearance);
    const int index = m_tabs->addTab(tab, tab->label());
    m_tabs->setCurrentIndex(index);
    updateTabAppearance(tab);
    addRecentDirectory(launch.workingDirectory);
    tab->terminal()->setFocus(Qt::OtherFocusReason);
    // start after the first layout pass so the pty gets the real grid size
    QTimer::singleShot(0, tab, [tab] { tab->start(); });
}

void MainWindow::closeTab(int index)
{
    SessionTab* tab = tabAt(index);
    if (!tab)
        return;
    if (tab->isRunning()) {
        const auto answer = QMessageBox::question(
            this, tr("Chiudere la sessione?"),
            tr("La sessione \"%1\" è ancora attiva. Chiuderla e terminare il processo?").arg(tab->label()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_tabs->removeTab(index);
    tab->terminateSession();
    delete tab;
    updateStatusBar();
    updateWindowTitle();
    focusCurrentTerminal();
}

void MainWindow::closeCurrentTab()
{
    if (m_tabs->count() > 0)
        closeTab(m_tabs->currentIndex());
}

void MainWindow::restartCurrent()
{
    if (SessionTab* t = currentTab())
        t->restart(true);
}

void MainWindow::broadcast()
{
    const QString text = m_broadcastEdit->text();
    if (text.trimmed().isEmpty())
        return;
    const bool onlyWaiting = m_onlyWaiting->isChecked();
    m_settings.setValue(QStringLiteral("broadcast/onlyWaiting"), onlyWaiting);
    int sent = 0;
    for (int i = 0; i < m_tabs->count(); ++i) {
        SessionTab* t = tabAt(i);
        if (!t || !t->isRunning())
            continue;
        if (onlyWaiting) {
            const SessionTab::Status s = t->status();
            if (s != SessionTab::Status::Waiting && s != SessionTab::Status::Attention)
                continue;
        }
        t->sendPrompt(text);
        ++sent;
    }
    statusBar()->showMessage(tr("Prompt inviato a %1 sessioni").arg(sent), 4000);
    m_broadcastEdit->clear();
}

void MainWindow::focusCurrentTerminal()
{
    if (SessionTab* t = currentTab())
        t->terminal()->setFocus(Qt::OtherFocusReason);
}

void MainWindow::onCurrentChanged(int index)
{
    if (SessionTab* t = tabAt(index)) {
        t->markSeen();
        t->terminal()->setFocus(Qt::OtherFocusReason);
    }
    updateWindowTitle();
}

// ---------------------------------------------------------------------------
// appearance

void MainWindow::updateTabAppearance(SessionTab* tab)
{
    const int i = indexOf(tab);
    if (i < 0)
        return;
    const SessionTab::Status s = tab->status();
    m_tabs->setTabText(i, glyphFor(s) + QLatin1Char(' ') + tab->label());
    m_tabs->tabBar()->setTabTextColor(i, colorFor(s));
    QString tip = SessionTab::statusText(s);
    if (!tab->sessionTitle().isEmpty())
        tip += QLatin1Char('\n') + tab->sessionTitle();
    tip += QLatin1Char('\n') + QDir::toNativeSeparators(tab->launch().workingDirectory);
    m_tabs->setTabToolTip(i, tip);
    if (tab == currentTab())
        updateWindowTitle();
    updateStatusBar();
}

void MainWindow::updateWindowTitle()
{
    QString title = QStringLiteral("ClaudeDeck");
    if (SessionTab* t = currentTab()) {
        title = t->label();
        if (!t->sessionTitle().isEmpty())
            title += QStringLiteral(" — ") + t->sessionTitle();
        title += QStringLiteral(" — ClaudeDeck");
    }
    setWindowTitle(title);
}

void MainWindow::updateStatusBar()
{
    int working = 0, waiting = 0, attention = 0, exited = 0;
    const int total = m_tabs->count();
    for (int i = 0; i < total; ++i) {
        SessionTab* t = tabAt(i);
        if (!t)
            continue;
        switch (t->status()) {
        case SessionTab::Status::Starting:
        case SessionTab::Status::Working:
            ++working;
            break;
        case SessionTab::Status::Waiting:
            ++waiting;
            break;
        case SessionTab::Status::Attention:
            ++attention;
            break;
        case SessionTab::Status::Exited:
        case SessionTab::Status::Failed:
            ++exited;
            break;
        }
    }
    if (total == 0) {
        m_statusLabel->setText(tr("Nessuna sessione — Ctrl+Shift+T per aprirne una"));
        return;
    }
    m_statusLabel->setText(tr("%1 sessioni  ·  ◐ %2 al lavoro  ·  ○ %3 in attesa  ·  ● %4 da vedere  ·  ✕ %5 terminate")
                               .arg(total)
                               .arg(working)
                               .arg(waiting)
                               .arg(attention)
                               .arg(exited));
}

void MainWindow::applyFontToAll()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (SessionTab* t = tabAt(i)) {
            t->terminal()->setTerminalFont(m_font);
            t->terminal()->emulator()->setMaxScrollback(m_scrollback);
        }
    }
}

void MainWindow::addRecentDirectory(const QString& dir)
{
    const QString native = QDir::toNativeSeparators(QDir(dir).absolutePath());
    for (int i = 0; i < m_recentDirs.size();) {
        if (m_recentDirs.at(i).compare(native, Qt::CaseInsensitive) == 0)
            m_recentDirs.removeAt(i);
        else
            ++i;
    }
    m_recentDirs.prepend(native);
    while (m_recentDirs.size() > kMaxRecent)
        m_recentDirs.removeLast();
    m_settings.setValue(QStringLiteral("session/recentDirs"), m_recentDirs);
}

// ---------------------------------------------------------------------------
// settings

void MainWindow::showSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Impostazioni"));
    dlg.setMinimumWidth(480);
    auto* form = new QFormLayout(&dlg);

    auto* command = new QLineEdit(m_claudeCommand, &dlg);
    command->setPlaceholderText(QStringLiteral("claude"));
    command->setToolTip(tr("Nome del comando (cercato nel PATH, .exe o .cmd) oppure percorso completo"));
    auto* fontBox = new QFontComboBox(&dlg);
    fontBox->setFontFilters(QFontComboBox::MonospacedFonts);
    fontBox->setCurrentFont(m_font);
    auto* size = new QSpinBox(&dlg);
    size->setRange(6, 40);
    size->setValue(m_font.pointSize());
    auto* scrollback = new QSpinBox(&dlg);
    scrollback->setRange(200, 200000);
    scrollback->setSingleStep(500);
    scrollback->setValue(m_scrollback);

    form->addRow(tr("Comando Claude Code:"), command);
    form->addRow(tr("Font del terminale:"), fontBox);
    form->addRow(tr("Dimensione font:"), size);
    form->addRow(tr("Righe di scrollback:"), scrollback);

    auto* hint = new QLabel(
        tr("Suggerimento: lo stato \u201cda vedere\u201d (●) scatta quando Claude Code suona la campanella. "
           "Per attivarla:  claude config set -g preferredNotifChannel terminal_bell"),
        &dlg);
    hint->setWordWrap(true);
    form->addRow(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    m_claudeCommand = command->text().trimmed();
    if (m_claudeCommand.isEmpty())
        m_claudeCommand = QStringLiteral("claude");
    m_font = fontBox->currentFont();
    m_font.setPointSize(size->value());
    m_scrollback = scrollback->value();

    m_settings.setValue(QStringLiteral("claude/command"), m_claudeCommand);
    m_settings.setValue(QStringLiteral("terminal/fontFamily"), m_font.family());
    m_settings.setValue(QStringLiteral("terminal/fontSize"), m_font.pointSize());
    m_settings.setValue(QStringLiteral("terminal/scrollback"), m_scrollback);
    applyFontToAll();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    const int running = runningCount();
    if (running > 0) {
        const auto answer =
            QMessageBox::question(this, tr("Uscire da ClaudeDeck?"),
                                  tr("Ci sono %1 sessioni attive. Chiuderle tutte e uscire?").arg(running),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    m_settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    for (int i = 0; i < m_tabs->count(); ++i)
        if (SessionTab* t = tabAt(i))
            t->terminateSession();
    e->accept();
}
