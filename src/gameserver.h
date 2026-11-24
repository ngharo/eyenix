#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <vector>

#include "eyenix/ttp.hpp"

class QTcpSocket;

// Stands in for Tobii's background service so that games built with Tobii's
// game integration library find a tracker. The library connects to
// 127.0.0.1:4455, asks about the device and its display, then subscribes to
// gaze and presence.
//
// Everything a client sends is untrusted: sizes are capped and unknown
// requests get an empty answer.
class GameServer : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 4455;

    explicit GameServer(QObject *parent = nullptr);
    ~GameServer() override;

    // Listens on the loopback interface only. Port 0 picks a free one.
    bool listen(quint16 port = kDefaultPort);
    void close();
    quint16 port() const { return m_server.serverPort(); }
    QString errorString() const { return m_server.errorString(); }

    // The answer to give for a request, replacing the built-in one.
    void setReply(quint32 op, const QByteArray &payload);
    // The screen the tracker is on, in pixels. Games refuse a tracker whose
    // screen does not contain their window.
    void setDisplaySize(int width, int height);

    // Gaze as fractions of the screen, origin top-left.
    void publishGaze(double x, double y, bool valid);
    void publishPresence(bool present);

    int clientCount() const { return m_clients.size(); }
    // Clients that asked for gaze or presence, i.e. games actually using the tracker.
    int subscriberCount() const;

    // The pieces of a gaze / presence event, exposed for tests.
    static eyenix::ttp::Bytes gazeRow(quint32 counter, qint64 timestampUs, double x, double y, bool valid);
    static eyenix::ttp::Bytes presenceRow(qint64 timestampUs, bool present);

signals:
    void clientsChanged();
    // A game sent a Tobii licence. Licences are not implemented; some features may stay off.
    void licenceOffered();

private:
    struct Client {
        QByteArray pending;
        QSet<quint32> streams;
        quint32 eventSeq = 1;
        bool licenceSeen = false;
    };

    void accept();
    void read(QTcpSocket *socket);
    void drop(QTcpSocket *socket);
    void handle(QTcpSocket *socket, Client &client, quint32 seq, quint32 op, const QByteArray &payload);
    void send(QTcpSocket *socket, quint32 magic, quint32 seq, quint32 op, quint32 status, const QByteArray &payload);
    void sendEvent(QTcpSocket *socket, Client &client, quint32 stream, const eyenix::ttp::Bytes &row);
    qint64 elapsedUs() const { return m_clock.nsecsElapsed() / 1000; }

    QTcpServer m_server;
    QHash<QTcpSocket *, Client> m_clients;
    QHash<quint32, QByteArray> m_replies;
    QElapsedTimer m_clock;
    quint32 m_gazeCounter = 0;
};
