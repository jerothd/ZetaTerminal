#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QLineEdit>
#include <QWidget>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QLabel>
#include <QIcon>
#include <QStyleFactory>
#include <QColorDialog>
#include <QInputDialog>
#include <QPalette>
#include <QColor>
#include <QStringList>
#include <QByteArray>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSocketNotifier>
#include <cstdlib>

#ifndef USE_QTERMWIDGET
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

#ifdef USE_QTERMWIDGET
#include <qtermwidget.h>
#else
class ZetaPtyTerminal : public QPlainTextEdit {
    Q_OBJECT
public:
    ZetaPtyTerminal(QWidget *parent = nullptr) : QPlainTextEdit(parent) {
        m_masterFd = -1;
        m_notifier = nullptr;

        // Visual setup
        setReadOnly(false);
        setUndoRedoEnabled(false);
        setLineWrapMode(QPlainTextEdit::NoWrap);

        startShell();
    }

    ~ZetaPtyTerminal() {
        if (m_masterFd != -1) ::close(m_masterFd);
    }

    void startShell() {
        struct winsize ws;
        ws.ws_col = 80;
        ws.ws_row = 24;

        pid_t pid = forkpty(&m_masterFd, NULL, NULL, &ws);
        if (pid == -1) {
            appendPlainText("Error: Failed to fork PTY.");
            return;
        }

        if (pid == 0) {
            // Child process
            unsetenv("QT_QUICK_BACKEND");
            setenv("TERM", "xterm", 1);
            execl("/bin/bash", "bash", "--login", NULL);
            _exit(1);
        }

        // Parent process
        int flags = fcntl(m_masterFd, F_GETFL, 0);
        fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

        m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &ZetaPtyTerminal::onPtyData);
    }

    void sendText(const QString &text) {
        if (m_masterFd != -1) {
            QByteArray ba = text.toLocal8Bit();
            ::write(m_masterFd, ba.data(), ba.size());
        }
    }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (m_masterFd == -1) return;

        QByteArray data;
        switch (e->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:     data = "\r"; break;
            case Qt::Key_Backspace: data = "\x7f"; break;
            case Qt::Key_Tab:       data = "\t"; break;
            case Qt::Key_Escape:    data = "\x1b"; break;
            case Qt::Key_Up:        data = "\x1b[A"; break;
            case Qt::Key_Down:      data = "\x1b[B"; break;
            case Qt::Key_Right:     data = "\x1b[C"; break;
            case Qt::Key_Left:      data = "\x1b[D"; break;
            default:
                data = e->text().toLocal8Bit();
                break;
        }

        if (!data.isEmpty()) {
            ::write(m_masterFd, data.data(), data.size());
        }
    }

private slots:
    void onPtyData() {
        char buf[4096];
        ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
        if (n > 0) {
            // Very basic ANSI strip for a simple text-based terminal experience
            // In a real terminal, we'd have a full ANSI parser.
            QString text = QString::fromLocal8Bit(buf, n);
            
            // Handle simple backspaces if they come in the stream
            // (Most shells will handle this, but just in case)
            
            QTextCursor cursor = textCursor();
            cursor.movePosition(QTextCursor::End);
            setTextCursor(cursor);
            
            // Filter out some common ANSI codes that would mess up QPlainTextEdit
            // This is a crude placeholder for a real terminal emulator
            // Handles CSI (ESC [ ... char) and OSC (ESC ] ... BEL/ST)
            text.remove(QRegularExpression("\x1b\\[[0-9;?]*[a-zA-Z]"));
            text.remove(QRegularExpression("\x1b\\].*?(\x07|\x1b\\\\)"));
            
            insertPlainText(text);
            ensureCursorVisible();
        } else if (n == 0 || (n == -1 && errno != EAGAIN)) {
            m_notifier->setEnabled(false);
            appendPlainText("\n[Shell Process Terminated]");
        }
    }

private:
    int m_masterFd;
    QSocketNotifier *m_notifier;
};
#endif

class ZetaEditor : public QDialog {
    Q_OBJECT
public:
    ZetaEditor(const QString &filePath, QWidget *parent = nullptr) : QDialog(parent), m_filePath(filePath) {
        setWindowTitle(QString("Zeta Editor - %1").arg(QDir(m_filePath).dirName()));
        resize(800, 600);

        auto *layout = new QVBoxLayout(this);
        m_editor = new QPlainTextEdit(this);

        QFont mono("Hack", 10);
        mono.setStyleHint(QFont::Monospace);
        mono.setFixedPitch(true);
        m_editor->setFont(mono);

        layout->addWidget(m_editor);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &ZetaEditor::saveAndClose);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        loadFile();
    }

