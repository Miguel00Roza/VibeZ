#include "client/ui/MainWindow.hpp"

#include "client/ui/SessionController.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QBrush>
#include <QCloseEvent>
#include <QDebug>
#include <QFormLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QStatusBar>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using vibez::client::ConnectionState;

QString StateText(const ConnectionState state) {
    switch (state) {
    case ConnectionState::Disconnected: return QStringLiteral("Desconectado");
    case ConnectionState::Connecting: return QStringLiteral("Conectando");
    case ConnectionState::Connected: return QStringLiteral("Conectado");
    case ConnectionState::Reconnecting: return QStringLiteral("Reconectando");
    }
    return QStringLiteral("Desconhecido");
}

const char* StateTextUtf8(const ConnectionState state) {
    switch (state) {
    case ConnectionState::Disconnected: return "desconectado";
    case ConnectionState::Connecting: return "conectando";
    case ConnectionState::Connected: return "conectado";
    case ConnectionState::Reconnecting: return "reconectando";
    }
    return "desconhecido";
}

QLabel* MakeTitle(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), controller_(new SessionController(this)) {
    buildUi();

    connect(controller_, &SessionController::connectionStateChanged, this, &MainWindow::onConnectionStateChanged);
    connect(controller_, &SessionController::sessionChanged, this, &MainWindow::onSessionChanged);
    connect(controller_, &SessionController::channelsChanged, this, &MainWindow::onChannelsChanged);
    connect(controller_, &SessionController::usersChanged, this, &MainWindow::onUsersChanged);
    connect(controller_, &SessionController::audioStateChanged, this, &MainWindow::updateAudioLabel);
    connect(controller_, &SessionController::errorRaised, this, [this](const QString& message) {
        appendLog(QStringLiteral("Servidor: ") + message);
    });
    connect(controller_, &SessionController::keyframeRequested, this, [this](const quint32 requesterId) {
        appendLog(QStringLiteral("Keyframe solicitado por #%1.").arg(requesterId));
        // The next captured frame is sent as a keyframe so the remote viewer can
        // rebuild its decoder state instead of waiting for the next periodic one.
        forceKeyframe_ = true;
    });
    connect(controller_, &SessionController::videoFrameReceived, this, &MainWindow::onVideoFrameReceived);
    connect(controller_, &SessionController::chatMessageReceived, this,
            [this](const quint32 senderId, const QString& senderName, const QString& text) {
        chatView_->appendPlainText(QStringLiteral("%1 (#%2): %3").arg(senderName).arg(senderId).arg(text));
    });

    auto* levelTimer = new QTimer(this);
    levelTimer->setInterval(100);
    connect(levelTimer, &QTimer::timeout, this, &MainWindow::updateLevels);
    levelTimer->start();

    shareTimer_ = new QTimer(this);
    shareTimer_->setInterval(100);
    connect(shareTimer_, &QTimer::timeout, this, &MainWindow::captureScreenFrame);

    setWindowTitle(QStringLiteral("VibeZ"));
    resize(1120, 720);
    showConnectPage();
    updateAudioLabel();
    updateStatusBar();
}

MainWindow::~MainWindow() {
    // Stop the worker threads and drop the UI connections before QObject
    // destroys the child controller, which emits state signals while stopping.
    disconnect(controller_, nullptr, this, nullptr);
    controller_->stop();
}

void MainWindow::buildUi() {
    pages_ = new QStackedWidget(this);
    connectPage_ = buildConnectPage();
    roomPage_ = buildRoomPage();
    pages_->addWidget(connectPage_);
    pages_->addWidget(roomPage_);
    setCentralWidget(pages_);
    statusBar()->showMessage(QStringLiteral("Desconectado"));
}

QWidget* MainWindow::buildConnectPage() {
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->addStretch(1);

    auto* card = new QWidget;
    card->setObjectName(QStringLiteral("card"));
    card->setMaximumWidth(480);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(32, 28, 32, 28);
    layout->setSpacing(14);

    auto* title = new QLabel(QStringLiteral("VibeZ"));
    title->setObjectName(QStringLiteral("appTitle"));
    auto* subtitle = new QLabel(QStringLiteral("Voz de baixa latência para Windows"));
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addSpacing(8);

    auto* form = new QFormLayout;
    form->setSpacing(10);
    nicknameEdit_ = new QLineEdit(QStringLiteral("Convidado"));
    nicknameEdit_->setMaxLength(32);
    nicknameEdit_->setPlaceholderText(QStringLiteral("Seu apelido"));
    hostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"));
    hostEdit_->setPlaceholderText(QStringLiteral("Endereço IPv4 do servidor"));
    portSpin_ = new QSpinBox;
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(28765);
    form->addRow(QStringLiteral("Apelido"), nicknameEdit_);
    form->addRow(QStringLiteral("Servidor"), hostEdit_);
    form->addRow(QStringLiteral("Porta"), portSpin_);
    layout->addLayout(form);

    connectButton_ = new QPushButton(QStringLiteral("Conectar"));
    connectButton_->setObjectName(QStringLiteral("primaryButton"));
    connectButton_->setDefault(true);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(connectButton_, 0);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    connectStatus_ = new QLabel(QStringLiteral("Informe o servidor para entrar."));
    connectStatus_->setObjectName(QStringLiteral("hint"));
    connectStatus_->setWordWrap(true);
    layout->addWidget(connectStatus_);

    connectAudio_ = new QLabel;
    connectAudio_->setObjectName(QStringLiteral("hint"));
    connectAudio_->setWordWrap(true);
    layout->addWidget(connectAudio_);

    auto* note = new QLabel(QStringLiteral("Sem autenticação ou TLS nesta versão. Áudio usa os dispositivos padrão do Windows."));
    note->setObjectName(QStringLiteral("hint"));
    note->setWordWrap(true);
    layout->addWidget(note);

    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch(1);

    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::connectToServer);
    connect(nicknameEdit_, &QLineEdit::returnPressed, this, &MainWindow::connectToServer);
    connect(hostEdit_, &QLineEdit::returnPressed, this, &MainWindow::connectToServer);
    return page;
}

