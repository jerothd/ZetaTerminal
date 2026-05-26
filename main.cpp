#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QWidget>
#include <QDir>
#include <QProcess>
#include <QSysInfo>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QLabel>
#include <QStyleFactory>
#include <QColorDialog>
#include <QInputDialog>
#include <QPalette>
#include <QColor>
#include <QByteArray>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QTextBlock>
#include <QTimer>
#include <QSocketNotifier>
#include <QScrollBar>
#include <QPainter>
#include <climits>
#ifndef USE_QTERMWIDGET
#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#endif

#ifdef USE_QTERMWIDGET
#include <qtermwidget.h>
#else

struct TermCell {
    uint32_t c = ' ';
    QColor fg = Qt::white;
    QColor bg = Qt::transparent;
    bool bold = false;
    bool inverse = false;
};

class ZetaPtyTerminal : public QWidget {
    Q_OBJECT
public:
    ZetaPtyTerminal(QWidget *parent = nullptr) : QWidget(parent) {
        m_masterFd = -1;
        m_shellPid = -1;
        m_notifier = nullptr;
        m_cursorX = 0;
        m_cursorY = 0;
        m_cols = 80;
        m_rows = 24;
        m_topMargin = 0;
        m_bottomMargin = m_rows - 1;

        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        
        // Visual setup
        m_font = QFont("Hack", 10);
        m_font.setStyleHint(QFont::Monospace);
        m_font.setFixedPitch(true);
        updateFontMetrics();

        // Set a reasonable minimum size for a 80x24 terminal
        setMinimumSize(m_charWidth * 80 + 4, m_charHeight * 24 + 4);

        initGrid();
        startShell();
    }

    ~ZetaPtyTerminal() {
        if (m_masterFd != -1) ::close(m_masterFd);
    }

    void startShell() {
        struct winsize ws;
        ws.ws_col = m_cols;
        ws.ws_row = m_rows;

        pid_t pid = forkpty(&m_masterFd, NULL, NULL, &ws);
        if (pid == -1) {
            return;
        }
        m_shellPid = pid;

        if (pid == 0) {
            // Child process
            setenv("TERM", "xterm-256color", 1);
            setenv("PS1", "[\\u@\\h \\W]\\$ ", 1);
            execl("/bin/bash", "bash", "-i", NULL);
            _exit(1);
        }

        // Parent process
        int flags = fcntl(m_masterFd, F_GETFL, 0);
        fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

        m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &ZetaPtyTerminal::onPtyData);
        
        QTimer::singleShot(100, this, &ZetaPtyTerminal::updatePtySize);
    }

    void writeLocal(const QString &text) {
        processPtyText(text.toUtf8());
    }

    void sendText(const QString &text) {
        if (m_masterFd != -1) {
            QByteArray ba = text.toUtf8();
            ::write(m_masterFd, ba.data(), ba.size());
        }
    }

