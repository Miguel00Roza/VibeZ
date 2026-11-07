#include "client/ui/SessionController.hpp"

#include <span>
#include <utility>

SessionController::SessionController(QObject* parent) : QObject(parent) {
    pollTimer_.setInterval(20);
    connect(&pollTimer_, &QTimer::timeout, this, &SessionController::pollEvents);
}

SessionController::~SessionController() { stop(); }

bool SessionController::start(const QString& host, const quint16 port, const QString& nickname, QString& error) {
    stop();

    vibez::client::ClientSessionConfig config;
    config.network.host = host.toStdString();
    config.network.port = port;
    config.network.nickname = nickname.toStdString();
    config.network.token.clear();
    config.network.autoReconnect = true;
    config.enableAudio = true;

    auto session = std::make_unique<vibez::client::ClientSession>();
    std::string startError;
    if (!session->Start(config, startError)) {
        error = QString::fromStdString(startError);
        return false;
    }

    session->SetVideoFrameCallback([this](const vibez::client::VideoFrame& frame) {
        emit videoFrameReceived(frame.sessionId,
                                QByteArray(reinterpret_cast<const char*>(frame.jpeg.data()),
                                           static_cast<qsizetype>(frame.jpeg.size())));
    });

    session_ = std::move(session);
    running_ = true;
    pollTimer_.start();
    emit connectionStateChanged();
    emit audioStateChanged();
    return true;
}

void SessionController::stop() {
    pollTimer_.stop();
    running_ = false;
    if (session_) {
        session_->SetVideoFrameCallback({});
        session_->Stop();
        session_.reset();
    }
    sessionId_ = 0;
    channels_.clear();
    users_.clear();
    emit connectionStateChanged();
    emit sessionChanged();
    emit channelsChanged();
    emit usersChanged();
    emit audioStateChanged();
}

bool SessionController::isRunning() const { return running_; }

bool SessionController::createChannel(const QString& name) {
    return session_ && session_->CreateChannel(name.toStdString());
}

bool SessionController::joinChannel(const quint32 channelId) {
    return session_ && session_->JoinChannel(channelId);
}

bool SessionController::leaveChannel() {
    return session_ && session_->LeaveChannel();
}

bool SessionController::setMuted(const bool muted) {
    return session_ && session_->SetMicrophoneMuted(muted);
}

bool SessionController::setSharing(const bool sharing, const quint16 width, const quint16 height) {
    return session_ && session_->SetSharing(sharing, width, height);
}

bool SessionController::setSystemAudioEnabled(const bool enabled, QString& error) {
    if (!session_) {
        error = QStringLiteral("Sessão não iniciada");
        return false;
    }
    std::string nativeError;
    const bool result = session_->SetSystemAudioEnabled(enabled, nativeError);
    error = QString::fromStdString(nativeError);
    return result;
}

bool SessionController::systemAudioEnabled() const { return session_ && session_->SystemAudioEnabled(); }

bool SessionController::sendScreenFrame(const QByteArray& jpeg) {
    if (!session_ || jpeg.isEmpty()) return false;
    return session_->SendVideoFrame(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(jpeg.constData()),
                                      static_cast<std::size_t>(jpeg.size())));
}

bool SessionController::requestKeyframe(const quint32 targetId) {
    return session_ && session_->RequestKeyframe(targetId);
}

bool SessionController::sendChat(const QString& text) {
    return session_ && session_->SendChat(text.toStdString());
}

void SessionController::setUserVolume(const quint32 userId, const float volume) {
    if (session_) session_->SetUserVolume(userId, volume);
}

void SessionController::setUserMuted(const quint32 userId, const bool muted) {
    if (session_) session_->SetUserMuted(userId, muted);
}

vibez::client::ConnectionState SessionController::state() const {
    return session_ ? session_->State() : vibez::client::ConnectionState::Disconnected;
}

quint32 SessionController::sessionId() const { return sessionId_; }
const std::vector<vibez::client::ChannelInfo>& SessionController::channels() const { return channels_; }
const std::vector<vibez::client::UserInfo>& SessionController::users() const { return users_; }
bool SessionController::audioRunning() const { return session_ && session_->AudioRunning(); }
QString SessionController::audioError() const {
    return session_ ? QString::fromStdString(session_->AudioError()) : QStringLiteral("Sessão não iniciada");
}
float SessionController::localLevel() const { return session_ ? session_->LocalLevel() : 0.0F; }
float SessionController::userLevel(const quint32 userId) const { return session_ ? session_->UserLevel(userId) : 0.0F; }
quint64 SessionController::receivedPackets() const { return session_ ? session_->ReceivedPackets() : 0; }

void SessionController::pollEvents() {
    if (!session_) return;

    bool stateChanged = false;
    bool sessionDataChanged = false;
    bool channelDataChanged = false;
    bool userDataChanged = false;

    while (const auto event = session_->TryPopEvent()) {
        using vibez::client::NetworkEvent;
        switch (event->type) {
        case NetworkEvent::Type::StateChanged:
            stateChanged = true;
            if (event->state != vibez::client::ConnectionState::Connected && sessionId_ != 0) {
                sessionId_ = 0;
                sessionDataChanged = true;
            }
            break;
        case NetworkEvent::Type::HelloAccepted:
            sessionId_ = event->sessionId;
            sessionDataChanged = true;
            break;
        case NetworkEvent::Type::ChannelsUpdated:
            channels_ = event->channels;
            channelDataChanged = true;
            break;
        case NetworkEvent::Type::PresenceUpdated:
            users_ = event->users;
            userDataChanged = true;
            break;
        case NetworkEvent::Type::Error:
            emit errorRaised(QString::fromStdString(event->text));
            break;
        case NetworkEvent::Type::KeyframeRequest:
            emit keyframeRequested(event->sessionId);
            break;
        case NetworkEvent::Type::ChatMessage:
            emit chatMessageReceived(event->sessionId, QString::fromStdString(event->senderName),
                                     QString::fromStdString(event->text));
            break;
        }
    }

    if (stateChanged) emit connectionStateChanged();
    if (sessionDataChanged) emit sessionChanged();
    if (channelDataChanged) emit channelsChanged();
    if (userDataChanged) emit usersChanged();
}