QWidget* MainWindow::buildRoomPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 8);
    layout->setSpacing(10);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    micButton_ = new QPushButton(QStringLiteral("Microfone ativo"));
    micButton_->setCheckable(true);
    micButton_->setChecked(true);
    micButton_->setToolTip(QStringLiteral("Clique para silenciar seu microfone."));
    shareButton_ = new QPushButton(QStringLiteral("Compartilhar tela"));
    shareButton_->setCheckable(true);
    shareButton_->setToolTip(QStringLiteral("Selecione uma tela para compartilhar com o canal."));
    keyframeButton_ = new QPushButton(QStringLiteral("Pedir keyframe"));
    keyframeButton_->setToolTip(QStringLiteral("Solicita um keyframe ao participante selecionado."));
    restartAudioButton_ = new QPushButton(QStringLiteral("Reiniciar áudio"));
    restartAudioButton_->setToolTip(QStringLiteral("Recria a captura e a reprodução de áudio sem desconectar."));
    disconnectButton_ = new QPushButton(QStringLiteral("Desconectar"));

    localLevel_ = new QProgressBar;
    localLevel_->setRange(0, 100);
    localLevel_->setValue(0);
    localLevel_->setTextVisible(false);
    localLevel_->setFixedWidth(120);
    localLevel_->setFixedHeight(10);
    localLevel_->setToolTip(QStringLiteral("Nível do seu microfone"));
    audioLabel_ = new QLabel;
    audioLabel_->setObjectName(QStringLiteral("hint"));

    toolbar->addWidget(micButton_);
    toolbar->addWidget(shareButton_);
    toolbar->addWidget(keyframeButton_);
    toolbar->addWidget(restartAudioButton_);
    toolbar->addSpacing(12);
    toolbar->addWidget(new QLabel(QStringLiteral("Nível")));
    toolbar->addWidget(localLevel_);
    toolbar->addWidget(audioLabel_);
    toolbar->addStretch(1);
    toolbar->addWidget(disconnectButton_);
    layout->addLayout(toolbar);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);

    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);
    currentChannel_ = MakeTitle(QStringLiteral("Canal atual: —"));
    channelList_ = new QListWidget;
    channelList_->setToolTip(QStringLiteral("Clique duas vezes em um canal para entrar."));
    auto* createRow = new QHBoxLayout;
    newChannelEdit_ = new QLineEdit;
    newChannelEdit_->setPlaceholderText(QStringLiteral("Nome do novo canal"));
    newChannelEdit_->setMaxLength(64);
    auto* createButton = new QPushButton(QStringLiteral("Criar"));
    createRow->addWidget(newChannelEdit_, 1);
    createRow->addWidget(createButton);
    joinButton_ = new QPushButton(QStringLiteral("Entrar no canal"));
    leaveButton_ = new QPushButton(QStringLiteral("Voltar ao Lobby"));
    leftLayout->addWidget(currentChannel_);
    leftLayout->addWidget(channelList_, 1);
    leftLayout->addLayout(createRow);
    leftLayout->addWidget(joinButton_);
    leftLayout->addWidget(leaveButton_);
    left->setMinimumWidth(260);

    auto* right = new QSplitter(Qt::Vertical);
    right->setChildrenCollapsible(false);
    auto* usersPanel = new QWidget;
    auto* usersLayout = new QVBoxLayout(usersPanel);
    usersLayout->setContentsMargins(0, 0, 0, 0);
    usersLayout->setSpacing(8);
    usersLayout->addWidget(MakeTitle(QStringLiteral("Participantes")));
    userTable_ = new QTableWidget(0, 8);
    userTable_->setHorizontalHeaderLabels({QStringLiteral("Participante"), QStringLiteral("ID"),
                                           QStringLiteral("Canal"), QStringLiteral("Estado"),
                                           QStringLiteral("Nível"), QStringLiteral("Volume"),
                                           QStringLiteral("Tela"), QStringLiteral("Áudio")});
    userTable_->verticalHeader()->setVisible(false);
    userTable_->verticalHeader()->setDefaultSectionSize(34);
    userTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    userTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    userTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    userTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    userTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    userTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    userTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    userTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    userTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    userTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    userTable_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    usersLayout->addWidget(userTable_, 1);

    auto* screenPanel = new QWidget;
    auto* screenLayout = new QVBoxLayout(screenPanel);
    screenLayout->setContentsMargins(0, 0, 0, 0);
    screenLayout->setSpacing(8);
    screenViewerTitle_ = MakeTitle(QStringLiteral("Compartilhamento de tela"));
    screenViewer_ = new QLabel(QStringLiteral("Nenhuma tela sendo recebida."));
    screenViewer_->setObjectName(QStringLiteral("hint"));
    screenViewer_->setAlignment(Qt::AlignCenter);
    screenViewer_->setMinimumHeight(230);
    screenViewer_->setStyleSheet(QStringLiteral("background: #090b0f; border: 1px solid #2c313a; border-radius: 8px;"));
    screenLayout->addWidget(screenViewerTitle_);
    screenLayout->addWidget(screenViewer_, 1);

    auto* logPanel = new QWidget;
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->setSpacing(8);
    logLayout->addWidget(MakeTitle(QStringLiteral("Registro")));
    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    logLayout->addWidget(logView_, 1);

    auto* chatPanel = new QWidget;
    auto* chatLayout = new QVBoxLayout(chatPanel);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(8);
    chatLayout->addWidget(MakeTitle(QStringLiteral("Chat do canal")));
    chatView_ = new QPlainTextEdit;
    chatView_->setReadOnly(true);
    chatView_->setMaximumBlockCount(300);
    chatView_->setPlaceholderText(QStringLiteral("As mensagens deste canal aparecem aqui."));
    chatLayout->addWidget(chatView_, 1);
    auto* chatRow = new QHBoxLayout;
    chatInput_ = new QLineEdit;
    chatInput_->setPlaceholderText(QStringLiteral("Enviar mensagem para o canal…"));
    chatInput_->setMaxLength(1000);
    auto* sendChatButton = new QPushButton(QStringLiteral("Enviar"));
    chatRow->addWidget(chatInput_, 1);
    chatRow->addWidget(sendChatButton);
    chatLayout->addLayout(chatRow);

    right->addWidget(screenPanel);
    right->addWidget(usersPanel);
    right->addWidget(chatPanel);
    right->addWidget(logPanel);
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 2);
    right->setStretchFactor(2, 2);
    right->setStretchFactor(3, 1);
    right->setMinimumWidth(540);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    connect(joinButton_, &QPushButton::clicked, this, &MainWindow::onJoinSelectedChannel);
    connect(leaveButton_, &QPushButton::clicked, this, [this] {
        if (controller_->leaveChannel()) appendLog(QStringLiteral("Voltando ao Lobby."));
        else appendLog(QStringLiteral("Não foi possível sair do canal."));
    });
    connect(channelList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { onJoinSelectedChannel(); });
    connect(createButton, &QPushButton::clicked, this, &MainWindow::onCreateChannel);
    connect(newChannelEdit_, &QLineEdit::returnPressed, this, &MainWindow::onCreateChannel);
    connect(sendChatButton, &QPushButton::clicked, this, &MainWindow::sendChat);
    connect(chatInput_, &QLineEdit::returnPressed, this, &MainWindow::sendChat);
    connect(micButton_, &QPushButton::toggled, this, &MainWindow::toggleMicrophone);
    connect(shareButton_, &QPushButton::toggled, this, &MainWindow::toggleSharing);
    connect(keyframeButton_, &QPushButton::clicked, this, &MainWindow::requestKeyframeFromSelection);
    connect(restartAudioButton_, &QPushButton::clicked, this, &MainWindow::restartAudio);
    connect(disconnectButton_, &QPushButton::clicked, this, &MainWindow::disconnectFromServer);
    return page;
}