protected:
    void updateFontMetrics() {
        QFontMetrics fm(m_font);
        m_charWidth = fm.horizontalAdvance('W');
        m_charHeight = fm.height();
    }

    void initGrid() {
        m_grid.clear();
        m_grid.resize(m_rows, QVector<TermCell>(m_cols));
    }

    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        updatePtySize();
    }

    void updatePtySize() {
        if (m_masterFd == -1) return;
        
        int newCols = qMax(10, width() / m_charWidth);
        int newRows = qMax(5, height() / m_charHeight);
        
        if (newCols != m_cols || newRows != m_rows) {
            m_cols = newCols;
            m_rows = newRows;
            
            // Resize grids while preserving content where possible
            auto resizeGrid = [&](QVector<QVector<TermCell>> &grid) {
                QVector<QVector<TermCell>> nextGrid(m_rows, QVector<TermCell>(m_cols));
                for (int r = 0; r < qMin((int)grid.size(), m_rows); ++r) {
                    for (int c = 0; c < qMin((int)grid[r].size(), m_cols); ++c) {
                        nextGrid[r][c] = grid[r][c];
                    }
                }
                grid = nextGrid;
            };

            resizeGrid(m_grid);
            if (!m_savedGrid.isEmpty()) resizeGrid(m_savedGrid);

            m_topMargin = 0;
            m_bottomMargin = m_rows - 1;
            
            // Constrain cursor
            m_cursorX = qMin(m_cursorX, m_cols - 1);
            m_cursorY = qMin(m_cursorY, m_rows - 1);

            struct winsize ws;
            ws.ws_col = m_cols;
            ws.ws_row = m_rows;
            ws.ws_xpixel = 0;
            ws.ws_ypixel = 0;
            ioctl(m_masterFd, TIOCSWINSZ, &ws);
            update();
        }
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setFont(m_font);
        painter.fillRect(rect(), palette().color(QPalette::Base));

        QFont boldFont = m_font;
        boldFont.setBold(true);

        for (int r = 0; r < m_rows; ++r) {
            for (int c = 0; c < m_cols; ++c) {
                const TermCell &cell = m_grid[r][c];
                QRect cellRect(c * m_charWidth, r * m_charHeight, m_charWidth, m_charHeight);
                
                QColor fg = cell.fg;
                QColor bg = cell.bg;
                if (cell.inverse) std::swap(fg, bg);
                
                if (bg != Qt::transparent) {
                    painter.fillRect(cellRect, bg);
                }
                
                if (cell.c > 32 && cell.c != 0x7f) {
                    painter.setPen(fg);
                    painter.setFont(cell.bold ? boldFont : m_font);
                    
                    // Use UCS-4 to support characters outside BMP (like 🌀)
                    char32_t ucs4 = (char32_t)cell.c;
                    QString s = QString::fromUcs4(&ucs4, 1);
                    painter.drawText(cellRect, Qt::AlignLeft | Qt::AlignTop, s);
                }
            }
        }

        // Draw cursor
        if (hasFocus() && m_cursorVisible) {
            painter.setCompositionMode(QPainter::CompositionMode_Difference);
            painter.fillRect(m_cursorX * m_charWidth, m_cursorY * m_charHeight, m_charWidth, m_charHeight, Qt::white);
        }
    }

    void keyPressEvent(QKeyEvent *e) override {
        if (m_masterFd == -1) return;

        QByteArray data;
        
        // Handle Meta (Alt) keys by prepending Esc
        if (e->modifiers() & Qt::AltModifier) {
            data.append('\x1b');
        }

        if (e->modifiers() & Qt::ControlModifier) {
            if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) {
                data.append((char)(e->key() - Qt::Key_A + 1));
            } else if (e->key() == Qt::Key_BracketLeft) {
                data.append("\x1b");
            } else if (e->key() == Qt::Key_Backslash) {
                data.append("\x1c");
            } else if (e->key() == Qt::Key_BracketRight) {
                data.append("\x1d");
            } else if (e->key() == Qt::Key_6) { // Common mapping for Ctrl+^
                data.append("\x1e");
            } else if (e->key() == Qt::Key_Minus) { // Common mapping for Ctrl+_
                data.append("\x1f");
            }
        }

        if (data.isEmpty() || (data.size() == 1 && data[0] == '\x1b' && e->modifiers() & Qt::AltModifier)) {
            switch (e->key()) {
                case Qt::Key_Return:
                case Qt::Key_Enter:     data.append("\r"); break;
                case Qt::Key_Backspace: data.append("\x7f"); break;
                case Qt::Key_Tab:       data.append("\t"); break;
                case Qt::Key_Escape:    data.append("\x1b"); break;
                case Qt::Key_Up:        data.append("\x1b[A"); break;
                case Qt::Key_Down:      data.append("\x1b[B"); break;
                case Qt::Key_Right:     data.append("\x1b[C"); break;
                case Qt::Key_Left:      data.append("\x1b[D"); break;
                case Qt::Key_Home:      data.append("\x1b[H"); break;
                case Qt::Key_End:       data.append("\x1b[F"); break;
                case Qt::Key_Insert:    data.append("\x1b[2~"); break;
                case Qt::Key_Delete:    data.append("\x1b[3~"); break;
                case Qt::Key_PageUp:    data.append("\x1b[5~"); break;
                case Qt::Key_PageDown:  data.append("\x1b[6~"); break;
                case Qt::Key_F1:        data.append("\x1bOP"); break;
                case Qt::Key_F2:        data.append("\x1bOQ"); break;
                case Qt::Key_F3:        data.append("\x1bOR"); break;
                case Qt::Key_F4:        data.append("\x1bOS"); break;
                case Qt::Key_F5:        data.append("\x1b[15~"); break;
                case Qt::Key_F6:        data.append("\x1b[17~"); break;
                case Qt::Key_F7:        data.append("\x1b[18~"); break;
                case Qt::Key_F8:        data.append("\x1b[19~"); break;
                case Qt::Key_F9:        data.append("\x1b[20~"); break;
                case Qt::Key_F10:       data.append("\x1b[21~"); break;
                case Qt::Key_F11:       data.append("\x1b[23~"); break;
                case Qt::Key_F12:       data.append("\x1b[24~"); break;
                default:
                    if (!e->text().isEmpty())
                        data.append(e->text().toUtf8());
                    break;
            }
        }

        if (!data.isEmpty()) {
            ::write(m_masterFd, data.data(), data.size());
        }
    }

