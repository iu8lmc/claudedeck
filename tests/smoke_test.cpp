// Headless end-to-end check (POSIX backend): runs a shell in the pty, feeds the
// emulator through SessionTab, watches the status transitions and renders the
// widget to a PNG. Usage: smoke_test [output.png]
#include "SessionTab.h"
#include "TerminalWidget.h"

#include <QApplication>
#include <QStringList>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    SessionTab::Launch launch;
    launch.claudeCommand = QStringLiteral("bash");
    launch.workingDirectory = QStringLiteral("/tmp");
    launch.arguments = {QStringLiteral("-c"),
                        QStringLiteral("printf 'ciao \\033[1;32mverde\\033[0m \\033[38;2;255;100;0mrgb\\033[0m\\n'; "
                                       "echo cwd=$PWD cols=$(tput cols) lines=$(tput lines); "
                                       "printf '\\033[?25l\\033[2A\\033[1G\\033[2K\\033[7m inverted line \\033[0m\\033[2B\\033[?25h'; "
                                       "printf '日本語 wide ✓ ⏺ ok\\n'; sleep 2; printf '\\a'; sleep 2; exit 3")};

    SessionTab tab(launch, QFont(QStringLiteral("DejaVu Sans Mono"), 11), 1000);
    tab.resize(720, 300);
    tab.show();

    QStringList statuses;
    QObject::connect(&tab, &SessionTab::statusChanged,
                     [&](SessionTab* t) { statuses << SessionTab::statusText(t->status()); });

    QTimer::singleShot(0, [&] { tab.start(); });
    QTimer::singleShot(6500, &app, &QCoreApplication::quit);
    app.exec();

    const QString screen = tab.terminal()->emulator()->screenText();
    std::printf("---- screen (%d x %d) ----\n%s\n---- statuses ----\n%s\n", tab.terminal()->columns(),
                tab.terminal()->rows(), qPrintable(screen), qPrintable(statuses.join(QStringLiteral(" -> "))));

    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/tmp/smoke.png");
    tab.terminal()->grab().save(out);
    std::printf("rendered %s\n", qPrintable(out));

    int fails = 0;
    auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++fails;
        }
    };
    expect(screen.contains(QStringLiteral("verde")), "coloured text present");
    expect(screen.contains(QStringLiteral("cwd=/tmp")), "working directory applied");
    expect(screen.contains(QStringLiteral("inverted line")), "cursor-up redraw");
    expect(screen.contains(QStringLiteral("日本語 wide")), "wide characters");
    expect(screen.contains(QStringLiteral("codice 3")), "exit code reported");
    expect(statuses.contains(QStringLiteral("Richiede attenzione")), "bell → attention");
    expect(statuses.last() == QStringLiteral("Terminata"), "final status Exited");
    expect(!tab.isRunning(), "process ended");
    std::printf(fails ? "smoke_test: %d failure(s)\n" : "smoke_test: all checks passed\n", fails);
    return fails ? 1 : 0;
}
