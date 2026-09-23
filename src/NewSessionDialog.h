#pragma once

#include <QDialog>
#include <QStringList>

class QComboBox;
class QLineEdit;
class QRadioButton;

// Asks for the project folder and how to start Claude Code (new / --continue / --resume).
class NewSessionDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewSessionDialog(const QStringList& recentDirectories, QWidget* parent = nullptr);

    QString workingDirectory() const;
    QStringList arguments() const;

private:
    void browse();
    void accept() override;

    QComboBox* m_dir;
    QRadioButton* m_new;
    QRadioButton* m_continue;
    QRadioButton* m_resume;
    QLineEdit* m_sessionId;
    QLineEdit* m_extraArgs;
};