void MainWindow::setConnectionFields(const QString& host, const quint16 port, const QString& nickname) {
    hostEdit_->setText(host);
    portSpin_->setValue(port);
    nicknameEdit_->setText(nickname);
}

void MainWindow::setAutoJoinChannel(const quint32 channelId) { autoJoinChannel_ = channelId; }

void MainWindow::connectToServer() {
    const QString host = hostEdit_->text().trimmed();
    const QString nickname = nicknameEdit_->text().trimmed();
    if (host.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("VibeZ"), QStringLiteral("Informe o endereço do servidor."));
        return;
    }
    if (nickname.isEmpty() || nickname.toUtf8().size() > 32) {
        QMessageBox::warning(this, QStringLiteral("VibeZ"), QStringLiteral("O apelido deve ter de 1 a 32 bytes."));
        return;
    }

    QString error;
    if (!controller_->start(host, static_cast<quint16>(portSpin_->value()), nickname, error)) {
        QMessageBox::critical(this, QStringLiteral("VibeZ"), QStringLiteral("Não foi possível iniciar a sessão: ") + error);
        return;
    }

    appendLog(QStringLiteral("Conectando a %1:%2 como %3…").arg(host).arg(portSpin_->value()).arg(nickname));
    if (!controller_->audioRunning()) {
        appendLog(QStringLiteral("Áudio indisponível: ") + controller_->audioError());
    }
    qInfo("VibeZ: audio %s", controller_->audioRunning() ? "ativo" : "indisponivel");

    connectButton_->setEnabled(false);
    connectButton_->setText(QStringLiteral("Conectando…"));
    connectStatus_->setText(QStringLiteral("Conectando a %1:%2…").arg(host).arg(portSpin_->value()));
    updateAudioLabel();
    updateStatusBar();
}

void MainWindow::disconnectFromServer() {
    const bool wasRunning = controller_->isRunning();
    inRoom_ = false;
    controller_->stop();
    clearRoom();
    appendLog(wasRunning ? QStringLiteral("Sessão encerrada.") : QStringLiteral("Sessão não estava ativa."));
    showConnectPage();
    updateStatusBar();
}