private:
    void loadFile() {
        QFile file(m_filePath);
        if (file.exists()) {
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                m_editor->setPlainText(in.readAll());
                file.close();
            } else {
                QMessageBox::warning(this, "Error", "Could not open file for reading.");
            }
        }
    }

    void saveAndClose() {
        QFile file(m_filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&file);
            out << m_editor->toPlainText();
            file.close();
            accept();
        } else {
            QMessageBox::warning(this, "Error", "Could not open file for writing.");
        }
    }

    QString m_filePath;
    QPlainTextEdit *m_editor;
};

class ZetaTerminal : public QMainWindow {
    Q_OBJECT
public:
    ZetaTerminal() {
        setWindowTitle("Zeta Terminal");
        resize(1100, 700);
        working_dir = QDir::currentPath();

        central = new QWidget(this);
        auto *mainLayout = new QHBoxLayout(central);
        mainLayout->setContentsMargins(4, 4, 4, 4);
        mainLayout->setSpacing(4);

        applyAdwaitaTheme();
        setupDefaultColors();

        QFont mono("Hack", 10);
        mono.setStyleHint(QFont::Monospace);
        mono.setFixedPitch(true);
        if (!QFontInfo(mono).fixedPitch()) {
            mono = QFont("Noto Sans Mono", 10);
            mono.setStyleHint(QFont::Monospace);
            mono.setFixedPitch(true);
        }

        // --- Left Panel: Terminal ---
        auto *termContainer = new QWidget(central);
        auto *termLayout = new QVBoxLayout(termContainer);
        termLayout->setContentsMargins(0, 0, 0, 0);

        setenv("PS1", "[\\u@\\h \\W]\\$ ", 1);
        unsetenv("PROMPT_COMMAND");

#ifdef USE_QTERMWIDGET
        termWidget = new QTermWidget(termContainer);
        termWidget->setTerminalFont(mono);
        termWidget->setScrollBarPosition(QTermWidget::ScrollBarRight);
        termWidget->startShellProgram();
        termLayout->addWidget(termWidget);
#else
        termWidget = new ZetaPtyTerminal(termContainer);
        termWidget->setFont(mono);
        termLayout->addWidget(termWidget);
#endif
        mainLayout->addWidget(termContainer, 3); // Terminal takes 3/4 space

        // --- Right Panel: AI Column ---
        auto *aiContainer = new QWidget(central);
        auto *aiLayout = new QVBoxLayout(aiContainer);
        aiLayout->setContentsMargins(0, 0, 0, 0);

        auto *aiHeader = new QLabel("<b>AI ASSISTANT</b>", aiContainer);
        aiHeader->setAlignment(Qt::AlignCenter);
        aiLayout->addWidget(aiHeader);

        aiOutput = new QPlainTextEdit(aiContainer);
        aiOutput->setReadOnly(true);
        aiOutput->setFont(mono);
        aiLayout->addWidget(aiOutput);

        entry = new QLineEdit(aiContainer);
        entry->setPlaceholderText("Ask AI something...");
        aiLayout->addWidget(entry);

        mainLayout->addWidget(aiContainer, 1); // AI takes 1/4 space

        setCentralWidget(central);
        connect(entry, &QLineEdit::returnPressed, this, &ZetaTerminal::on_submit);

        updateStyles();

        if (termWidget) termWidget->setFocus();

        netman = new QNetworkAccessManager(this);
        aiModel = "llama3.1:8b";
        aiProcess = nullptr;
        aiMode = false;
        themeMode = false;

        QJsonObject systemMsg;
        systemMsg["role"] = QString("system");
        systemMsg["content"] = QString("You are a helpful AI assistant. Maintain a continuous chat session.");
        aiHistory.append(systemMsg);

        const QString banner = R"BANNER(
 ✦ ─── ❖ ── ✦ ── 🌀  ✦ ── ZETA - TERMINAL ── ✦  🌀 ── ✦ ── ❖ ─── ✦
  ☼                                                             ☼
 ⛩  ░░▒▒▓▓██████████████████████████████████████████▓▓▒▒░░  ⛩
 ❖  ░▒▒▓▓██████████████████████████████████████████████▓▓▒▒░  ❖
 ✦  ▒▓▓█████████████████████████████████████████████████▓▓▒  ✦
 ☼  ▓▓████████████████████████████████████████████████████▓▓  ☼
 ☼  ▓▓██████████████████████████████████████████████████▓▓  ☼
 ✦  ▒▓▓█████████████████████████████████████████████████▓▓▒  ✦
 ❖  ░▒▒▓▓█████████████████████████████████████████████▓▓▒▒░  ❖
 ⛩  ░░▒▒▓▓██████████████████████████████████████████▓▓▒▒░░  ⛩
  ☼                                                             ☼
 ✦ ─── ❖ ── ✦ ── 🌀 ─── [ Z E T A D A T A ] ─── 🌀 ── ✦ ── ❖ ─── ✦
)BANNER";

