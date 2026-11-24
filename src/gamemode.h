#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>
#include <functional>

#include "backend.h"
#include "gameserver.h"
#include "wineprefix.h"

// Game mode: makes games that support Tobii eye trackers find one.
//
// While it is on, Eyenix answers on the port Tobii's game library looks for
// (gaze), and watches for a game that ships that library starting under Wine
// or Proton. When one does, a small helper (eyenix-bridge.exe) is started
// inside the game's Wine prefix to hand it head pose. Nothing in the game or
// its files is changed.
//
// The tracker is only asked for while such a game is running.
class GameMode : public QObject {
    Q_OBJECT

public:
    // Head pose travels to the bridge on this UDP port, loopback only.
    static constexpr quint16 kPosePort = 4243;

    // How running prefixes are found; tests supply their own.
    using Scanner = std::function<QList<wineprefix::Running>()>;

    explicit GameMode(Backend *backend, QObject *parent = nullptr, quint16 gazePort = GameServer::kDefaultPort,
                      Scanner scanner = wineprefix::running);
    ~GameMode() override;

    bool enabled() const { return m_enabled; }
    // Returns why it could not be turned on, or an empty string.
    QString setEnabled(bool enabled);

    // A game that can use the tracker is running, or something is connected
    // for gaze, so the tracker is needed.
    bool active() const { return m_active; }

    // For the bar plugin: enabled, state ("off", "waiting", "starting",
    // "serving"), game, head and gaze (whether the game is taking each), problem.
    QJsonObject status() const;

    // Looks for games now instead of at the next tick.
    void scan();

    // The UDP packet for a head pose: see bridge/sesp.go.
    static QByteArray posePacket(const eyenix::HeadPose &pose, bool found);

signals:
    void changed();
    void activeChanged();

private:
    void stopBridge();
    void startBridge(const wineprefix::Running &prefix);
    void readBridge();
    void applyDescriptions();
    void applyDisplay();
    void setProblem(const QString &problem);
    void updateActive();

    Backend *m_backend;
    Scanner m_scanner;
    quint16 m_gazePort;
    GameServer m_server;
    QUdpSocket m_udp;
    QTimer m_scanTimer, m_heartbeat;

    bool m_enabled = false, m_active = false;
    QString m_problem;

    QString m_game;        // name of the game being served, empty if none
    QString m_prefix;      // its Wine prefix
    QProcess *m_bridge = nullptr;
    bool m_bridgeReady = false, m_headTaken = false, m_licence = false;
    int m_failures = 0;
    QElapsedTimer m_retryAfter; // running while waiting to retry a bridge that keeps dying

    QHash<QString, QString> m_libraries; // prefix -> Tobii library path ("" if none); looked up once
    bool m_facePresent = false, m_eyesPresent = false, m_presenceSent = false;
    QElapsedTimer m_presenceAge;
};
