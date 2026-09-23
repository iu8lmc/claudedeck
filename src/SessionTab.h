#pragma once

#include <QElapsedTimer>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class ConPtySession;
class TerminalWidget;

// One tab = one Claude Code process in a ConPTY, its emulator and widget, plus
// the activity heuristics that drive the tab status.
class SessionTab : public QWidget {
    Q_OBJECT
public:
    enum class Status { Starting, Working, Waiting, Attention, Exited, Failed };

    struct Launch {
        QString claudeCommand;    // "claude" or a full path
        QString workingDirectory; // project folder
        QStringList arguments;    // e.g. {"--continue"}, {"--resume", "<id>"}, {"--model", "opus"}
        QString label;            // tab label; derived from the folder when empty
    };

    SessionTab(const Launch& launch, const QFont& font, int scrollbackLines, QWidget* parent = nullptr);
    ~SessionTab() override;

    void start();
    void restart(bool continueConversation);
    void terminateSession();
    bool isRunning() const;

    Status status() const;
    QString label() const { return m_launch.label; }
    QString sessionTitle() const { return m_oscTitle; } // set by Claude Code via OSC 0/2
    const Launch& launch() const { return m_launch; }
    TerminalWidget* terminal() const { return m_term; }

    void sendPrompt(const QString& text); // types the text, then Enter
    void markSeen();                      // clears the "attention" flag

    static QString statusText(Status s);

signals:
    void statusChanged(SessionTab* tab);
    void titleChanged(SessionTab* tab);

private:
    void launchProcess();
    void onData(const QByteArray& data);
    void onFinished(int code);
    void onTick();
    void setBaseStatus(Status s);
    void reportStatus();
    void note(const QString& text, int sgr = 90);

    Launch m_launch;
    TerminalWidget* m_term;
    ConPtySession* m_session = nullptr;
    Status m_base = Status::Starting;
    bool m_attention = false;
    Status m_lastReported = Status::Starting;
    bool m_pendingRestart = false;
    QElapsedTimer m_lastOutput;
    QTimer m_ticker;
    QString m_oscTitle;
};
