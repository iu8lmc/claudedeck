#include "SessionTab.h"

#include "ConPtySession.h"
#include "TerminalWidget.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QVBoxLayout>

namespace {
constexpr int kQuietMs = 1500; // no output for this long → "waiting for input"
}

SessionTab::SessionTab(const Launch& launch, const QFont& font, int scrollbackLines, QWidget* parent)
    : QWidget(parent)
    , m_launch(launch)
    , m_term(new TerminalWidget(this))
{
    if (m_launch.label.isEmpty()) {
        const QString dir = QDir(m_launch.workingDirectory).dirName();
        m_launch.label = dir.isEmpty() ? QStringLiteral("claude") : dir;
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_term);
    setFocusProxy(m_term);

    m_term->setTerminalFont(font);
    m_term->emulator()->setMaxScrollback(scrollbackLines);

    connect(m_term->emulator(), &vt::Emulator::bell, this, [this] {
        if ((m_base == Status::Working || m_base == Status::Waiting || m_base == Status::Starting) && !m_attention) {
            m_attention = true;
            reportStatus();
        }
    });
    connect(m_term->emulator(), &vt::Emulator::titleChanged, this, [this](const QString& title) {
        m_oscTitle = title;
        emit titleChanged(this);
    });
    connect(m_term, &TerminalWidget::userTyped, this, &SessionTab::markSeen);
    connect(m_term, &TerminalWidget::gridResized, this, [this](int cols, int rows) {
        if (m_session)
            m_session->resize(cols, rows);
    });
    connect(m_term, &TerminalWidget::inputReady, this, [this](const QByteArray& bytes) {
        if (m_session)
            m_session->write(bytes);
    });

    m_ticker.setInterval(250);
    connect(&m_ticker, &QTimer::timeout, this, &SessionTab::onTick);
}

SessionTab::~SessionTab()
{
    m_ticker.stop();
    if (m_session) {
        m_session->disconnect(this);
        delete m_session; // kills the process tree and joins the I/O threads
        m_session = nullptr;
    }
}

void SessionTab::start()
{
    launchProcess();
}

void SessionTab::launchProcess()
{
    if (m_session) {
        m_session->disconnect(this);
        m_session->deleteLater();
        m_session = nullptr;
    }
    m_session = new ConPtySession(this);
    connect(m_session, &ConPtySession::dataReceived, this, &SessionTab::onData);
    connect(m_session, &ConPtySession::finished, this, &SessionTab::onFinished);

    ConPtySession::Options opt;
    opt.program = m_launch.claudeCommand.trimmed().isEmpty() ? QStringLiteral("claude") : m_launch.claudeCommand.trimmed();
    opt.arguments = m_launch.arguments;
    opt.workingDirectory = m_launch.workingDirectory;
    opt.cols = m_term->columns();
    opt.rows = m_term->rows();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    env.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("ClaudeDeck"));
    env.insert(QStringLiteral("TERM_PROGRAM_VERSION"), QStringLiteral("0.1.0"));
    env.insert(QStringLiteral("CLAUDEDECK"), QStringLiteral("1"));
    opt.environment = env;

    m_attention = false;
    m_oscTitle.clear();
    note(QStringLiteral("[ClaudeDeck] %1 %2   (%3)")
             .arg(opt.program, opt.arguments.join(QLatin1Char(' ')), QDir::toNativeSeparators(opt.workingDirectory)));

    QString error;
    if (!m_session->start(opt, &error)) {
        note(error, 31);
        m_ticker.stop();
        setBaseStatus(Status::Failed);
        return;
    }
    m_lastOutput.start();
    m_ticker.start();
    setBaseStatus(Status::Starting);
}

void SessionTab::restart(bool continueConversation)
{
    QStringList args = m_launch.arguments;
    for (int i = 0; i < args.size();) {
        const QString a = args.at(i);
        if (a == QLatin1String("--continue") || a == QLatin1String("-c")) {
            args.removeAt(i);
        } else if (a == QLatin1String("--resume") || a == QLatin1String("-r")) {
            args.removeAt(i);
            if (i < args.size() && !args.at(i).startsWith(QLatin1Char('-')))
                args.removeAt(i); // the session id
        } else {
            ++i;
        }
    }
    if (continueConversation)
        args.prepend(QStringLiteral("--continue"));
    m_launch.arguments = args;

    if (m_session && m_session->isRunning()) {
        m_pendingRestart = true;
        m_session->terminate();
    } else {
        launchProcess();
    }
}

void SessionTab::terminateSession()
{
    m_pendingRestart = false;
    if (m_session)
        m_session->terminate();
}

bool SessionTab::isRunning() const
{
    return m_session && m_session->isRunning();
}

SessionTab::Status SessionTab::status() const
{
    if (m_base == Status::Exited || m_base == Status::Failed)
        return m_base;
    if (m_attention)
        return Status::Attention;
    return m_base;
}

QString SessionTab::statusText(Status s)
{
    switch (s) {
    case Status::Starting:
        return tr("Avvio");
    case Status::Working:
        return tr("Lavora");
    case Status::Waiting:
        return tr("In attesa di input");
    case Status::Attention:
        return tr("Richiede attenzione");
    case Status::Exited:
        return tr("Terminata");
    case Status::Failed:
        return tr("Errore di avvio");
    }
    return QString();
}

void SessionTab::sendPrompt(const QString& text)
{
    if (!isRunning())
        return;
    m_term->sendText(text);
    QTimer::singleShot(80, this, [this] {
        if (m_session && m_session->isRunning())
            m_session->write(QByteArray("\r"));
    });
    markSeen();
}

void SessionTab::markSeen()
{
    if (m_attention) {
        m_attention = false;
        reportStatus();
    }
}

void SessionTab::onData(const QByteArray& data)
{
    m_term->emulator()->feed(data);
    m_lastOutput.restart();
    if (m_base == Status::Starting || m_base == Status::Waiting)
        setBaseStatus(Status::Working);
}

void SessionTab::onTick()
{
    if (m_base == Status::Working && m_lastOutput.isValid() && m_lastOutput.elapsed() > kQuietMs)
        setBaseStatus(Status::Waiting);
}

void SessionTab::onFinished(int code)
{
    m_ticker.stop();
    note(tr("[ClaudeDeck] processo terminato (codice %1)").arg(code));
    m_attention = false;
    setBaseStatus(Status::Exited);
    if (m_pendingRestart) {
        m_pendingRestart = false;
        launchProcess();
    }
}

void SessionTab::setBaseStatus(Status s)
{
    m_base = s;
    reportStatus();
}

void SessionTab::reportStatus()
{
    const Status now = status();
    if (now != m_lastReported) {
        m_lastReported = now;
        emit statusChanged(this);
    }
}

void SessionTab::note(const QString& text, int sgr)
{
    QByteArray bytes = "\r\n\x1b[" + QByteArray::number(sgr) + "m" + text.toUtf8() + "\x1b[0m\r\n";
    m_term->emulator()->feed(bytes);
}