void MainWindow::onConnectionStateChanged() {
    const ConnectionState state = controller_->state();
    qInfo("VibeZ: estado da conexao: %s", StateTextUtf8(state));

    switch (state) {
    case ConnectionState::Disconnected:
        connectStatus_->setText(QStringLiteral("Desconectado."));
        if (restartAudioButton_) restartAudioButton_->setEnabled(false);
        if (inRoom_ && controller_->isRunning()) {
            appendLog(QStringLiteral("Desconectado do servidor."));
            disconnectFromServer();
        }
        break;
    case ConnectionState::Connecting:
        connectStatus_->setText(QStringLiteral("Conectando…"));
        break;
    case ConnectionState::Connected:
        connectStatus_->setText(QStringLiteral("Conectado."));
        if (restartAudioButton_) restartAudioButton_->setEnabled(true);
        if (!inRoom_) {
            showRoomPage();
            appendLog(QStringLiteral("Conectado ao servidor."));
        }
        break;
    case ConnectionState::Reconnecting:
        connectStatus_->setText(QStringLiteral("Conexão perdida; reconectando…"));
        if (inRoom_) appendLog(QStringLiteral("Conexão perdida; tentando reconectar…"));
        break;
    }
    updateStatusBar();
}

void MainWindow::onSessionChanged() {
    if (controller_->sessionId() != 0 && autoJoinChannel_ != 0) {
        const quint32 channelId = autoJoinChannel_;
        autoJoinChannel_ = 0;
        if (controller_->joinChannel(channelId)) {
            appendLog(QStringLiteral("Entrando no canal %1…").arg(channelName(channelId)));
        }
    }
    updateStatusBar();
}

void MainWindow::onChannelsChanged() {
    const quint32 selected = channelList_->currentItem() ? channelList_->currentItem()->data(Qt::UserRole).toUInt() : 0U;
    const QSignalBlocker blocker(channelList_);
    channelList_->clear();
    for (const auto& channel : controller_->channels()) {
        auto* item = new QListWidgetItem(QStringLiteral("#%1  %2").arg(channel.id).arg(QString::fromStdString(channel.name)));
        item->setData(Qt::UserRole, channel.id);
        channelList_->addItem(item);
        if (channel.id == selected && selected != 0) channelList_->setCurrentItem(item);
    }
}

void MainWindow::onUsersChanged() {
    updateUserRows();
    updateStatusBar();
}

void MainWindow::onJoinSelectedChannel() {
    auto* item = channelList_->currentItem();
    if (!item) {
        appendLog(QStringLiteral("Selecione um canal na lista."));
        return;
    }
    const quint32 channelId = item->data(Qt::UserRole).toUInt();
    if (channelId == 0) return;
    for (const auto& user : controller_->users()) {
        if (user.id == controller_->sessionId() && user.channel == channelId) {
            appendLog(QStringLiteral("Você já está no canal ") + channelName(channelId) + QStringLiteral("."));
            return;
        }
    }
    if (!controller_->joinChannel(channelId)) {
        appendLog(QStringLiteral("Não foi possível entrar no canal."));
    } else {
        appendLog(QStringLiteral("Entrando no canal %1…").arg(channelName(channelId)));
    }
}

void MainWindow::onCreateChannel() {
    const QString name = newChannelEdit_->text().trimmed();
    if (name.isEmpty()) return;
    if (name.toUtf8().size() > 64) {
        appendLog(QStringLiteral("O nome do canal deve ter no máximo 64 bytes."));
        return;
    }
    if (!controller_->createChannel(name)) {
        appendLog(QStringLiteral("Não foi possível criar o canal."));
        return;
    }
    appendLog(QStringLiteral("Criando canal \"%1\"…").arg(name));
    newChannelEdit_->clear();
}

void MainWindow::sendChat() {
    const QString text = chatInput_->text().trimmed();
    if (text.isEmpty()) return;
    if (text.toUtf8().size() > 1000) {
        appendLog(QStringLiteral("A mensagem do chat pode ter no máximo 1000 bytes."));
        return;
    }
    if (!controller_->sendChat(text)) {
        appendLog(QStringLiteral("Não foi possível enviar a mensagem."));
        return;
    }
    chatInput_->clear();
}

void MainWindow::toggleMicrophone(const bool active) {
    muted_ = !active;
    micButton_->setText(active ? QStringLiteral("Microfone ativo") : QStringLiteral("Microfone mudo"));
    micButton_->setToolTip(active ? QStringLiteral("Clique para silenciar seu microfone.")
                                  : QStringLiteral("Clique para ativar seu microfone."));
    if (!controller_->isRunning()) return;
    if (!controller_->setMuted(muted_)) {
        appendLog(QStringLiteral("Falha ao enviar o estado do microfone."));
    } else {
        appendLog(muted_ ? QStringLiteral("Microfone silenciado.") : QStringLiteral("Microfone ativado."));
    }
}

