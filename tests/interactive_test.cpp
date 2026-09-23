#include "SessionTab.h"
#include "TerminalWidget.h"
#include <QApplication>
#include <QTimer>
#include <cstdio>
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    SessionTab::Launch l; l.claudeCommand = "bash"; l.workingDirectory = "/tmp"; l.arguments = {"--norc", "-i"};
    SessionTab tab(l, QFont("DejaVu Sans Mono", 11), 1000); tab.resize(640, 240); tab.show();
    auto* term = tab.terminal();
    QStringList log;
    QTimer::singleShot(0, [&]{ tab.start(); });
    QTimer::singleShot(600, [&]{ tab.sendPrompt("echo hi $((6*7))"); });
    QTimer::singleShot(1200, [&]{ log << "after echo:\n" + term->emulator()->screenText(); tab.sendPrompt("less /etc/hostname"); });
    QTimer::singleShot(1800, [&]{ log << QString("alt=%1\n").arg(term->emulator()->altScreen()) + term->emulator()->screenText(); term->sendRaw("q"); });
    QTimer::singleShot(2400, [&]{ log << QString("alt=%1 status=%2").arg(term->emulator()->altScreen()).arg(SessionTab::statusText(tab.status())); term->sendRaw("\x15"); tab.sendPrompt("exit"); });
    QTimer::singleShot(3200, &app, &QCoreApplication::quit);
    app.exec();
    for (auto& s : log) std::printf("---\n%s\n", qPrintable(s));
    std::printf("--- final status: %s running=%d\n", qPrintable(SessionTab::statusText(tab.status())), tab.isRunning());
    return 0;
}
