#pragma once

#include "client/core/ClientSession.hpp"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <vector>

// Qt adapter around ClientSession. Networking and audio stay on their worker
// threads; this object only polls the event queue from the GUI thread and
// exposes plain Qt signals to the widgets.
class SessionController : public QObject {
    Q_OBJECT

public:
    explicit SessionController(QObject* parent = nullptr);
    ~SessionController() override;

    bool start(const QString& host, quint16 port, const QString& nickname, QString& error);
    void stop();
    [[nodiscard]] bool isRunning() const;

    bool createChannel(const QString& name);
    bool joinChannel(quint32 channelId);
    bool leaveChannel();
    bool setMuted(bool muted);
    bool setSharing(bool sharing, quint16 width, quint16 height);
    bool setSystemAudioEnabled(bool enabled, QString& error);
    [[nodiscard]] bool systemAudioEnabled() const;
    bool sendScreenFrame(const QByteArray& jpeg, bool keyframe = false);
    void setVideoBitrate(double bitrateBps);
    bool requestKeyframe(quint32 targetId);
    bool sendChat(const QString& text);
    void setUserVolume(quint32 userId, float volume);
    void setUserMuted(quint32 userId, bool muted);
    void setUserScreenVolume(quint32 userId, float volume);
    bool restartAudio(QString& error);

    [[nodiscard]] vibez::client::ConnectionState state() const;
    [[nodiscard]] quint32 sessionId() const;
    [[nodiscard]] const std::vector<vibez::client::ChannelInfo>& channels() const;
    [[nodiscard]] const std::vector<vibez::client::UserInfo>& users() const;
    [[nodiscard]] bool audioRunning() const;
    [[nodiscard]] QString audioError() const;
    [[nodiscard]] float localLevel() const;
    [[nodiscard]] float userLevel(quint32 userId) const;
    [[nodiscard]] float userScreenLevel(quint32 userId) const;
    [[nodiscard]] quint64 receivedPackets() const;
    [[nodiscard]] quint64 receivedSystemPackets() const;

signals:
    void connectionStateChanged();
    void sessionChanged();
    void channelsChanged();
    void usersChanged();
    void errorRaised(const QString& message);
    void keyframeRequested(quint32 requesterId);
    void videoFrameReceived(quint32 senderId, const QByteArray& jpeg);
    void chatMessageReceived(quint32 senderId, const QString& senderName, const QString& text);
    void audioStateChanged();

private slots:
    void pollEvents();

private:
    std::unique_ptr<vibez::client::ClientSession> session_;
    QTimer pollTimer_;
    std::uint32_t sessionId_{};
    std::vector<vibez::client::ChannelInfo> channels_;
    std::vector<vibez::client::UserInfo> users_;
    double videoBitrateBps_{vibez::client::kVideoTargetBitrateBps};
    bool running_{};
};