void MainWindow::toggleSharing(const bool sharing) {
    if (!controller_->isRunning()) {
        const QSignalBlocker blocker(shareButton_);
        shareButton_->setChecked(false);
        return;
    }

    if (sharing) {
        QDialog settings(this);
        settings.setWindowTitle(QStringLiteral("Configurações do compartilhamento"));
        auto* form = new QFormLayout(&settings);
        auto* fps = new QComboBox(&settings);
        for (const int value : {5, 10, 15, 20, 30, 60}) fps->addItem(QStringLiteral("%1 FPS").arg(value), value);
        fps->setCurrentIndex(fps->findData(shareFps_));
        auto* quality = new QComboBox(&settings);
        quality->addItem(QStringLiteral("Baixa — mais leve"), 35);
        quality->addItem(QStringLiteral("Média — recomendada"), 55);
        quality->addItem(QStringLiteral("Alta"), 75);
        quality->addItem(QStringLiteral("Muito alta"), 90);
        quality->setCurrentIndex(quality->findData(shareJpegQuality_));
        auto* resolution = new QComboBox(&settings);
        resolution->addItem(QStringLiteral("640 px — economia de dados"), 640);
        resolution->addItem(QStringLiteral("960 px — recomendada"), 960);
        resolution->addItem(QStringLiteral("1280 px — alta definição"), 1280);
        resolution->addItem(QStringLiteral("1920 px — 1080p"), 1920);
        resolution->addItem(QStringLiteral("Nativa — resolução do monitor"), 0);
        resolution->setCurrentIndex(resolution->findData(shareMaxWidth_));
        auto* bitrate = new QComboBox(&settings);
        bitrate->addItem(QStringLiteral("4 Mbps — internet"), 4);
        bitrate->addItem(QStringLiteral("8 Mbps — recomendada (LAN)"), 8);
        bitrate->addItem(QStringLiteral("12 Mbps — alta qualidade"), 12);
        bitrate->addItem(QStringLiteral("20 Mbps — rede local rápida"), 20);
        bitrate->setCurrentIndex(bitrate->findData(shareBitrateMbps_));
        auto* systemAudio = new QCheckBox(QStringLiteral("Compartilhar o som do computador"), &settings);
        systemAudio->setChecked(shareSystemAudio_);
        form->addRow(QStringLiteral("Fluidez:"), fps);
        form->addRow(QStringLiteral("Qualidade:"), quality);
        form->addRow(QStringLiteral("Resolução máxima:"), resolution);
        form->addRow(QStringLiteral("Banda do vídeo:"), bitrate);
        form->addRow(QString(), systemAudio);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &settings);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &settings, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &settings, &QDialog::reject);
        if (settings.exec() != QDialog::Accepted) {
            const QSignalBlocker blocker(shareButton_);
            shareButton_->setChecked(false);
            return;
        }
        shareFps_ = fps->currentData().toInt();
        shareJpegQuality_ = quality->currentData().toInt();
        shareMaxWidth_ = resolution->currentData().toInt();
        shareBitrateMbps_ = bitrate->currentData().toInt();
        shareSystemAudio_ = systemAudio->isChecked();
        controller_->setVideoBitrate(shareBitrateMbps_ * 1000000.0);
        shareEncodeScale_ = 1.0;
        shareFrameBytes_ = 0;

        const auto screens = QGuiApplication::screens();
        if (screens.empty()) {
            const QSignalBlocker blocker(shareButton_);
            shareButton_->setChecked(false);
            return;
        }
        QScreen* screen = screens.front();
        if (screens.size() > 1) {
            QStringList choices;
            for (int index = 0; index < screens.size(); ++index) {
                const QSize screenSize = screens.at(index)->size();
                choices.append(QStringLiteral("Tela %1 — %2 (%3×%4)")
                    .arg(index + 1).arg(screens.at(index)->name()).arg(screenSize.width()).arg(screenSize.height()));
            }
            bool accepted = false;
            const QString selected = QInputDialog::getItem(this, QStringLiteral("Compartilhar tela"),
                QStringLiteral("Escolha a tela:"), choices, 0, false, &accepted);
            if (!accepted) {
                const QSignalBlocker blocker(shareButton_);
                shareButton_->setChecked(false);
                return;
            }
            screen = screens.at(choices.indexOf(selected));
        }
        if (!screen) {
            appendLog(QStringLiteral("Nenhuma tela disponível para compartilhar."));
            const QSignalBlocker blocker(shareButton_);
            shareButton_->setChecked(false);
            return;
        }
        const QSize size = screen->size();
        QString audioError;
        if (!controller_->setSystemAudioEnabled(shareSystemAudio_, audioError)) {
            appendLog(QStringLiteral("Não foi possível capturar o som do computador: ") + audioError);
            const QSignalBlocker blocker(shareButton_);
            shareButton_->setChecked(false);
            return;
        }
        if (!controller_->setSharing(true, static_cast<quint16>(size.width()), static_cast<quint16>(size.height()))) {
            QString ignored;
            controller_->setSystemAudioEnabled(false, ignored);
            appendLog(QStringLiteral("Não foi possível anunciar o compartilhamento."));
            const QSignalBlocker blocker(shareButton_);
            shareButton_->setChecked(false);
            return;
        }
        sharedScreen_ = screen;
        sharing_ = true;
        forceKeyframe_ = true;
        shareTimer_->start(1000 / shareFps_);
        appendLog(QStringLiteral("Compartilhando tela em %1×%2.")
                      .arg(size.width())
                      .arg(size.height()));
    } else {
        shareTimer_->stop();
        sharedScreen_ = nullptr;
        QString ignored;
        controller_->setSystemAudioEnabled(false, ignored);
        controller_->setSharing(false, 0, 0);
        sharing_ = false;
        forceKeyframe_ = false;
        appendLog(QStringLiteral("Compartilhamento encerrado."));
    }
}

