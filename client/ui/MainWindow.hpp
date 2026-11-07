#pragma once

#include <QHash>
#include <QByteArray>
#include <QMainWindow>
#include <QSet>

class SessionController;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
class QScreen;

// One row of the participant table. Widgets are owned by the table.
struct UserRow {
    QTableWidgetItem* name{};
    QTableWidgetItem* id{};
    QTableWidgetItem* channel{};
    QTableWidgetItem* status{};
    QProgressBar* level{};
    QSlider* volume{};
    QPushButton* mute{};
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void setConnectionFields(const QString& host, quint16 port, const QString& nickname);
    void setAutoJoinChannel(quint32 channelId);

public slots:
    void connectToServer();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    QWidget* buildConnectPage();
    QWidget* buildRoomPage();

    void disconnectFromServer();
    void onConnectionStateChanged();
    void onSessionChanged();
    void onChannelsChanged();
    void onUsersChanged();
    void onJoinSelectedChannel();
    void onCreateChannel();
    void sendChat();
    void toggleMicrophone(bool active);
    void toggleSharing(bool sharing);
    void requestKeyframeFromSelection();
    void captureScreenFrame();
    void onVideoFrameReceived(quint32 senderId, const QByteArray& jpeg);

    void rebuildUserTable();
    void updateUserRows();
    void updateLevels();
    void updateAudioLabel();
    void updateStatusBar();
    void showConnectPage();
    void showRoomPage();
    void clearRoom();
    void appendLog(const QString& message);
    [[nodiscard]] QString channelName(quint32 channelId) const;

    SessionController* controller_{};
    QStackedWidget* pages_{};
    QWidget* connectPage_{};
    QWidget* roomPage_{};

    QLineEdit* hostEdit_{};
    QSpinBox* portSpin_{};
    QLineEdit* nicknameEdit_{};
    QPushButton* connectButton_{};
    QLabel* connectStatus_{};
    QLabel* connectAudio_{};

    QLabel* currentChannel_{};
    QListWidget* channelList_{};
    QLineEdit* newChannelEdit_{};
    QPushButton* joinButton_{};
    QPushButton* leaveButton_{};
    QPushButton* micButton_{};
    QPushButton* shareButton_{};
    QPushButton* keyframeButton_{};
    QPushButton* disconnectButton_{};
    QLabel* screenViewerTitle_{};
    QLabel* screenViewer_{};
    QProgressBar* localLevel_{};
    QTableWidget* userTable_{};
    QPlainTextEdit* logView_{};
    QPlainTextEdit* chatView_{};
    QLineEdit* chatInput_{};
    QLabel* audioLabel_{};

    QHash<quint32, UserRow> userRows_;
    QHash<quint32, int> userVolumes_;
    QSet<quint32> userPlaybackMuted_;
    quint32 autoJoinChannel_{};
    bool inRoom_{};
    bool muted_{};
    bool sharing_{};
    int shareFps_{10};
    int shareJpegQuality_{55};
    int shareMaxWidth_{960};
    bool shareSystemAudio_{true};
    QScreen* sharedScreen_{};
    QTimer* shareTimer_{};
};