private slots:
    void onPtyData() {
        char buf[8192];
        ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
        if (n > 0) {
            processPtyText(QByteArray(buf, n));
        } else if (n == 0 || (n == -1 && errno != EAGAIN)) {
            m_notifier->setEnabled(false);
            // Crude termination message
            for (char c : std::string("\n[Shell Terminated]")) putChar(c);
            update();
        }
    }

    void processPtyText(const QByteArray &data) {
        m_buffer.append(data);
        
        while (!m_buffer.isEmpty()) {
            int consumed = 0;
            uint8_t c = (uint8_t)m_buffer[0];
            
            if (c == '\x1b') {
                if (m_buffer.size() < 2) break;
                uint8_t next = (uint8_t)m_buffer[1];
                
                if (next == '[') {
                    // CSI
                    int j = 2;
                    QString params;
                    while (j < m_buffer.size() && (isdigit((uint8_t)m_buffer[j]) || m_buffer[j] == ';' || m_buffer[j] == '?' || m_buffer[j] == ' ')) {
                        params += (char)m_buffer[j++];
                    }
                    if (j >= m_buffer.size()) break;
                    handleAnsi(m_buffer[j], params);
                    consumed = j + 1;
                } else if (next == '(' || next == ')') {
                    if (m_buffer.size() < 3) break;
                    consumed = 3;
                } else if (next == ']') {
                    // OSC
                    int j = 2;
                    while (j < m_buffer.size() && (uint8_t)m_buffer[j] != '\a' && (uint8_t)m_buffer[j] != '\x1b') j++;
                    if (j >= m_buffer.size()) break;
                    if ((uint8_t)m_buffer[j] == '\x1b') {
                        if (j + 1 < m_buffer.size() && (uint8_t)m_buffer[j+1] == '\\') j++;
                    }
                    consumed = j + 1;
                } else if (next == 'M') {
                    // RI (Reverse Index)
                    if (m_cursorY == m_topMargin) {
                        scrollDown();
                    } else if (m_cursorY > 0) {
                        m_cursorY--;
                    }
                    consumed = 2;
                } else if (next == '7') {
                    // DECSC (Save Cursor)
                    m_savedX = m_cursorX;
                    m_savedY = m_cursorY;
                    consumed = 2;
                } else if (next == '8') {
                    // DECRC (Restore Cursor)
                    m_cursorX = m_savedX;
                    m_cursorY = m_savedY;
                    consumed = 2;
                } else {
                    consumed = 2;
                }
            } else if (c == '\r') {
                m_cursorX = 0;
                consumed = 1;
            } else if (c == '\n') {
                m_cursorY++;
                if (m_cursorY > m_bottomMargin) {
                    m_cursorY = m_bottomMargin;
                    scrollUp();
                }
                consumed = 1;
            } else if (c == '\b') {
                if (m_cursorX > 0) m_cursorX--;
                consumed = 1;
            } else if (c == '\t') {
                int spaces = 8 - (m_cursorX % 8);
                for(int i=0; i<spaces; ++i) putChar(' ');
                consumed = 1;
            } else if (c >= 0x80) {
                // UTF-8
                int len = 0;
                if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                else {
                    consumed = 1; // Invalid UTF-8 start
                }
                if (len > 0) {
                    if (m_buffer.size() < len) break;
                    QString s = QString::fromUtf8(m_buffer.left(len));
                    if (!s.isEmpty()) {
                        QList<uint> ucs4 = s.toUcs4();
                        if (!ucs4.isEmpty()) putChar(ucs4[0]);
                    }
                    consumed = len;
                }
            } else if (c < 32 || c == 0x7f) {
                // Ignore other control characters to avoid hollow blocks
                consumed = 1;
            } else {
                putChar(c);
                consumed = 1;
            }
            
            if (consumed > 0) {
                m_buffer.remove(0, consumed);
            } else {
                break;
            }
        }
        update();
    }

    void putChar(uint32_t c) {
        if (m_cursorX >= m_cols) {
            m_cursorX = 0;
            m_cursorY++;
            if (m_cursorY > m_bottomMargin) {
                m_cursorY = m_bottomMargin;
                scrollUp();
            } else if (m_cursorY >= m_rows) {
                m_cursorY = m_rows - 1;
            }
        }
        if (m_cursorY < m_rows && m_cursorX < m_cols) {
            TermCell &cell = m_grid[m_cursorY][m_cursorX];
            cell.c = c;
            cell.fg = m_currentFg;
            cell.bg = m_currentBg;
            cell.bold = m_currentBold;
            cell.inverse = m_currentInverse;
            m_cursorX++;
        }
    }

    void scrollUp() {
        int top = m_topMargin;
        int bottom = m_bottomMargin;
        if (top < 0 || bottom >= m_rows || top >= bottom) {
            top = 0; bottom = m_rows - 1;
        }
        for (int r = top; r < bottom; ++r) {
            m_grid[r] = m_grid[r+1];
        }
        m_grid[bottom] = QVector<TermCell>(m_cols);
    }

    void scrollDown() {
        int top = m_topMargin;
        int bottom = m_bottomMargin;
        if (top < 0 || bottom >= m_rows || top >= bottom) {
            top = 0; bottom = m_rows - 1;
        }
        for (int r = bottom; r > top; --r) {
            m_grid[r] = m_grid[r-1];
        }
        m_grid[top] = QVector<TermCell>(m_cols);
    }

    void scrollIfNeeded() {
        // Simple line feed handling is done in processPtyText
    }

    void handleAnsi(char code, const QString &params) {
        QStringList p = params.split(';');
        if (code == 'H' || code == 'f') {
            int r = p.size() > 0 ? qMax(1, p[0].toInt()) : 1;
            int c = p.size() > 1 ? qMax(1, p[1].toInt()) : 1;
            m_cursorY = qBound(0, r - 1, m_rows - 1);
            m_cursorX = qBound(0, c - 1, m_cols - 1);
        } else if (code == 'A') { // CUU
            int n = params.isEmpty() ? 1 : p[0].toInt();
            m_cursorY = qMax(0, m_cursorY - (n == 0 ? 1 : n));
        } else if (code == 'B') { // CUD
            int n = params.isEmpty() ? 1 : p[0].toInt();
            m_cursorY = qMin(m_rows - 1, m_cursorY + (n == 0 ? 1 : n));
        } else if (code == 'C') { // CUF
            int n = params.isEmpty() ? 1 : p[0].toInt();
            m_cursorX = qMin(m_cols - 1, m_cursorX + (n == 0 ? 1 : n));
        } else if (code == 'D') { // CUB
            int n = params.isEmpty() ? 1 : p[0].toInt();
            m_cursorX = qMax(0, m_cursorX - (n == 0 ? 1 : n));
        } else if (code == 'G') { // CHA
            int c = params.isEmpty() ? 1 : p[0].toInt();
            m_cursorX = qBound(0, c - 1, m_cols - 1);
        } else if (code == 'd') { // VPA
            int r = params.isEmpty() ? 1 : p[0].toInt();
            m_cursorY = qBound(0, r - 1, m_rows - 1);
        } else if (code == 'L') { // IL (Insert Line)
            int n = params.isEmpty() ? 1 : p[0].toInt();
            int count = qBound(0, (n == 0 ? 1 : n), m_rows);
            if (m_cursorY >= m_topMargin && m_cursorY <= m_bottomMargin) {
                for (int i = 0; i < count; ++i) {
                    m_grid.insert(m_cursorY, QVector<TermCell>(m_cols));
                    m_grid.remove(m_bottomMargin + 1);
                }
            }
        } else if (code == 'M') { // DL (Delete Line)
            int n = params.isEmpty() ? 1 : p[0].toInt();
            int count = qBound(0, (n == 0 ? 1 : n), m_rows);
            if (m_cursorY >= m_topMargin && m_cursorY <= m_bottomMargin) {
                for (int i = 0; i < count; ++i) {
                    m_grid.remove(m_cursorY);
                    m_grid.insert(m_bottomMargin, QVector<TermCell>(m_cols));
                }
            }
        } else if (code == 'P') { // DCH
            int n = params.isEmpty() ? 1 : p[0].toInt();
            int count = (n == 0 ? 1 : n);
            for (int i = 0; i < count; ++i) {
                if (m_cursorX < m_cols) {
                    m_grid[m_cursorY].remove(m_cursorX);
                    m_grid[m_cursorY].push_back(TermCell());
                }
            }
        } else if (code == 'X') { // ECH
            int n = params.isEmpty() ? 1 : p[0].toInt();
            int count = (n == 0 ? 1 : n);
            for (int i = 0; i < count && (m_cursorX + i) < m_cols; ++i) {
                m_grid[m_cursorY][m_cursorX + i] = TermCell();
            }
        } else if (code == 'h' || code == 'l') {
            bool set = (code == 'h');
            for (const QString &param : p) {
                if (param == "?25") {
                    m_cursorVisible = set;
                } else if (param == "?1049") {
                    if (set && !m_isAltScreen) {
                        m_savedGrid = m_grid;
                        m_savedCursorX = m_cursorX;
                        m_savedCursorY = m_cursorY;
                        m_isAltScreen = true;
                        // Clear alt screen
                        for(int r=0; r<m_rows; ++r) m_grid[r] = QVector<TermCell>(m_cols);
                        m_cursorX = 0; m_cursorY = 0;
                    } else if (!set && m_isAltScreen) {
                        m_grid = m_savedGrid;
                        m_cursorX = m_savedCursorX;
                        m_cursorY = m_savedCursorY;
                        m_isAltScreen = false;
                    }
                }
            }
        } else if (code == 'J') {
            int mode = params.isEmpty() ? 0 : p[0].toInt();
            if (mode == 2) { // Clear entire screen
                for(int r=0; r<m_rows; ++r) m_grid[r] = QVector<TermCell>(m_cols);
                m_cursorX = 0; m_cursorY = 0;
            } else if (mode == 0) { // Clear from cursor to end
                for (int c = m_cursorX; c < m_cols; ++c) m_grid[m_cursorY][c] = TermCell();
                for (int r = m_cursorY + 1; r < m_rows; ++r) m_grid[r] = QVector<TermCell>(m_cols);
            } else if (mode == 1) { // Clear from start to cursor
                for (int r = 0; r < m_cursorY; ++r) m_grid[r] = QVector<TermCell>(m_cols);
                for (int c = 0; c <= m_cursorX; ++c) m_grid[m_cursorY][c] = TermCell();
            }
        } else if (code == 'K') {
            int mode = params.isEmpty() ? 0 : p[0].toInt();
            if (mode == 0) { // Clear to end of line
                for(int c = m_cursorX; c < m_cols; ++c) m_grid[m_cursorY][c] = TermCell();
            } else if (mode == 1) { // Clear from start to cursor
                for(int c = 0; c <= m_cursorX; ++c) m_grid[m_cursorY][c] = TermCell();
            } else if (mode == 2) { // Clear entire line
                m_grid[m_cursorY] = QVector<TermCell>(m_cols);
            }
        } else if (code == 'm') {
            for (int i = 0; i < p.size(); ++i) {
                int val = p[i].toInt();
                if (val == 0) {
                    m_currentFg = Qt::white;
                    m_currentBg = Qt::transparent;
                    m_currentBold = false;
                    m_currentInverse = false;
                } else if (val == 1) m_currentBold = true;
                else if (val == 7) m_currentInverse = true;
                else if (val == 27) m_currentInverse = false;
                else if (val >= 30 && val <= 37) m_currentFg = getAnsiColor(val - 30, false);
                else if (val >= 40 && val <= 47) m_currentBg = getAnsiColor(val - 40, false);
                else if (val >= 90 && val <= 97) m_currentFg = getAnsiColor(val - 90, true);
                else if (val >= 100 && val <= 107) m_currentBg = getAnsiColor(val - 100, true);
                else if (val == 39) m_currentFg = Qt::white;
                else if (val == 49) m_currentBg = Qt::transparent;
                else if (val == 38 || val == 48) {
                    if (i + 2 < p.size() && p[i+1].toInt() == 5) {
                        QColor c = get256Color(p[i+2].toInt());
                        if (val == 38) m_currentFg = c; else m_currentBg = c;
                        i += 2;
                    } else if (i + 4 < p.size() && p[i+1].toInt() == 2) {
                        QColor c(p[i+2].toInt(), p[i+3].toInt(), p[i+4].toInt());
                        if (val == 38) m_currentFg = c; else m_currentBg = c;
                        i += 4;
                    }
                }
            }
        } else if (code == 'r') { // DECSTBM
            int top = p.size() > 0 ? qMax(1, p[0].toInt()) : 1;
            int bottom = p.size() > 1 ? qMax(1, p[1].toInt()) : m_rows;
            m_topMargin = qBound(0, top - 1, m_rows - 1);
            m_bottomMargin = qBound(0, bottom - 1, m_rows - 1);
            m_cursorX = 0;
            m_cursorY = 0;
        } else if (code == 'S') { // SU
            int n = params.isEmpty() ? 1 : p[0].toInt();
            for(int i=0; i<n; ++i) scrollUp();
        } else if (code == 'T') { // SD
            int n = params.isEmpty() ? 1 : p[0].toInt();
            for(int i=0; i<n; ++i) scrollDown();
        } else if (code == 's') { // SCOSC (Save Cursor)
            m_savedX = m_cursorX;
            m_savedY = m_cursorY;
        } else if (code == 'u') { // SCORC (Restore Cursor)
            m_cursorX = m_savedX;
            m_cursorY = m_savedY;
        }
    }

    QColor getAnsiColor(int code, bool bright) {
        static const QColor colors[] = {
            Qt::black, Qt::red, Qt::green, Qt::yellow,
            Qt::blue, Qt::magenta, Qt::cyan, Qt::white
        };
        if (code >= 0 && code < 8) return colors[code];
        return Qt::white;
    }

    QColor get256Color(int val) {
        if (val < 8) return getAnsiColor(val, false);
        if (val < 16) return getAnsiColor(val - 8, true);
        if (val < 232) {
            int r = (val - 16) / 36;
            int g = ((val - 16) % 36) / 6;
            int b = (val - 16) % 6;
            return QColor(r ? (r * 40 + 55) : 0, g ? (g * 40 + 55) : 0, b ? (b * 40 + 55) : 0);
        }
        int gray = (val - 232) * 10 + 8;
        return QColor(gray, gray, gray);
    }