void MainWindow::captureScreenFrame() {
    if (!sharing_ || !sharedScreen_ || !controller_->isRunning()) return;
    QImage image = sharedScreen_->grabWindow(0).toImage();
    if (image.isNull()) return;
    if (shareMaxWidth_ > 0 && image.width() > shareMaxWidth_) {
        image = image.scaledToWidth(shareMaxWidth_, Qt::SmoothTransformation);
    }

    const auto fps = shareFps_ > 0 ? shareFps_ : 10;
    const bool keyframe = forceKeyframe_;
    const auto bitrate = (std::max)(500000.0, shareBitrateMbps_ * 1000000.0);
    // The budget follows the configured bitrate exactly (plus a tiny margin)
    // so the pacer never has more data queued than it can actually send. A
    // global floor used to allow 64 KB frames against a 4 Mbps channel, which
    // made the sender drop two thirds of the frames.
    const auto budget = static_cast<std::size_t>(bitrate / 8.0 / fps * (keyframe ? 2.0 : 1.05));
    const auto frameLimit = (std::min)((std::max)(budget, static_cast<std::size_t>(12 * 1024)),
                                       vibez::protocol::kMaxVideoFrameBytes);

    QByteArray jpeg;
    int quality = shareJpegQuality_;
    double scale = std::clamp(shareEncodeScale_, 0.30, 1.0);
    // Recover detail when the previous frame had plenty of room to grow.
    if (shareFrameBytes_ > 0 && shareFrameBytes_ < static_cast<qsizetype>(frameLimit * 2 / 3)) {
        scale = (std::min)(1.0, scale * 1.08);
    }

    qsizetype previousSize = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QImage candidate = image;
        if (scale < 0.999) {
            candidate = image.scaled((std::max)(1, static_cast<int>(image.width() * scale)),
                                     (std::max)(1, static_cast<int>(image.height() * scale)),
                                     Qt::KeepAspectRatio,
                                     attempt == 0 ? Qt::SmoothTransformation : Qt::FastTransformation);
        }
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        if (!candidate.save(&buffer, "JPG", quality)) return;
        jpeg = std::move(encoded);
        if (jpeg.size() <= static_cast<qsizetype>(frameLimit)) break;
        // Stop early instead of burning the frame interval when we are not
        // making progress.
        if (attempt > 0 && jpeg.size() >= previousSize) break;
        previousSize = jpeg.size();
        // Spend quality first, then resolution; the screen stays readable while
        // the bitrate stays bounded.
        const double ratio = static_cast<double>(frameLimit) / static_cast<double>(jpeg.size());
        if (quality > 35 && ratio > 0.7) {
            quality = (std::max)(35, quality - 10);
        } else {
            scale = std::clamp(scale * std::sqrt((std::max)(0.20, ratio)), 0.30, 1.0);
        }
    }
    shareEncodeScale_ = scale;
    if (jpeg.isEmpty() || jpeg.size() > static_cast<qsizetype>(frameLimit)) return;
    shareFrameBytes_ = jpeg.size();

    if (controller_->sendScreenFrame(jpeg, keyframe)) forceKeyframe_ = false;
}

