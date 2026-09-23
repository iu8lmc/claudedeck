#include "ConPtySession.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

ConPtySession::ConPtySession(QObject* parent)
    : QObject(parent)
{
}

ConPtySession::~ConPtySession()
{
    terminate();
    shutdownPty();
}

void ConPtySession::onProcessExited(int code)
{
    m_exitCode = code;
    shutdownPty();
    emit finished(code);
}

#ifdef _WIN32
// ===========================================================================
// Windows: ConPTY
// ===========================================================================
#include <windows.h>

#include <string>

namespace {

using PtyHandle = void*;
typedef HRESULT(WINAPI* PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, PtyHandle*);
typedef HRESULT(WINAPI* PFN_ResizePseudoConsole)(PtyHandle, COORD);
typedef void(WINAPI* PFN_ClosePseudoConsole)(PtyHandle);

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

struct ConPtyApi {
    PFN_CreatePseudoConsole create = nullptr;
    PFN_ResizePseudoConsole resize = nullptr;
    PFN_ClosePseudoConsole close = nullptr;
    bool ok = false;
};

const ConPtyApi& conpty()
{
    static const ConPtyApi api = [] {
        ConPtyApi a;
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
            a.create = reinterpret_cast<PFN_CreatePseudoConsole>(GetProcAddress(k32, "CreatePseudoConsole"));
            a.resize = reinterpret_cast<PFN_ResizePseudoConsole>(GetProcAddress(k32, "ResizePseudoConsole"));
            a.close = reinterpret_cast<PFN_ClosePseudoConsole>(GetProcAddress(k32, "ClosePseudoConsole"));
        }
        a.ok = a.create && a.resize && a.close;
        return a;
    }();
    return api;
}

QString lastErrorString(DWORD err)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text = buf ? QString::fromWCharArray(buf).trimmed() : QString();
    if (buf)
        LocalFree(buf);
    return QStringLiteral("%1 (errore %2)").arg(text).arg(err);
}