private:
    int m_masterFd;
    pid_t m_shellPid;
    QSocketNotifier *m_notifier;
    QByteArray m_buffer;
    
    int m_cols, m_rows;
    int m_cursorX, m_cursorY;
    QVector<QVector<TermCell>> m_grid;
    
    QFont m_font;
    int m_charWidth, m_charHeight;
    
    QColor m_currentFg = Qt::white;
    QColor m_currentBg = Qt::transparent;
    bool m_currentBold = false;
    bool m_currentInverse = false;

    bool m_cursorVisible = true;
    QVector<QVector<TermCell>> m_savedGrid;
    int m_savedCursorX = 0, m_savedCursorY = 0;
    bool m_isAltScreen = false;
    int m_topMargin = 0, m_bottomMargin = 23;
    int m_savedX = 0, m_savedY = 0;
};
#endif

class ZetaTerminal : public QMainWindow {
    Q_OBJECT
public:
    ZetaTerminal() {
        setWindowTitle("Zeta Terminal");
        resize(1100, 700);
        setMinimumSize(1000, 600); // Ensure terminal and AI panel have room
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
#ifdef USE_QTERMWIDGET
        termWidget = new QTermWidget(central);
        termWidget->setTerminalFont(mono);
        termWidget->setScrollBarPosition(QTermWidget::ScrollBarRight);
        termWidget->startShellProgram();
#else
        termWidget = new ZetaPtyTerminal(central);
        termWidget->setFont(mono);
#endif
        mainLayout->addWidget(termWidget, 3); // Left panel takes 3/4 space

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

        QString baseStyle = QString(
            "background-color: %1; color: %2; border: 1px solid %3; "
            "font-family: 'Hack', 'Noto Sans Mono', monospace; font-size: 10pt; "
            "selection-background-color: %4; selection-color: %1; caret-color: %5;")
            .arg(bgColor, fgColor, borderColor, accentColor, caretColor);

        if (aiOutput) {
            aiOutput->setStyleSheet(baseStyle + "padding: 4px;");
        }

        if (termWidget) {
            termWidget->setStyleSheet(baseStyle + "padding: 0px;");
        }

        entry->setStyleSheet(baseStyle + "padding: 4px;");
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

    void append_text(const QString &text, const QString &colorHex = "#f4eedc") {
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
        if (aiOutput && !text.contains("]$") && !text.contains("[root@")) {
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
        // Terminal input is handled directly by ZetaPtyTerminal widget.
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
