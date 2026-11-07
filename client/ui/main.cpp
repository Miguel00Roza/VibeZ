#include "client/ui/MainWindow.hpp"

#include <QApplication>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VibeZ"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("VibeZ"));

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#16181d")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e6e8ec")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#1a1d24")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#1f232b")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#222731")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#e6e8ec")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#e6e8ec")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#2a3140")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e6e8ec")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ff6b6b")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#3f9d74")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2e6b4f")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6b7280")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#6b7280")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#6b7280")));
    app.setPalette(palette);

    app.setStyleSheet(QStringLiteral(R"(
        QWidget { font-size: 10pt; }
        QMainWindow, QStackedWidget { background: #16181d; }
        QLabel#appTitle { font-size: 34pt; font-weight: 700; color: #f2f4f8; }
        QLabel#appSubtitle { font-size: 11pt; color: #9aa4b2; }
        QLabel#sectionTitle { font-weight: 600; color: #c7cedb; }
        QLabel#hint { color: #8a92a2; }
        QWidget#card {
            background: #1b1f27;
            border: 1px solid #2c313a;
            border-radius: 12px;
        }
        QLineEdit, QSpinBox {
            background: #1f232b;
            border: 1px solid #333a45;
            border-radius: 6px;
            padding: 7px 9px;
            selection-background-color: #2e6b4f;
        }
        QLineEdit:focus, QSpinBox:focus { border-color: #3f9d74; }
        QPushButton {
            background: #2a3140;
            border: 1px solid #3a4356;
            border-radius: 6px;
            padding: 7px 14px;
        }
        QPushButton:hover { background: #344055; }
        QPushButton:pressed { background: #222836; }
        QPushButton:checked { background: #2e6b4f; border-color: #3f9d74; }
        QPushButton:disabled { color: #6b7280; background: #20242c; }
        QPushButton#primaryButton {
            background: #2e6b4f;
            border-color: #3f9d74;
            font-weight: 600;
            padding: 9px 28px;
        }
        QPushButton#primaryButton:hover { background: #357a5b; }
        QListWidget, QTableWidget, QPlainTextEdit {
            background: #1a1d24;
            border: 1px solid #2c313a;
            border-radius: 8px;
            alternate-background-color: #1e222a;
        }
        QListWidget::item { padding: 7px 6px; border-radius: 4px; }
        QListWidget::item:selected { background: #2e6b4f; }
        QHeaderView::section {
            background: #222731;
            color: #c7cedb;
            border: 0;
            border-bottom: 1px solid #2c313a;
            padding: 7px 6px;
            font-weight: 600;
        }
        QTableWidget::item:selected { background: #2e6b4f; }
        QProgressBar {
            background: #1f232b;
            border: 1px solid #333a45;
            border-radius: 4px;
        }
        QProgressBar::chunk { background: #3f9d74; border-radius: 3px; }
        QSlider::groove:horizontal {
            height: 4px;
            background: #333a45;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal { background: #3f9d74; border-radius: 2px; }
        QSlider::handle:horizontal {
            background: #e6e8ec;
            width: 12px;
            margin: -5px 0;
            border-radius: 6px;
        }
        QSplitter::handle { background: #242932; }
        QStatusBar { background: #12151a; color: #8a92a2; }
        QScrollBar:vertical { background: #16181d; width: 10px; }
        QScrollBar::handle:vertical { background: #333a45; border-radius: 5px; min-height: 24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cliente de voz VibeZ"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption hostOption({QStringLiteral("H"), QStringLiteral("host")},
                                        QStringLiteral("Servidor (IPv4)."), QStringLiteral("host"),
                                        QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOption({QStringLiteral("p"), QStringLiteral("port")},
                                        QStringLiteral("Porta do servidor."), QStringLiteral("port"),
                                        QStringLiteral("28765"));
    const QCommandLineOption nicknameOption({QStringLiteral("n"), QStringLiteral("nickname")},
                                            QStringLiteral("Apelido."), QStringLiteral("nickname"),
                                            QStringLiteral("Convidado"));
    const QCommandLineOption autoConnectOption({QStringLiteral("a"), QStringLiteral("auto-connect")},
                                               QStringLiteral("Conecta automaticamente ao iniciar."));
    const QCommandLineOption joinOption(QStringLiteral("join-channel"),
                                        QStringLiteral("Entra no canal informado após conectar."),
                                        QStringLiteral("id"));
    const QCommandLineOption quitOption(QStringLiteral("quit-after"),
                                        QStringLiteral("Encerra após N milissegundos (teste de fumaça)."),
                                        QStringLiteral("ms"));
    parser.addOptions({hostOption, portOption, nicknameOption, autoConnectOption, joinOption, quitOption});
    parser.process(app);

    bool portOk = false;
    const uint port = parser.value(portOption).toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) {
        qCritical("Porta inválida: %s", qPrintable(parser.value(portOption)));
        return 2;
    }

    MainWindow window;
    window.setConnectionFields(parser.value(hostOption), static_cast<quint16>(port),
                               parser.value(nicknameOption));

    bool joinOk = false;
    const uint joinId = parser.value(joinOption).toUInt(&joinOk);
    if (joinOk && joinId != 0) window.setAutoJoinChannel(joinId);

    window.show();

    if (parser.isSet(autoConnectOption)) {
        QTimer::singleShot(0, &window, &MainWindow::connectToServer);
    }

    bool quitOk = false;
    const int quitAfterMs = parser.value(quitOption).toInt(&quitOk);
    if (quitOk && quitAfterMs > 0) {
        QTimer::singleShot(quitAfterMs, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
