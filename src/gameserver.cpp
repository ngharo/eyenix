// The protocol behaviour here follows tobii-faithful v1.1.8 (src/middleware/server.cpp,
// https://github.com/SciFi-Bob/tobii-faithful), Copyright its authors, Apache License 2.0
// (licenses/Apache-2.0.txt). Rewritten for Qt and Eyenix's own wire-format code.
#include "gameserver.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>

#include "gamereplies.h"

using namespace eyenix;

namespace {

constexpr int kHeaderSize = 24;                 // six big-endian u32: magic, seq, 1, op, status, length
constexpr quint32 kMaxRequestBytes = 1 << 20;   // far above anything a game sends
constexpr qint64 kMaxBacklogBytes = 64 * 1024;  // a client this far behind is dropped
constexpr int kMaxClients = 16;

constexpr quint32 kOpSubscribe = ttp::Subscribe, kOpUnsubscribe = ttp::Unsubscribe;
constexpr quint32 kOpDisplayArea = ttp::GetDisplayArea;
constexpr quint32 kOpPresenceQuery = 0x06a4;    // asked once on connect; answered with a presence event

// Per-eye values games receive. Only the combined gaze point is real.
constexpr double kEyeOffset = 0.01, kEyeSeparationMm = 30.0, kEyeDistanceMm = 635.0, kPupilMm = 4.1;

double unit(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.5; }

ttp::Writer &column(ttp::Writer &w, quint32 id) { return w.structHeader(2, ttp::kColumn).u32(id); }

ttp::Writer &be(ttp::Writer &w, std::uint8_t type, quint64 value, int width) {
    std::uint8_t bytes[8];
    for (int i = 0; i < width; ++i)
        bytes[i] = std::uint8_t(value >> (8 * (width - 1 - i)));
    return w.item(type, {bytes, std::size_t(width)});
}

ttp::Writer &point2(ttp::Writer &w, double x, double y) { return w.structHeader(2, ttp::kPoint2).fixed(x).fixed(y); }

// The stream id is the first small u32 in a subscribe / unsubscribe request.
quint32 streamId(const QByteArray &payload) {
    const auto span = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.constData()),
                                                    std::size_t(payload.size()));
    for (const auto &item : ttp::parseItems(span)) {
        if (item.type == ttp::U32 && item.value.size() == 4) {
            const auto value = item.asUint();
            if (value != 0 && value < 0x10000)
                return quint32(value);
        }
    }
    return 0;
}

// Tobii licences carry this marker, as ASCII or UTF-16.
bool containsLicence(const QByteArray &data) {
    static const QByteArray ascii("--SE-SIGNATURE--");
    static const QByteArray wide = [] {
        QByteArray w;
        for (char c : ascii) {
            w.append(c);
            w.append('\0');
        }
        return w;
    }();
    return data.contains(ascii) || data.contains(wide);
}

} // namespace

GameServer::GameServer(QObject *parent) : QObject(parent) {
    m_clock.start();
    connect(&m_server, &QTcpServer::newConnection, this, &GameServer::accept);
}

GameServer::~GameServer() { close(); }

bool GameServer::listen(quint16 port) { return m_server.listen(QHostAddress::LocalHost, port); }

void GameServer::close() {
    m_server.close();
    const auto sockets = m_clients.keys();
    for (QTcpSocket *socket : sockets)
        drop(socket);
}

void GameServer::setReply(quint32 op, const QByteArray &payload) { m_replies.insert(op, payload); }

void GameServer::setDisplaySize(int width, int height) {
    // The service reports the screen as a flat rectangle, bottom edge centred on the tracker.
    const double half = width / 2.0;
    ttp::Writer w;
    w.point3(-half, height, 0).point3(half, height, 0).point3(-half, 0, 0).structHeader(1, 0x0100).u32(0x3039);
    setReply(kOpDisplayArea, QByteArray(reinterpret_cast<const char *>(w.bytes().data()), qsizetype(w.bytes().size())));
}

int GameServer::subscriberCount() const {
    return int(std::count_if(m_clients.begin(), m_clients.end(), [](const Client &c) { return !c.streams.isEmpty(); }));
}

ttp::Bytes GameServer::gazeRow(quint32 counter, qint64 timestampUs, double x, double y, bool valid) {
    x = unit(x);
    y = unit(y);
    const quint32 flag = valid ? 1 : 0;
    ttp::Writer w;
    w.structHeader(19, ttp::kSample);
    be(column(w, 0x01), ttp::U64, quint64(timestampUs), 8);
    column(w, 0x02).point3(-kEyeSeparationMm, 0, kEyeDistanceMm);
    point2(column(w, 0x05), unit(x - kEyeOffset), y);
    be(column(w, 0x06), ttp::Fixed16, quint64(std::llround(kPupilMm * 65536)), 4);
    column(w, 0x07).u32(0);
    column(w, 0x08).point3(kEyeSeparationMm, 0, kEyeDistanceMm);
    point2(column(w, 0x0b), unit(x + kEyeOffset), y);
    be(column(w, 0x0c), ttp::Fixed16, quint64(std::llround(kPupilMm * 65536)), 4);
    column(w, 0x0d).u32(0);
    column(w, 0x14).u32(counter);
    column(w, 0x15).u32(flag);
    column(w, 0x16).u32(flag);
    column(w, 0x1b).u32(flag);
    point2(column(w, 0x1c), x, y);
    column(w, 0x1d).u32(flag);
    column(w, 0x1e).u32(flag);
    column(w, 0x1f).u32(flag);
    point2(column(w, 0x20), x, y);
    column(w, 0x21).u32(flag);
    return w.bytes();
}