void MainWindow::onVideoFrameReceived(const quint32 senderId, const QByteArray& jpeg) {
    QPixmap pixmap;
    if (!pixmap.loadFromData(jpeg, "JPG")) return;
    screenViewerTitle_->setText(QStringLiteral("Tela de participante #%1").arg(senderId));
    screenViewer_->setPixmap(pixmap.scaled(screenViewer_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void MainWindow::requestKeyframeFromSelection() {
    auto* item = userTable_->currentItem();
    if (!item) {
        appendLog(QStringLiteral("Selecione um participante para pedir um keyframe."));
        return;
    }
    auto* idItem = userTable_->item(item->row(), 1);
    const quint32 userId = idItem ? idItem->text().toUInt() : 0U;
    if (userId == 0 || userId == controller_->sessionId()) {
        appendLog(QStringLiteral("Selecione outro participante."));
        return;
    }
    if (!controller_->requestKeyframe(userId)) {
        appendLog(QStringLiteral("Não foi possível pedir o keyframe."));
    } else {
        appendLog(QStringLiteral("Keyframe solicitado a #%1.").arg(userId));
    }
}

void MainWindow::restartAudio() {
    if (!controller_->isRunning()) {
        appendLog(QStringLiteral("Conecte-se antes de reiniciar o áudio."));
        return;
    }
    QString error;
    if (controller_->restartAudio(error)) {
        appendLog(QStringLiteral("Áudio reiniciado."));
    } else {
        appendLog(QStringLiteral("Falha ao reiniciar o áudio: ") + error);
    }
    updateAudioLabel();
}

void MainWindow::rebuildUserTable() {
    userTable_->setRowCount(0);
    userRows_.clear();

    const auto& users = controller_->users();
    qInfo("VibeZ: participantes %d", static_cast<int>(users.size()));
    userTable_->setRowCount(static_cast<int>(users.size()));
    for (int row = 0; row < static_cast<int>(users.size()); ++row) {
        const auto& user = users[static_cast<std::size_t>(row)];
        UserRow widgets;
        widgets.name = new QTableWidgetItem;
        widgets.id = new QTableWidgetItem;
        widgets.channel = new QTableWidgetItem;
        widgets.status = new QTableWidgetItem;
        userTable_->setItem(row, 0, widgets.name);
        userTable_->setItem(row, 1, widgets.id);
        userTable_->setItem(row, 2, widgets.channel);
        userTable_->setItem(row, 3, widgets.status);

        widgets.level = new QProgressBar;
        widgets.level->setRange(0, 100);
        widgets.level->setValue(0);
        widgets.level->setTextVisible(false);
        widgets.level->setFixedHeight(10);
        auto* levelHolder = new QWidget;
        auto* levelLayout = new QHBoxLayout(levelHolder);
        levelLayout->setContentsMargins(6, 10, 6, 10);
        levelLayout->addWidget(widgets.level);
        userTable_->setCellWidget(row, 4, levelHolder);

        widgets.volume = new QSlider(Qt::Horizontal);
        widgets.volume->setRange(0, 100);
        widgets.volume->setValue(userVolumes_.value(user.id, 100));
        widgets.volume->setToolTip(QStringLiteral("Volume de %1: %2%").arg(QString::fromStdString(user.name)).arg(widgets.volume->value()));
        auto* volumeHolder = new QWidget;
        auto* volumeLayout = new QHBoxLayout(volumeHolder);
        volumeLayout->setContentsMargins(6, 6, 6, 6);
        volumeLayout->addWidget(widgets.volume);
        userTable_->setCellWidget(row, 5, volumeHolder);

        widgets.screenVolume = new QSlider(Qt::Horizontal);
        widgets.screenVolume->setRange(0, 150);
        widgets.screenVolume->setValue(userScreenVolumes_.value(user.id, 100));
        widgets.screenVolume->setEnabled(user.sharing);
        widgets.screenVolume->setToolTip(QStringLiteral("Volume do som da tela de %1: %2%")
                                             .arg(QString::fromStdString(user.name)).arg(widgets.screenVolume->value()));
        auto* screenVolumeHolder = new QWidget;
        auto* screenVolumeLayout = new QHBoxLayout(screenVolumeHolder);
        screenVolumeLayout->setContentsMargins(6, 6, 6, 6);
        screenVolumeLayout->addWidget(widgets.screenVolume);
        userTable_->setCellWidget(row, 6, screenVolumeHolder);

        widgets.mute = new QPushButton(QStringLiteral("Silenciar"));
        widgets.mute->setCheckable(true);
        widgets.mute->setChecked(userPlaybackMuted_.contains(user.id));
        widgets.mute->setText(widgets.mute->isChecked() ? QStringLiteral("Ouvir") : QStringLiteral("Silenciar"));
        auto* muteHolder = new QWidget;
        auto* muteLayout = new QHBoxLayout(muteHolder);
        muteLayout->setContentsMargins(6, 4, 6, 4);
        muteLayout->addWidget(widgets.mute);
        userTable_->setCellWidget(row, 7, muteHolder);

        const quint32 userId = user.id;
        QSlider* slider = widgets.volume;
        connect(slider, &QSlider::valueChanged, this, [this, userId, slider](const int value) {
            userVolumes_.insert(userId, value);
            controller_->setUserVolume(userId, static_cast<float>(value) / 100.0F);
            slider->setToolTip(QStringLiteral("Volume: %1%").arg(value));
        });
        QSlider* screenSlider = widgets.screenVolume;
        connect(screenSlider, &QSlider::valueChanged, this, [this, userId, screenSlider](const int value) {
            userScreenVolumes_.insert(userId, value);
            controller_->setUserScreenVolume(userId, static_cast<float>(value) / 100.0F);
            screenSlider->setToolTip(QStringLiteral("Volume da tela: %1%").arg(value));
        });
        QPushButton* mute = widgets.mute;
        connect(mute, &QPushButton::toggled, this, [this, userId, mute](const bool muted) {
            if (muted) userPlaybackMuted_.insert(userId);
            else userPlaybackMuted_.remove(userId);
            controller_->setUserMuted(userId, muted);
            mute->setText(muted ? QStringLiteral("Ouvir") : QStringLiteral("Silenciar"));
        });

        userRows_.insert(user.id, widgets);
    }
}

void MainWindow::updateUserRows() {
    const auto& users = controller_->users();
    bool needsRebuild = userRows_.size() != static_cast<int>(users.size());
    if (!needsRebuild) {
        for (const auto& user : users) {
            if (!userRows_.contains(user.id)) {
                needsRebuild = true;
                break;
            }
        }
    }
    if (needsRebuild) rebuildUserTable();

    const bool selfPresent = controller_->sessionId() != 0;
    for (int row = 0; row < static_cast<int>(users.size()); ++row) {
        const auto& user = users[static_cast<std::size_t>(row)];
        const auto it = userRows_.constFind(user.id);
        if (it == userRows_.constEnd()) continue;
        const UserRow& widgets = it.value();

        QString name = QString::fromStdString(user.name);
        if (selfPresent && user.id == controller_->sessionId()) name += QStringLiteral(" (você)");
        widgets.name->setText(name);
        widgets.id->setText(QStringLiteral("#%1").arg(user.id));
        widgets.channel->setText(channelName(user.channel));

        QString status = QStringLiteral("—");
        QColor color(0xd0, 0xd4, 0xdc);
        if (user.muted && user.sharing) {
            status = QStringLiteral("Mudo, compartilhando %1×%2").arg(user.shareWidth).arg(user.shareHeight);
            color = QColor(0x3f, 0x9d, 0x74);
        } else if (user.muted) {
            status = QStringLiteral("Mudo");
            color = QColor(0x8a, 0x92, 0xa2);
        } else if (user.sharing) {
            status = QStringLiteral("Compartilhando %1×%2").arg(user.shareWidth).arg(user.shareHeight);
            color = QColor(0x3f, 0x9d, 0x74);
        }
        widgets.status->setText(status);
        widgets.status->setForeground(QBrush(color));

        // The screen-audio slider only makes sense while this participant is
        // actually sharing their computer sound.
        const bool canControlScreen = user.sharing && user.id != controller_->sessionId();
        widgets.screenVolume->setEnabled(canControlScreen);
        widgets.screenVolume->setToolTip(canControlScreen
            ? QStringLiteral("Volume do som da tela de %1: %2%")
                  .arg(QString::fromStdString(user.name))
                  .arg(widgets.screenVolume->value())
            : QStringLiteral("Disponível quando %1 compartilhar o som da tela.")
                  .arg(QString::fromStdString(user.name)));
    }

    if (controller_->sessionId() != 0) {
        const auto it = std::find_if(users.begin(), users.end(), [this](const auto& user) {
            return user.id == controller_->sessionId();
        });
        currentChannel_->setText(QStringLiteral("Canal atual: ") +
                                 (it != users.end() ? channelName(it->channel) : QStringLiteral("—")));
    } else {
        currentChannel_->setText(QStringLiteral("Canal atual: —"));
    }
}

void MainWindow::updateLevels() {
    if (!controller_->isRunning()) {
        localLevel_->setValue(0);
        return;
    }
    localLevel_->setValue(std::clamp(static_cast<int>(controller_->localLevel() * 100.0F), 0, 100));
    for (auto it = userRows_.constBegin(); it != userRows_.constEnd(); ++it) {
        it.value().level->setValue(std::clamp(static_cast<int>(controller_->userLevel(it.key()) * 100.0F), 0, 100));
    }
    updateStatusBar();
}

void MainWindow::updateAudioLabel() {
    if (!controller_->isRunning()) {
        audioLabel_->setText(QStringLiteral("Áudio: —"));
        connectAudio_->setText(QString());
        return;
    }
    if (controller_->audioRunning()) {
        const QString text = QStringLiteral("Áudio: ativo (48 kHz mono, Opus)");
        audioLabel_->setText(text);
        connectAudio_->setText(text);
        lastAudioError_.clear();
    } else {
        const QString error = controller_->audioError();
        const QString text = QStringLiteral("Áudio indisponível: ") + error;
        audioLabel_->setText(text);
        connectAudio_->setText(text);
        if (!error.isEmpty() && error != lastAudioError_) {
            lastAudioError_ = error;
            appendLog(QStringLiteral("Áudio indisponível: ") + error);
        }
    }
}

void MainWindow::updateStatusBar() {
    QString message = StateText(controller_->state());
    if (controller_->sessionId() != 0) message += QStringLiteral("   •   você é #%1").arg(controller_->sessionId());
    if (controller_->isRunning()) message += QStringLiteral("   •   pacotes recebidos: %1").arg(controller_->receivedPackets());
    statusBar()->showMessage(message);
}

void MainWindow::showConnectPage() {
    pages_->setCurrentWidget(connectPage_);
    inRoom_ = false;
    connectButton_->setEnabled(true);
    connectButton_->setText(QStringLiteral("Conectar"));
    {
        const QSignalBlocker blocker(micButton_);
        micButton_->setChecked(true);
        micButton_->setText(QStringLiteral("Microfone ativo"));
        muted_ = false;
    }
    {
        const QSignalBlocker blocker(shareButton_);
        shareButton_->setChecked(false);
        sharing_ = false;
    }
    if (shareTimer_) shareTimer_->stop();
    sharedScreen_ = nullptr;
    forceKeyframe_ = false;
}

void MainWindow::showRoomPage() {
    pages_->setCurrentWidget(roomPage_);
    inRoom_ = true;
}

void MainWindow::clearRoom() {
    if (shareTimer_) shareTimer_->stop();
    sharedScreen_ = nullptr;
    forceKeyframe_ = false;
    channelList_->clear();
    userTable_->setRowCount(0);
    userRows_.clear();
    chatView_->clear();
    chatInput_->clear();
    currentChannel_->setText(QStringLiteral("Canal atual: —"));
    localLevel_->setValue(0);
    screenViewerTitle_->setText(QStringLiteral("Compartilhamento de tela"));
    screenViewer_->setPixmap({});
    screenViewer_->setText(QStringLiteral("Nenhuma tela sendo recebida."));
}

void MainWindow::appendLog(const QString& message) {
    logView_->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message));
}

QString MainWindow::channelName(const quint32 channelId) const {
    for (const auto& channel : controller_->channels()) {
        if (channel.id == channelId) return QString::fromStdString(channel.name);
    }
    return QStringLiteral("#%1").arg(channelId);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    controller_->stop();
    QMainWindow::closeEvent(event);
}