        append_text(banner, "#a0c0ff");
    }

private slots:
    void applyAdwaitaTheme() {
        if (QStyleFactory::keys().contains("Fusion")) {
            QApplication::setStyle(QStyleFactory::create("Fusion"));
        }
    }

    void setupDefaultColors() {
        bgColor = "#232627";
        fgColor = "#FCFCFC";
        borderColor = "#2f343c";
        accentColor = "#7F8C8D";
        caretColor = "#a0c0ff";
    }

    void updateStyles() {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor(bgColor));
        palette.setColor(QPalette::WindowText, QColor(fgColor));
        palette.setColor(QPalette::Base, QColor(bgColor));
        palette.setColor(QPalette::Text, QColor(fgColor));
        palette.setColor(QPalette::Button, QColor(bgColor));
        palette.setColor(QPalette::ButtonText, QColor(fgColor));
        palette.setColor(QPalette::Highlight, QColor(accentColor));
        palette.setColor(QPalette::HighlightedText, QColor(bgColor));

        central->setPalette(palette);
        central->setAutoFillBackground(true);

        QString commonStyle = QString(
            "background-color: %1; color: %2; border: 1px solid %3; padding: 4px; "
            "font-family: 'Hack', 'Noto Sans Mono', monospace; font-size: 10pt; "
            "selection-background-color: %4; selection-color: %1; caret-color: %5;")
            .arg(bgColor, fgColor, borderColor, accentColor, caretColor);

        if (aiOutput) {
            aiOutput->setStyleSheet(commonStyle);
        }

        if (termWidget) {
            termWidget->setStyleSheet(commonStyle);
        }

        entry->setStyleSheet(commonStyle);
    }

    QString wrap_ai_text(const QString &text) {
        if (!text.startsWith("AI ← ") && !text.startsWith("AI → ")) {
            return text;
        }

        QString prefix = text.left(5);
        QString body = text.mid(5);

        // We can't easily wrap based on termWidget width here without knowing its columns,
        // so we'll use a sensible default or skip wrapping for now as it's a real terminal.
        return text;
    }

    void append_text(const QString &text, const QString & = "#f4eedc") {
        if (text.contains("ZETA - TERMINAL") || text.contains("[ Z E T A D A T A ]")) {
#ifndef USE_QTERMWIDGET
            if (termWidget) {
                termWidget->appendPlainText(text);
                termWidget->ensureCursorVisible();
                return;
            }
#endif
        }

        if (text.startsWith("AI ←") || text.startsWith("AI →") || text.startsWith("\nAI Mode") || 
            text.contains("Theme Customization Mode") || text.contains("Updated bg to")) {
            if (aiOutput) {
                aiOutput->appendPlainText(text);
                aiOutput->ensureCursorVisible();
            }
            return;
        }

        // Real terminal handles its own shell output and prompts.
        // We only log non-terminal messages to the AI output window.
        if (aiOutput && !text.contains("]$")) {
            aiOutput->appendPlainText(text);
            aiOutput->ensureCursorVisible();
        }
    }

    QString prompt_label() const {
        QString user = qgetenv("USER");
        if (user.isEmpty()) user = "user";
        QString host = QSysInfo::machineHostName();
        if (host.isEmpty()) host = "localhost";
        QString dir = QDir(working_dir).dirName();
        if (dir.isEmpty()) dir = "/";
        return QString("[%1@%2 %3]$").arg(user, host, dir);
    }

    void handle_theme_command(const QString &input) {
        QString cmd = input.trimmed().toLower();
        if (cmd == "exit" || cmd == "#~#") {
            themeMode = false;
            entry->setPlaceholderText("Type command and press Enter...");
            append_text("\nExited Theme Customization Mode.");
            append_text(prompt_label());
            return;
        }

        if (cmd == "bg" || cmd == "fg" || cmd == "border" || cmd == "accent" || cmd == "caret") {
            QColor color = QColorDialog::getColor(QColor(bgColor), this, "Select Color");
            if (color.isValid()) {
                QString hex = color.name();
                if (cmd == "bg") bgColor = hex;
                else if (cmd == "fg") fgColor = hex;
                else if (cmd == "border") borderColor = hex;
                else if (cmd == "accent") accentColor = hex;
                else if (cmd == "caret") caretColor = hex;

                updateStyles();
                append_text(QString("Updated %1 to %2").arg(cmd, hex));
            }
        } else {
            append_text("Unknown option. Options: bg, fg, border, accent, caret, exit");
        }
        append_text("Theme Config Mode -> ");
    }

    void on_submit() {
        QString input = entry->text();
        entry->clear();

        if (input.isEmpty()) return;

        bool isPassword = (entry->echoMode() == QLineEdit::Password);
        
        if (themeMode) {
            if (!isPassword) append_text(input);
            handle_theme_command(input);
            return;
        }

        if (input == "#~#") {
            themeMode = true;
            append_text("\n--- Theme Customization Mode ---");
            append_text("Commands: 'bg', 'fg', 'border', 'accent', 'caret' to open picker. 'exit' to quit.");
            append_text("Theme Config Mode -> ");
            entry->setPlaceholderText("Enter theme targeting flag...");
            return;
        }

        if (input == "*~*") {
            aiMode = !aiMode;
            if (aiMode) {
                append_text("\nAI Mode Enabled. Connecting to Ollama...");
                if (!aiProcess) {
                    aiProcess = new QProcess(this);
                }
                append_text("AI Mode is now ON.");
            } else {
                append_text("AI Mode Disabled.");
            }
            return;
        }

        // In this new design, 'entry' is dedicated to AI questions.
        sendAiMessage(input);
    }

    void sendAiMessage(const QString &message) {
        if (!netman) {
            append_text("AI network manager not initialized.", "#ff8a80");
            return;
        }
        append_text("AI ← " + message, "#a0c0ff");

        QJsonObject userMsg;
        userMsg["role"] = QString("user");
        userMsg["content"] = message;
        aiHistory.append(userMsg);

        QUrl url("http://127.0.0.1:11434/v1/chat/completions");
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QJsonObject body;
        body["model"] = aiModel;
        body["messages"] = aiHistory;

        QNetworkReply *reply = netman->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            QByteArray resp = reply->readAll();
            reply->deleteLater();
            if (resp.isEmpty()) {
                append_text("AI: empty response", "#ff8a80");
                return;
            }
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(resp, &err);
            if (err.error != QJsonParseError::NoError) {
                append_text(QString("AI: parse error: %1").arg(err.errorString()), "#ff8a80");
                append_text(QString::fromLocal8Bit(resp), "#f8f4de");
                return;
            }
            QJsonObject obj = doc.object();
            QString out;
            if (obj.contains("choices") && obj["choices"].isArray()) {
                QJsonArray choices = obj["choices"].toArray();
                if (!choices.isEmpty() && choices[0].isObject()) {
                    QJsonObject first = choices[0].toObject();
                    if (first.contains("message") && first["message"].isObject()) {
                        out = first["message"].toObject()["content"].toString();
                    } else if (first.contains("text")) {
                        out = first["text"].toString();
                    }
                }
            } else if (obj.contains("text")) {
                out = obj["text"].toString();
            }
            if (out.isEmpty()) out = QString::fromLocal8Bit(resp);
            append_text("AI → " + out, "#f8f4de");

            QJsonObject assistantMsg;
            assistantMsg["role"] = QString("assistant");
            assistantMsg["content"] = out;
            aiHistory.append(assistantMsg);
        });
    }

private:
#ifdef USE_QTERMWIDGET
    QTermWidget *termWidget = nullptr;
#else
    ZetaPtyTerminal *termWidget = nullptr;
#endif
    QPlainTextEdit *aiOutput = nullptr;
    QWidget *central = nullptr;
    QLineEdit *entry = nullptr;
    QString working_dir;
    bool themeMode = false;
    bool aiMode = false;
    QNetworkAccessManager *netman = nullptr;
    QString aiModel;
    QJsonArray aiHistory;
    QProcess *aiProcess = nullptr;
    QString bgColor;
    QString fgColor;
    QString borderColor;
    QString accentColor;
    QString caretColor;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    ZetaTerminal terminal;
    terminal.show();
    return app.exec();
}

#include "main.moc"