ttp::Bytes GameServer::presenceRow(qint64 timestampUs, bool present) {
    ttp::Writer w;
    w.structHeader(2, ttp::kSample);
    be(column(w, 0x01), ttp::U64, quint64(timestampUs), 8);
    be(column(w, 0x02), 0x01, present ? 1 : 0, 4);
    return w.bytes();
}

void GameServer::publishGaze(double x, double y, bool valid) {
    if (m_clients.isEmpty())
        return;
    const auto row = gazeRow(m_gazeCounter++, elapsedUs(), x, y, valid);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        if (it->streams.contains(ttp::GazeStream))
            sendEvent(it.key(), *it, ttp::GazeStream, row);
}

void GameServer::publishPresence(bool present) {
    if (m_clients.isEmpty())
        return;
    const auto row = presenceRow(elapsedUs(), present);
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        if (it->streams.contains(ttp::PresenceStream))
            sendEvent(it.key(), *it, ttp::PresenceStream, row);
}

void GameServer::accept() {
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        if (m_clients.size() >= kMaxClients) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_clients.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { drop(socket); });
        emit clientsChanged();
    }
}

void GameServer::drop(QTcpSocket *socket) {
    if (!m_clients.remove(socket))
        return;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    emit clientsChanged();
}

void GameServer::read(QTcpSocket *socket) {
    auto it = m_clients.find(socket);
    if (it == m_clients.end())
        return;
    it->pending += socket->readAll();
    if (!it->licenceSeen && containsLicence(it->pending)) {
        it->licenceSeen = true;
        emit licenceOffered();
    }

    while (it->pending.size() >= kHeaderSize) {
        const auto *raw = reinterpret_cast<const uchar *>(it->pending.constData());
        const quint32 magic = qFromBigEndian<quint32>(raw), seq = qFromBigEndian<quint32>(raw + 4);
        const quint32 op = qFromBigEndian<quint32>(raw + 12), length = qFromBigEndian<quint32>(raw + 20);
        if (length > kMaxRequestBytes) { // not our protocol, or hostile
            drop(socket);
            return;
        }
        if (it->pending.size() < kHeaderSize + qsizetype(length))
            break;
        const QByteArray payload = it->pending.mid(kHeaderSize, length);
        it->pending.remove(0, kHeaderSize + length);
        if (magic == ttp::Request)
            handle(socket, *it, seq, op, payload);
        it = m_clients.find(socket); // handle() may have dropped it
        if (it == m_clients.end())
            return;
    }
}

void GameServer::handle(QTcpSocket *socket, Client &client, quint32 seq, quint32 op, const QByteArray &payload) {
    const bool wasSubscribed = !client.streams.isEmpty();
    bool presenceNow = op == kOpPresenceQuery;
    if (op == kOpSubscribe) {
        if (const quint32 stream = streamId(payload)) {
            if (client.streams.size() < 32)
                client.streams.insert(stream);
            presenceNow |= stream == ttp::PresenceStream;
        }
    } else if (op == kOpUnsubscribe) {
        client.streams.remove(streamId(payload));
    }

    QByteArray reply;
    quint32 status = 0;
    if (const auto custom = m_replies.constFind(op); custom != m_replies.constEnd()) {
        reply = *custom;
    } else {
        for (const auto &known : gamereplies::kMiddlewareReplies) {
            if (known.object == op) {
                reply = QByteArray(reinterpret_cast<const char *>(known.payload), qsizetype(known.size));
                status = known.status;
                break;
            }
        }
    }
    send(socket, ttp::Response, seq, op, status, reply);

    if (presenceNow) {
        // The library waits for a first presence event before it treats the tracker as live.
        QTimer::singleShot(30, socket, [this, socket] {
            if (auto it = m_clients.find(socket); it != m_clients.end())
                sendEvent(socket, *it, ttp::PresenceStream, presenceRow(elapsedUs(), true));
        });
    }
    if (wasSubscribed != !client.streams.isEmpty())
        emit clientsChanged();
}

void GameServer::send(QTcpSocket *socket, quint32 magic, quint32 seq, quint32 op, quint32 status, const QByteArray &payload) {
    if (socket->bytesToWrite() + payload.size() > kMaxBacklogBytes) {
        // Not reading what we send. Drop it once we are back in the event loop.
        QTimer::singleShot(0, socket, [this, socket] { drop(socket); });
        return;
    }
    uchar header[kHeaderSize];
    const quint32 fields[6] = {magic, seq, 1, op, status, quint32(payload.size())};
    for (int i = 0; i < 6; ++i)
        qToBigEndian(fields[i], header + 4 * i);
    socket->write(reinterpret_cast<const char *>(header), kHeaderSize);
    socket->write(payload);
}

void GameServer::sendEvent(QTcpSocket *socket, Client &client, quint32 stream, const ttp::Bytes &row) {
    send(socket, ttp::Stream, client.eventSeq++, stream, 0,
         QByteArray::fromRawData(reinterpret_cast<const char *>(row.data()), qsizetype(row.size())));
}
