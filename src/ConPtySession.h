#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <atomic>
#include <thread>

// One child process attached to a pseudo console.
//
// Windows: ConPTY (CreatePseudoConsole, Windows 10 1809+), loaded dynamically
// from kernel32 so the build does not depend on SDK version and the app can
// report a clear error on older systems. The child is put in a Job Object
// with KILL_ON_JOB_CLOSE so the whole process tree dies with the tab.
//
// Other platforms: forkpty() — used for development/testing of the shared code.
class ConPtySession : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString program;                 // executable name (searched in PATH) or full path
        QStringList arguments;
        QString workingDirectory;
        QProcessEnvironment environment; // empty → inherit the current environment
        int cols = 120;
        int rows = 30;
    };

    explicit ConPtySession(QObject* parent = nullptr);
    ~ConPtySession() override;

    bool start(const Options& options, QString* errorMessage = nullptr);
    void write(const QByteArray& data);
    void resize(int cols, int rows);
    void terminate(); // kill the process tree
    bool isRunning() const { return m_running.load(); }
    int exitCode() const { return m_exitCode; }

signals:
    void dataReceived(const QByteArray& data); // raw VT output (emitted from the reader thread; auto-queued)
    void finished(int exitCode);

private:
    void readerLoop();
    void onProcessExited(int code);
    void shutdownPty();
    void closeHandles();

    std::thread m_reader;
    std::thread m_waiter;
    std::atomic<bool> m_running{false};
    bool m_started = false;
    int m_exitCode = -1;

#ifdef _WIN32
    void* m_pty = nullptr;         // HPCON
    void* m_inputWrite = nullptr;  // our end of the pty input pipe
    void* m_outputRead = nullptr;  // our end of the pty output pipe
    void* m_process = nullptr;     // HANDLE
    void* m_job = nullptr;         // HANDLE
#else
    int m_fd = -1;
    int m_pid = -1;
#endif
};