// Quote one argument following the CommandLineToArgvW rules.
std::wstring quoteArg(const QString& arg)
{
    const std::wstring s = arg.toStdWString();
    if (!s.empty() && s.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        size_t backslashes = 0;
        while (i < s.size() && s[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == s.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (s[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(s[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring buildEnvBlock(const QProcessEnvironment& env)
{
    QStringList entries = env.toStringList();
    std::sort(entries.begin(), entries.end(),
              [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    std::wstring block;
    for (const QString& e : entries) {
        block += e.toStdWString();
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

QString searchPath(const QString& name, const wchar_t* extension)
{
    wchar_t buf[MAX_PATH * 2];
    wchar_t* filePart = nullptr;
    const std::wstring wname = name.toStdWString();
    const DWORD n = SearchPathW(nullptr, wname.c_str(), extension, DWORD(sizeof(buf) / sizeof(buf[0])), buf, &filePart);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return QString();
    return QString::fromWCharArray(buf, int(n));
}

// Resolve "claude" → C:\...\claude.exe, or C:\...\claude.cmd (npm shim, run via cmd.exe).
struct Resolved {
    QString exe;    // what CreateProcess will start
    QString script; // non-empty when exe is cmd.exe and this is the .cmd/.bat to run
};

bool resolveProgram(const QString& command, Resolved* out, QString* error)
{
    QString cmd = command.trimmed();
    if (cmd.isEmpty())
        cmd = QStringLiteral("claude");

    QString found;
    if (cmd.contains(QLatin1Char('/')) || cmd.contains(QLatin1Char('\\'))) {
        const QString native = QDir::toNativeSeparators(cmd);
        for (const QString& candidate : {native, native + QLatin1String(".exe"), native + QLatin1String(".cmd"),
                                         native + QLatin1String(".bat")}) {
            if (QFileInfo(candidate).isFile()) {
                found = candidate;
                break;
            }
        }
    } else {
        found = searchPath(cmd, nullptr);
        if (found.isEmpty())
            found = searchPath(cmd, L".exe");
        if (found.isEmpty())
            found = searchPath(cmd, L".cmd");
        if (found.isEmpty())
            found = searchPath(cmd, L".bat");
    }
    if (found.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("ConPtySession",
                                                 "Comando \"%1\" non trovato nel PATH. Controlla che Claude Code sia "
                                                 "installato oppure imposta il percorso completo nelle Impostazioni.")
                         .arg(cmd);
        return false;
    }
    const QString lower = found.toLower();
    if (lower.endsWith(QLatin1String(".cmd")) || lower.endsWith(QLatin1String(".bat"))) {
        QString comspec = qEnvironmentVariable("ComSpec");
        if (comspec.isEmpty())
            comspec = QStringLiteral("cmd.exe");
        out->exe = comspec;
        out->script = found;
    } else {
        out->exe = found;
    }
    return true;
}

std::wstring buildCommandLine(const Resolved& r, const QStringList& args)
{
    if (r.script.isEmpty()) {
        std::wstring cl = quoteArg(r.exe);
        for (const QString& a : args) {
            cl.push_back(L' ');
            cl += quoteArg(a);
        }
        return cl;
    }
    // cmd.exe /d /s /c "<script> <args>" — /s makes cmd strip exactly the outer quotes.
    std::wstring inner = quoteArg(r.script);
    for (const QString& a : args) {
        inner.push_back(L' ');
        inner += quoteArg(a);
    }
    return quoteArg(r.exe) + L" /d /s /c \"" + inner + L"\"";
}

} // namespace

bool ConPtySession::start(const Options& opt, QString* errorMessage)
{
    auto fail = [&](const QString& msg) {
        if (errorMessage)
            *errorMessage = msg;
        closeHandles(); // no reader thread yet: drop our pipe ends first, then the console
        if (m_pty) {
            conpty().close(m_pty);
            m_pty = nullptr;
        }
        return false;
    };
    if (m_started)
        return fail(tr("Sessione già avviata."));

    const ConPtyApi& api = conpty();
    if (!api.ok)
        return fail(tr("ConPTY non disponibile: serve Windows 10 versione 1809 o successiva."));

    Resolved resolved;
    QString err;
    if (!resolveProgram(opt.program, &resolved, &err))
        return fail(err);

    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0))
        return fail(tr("CreatePipe fallita: %1").arg(lastErrorString(GetLastError())));
    if (!CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        const DWORD e = GetLastError();
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return fail(tr("CreatePipe fallita: %1").arg(lastErrorString(e)));
    }

    COORD size;
    size.X = SHORT(std::clamp(opt.cols, 2, 32000));
    size.Y = SHORT(std::clamp(opt.rows, 1, 32000));
    PtyHandle pty = nullptr;
    const HRESULT hr = api.create(size, inRead, outWrite, 0, &pty);
    // conhost owns its own copies of these two ends now
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (FAILED(hr)) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return fail(tr("CreatePseudoConsole fallita (HRESULT 0x%1)").arg(ulong(hr), 8, 16, QLatin1Char('0')));
    }
    m_pty = pty;
    m_inputWrite = inWrite;
    m_outputRead = outRead;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize ? attrSize : 1);
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize))
        return fail(tr("InitializeProcThreadAttributeList fallita: %1").arg(lastErrorString(GetLastError())));
    if (!UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_pty, sizeof(m_pty), nullptr,
                                   nullptr)) {
        const DWORD e = GetLastError();
        DeleteProcThreadAttributeList(attrList);
        return fail(tr("UpdateProcThreadAttribute fallita: %1").arg(lastErrorString(e)));
    }

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrList;

    std::wstring cmdline = buildCommandLine(resolved, opt.arguments);
    const QProcessEnvironment env = opt.environment.isEmpty() ? QProcessEnvironment::systemEnvironment() : opt.environment;
    std::wstring envBlock = buildEnvBlock(env);
    const std::wstring wdir =
        opt.workingDirectory.isEmpty() ? std::wstring() : QDir::toNativeSeparators(opt.workingDirectory).toStdWString();

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                   envBlock.data(), wdir.empty() ? nullptr : wdir.c_str(), &si.StartupInfo, &pi);
    const DWORD createErr = GetLastError();
    DeleteProcThreadAttributeList(attrList);
    if (!ok)
        return fail(tr("Impossibile avviare \"%1\": %2")
                        .arg(QString::fromStdWString(cmdline), lastErrorString(createErr)));

    // Job object: the whole tree (claude → node → ripgrep …) dies with the session.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
        m_job = job;
    }
    m_process = pi.hProcess;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    m_started = true;
    m_running = true;
    m_reader = std::thread([this] { readerLoop(); });
    HANDLE hProcess = pi.hProcess;
    m_waiter = std::thread([this, hProcess] {
        WaitForSingleObject(hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(hProcess, &code);
        m_running = false;
        QMetaObject::invokeMethod(this, [this, code] { onProcessExited(int(code)); }, Qt::QueuedConnection);
    });
    return true;
}

void ConPtySession::readerLoop()
{
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(m_outputRead, buf.data(), DWORD(buf.size()), &n, nullptr) || n == 0)
            break;
        emit dataReceived(QByteArray(buf.data(), int(n)));
    }
}

void ConPtySession::write(const QByteArray& data)
{
    if (!m_running.load() || !m_inputWrite || data.isEmpty())
        return;
    const char* p = data.constData();
    DWORD left = DWORD(data.size());
    while (left > 0) {
        DWORD written = 0;
        if (!WriteFile(m_inputWrite, p, left, &written, nullptr) || written == 0)
            break;
        p += written;
        left -= written;
    }
}

void ConPtySession::resize(int cols, int rows)
{
    if (!m_pty)
        return;
    COORD size;
    size.X = SHORT(std::clamp(cols, 2, 32000));
    size.Y = SHORT(std::clamp(rows, 1, 32000));
    conpty().resize(m_pty, size);
}

void ConPtySession::terminate()
{
    if (!m_running.load())
        return;
    if (m_job)
        TerminateJobObject(m_job, 1);
    else if (m_process)
        TerminateProcess(m_process, 1);
}

void ConPtySession::shutdownPty()
{
    // Closing the pseudo console ends conhost, which closes the output pipe
    // and unblocks the reader thread (we keep draining meanwhile).
    if (m_pty) {
        conpty().close(m_pty);
        m_pty = nullptr;
    }
    if (m_reader.joinable())
        m_reader.join();
    if (m_waiter.joinable() && m_waiter.get_id() != std::this_thread::get_id())
        m_waiter.join();
    closeHandles();
}

void ConPtySession::closeHandles()
{
    if (m_inputWrite) {
        CloseHandle(m_inputWrite);
        m_inputWrite = nullptr;
    }
    if (m_outputRead) {
        CloseHandle(m_outputRead);
        m_outputRead = nullptr;
    }
    if (m_process) {
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_job) {
        CloseHandle(m_job); // KILL_ON_JOB_CLOSE: any leftover descendants die here
        m_job = nullptr;
    }
}

#else
// ===========================================================================
// POSIX fallback: forkpty()  (Linux / macOS — development & testing)
// ===========================================================================
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

bool ConPtySession::start(const Options& opt, QString* errorMessage)
{
    auto fail = [&](const QString& msg) {
        if (errorMessage)
            *errorMessage = msg;
        return false;
    };
    if (m_started)
        return fail(tr("Sessione già avviata."));

    QString program = opt.program.trimmed();
    if (program.isEmpty())
        program = QStringLiteral("claude");
    if (!program.contains(QLatin1Char('/'))) {
        const QString found = QStandardPaths::findExecutable(program);
        if (found.isEmpty())
            return fail(tr("Comando \"%1\" non trovato nel PATH.").arg(program));
        program = found;
    }

    std::vector<QByteArray> argvStore;
    argvStore.push_back(program.toLocal8Bit());
    for (const QString& a : opt.arguments)
        argvStore.push_back(a.toLocal8Bit());
    std::vector<char*> argv;
    for (QByteArray& a : argvStore)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    const QProcessEnvironment env = opt.environment.isEmpty() ? QProcessEnvironment::systemEnvironment() : opt.environment;
    std::vector<QByteArray> envStore;
    for (const QString& e : env.toStringList())
        envStore.push_back(e.toLocal8Bit());
    std::vector<char*> envp;
    for (QByteArray& e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    const QByteArray wdir = opt.workingDirectory.toLocal8Bit();

    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(std::clamp(opt.cols, 2, 32000));
    ws.ws_row = static_cast<unsigned short>(std::clamp(opt.rows, 1, 32000));

    int fd = -1;
    const pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0)
        return fail(tr("forkpty fallita: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    if (pid == 0) {
        if (!wdir.isEmpty() && ::chdir(wdir.constData()) != 0)
            _exit(126);
        ::execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    m_fd = fd;
    m_pid = pid;
    m_started = true;
    m_running = true;
    m_reader = std::thread([this] { readerLoop(); });
    m_waiter = std::thread([this, pid] {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        int code = -1;
        if (WIFEXITED(status))
            code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            code = 128 + WTERMSIG(status);
        m_running = false;
        QMetaObject::invokeMethod(this, [this, code] { onProcessExited(code); }, Qt::QueuedConnection);
    });
    return true;
}

void ConPtySession::readerLoop()
{
    std::vector<char> buf(64 * 1024);
    for (;;) {
        const ssize_t n = ::read(m_fd, buf.data(), buf.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        emit dataReceived(QByteArray(buf.data(), int(n)));
    }
}

void ConPtySession::write(const QByteArray& data)
{
    if (!m_running.load() || m_fd < 0 || data.isEmpty())
        return;
    const char* p = data.constData();
    size_t left = size_t(data.size());
    while (left > 0) {
        const ssize_t w = ::write(m_fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        p += w;
        left -= size_t(w);
    }
}

void ConPtySession::resize(int cols, int rows)
{
    if (m_fd < 0)
        return;
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(std::clamp(cols, 2, 32000));
    ws.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 32000));
    ::ioctl(m_fd, TIOCSWINSZ, &ws);
}

void ConPtySession::terminate()
{
    if (!m_running.load() || m_pid <= 0)
        return;
    ::kill(-m_pid, SIGHUP);
    ::kill(-m_pid, SIGKILL);
    ::kill(m_pid, SIGKILL);
}

void ConPtySession::shutdownPty()
{
    if (m_reader.joinable())
        m_reader.join();
    if (m_waiter.joinable() && m_waiter.get_id() != std::this_thread::get_id())
        m_waiter.join();
    closeHandles();
}

void ConPtySession::closeHandles()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

#endif
