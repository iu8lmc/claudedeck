#pragma once

#include "SessionTab.h"

#include <QFont>
#include <QMainWindow>
#include <QSettings>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QTabWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildMenus();
    void newSessionDialog();
    void quickNewSession();
    void openSession(const SessionTab::Launch& launch);
    void closeTab(int index);
    void closeCurrentTab();
    void restartCurrent();
    void broadcast();
    void showSettings();
    void applyFontToAll();
    void focusCurrentTerminal();
    void onCurrentChanged(int index);
    void updateTabAppearance(SessionTab* tab);
    void updateWindowTitle();
    void updateStatusBar();
    void addRecentDirectory(const QString& dir);
    SessionTab* tabAt(int index) const;
    SessionTab* currentTab() const;
    int indexOf(SessionTab* tab) const;
    int runningCount() const;

    QTabWidget* m_tabs;
    QLineEdit* m_broadcastEdit;
    QCheckBox* m_onlyWaiting;
    QLabel* m_statusLabel;
    QSettings m_settings;

    QString m_claudeCommand;
    QFont m_font;
    int m_scrollback = 5000;
    QStringList m_recentDirs;
};
