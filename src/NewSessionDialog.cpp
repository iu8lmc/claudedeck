#include "NewSessionDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

NewSessionDialog::NewSessionDialog(const QStringList& recentDirectories, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Nuova sessione Claude"));
    setMinimumWidth(520);

    auto* form = new QFormLayout;

    m_dir = new QComboBox(this);
    m_dir->setEditable(true);
    m_dir->addItems(recentDirectories);
    if (recentDirectories.isEmpty())
        m_dir->setEditText(QDir::toNativeSeparators(QDir::homePath()));
    m_dir->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* browseBtn = new QPushButton(tr("Sfoglia…"), this);
    connect(browseBtn, &QPushButton::clicked, this, &NewSessionDialog::browse);
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browseBtn);
    form->addRow(tr("Cartella progetto:"), dirRow);

    auto* modeBox = new QGroupBox(tr("Avvio"), this);
    auto* modeLayout = new QVBoxLayout(modeBox);
    m_new = new QRadioButton(tr("Nuova conversazione"), modeBox);
    m_continue = new QRadioButton(tr("Continua l'ultima conversazione in questa cartella  (--continue)"), modeBox);
    m_resume = new QRadioButton(tr("Riprendi una conversazione  (--resume, mostra l'elenco se l'ID è vuoto)"), modeBox);
    m_new->setChecked(true);
    m_sessionId = new QLineEdit(modeBox);
    m_sessionId->setPlaceholderText(tr("ID sessione (facoltativo)"));
    m_sessionId->setEnabled(false);
    connect(m_resume, &QRadioButton::toggled, m_sessionId, &QLineEdit::setEnabled);
    modeLayout->addWidget(m_new);
    modeLayout->addWidget(m_continue);
    modeLayout->addWidget(m_resume);
    auto* idRow = new QHBoxLayout;
    idRow->addSpacing(24);
    idRow->addWidget(m_sessionId);
    modeLayout->addLayout(idRow);
    form->addRow(modeBox);

    m_extraArgs = new QLineEdit(this);
    m_extraArgs->setPlaceholderText(tr("es. --model opus --permission-mode plan"));
    form->addRow(tr("Argomenti extra:"), m_extraArgs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Avvia"));
    connect(buttons, &QDialogButtonBox::accepted, this, &NewSessionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &NewSessionDialog::reject);

    auto* main = new QVBoxLayout(this);
    main->addLayout(form);
    main->addWidget(buttons);

    m_dir->setFocus();
}

void NewSessionDialog::browse()
{
    const QString start = workingDirectory().isEmpty() ? QDir::homePath() : workingDirectory();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Scegli la cartella del progetto"), start);
    if (!dir.isEmpty())
        m_dir->setEditText(QDir::toNativeSeparators(dir));
}

QString NewSessionDialog::workingDirectory() const
{
    return QDir::fromNativeSeparators(m_dir->currentText().trimmed());
}

QStringList NewSessionDialog::arguments() const
{
    QStringList args;
    if (m_continue->isChecked()) {
        args << QStringLiteral("--continue");
    } else if (m_resume->isChecked()) {
        args << QStringLiteral("--resume");
        const QString id = m_sessionId->text().trimmed();
        if (!id.isEmpty())
            args << id;
    }
    args << QProcess::splitCommand(m_extraArgs->text());
    return args;
}

void NewSessionDialog::accept()
{
    const QString dir = workingDirectory();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        QMessageBox::warning(this, tr("Cartella non valida"), tr("La cartella \"%1\" non esiste.").arg(dir));
        return;
    }
    QDialog::accept();
}
