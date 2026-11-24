#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMutex>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <memory>

#include "backend.h"
#include "control.h"
#include "session.h"
#include "store.h"
#include "gamemode.h"
#include "gameserver.h"
#include "wineprefix.h"
#include "gazefilter.h"
#include <QTcpSocket>
#include "models.h"
#include "eyenix/headpose.hpp"
#include "eyenix/vision/anchors.hpp"
#include "eyenix/vision/face_detector.hpp"
#include "eyenix/vision/pose.hpp"
#include "theme.h"
#include "translator.h"

using namespace eyenix;

namespace {

ttp::Bytes unhex(const char *text) {
    const QByteArray raw = QByteArray::fromHex(text);
    return ttp::Bytes(raw.begin(), raw.end());
}

// Set display area as Tobii's Windows software sent it (request 44 of captures/04).
const char kSetDisplayArea[] =
    "00000000bc000000000000510000002c00000000000005a000000000000000a4"
    "0000050000000400031f410400000008fffd76000000000004000000080003241988000000040000000800010951fe000000"
    "050000000400031f41040000000800028a000000000004000000080003241988000000040000000800010951fe000000"
    "050000000400031f410400000008fffd760000000000040000000800002911bf0000000400000008fffff39994500000"
    "050000000400010100020000000400003039";

// What a test can make the fake tracker do, and what the app asked of it.
struct FakeState {
    std::atomic<bool> present{true}, unplug{false}, looking{true};
    QMutex mutex;
    QStringList calls;
    QString refuse; // a call name to answer with an error

    void record(const QString &call) {
        {
            QMutexLocker lock(&mutex);
            calls << call;
        }
        const QString name = call.section(' ', 0, 0);
        QMutexLocker lock(&mutex);
        if (name == refuse)
            throw std::runtime_error("refused " + name.toStdString());
    }
    QStringList taken() {
        QMutexLocker lock(&mutex);
        return calls;
    }
    void forget() {
        QMutexLocker lock(&mutex);
        calls.clear();
    }
};

class FakeDevice : public Device {
public:
    explicit FakeDevice(std::shared_ptr<FakeState> state) : s(std::move(state)) {}

    QString name() override { return "Fake Tracker"; }
    void setDisplayArea(const DisplayArea &area) override {
        s->record(QString("setDisplayArea %1 %2").arg(area.topLeft.x, 0, 'f', 1).arg(area.topRight.x, 0, 'f', 1));
    }
    void startGaze() override { s->record("startGaze"); }
    std::optional<GazeSample> readGaze(int) override {
        if (s->unplug.exchange(false))
            throw Disconnected("unplugged", -4);
        QThread::msleep(2);
        GazeSample sample;
        sample.timestampUs = (m_time += 30000);
        sample.valid = s->looking;
        sample.gaze = {0.25, 0.75};
        sample.left.originValid = sample.right.originValid = s->looking;
        sample.left.originMm = {-30, 0, 600};
        sample.right.originMm = {30, 0, 620};
        return sample;
    }
    void calibrationStart() override { s->record("start"); }
    void calibrationClear() override { s->record("clear"); }
    void calibrationAddPoint(Point2 p) override { s->record(QString("add %1,%2").arg(p.x).arg(p.y)); }
    void calibrationCompute() override { s->record("compute"); }
    void calibrationStop() override { s->record("stop"); }
    QByteArray calibration() override {
        s->record("get");
        return "new calibration";
    }
    void setCalibration(const QByteArray &blob) override { s->record("setCalibration " + QString::fromUtf8(blob)); }

    QString serial() override { return "FAKE-0001"; }
    QHash<quint32, QByteArray> descriptions() override { return {{quint32(ttp::DeviceInfo), QByteArray("\0\0fake", 6)}}; }

private:
    std::shared_ptr<FakeState> s;
    quint64 m_time = 0;
};

DeviceFactory fakeFactory(std::shared_ptr<FakeState> state) {
    return [state]() -> std::unique_ptr<Device> {
        if (!state->present)
            throw UsbError("no tracker here", -5);
        return std::make_unique<FakeDevice>(state);
    };
}

}

class EyenixTests : public QObject {
    Q_OBJECT

private slots:
    // --- the wire format, against bytes captured from Tobii's software ---

    void encodesRequestsLikeTobii()
    {
        // Subscribe to gaze (request 47 of captures/04).
        const auto payload = ttp::Writer().u32(ttp::GazeStream).item(0x17, ttp::Bytes{0, 0, 0, 0}).bytes();
        const auto packets = ttp::encodeRequest(47, ttp::Subscribe, payload);
        QCOMPARE(packets.size(), size_t(1));
        QCOMPARE(packets[0], unhex("000000002c000000000000510000002f00000000000004c40000000000000014"
                                   "0000020000000400000500170000000400000000"));

        // Calibration point at the centre (request 63 of captures/05).
        QCOMPARE(ttp::Writer().fixed(0.5).fixed(0.5).u32(3).bytes(),
                 unhex("00000400000008000002000000000004000000080000020000000000020000000400000003"));

        QCOMPARE(ttp::Writer().blob(ttp::Bytes{0xaa, 0xbb}).bytes(), unhex("0000150000000600000002aabb"));
    }

    void splitsLargeRequestsIntoPackets()
    {
        // The 318 kB calibration upload of captures/05 went out as full 4095-byte packets.
        const ttp::Bytes blob(317933, 0x5a);
        const auto packets = ttp::encodeRequest(101, ttp::CalSet, ttp::Writer().blob(blob).bytes());
        QCOMPARE(packets.size(), size_t(78));
        size_t carried = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            const auto &p = packets[i];
            QVERIFY(i + 1 == packets.size() ? p.size() <= 4095 : p.size() == 4095);
            QCOMPARE(size_t(p[4] | (p[5] << 8)), p.size() - 8);
            carried += p.size() - 8;
        }
        QCOMPARE(carried, size_t(24 + 317944));
        const auto head = unhex("00000000f70f000000000051000000650000000000000456000000000004d9f80000150004d9f10004d9ed");
        QVERIFY(std::equal(head.begin(), head.end(), packets[0].begin()));
    }

    void displayAreaRoundTrips()
    {
        const auto captured = unhex(kSetDisplayArea);
        std::vector<double> v;
        for (const auto &item : ttp::parseItems({captured.data() + 32, captured.size() - 32}))
            if (item.type == ttp::Fixed42)
                v.push_back(item.asFixed());
        QCOMPARE(v.size(), size_t(9));
        QCOMPARE(v[0], -162.5);

        const auto again = ttp::Writer().point3(v[0], v[1], v[2]).point3(v[3], v[4], v[5]).point3(v[6], v[7], v[8])
                               .structHeader(1, 0x0100).u32(0x3039).bytes();
        const auto packets = ttp::encodeRequest(44, ttp::SetDisplayArea, again);
        QCOMPARE(packets.size(), size_t(1));
        QCOMPARE(packets[0], captured);
    }

    void alignmentGivesTheCornersTobiiSent()
    {
        // A 325 x 203 mm screen with the tracker centred, as in kSetDisplayArea.
        Alignment a;
        a.screenWidthPx = 1300;
        a.screenHeightPx = 812;
        a.leftMarkPx = 650 - 368;
        a.rightMarkPx = 650 + 368;
        QCOMPARE(mmPerPixel(a), 0.25);
        auto d = displayAreaFromAlignment(a);
        QVERIFY(qAbs(d.topLeft.x + 162.5) < 0.01 && qAbs(d.topLeft.y - 201.02) < 0.02 && qAbs(d.topLeft.z - 66.33) < 0.02);
        QVERIFY(qAbs(d.topRight.x - 162.5) < 0.01);
        QVERIFY(qAbs(d.bottomLeft.y - 10.27) < 0.02 && qAbs(d.bottomLeft.z + 3.10) < 0.02);

        // With the tracker right of centre, the screen sits further left of it.
        a.leftMarkPx += 100;
        a.rightMarkPx += 100;
        d = displayAreaFromAlignment(a);
        QVERIFY(qAbs(d.topLeft.x + 187.5) < 0.01 && qAbs(d.topRight.x - 137.5) < 0.01);

        a.rightMarkPx = a.leftMarkPx;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, displayAreaFromAlignment(a));
    }

    void reassemblesReplies()
    {
        const auto one = unhex("010000002b000000000000520000000100000001000003e8000000000000000b0000020000000400010008");
        const auto two = unhex("0100000020000000000000520000001100000001000007770000000000000000");
        ttp::Bytes all = one;
        all.insert(all.end(), two.begin(), two.end());

        ttp::FrameParser parser;
        parser.feed({all.data(), 5});
        QVERIFY(!parser.next());
        parser.feed({all.data() + 5, 50});
        auto m = parser.next();
        QVERIFY(m && m->magic == ttp::Response && m->seq == 1 && m->op == ttp::Hello && m->payload.size() == 11);
        QVERIFY(!parser.next());
        parser.feed({all.data() + 55, all.size() - 55});
        m = parser.next();
        QVERIFY(m && m->seq == 0x11 && m->op == 0x777 && m->payload.empty());

        // A payload that continues in further envelopes, as calibration blobs do.
        parser.feed(unhex("010000002b000000000000520000002e000000010000044c00000000000000130000150000000c00000008"));
        QVERIFY(!parser.next());
        parser.feed(unhex("010000000c00000011223344"));
        QVERIFY(!parser.next());
        parser.feed(unhex("010000000c00000055667788"));
        m = parser.next();
        QVERIFY(m && m->op == ttp::CalGet);
        const auto items = ttp::parseItems(m->payload);
        QCOMPARE(items.size(), size_t(1));
        QCOMPARE(items[0].asData().size(), size_t(8));
        QCOMPARE(items[0].asData()[7], std::uint8_t(0x88));

        parser.feed(ttp::Bytes(64, 0xff));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, parser.next());
    }

    void decodesGazeSamples()
    {
        using namespace ttp;
        auto column = [](Writer &w, std::uint32_t id) -> Writer & { return w.structHeader(2, kColumn).u32(id); };
        Writer w;
        w.structHeader(8, kSample);
        column(w, 1).item(U64, Bytes{0, 0, 0, 0, 0x23, 0x7c, 0x9a, 0x47});
        column(w, 27).u32(1);
        column(w, 28).structHeader(2, kPoint2).fixed(0.25).fixed(0.75);
        column(w, 22).u32(1);
        column(w, 2).point3(-30.5, 12.0, 600.0);
        column(w, 6).item(Fixed16, Bytes{0x00, 0x04, 0x80, 0x00});
        column(w, 30).u32(0);
        column(w, 99).structHeader(3, kPoint3).fixed(1).fixed(2).fixed(3); // a column we don't know

        const auto s = decodeGazeSample(w.bytes());
        QCOMPARE(s.timestampUs, std::uint64_t(0x237c9a47));
        QVERIFY(s.valid);
        QCOMPARE(s.gaze.x, 0.25);
        QCOMPARE(s.gaze.y, 0.75);
        QVERIFY(s.left.originValid);
        QCOMPARE(s.left.originMm.z, 600.0);
        QCOMPARE(s.left.pupilMm, 4.5);
        QVERIFY(!s.right.gazeValid && !s.right.originValid && !s.left.gazeValid);

        // A sample cut short anywhere must not crash.
        const auto bytes = w.bytes();
        for (size_t n = 0; n < bytes.size(); ++n)
            decodeGazeSample({bytes.data(), n});
    }

    void decodesCameraFrames()
    {
        using namespace ttp;
        auto column = [](Writer &w, std::uint32_t id) -> Writer & { return w.structHeader(2, kColumn).u32(id); };
        auto frame = [&](std::uint32_t bits, std::uint32_t width, std::uint32_t stride, std::uint32_t height,
                         const Bytes &pixels) {
            Writer w;
            w.structHeader(6, kSample);
            column(w, 1).item(U64, Bytes{0, 0, 0, 1, 0x59, 0x8e, 0x1b, 0x18});
            column(w, 2).u32(bits);
            column(w, 3).u32(width);
            column(w, 4).u32(stride);
            column(w, 5).u32(height);
            column(w, 6).blob(pixels);
            return w.bytes();
        };

        Bytes pixels(4 * 2);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = std::uint8_t(i * 10);
        const auto bytes = frame(8, 3, 4, 2, pixels); // rows padded to 4 bytes
        const auto f = decodeImageFrame(bytes);
        QVERIFY(f.has_value());
        QCOMPARE(f->timestampUs, std::uint64_t(0x1598e1b18));
        QCOMPARE(f->width, 3u);
        QCOMPARE(f->height, 2u);
        QCOMPARE(f->stride, 4u);
        QCOMPARE(f->pixels, pixels);

        // Frames we cannot use are refused rather than half-read.
        QVERIFY(!decodeImageFrame(frame(16, 3, 4, 2, pixels)));        // not 8-bit
        QVERIFY(!decodeImageFrame(frame(8, 3, 4, 3, pixels)));         // fewer pixels than announced
        QVERIFY(!decodeImageFrame(frame(8, 5, 4, 2, pixels)));         // row wider than its stride
        QVERIFY(!decodeImageFrame(frame(8, 0, 0, 0, Bytes{})));        // empty
        QVERIFY(!decodeImageFrame(frame(8, 100000, 100000, 100000, pixels))); // absurd size
        for (size_t n = 0; n < bytes.size(); ++n)
            QVERIFY(!decodeImageFrame({bytes.data(), n}));
    }

    // --- head pose ---

    void faceDetectorAnchorsMatchTheModel()
    {
        // The detector model answers with one box per anchor: 896 of them.
        const auto anchors = vision::GenerateAnchors();
        QCOMPARE(anchors.size(), size_t(896));
        QCOMPARE(anchors.front().x_center, 0.5f / 16);
        QCOMPARE(anchors.back().y_center, 7.5f / 8);
    }

    void overlappingFaceBoxesAreMerged()
    {
        vision::Detection a, b, c;
        a.score = 0.9f;
        a.box = {0.10f, 0.10f, 0.20f, 0.20f};
        b.score = 0.3f;
        b.box = {0.12f, 0.10f, 0.20f, 0.20f}; // nearly the same face
        c.score = 0.6f;
        c.box = {0.70f, 0.70f, 0.10f, 0.10f}; // a different one
        const auto kept = vision::WeightedNms({b, c, a}, 0.3f);
        QCOMPARE(kept.size(), size_t(2));
        QCOMPARE(kept[0].score, 0.9f);
        QVERIFY(qAbs(kept[0].box.x - (0.10f * 0.9f + 0.12f * 0.3f) / 1.2f) < 1e-6f);
        QCOMPARE(kept[1].box.x, 0.70f);
    }

    void headRotationIsRecoveredFromLandmarks()
    {
        // Turn, scale and shift a cloud of points; the fit must find the same turn.
        const float yaw = 0.4f, c = std::cos(yaw), s = std::sin(yaw);
        std::vector<cv::Point3f> from, to;
        std::vector<float> weights;
        for (int i = 0; i < 12; ++i) {
            const cv::Point3f p(float(i % 3) - 1, float(i % 4) * 0.7f, float(i % 5) * 0.3f - 0.5f);
            from.push_back(p);
            to.emplace_back(2 * (c * p.x + s * p.z) + 5, 2 * p.y - 1, 2 * (-s * p.x + c * p.z) + 3);
            weights.push_back(1.0f + float(i % 2));
        }
        const auto fit = vision::SolveWeightedProcrustes(from, to, weights);
        QVERIFY(fit.has_value());
        QVERIFY(qAbs(fit->scale - 2.0f) < 1e-4f);
        const auto pose = vision::PoseFromRotation(fit->rotation);
        QVERIFY(qAbs(pose.yaw - yaw * 57.29578f) < 1e-2f);
        QVERIFY(qAbs(pose.pitch) < 1e-2f && qAbs(pose.roll) < 1e-2f);

        QVERIFY(!vision::SolveWeightedProcrustes(from, {}, weights));
    }

    void headTrackerSeesNoFaceInAnEmptyFrame()
    {
        const QString models = QFINDTESTDATA("../models");
        if (!QFile::exists(models + "/face_detector.onnx"))
            QSKIP("face models not built (tools/convert_models.py)");
        HeadTracker head(models.toStdString());
        ImageFrame frame;
        frame.width = frame.height = frame.stride = 280;
        frame.pixels.assign(280 * 280, 12);
        QVERIFY(!head.onImage(frame));
        frame.pixels.resize(100); // shorter than it claims
        QVERIFY(!head.onImage(frame));
    }

    void headTrackerReportsMissingModels()
    {
        QTemporaryDir empty;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, HeadTracker(empty.path().toStdString()));
    }

    void modelDirectoryFollowsTheEnvironment()
    {
        QTemporaryDir dir;
        qputenv("EYENIX_MODEL_DIR", dir.path().toUtf8());
        QVERIFY(modelDirectory() != QFileInfo(dir.path()).canonicalFilePath()); // nothing in it yet
        write(dir.path() + "/face_detector.onnx", "x");
        write(dir.path() + "/face_landmarks_detector.onnx", "x");
        QCOMPARE(modelDirectory(), QFileInfo(dir.path()).canonicalFilePath());
        qunsetenv("EYENIX_MODEL_DIR");
    }

    // --- game server ---

    // A message as it travels between a game and Tobii's service: 24-byte header, then payload.
    static QByteArray gameMessage(quint32 magic, quint32 seq, quint32 op, const ttp::Bytes &payload = {})
    {
        QByteArray out(24, '\0');
        const quint32 fields[6] = {magic, seq, 1, op, 0, quint32(payload.size())};
        for (int i = 0; i < 6; ++i)
            qToBigEndian(fields[i], reinterpret_cast<uchar *>(out.data()) + 4 * i);
        out.append(reinterpret_cast<const char *>(payload.data()), qsizetype(payload.size()));
        return out;
    }

    // Reads one message from the socket; returns false on timeout.
    // The server lives on this thread too, so wait by running the event loop.
    static bool nextGameMessage(QTcpSocket &socket, QByteArray &buffer, ttp::Message &out)
    {
        QElapsedTimer waited;
        waited.start();
        for (;;) {
            if (buffer.size() >= 24) {
                const auto *raw = reinterpret_cast<const uchar *>(buffer.constData());
                const quint32 length = qFromBigEndian<quint32>(raw + 20);
                if (buffer.size() >= 24 + qsizetype(length)) {
                    out.magic = qFromBigEndian<quint32>(raw);
                    out.seq = qFromBigEndian<quint32>(raw + 4);
                    out.op = qFromBigEndian<quint32>(raw + 12);
                    out.status = qFromBigEndian<quint32>(raw + 16);
                    out.payload.assign(raw + 24, raw + 24 + length);
                    buffer.remove(0, 24 + length);
                    return true;
                }
            }
            if (waited.elapsed() > 2000)
                return false;
            QTest::qWait(2);
            buffer += socket.readAll();
        }
    }

    void gameServerAnswersAGameAndStreamsGaze()
    {
        GameServer server;
        QVERIFY2(server.listen(0), qPrintable(server.errorString()));
        server.setDisplaySize(2560, 1440);
        server.setReply(ttp::DeviceInfo, QByteArray("\0\0mine", 6));
        QSignalSpy changed(&server, &GameServer::clientsChanged);

        QTcpSocket game;
        game.connectToHost(QHostAddress::LocalHost, server.port());
        QTRY_COMPARE(game.state(), QAbstractSocket::ConnectedState);
        QByteArray buffer;
        ttp::Message m;

        // Discovery: hello gets the recorded answer, device info ours, the display our screen.
        game.write(gameMessage(ttp::Request, 7, ttp::Hello));
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.magic, quint32(ttp::Response));
        QCOMPARE(m.seq, 7u);
        QCOMPARE(m.op, quint32(ttp::Hello));
        QCOMPARE(m.payload.size(), size_t(11));

        game.write(gameMessage(ttp::Request, 8, ttp::DeviceInfo));
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(m.payload.data()), qsizetype(m.payload.size())), QByteArray("\0\0mine", 6));

        game.write(gameMessage(ttp::Request, 9, ttp::GetDisplayArea));
        QVERIFY(nextGameMessage(game, buffer, m));
        std::vector<double> numbers;
        for (const auto &item : ttp::parseItems(m.payload))
            if (item.type == ttp::Fixed42)
                numbers.push_back(item.asFixed());
        QCOMPARE(numbers, (std::vector<double>{-1280, 1440, 0, 1280, 1440, 0, -1280, 0, 0}));

        game.write(gameMessage(ttp::Request, 10, 0x7777)); // something we have never heard of
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.seq, 10u);
        QVERIFY(m.payload.empty());
        QCOMPARE(server.subscriberCount(), 0);

        // Nothing is streamed before the game subscribes.
        server.publishGaze(0.1, 0.1, true);
        game.write(gameMessage(ttp::Request, 11, ttp::Subscribe,
                               ttp::Writer().u32(ttp::GazeStream).item(0x17, ttp::Bytes{0, 0, 0, 0}).bytes()));
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.seq, 11u);
        QCOMPARE(server.subscriberCount(), 1);

        server.publishGaze(0.25, 0.75, true);
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.magic, quint32(ttp::Stream));
        QCOMPARE(m.op, quint32(ttp::GazeStream));
        const auto sample = decodeGazeSample(m.payload); // our own driver must be able to read it back
        QVERIFY(sample.valid);
        QCOMPARE(sample.gaze.x, 0.25);
        QCOMPARE(sample.gaze.y, 0.75);
        QVERIFY(qAbs(sample.left.pupilMm - 4.1) < 1e-4);

        server.publishGaze(qQNaN(), 7, false); // nonsense in, something harmless out
        QVERIFY(nextGameMessage(game, buffer, m));
        const auto lost = decodeGazeSample(m.payload);
        QVERIFY(!lost.valid);
        QCOMPARE(lost.gaze.x, 0.5);
        QCOMPARE(lost.gaze.y, 1.0);

        // Subscribing to presence brings a first presence event without being asked.
        game.write(gameMessage(ttp::Request, 12, ttp::Subscribe, ttp::Writer().u32(ttp::PresenceStream).bytes()));
        QVERIFY(nextGameMessage(game, buffer, m));
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.op, quint32(ttp::PresenceStream));

        game.write(gameMessage(ttp::Request, 13, ttp::Unsubscribe, ttp::Writer().u32(ttp::GazeStream).bytes()));
        QVERIFY(nextGameMessage(game, buffer, m));
        server.publishGaze(0.5, 0.5, true);
        game.write(gameMessage(ttp::Request, 14, ttp::Hello));
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.seq, 14u); // the hello answer, not a gaze event

        game.disconnectFromHost();
        QTRY_COMPARE(server.clientCount(), 0);
        QVERIFY(changed.count() >= 3);
    }

    void gameServerSurvivesGarbage()
    {
        GameServer server;
        QVERIFY(server.listen(0));
        QSignalSpy licence(&server, &GameServer::licenceOffered);

        // A header announcing an absurd length gets the client dropped, not buffered.
        QTcpSocket hostile;
        hostile.connectToHost(QHostAddress::LocalHost, server.port());
        QTRY_COMPARE(hostile.state(), QAbstractSocket::ConnectedState);
        QByteArray huge = gameMessage(ttp::Request, 1, ttp::Hello);
        qToBigEndian<quint32>(0xfffffff0u, reinterpret_cast<uchar *>(huge.data()) + 20);
        hostile.write(huge);
        QTRY_COMPARE(hostile.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(server.clientCount(), 0);

        // Messages that are not requests are ignored; a licence is noticed; requests still work,
        // even when they arrive a byte at a time.
        QTcpSocket game;
        game.connectToHost(QHostAddress::LocalHost, server.port());
        QTRY_COMPARE(game.state(), QAbstractSocket::ConnectedState);
        game.write(gameMessage(ttp::Response, 1, ttp::Hello));
        const QByteArray text("xx--SE-SIGNATURE--xx");
        game.write(gameMessage(ttp::Request, 2, 0x0bf4, ttp::Bytes(text.begin(), text.end())));
        const QByteArray hello = gameMessage(ttp::Request, 3, ttp::Hello);
        for (char c : hello) {
            game.write(&c, 1);
            game.flush();
        }
        QByteArray buffer;
        ttp::Message m;
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.seq, 2u);
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(m.seq, 3u);
        QCOMPARE(licence.count(), 1);
    }

    // --- finding games ---

    void wineProcessesAreGroupedByPrefix()
    {
        // A Wine installation is recognised by its layout: <root>/bin/wine beside what is running.
        QTemporaryDir root;
        QVERIFY(QDir().mkpath(root.path() + "/bin"));
        QVERIFY(QDir().mkpath(root.path() + "/lib/wine/x86_64-unix"));
        write(root.path() + "/bin/wine", "#!/bin/sh\n");
        QVERIFY(QFile::setPermissions(root.path() + "/bin/wine", QFile::ReadOwner | QFile::ExeOwner));
        const QString wine = root.path() + "/bin/wine";

        QCOMPARE(wineprefix::wineBinaryFor(root.path() + "/bin/wine-preloader"), wine);
        QCOMPARE(wineprefix::wineBinaryFor(root.path() + "/bin/wineserver"), wine);
        QCOMPARE(wineprefix::wineBinaryFor(root.path() + "/lib/wine/x86_64-unix/wine64-preloader"), wine);
        QCOMPARE(wineprefix::wineBinaryFor("/run/host" + root.path() + "/bin/wine64"), wine);
        QCOMPARE(wineprefix::wineBinaryFor("/usr/bin/bash"), QString());
        QCOMPARE(wineprefix::wineBinaryFor("/nowhere/bin/wine"), QString());

        QVERIFY(wineprefix::isGame("C:\\Games\\StarCitizen.exe -no_splash"));
        QVERIFY(!wineprefix::isGame("C:\\windows\\system32\\services.exe"));
        QVERIFY(!wineprefix::isGame(wine + " /usr/lib/eyenix/eyenix-bridge.exe -serial X"));
        QVERIFY(!wineprefix::isGame(""));

        const QString preloader = root.path() + "/bin/wine-preloader";
        const auto groups = wineprefix::group({
            {10, "/games/sc", root.path() + "/bin/wineserver", "wineserver"},
            {11, "/games/sc", preloader, "C:\\windows\\system32\\explorer.exe /desktop"},
            {12, "/games/sc", preloader, "C:\\RSI\\StarCitizen.exe"},
            {13, "/games/sc", "/usr/bin/bash", "bash sc-launch.sh"},
            {20, "/games/idle", preloader, "C:\\windows\\system32\\services.exe"},
            {30, "/games/odd", "/usr/bin/python3", "python launcher.py"},
        });
        QCOMPARE(groups.size(), 3);
        QCOMPARE(groups[0].path, QString("/games/sc"));
        QCOMPARE(groups[0].pid, qint64(12)); // the game itself, not a service or the launch script
        QVERIFY(groups[0].hasGame);
        QCOMPARE(groups[0].wineBinary, wine);
        QVERIFY(!groups[1].hasGame);
        QVERIFY(!groups[2].hasGame && groups[2].wineBinary.isEmpty());

        // The real thing must at least run, and this test is no Wine process.
        for (const auto &process : wineprefix::processes())
            QVERIFY(process.pid != QCoreApplication::applicationPid());
        QVERIFY(wineprefix::environmentOf(QCoreApplication::applicationPid()).contains("PATH="));
    }

    void tobiiLibraryIsFoundAndTheGameNamed()
    {
        QTemporaryDir prefix;
        const QString game = prefix.path() + "/drive_c/Program Files/Roberts Space Industries/StarCitizen/LIVE/Bin64";
        QVERIFY(QDir().mkpath(game));
        QVERIFY(QDir().mkpath(prefix.path() + "/drive_c/windows/system32"));
        QVERIFY(QDir().mkpath(prefix.path() + "/drive_c/users/me/Documents"));
        QCOMPARE(wineprefix::findTobiiLibrary(prefix.path()), QString());

        // Copies in Windows' own folders or the user's documents do not count.
        write(prefix.path() + "/drive_c/windows/system32/tobii_gameintegration_x64.dll", "x");
        write(prefix.path() + "/drive_c/users/me/Documents/tobii_gameintegration_x64.dll", "x");
        QCOMPARE(wineprefix::findTobiiLibrary(prefix.path()), QString());

        write(game + "/tobii_gameintegration_x64.dll", "x");
        QCOMPARE(wineprefix::findTobiiLibrary(prefix.path()), game + "/tobii_gameintegration_x64.dll");
        QCOMPARE(wineprefix::findTobiiLibrary(prefix.path() + "/nowhere"), QString());

        QCOMPARE(wineprefix::gameName(game + "/tobii_gameintegration_x64.dll"), QString("Star Citizen"));
        QCOMPARE(wineprefix::gameName("/p/drive_c/Games/Elite Dangerous/Win64/tobii_gameintegration_x64.dll"),
                 QString("Elite Dangerous"));
        QCOMPARE(wineprefix::gameName("/p/drive_c/bin/tobii_gameintegration_x64.dll"), QString("Game"));
        // Whatever the folder is called, only plain text comes out.
        const QString odd = wineprefix::gameName("/p/drive_c/<b>Evil</b>\n$(game);\x1b[31m/x64/tobii_gameintegration_x64.dll");
        QVERIFY(QRegularExpression("^[A-Za-z0-9 .'&-]{1,48}$").match(odd).hasMatch());
    }

    // --- game mode ---

    void gameModeChoiceIsSavedBesideTheProfile()
    {
        QTemporaryDir dir;
        Store store(dir.path());
        QVERIFY(!store.gamesEnabled());
        QVERIFY(store.saveCalibration("Work", "blob"));
        QVERIFY(store.setProfile("Work"));
        QVERIFY(store.setGamesEnabled(true));
        QCOMPARE(Store(dir.path()).profile(), QString("Work")); // one setting does not wipe the other
        QVERIFY(Store(dir.path()).gamesEnabled());
        QVERIFY(store.setProfile("Work"));
        QVERIFY(Store(dir.path()).gamesEnabled());
    }

    void headPosePacketIsWhatTheBridgeReads()
    {
        HeadPose pose;
        pose.positionValid = true;
        pose.positionMm = {12.5, 30.25, 615};
        pose.pitch = 0.1;
        pose.yaw = -0.2;
        pose.roll = 0.05;
        // Same bytes as bridge/sesp_test.go's packet, apart from x: games count it the other way.
        QCOMPARE(GameMode::posePacket(pose, true).toHex(),
                 QByteArray("45594e5803000000000048c10000f24100c01944cdcccc3dcdcc4cbecdcc4c3d"));
        QCOMPARE(GameMode::posePacket(pose, false).mid(4, 4).toHex(), QByteArray("00000000")); // a heartbeat
        pose.positionValid = false;
        pose.yaw = qQNaN();
        const QByteArray partial = GameMode::posePacket(pose, true);
        QCOMPARE(partial.mid(4, 4).toHex(), QByteArray("02000000"));
        QCOMPARE(partial.mid(24, 4).toHex(), QByteArray("00000000"));
    }

    void gameModeServesARunningGameAndRestsOtherwise()
    {
        QTemporaryDir dir, prefix, tools;
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        Session session(&backend, nullptr, 100);

        // A "game": a prefix holding Tobii's library, and a stand-in for wine that
        // behaves like the bridge and notes how it was started.
        QVERIFY(QDir().mkpath(prefix.path() + "/drive_c/StarCitizen/LIVE/Bin64"));
        write(prefix.path() + "/drive_c/StarCitizen/LIVE/Bin64/tobii_gameintegration_x64.dll", "x");
        const QString wine = tools.path() + "/wine", started = tools.path() + "/started";
        write(wine, ("#!/bin/sh\nprintf '%s\\n' \"$WINEPREFIX\" \"$@\" > '" + started + "'\n"
                     "echo ready\necho headpose\nexec sleep 30\n").toUtf8());
        QVERIFY(QFile::setPermissions(wine, QFile::ReadOwner | QFile::ExeOwner));
        write(tools.path() + "/eyenix-bridge.exe", "x");
        qputenv("EYENIX_BRIDGE", (tools.path() + "/eyenix-bridge.exe").toUtf8());

        bool gameRunning = false;
        GameMode games(&backend, nullptr, 0, [&]() -> QList<wineprefix::Running> {
            if (!gameRunning)
                return {};
            return {{prefix.path(), QCoreApplication::applicationPid(), wine, true}};
        });
        session.setGameMode(&games);

        // Not before the screen is aligned: a game must be told which screen the tracker is on.
        QVERIFY(!session.setGames(true).isEmpty());
        QTRY_VERIFY(backend.connected());
        backend.applyAlignment(880, 1670, 2560, 1440, "DP-1");
        QCOMPARE(session.setGames(true), QString());
        QVERIFY(Store(dir.path()).gamesEnabled());
        QCOMPARE(games.status().value("state").toString(), QString("waiting"));

        // With only game mode on and no game, the app stays but lets the tracker go.
        QSignalSpy leaving(&session, &Session::nothingLeftToShow);
        QTRY_VERIFY(backend.idle());
        QVERIFY(!backend.connected());
        QTest::qWait(450);
        QCOMPARE(leaving.count(), 0);
        QVERIFY(session.status().value("idle").toBool());

        // A game starts: the tracker comes back and the bridge is started in its prefix.
        gameRunning = true;
        games.scan();
        QVERIFY(games.active());
        QTRY_VERIFY(backend.connected());
        QTRY_COMPARE(games.status().value("state").toString(), QString("serving"));
        QCOMPARE(games.status().value("game").toString(), QString("Star Citizen"));
        QVERIFY(games.status().value("head").toBool());
        QTRY_VERIFY(QFile::exists(started));
        QFile file(started);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList arguments = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
        QCOMPARE(arguments.value(0), prefix.path());
        QCOMPARE(arguments.value(1), QFileInfo(tools.path() + "/eyenix-bridge.exe").canonicalFilePath());
        QVERIFY(arguments.contains("FAKE-0001"));
        QCOMPARE(arguments.value(arguments.indexOf("-display-width") + 1), QString("2560"));

        // The game leaves: back to waiting, tracker released, bridge stopped.
        gameRunning = false;
        games.scan();
        QVERIFY(!games.active());
        QCOMPARE(games.status().value("state").toString(), QString("waiting"));
        QTRY_VERIFY(backend.idle());

        // Turned off with nothing else showing, the app has no reason to stay.
        QCOMPARE(session.setGames(false), QString());
        QVERIFY(!Store(dir.path()).gamesEnabled());
        QTRY_COMPARE(leaving.count(), 1);
        qunsetenv("EYENIX_BRIDGE");
    }

    void gameModePassesGazeAndTheTrackersOwnAnswersToAGame()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(backend.connected());
        backend.applyAlignment(880, 1670, 2560, 1440, "DP-1");

        GameServer probe; // find a free port
        QVERIFY(probe.listen(0));
        const quint16 port = probe.port();
        probe.close();
        GameMode games(&backend, nullptr, port, [] { return QList<wineprefix::Running>(); });
        QCOMPARE(games.setEnabled(true), QString());

        // The port is ours now; a second game mode cannot have it and says so.
        GameMode second(&backend, nullptr, port, [] { return QList<wineprefix::Running>(); });
        QVERIFY(second.setEnabled(true).contains(QString::number(port)));

        QTcpSocket game;
        game.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(game.state(), QAbstractSocket::ConnectedState);
        QByteArray buffer;
        ttp::Message m;
        game.write(gameMessage(ttp::Request, 1, ttp::DeviceInfo));
        QVERIFY(nextGameMessage(game, buffer, m));
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(m.payload.data()), qsizetype(m.payload.size())),
                 QByteArray("\0\0fake", 6)); // the tracker's own answer, not the stock one

        game.write(gameMessage(ttp::Request, 2, ttp::Subscribe, ttp::Writer().u32(ttp::GazeStream).bytes()));
        QVERIFY(nextGameMessage(game, buffer, m));
        QTRY_VERIFY(games.status().value("gaze").toBool());
        QVERIFY(games.active()); // a connection for gaze is reason enough to hold the tracker
        do {
            QVERIFY(nextGameMessage(game, buffer, m));
        } while (m.magic != ttp::Stream);
        const auto sample = decodeGazeSample(m.payload);
        QVERIFY(sample.valid);
        QCOMPARE(sample.gaze.x, 0.25); // straight from the fake tracker, unsmoothed
        QCOMPARE(sample.gaze.y, 0.75);

        games.setEnabled(false);
        QTRY_COMPARE(game.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(!games.active());
    }

    // --- smoothing ---

    void filterSteadiesJitterButFollowsJumps()
    {
        GazeFilter filter;
        double t = 0, worst = 0;
        for (int i = 0; i < 60; ++i) {
            const auto p = filter.filter({0.5 + (i % 2 ? 0.01 : -0.01), 0.5}, t += 0.03);
            if (i > 10)
                worst = qMax(worst, qAbs(p.x - 0.5));
        }
        QVERIFY2(worst < 0.004, "resting jitter should shrink well below the raw 0.01");

        GazeFilter::Point p;
        for (int i = 0; i < 6; ++i)
            p = filter.filter({0.9, 0.2}, t += 0.03);
        QVERIFY2(qAbs(p.x - 0.9) < 0.05 && qAbs(p.y - 0.2) < 0.05, "a jump should be followed within 0.2 s");

        // After a gap the old position is forgotten.
        p = filter.filter({0.1, 0.1}, t += 1.0);
        QCOMPARE(p.x, 0.1);
    }

    // --- the backend, against a fake tracker ---

    void connectsAndReportsGaze()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(backend.connected());
        QCOMPARE(backend.deviceName(), QString("Fake Tracker"));
        QVERIFY(!backend.aligned() && !backend.calibrated());
        QTRY_VERIFY(backend.gazeValid());
        QCOMPARE(backend.gazeX(), 0.25);
        QCOMPARE(backend.gazeY(), 0.75);
        QCOMPARE(backend.distanceCm(), 61.0);

        state->looking = false;
        QTRY_VERIFY(!backend.gazeValid());
        QTRY_COMPARE(backend.distanceCm(), 0.0);
    }

    void saysWhyThereIsNoTrackerAndReconnects()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        state->present = false;
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_COMPARE(backend.problem(), QString("no tracker here"));
        QVERIFY(!backend.connected());

        state->present = true;
        QTRY_VERIFY(backend.connected());
        QCOMPARE(backend.problem(), QString());

        // An unplug and replug can pass within one turn of the event loop, so
        // count the changes instead of trying to catch the gap.
        QSignalSpy changes(&backend, &Backend::connectionChanged);
        state->unplug = true;
        QTRY_COMPARE(changes.count(), 2);
        QVERIFY(backend.connected());
    }

    void alignmentIsSavedSentAndRestored()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        {
            Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
            QTRY_VERIFY(backend.connected());
            QVERIFY(backend.screenSize(100, 100, 1300, 812).isEmpty());
            const auto size = backend.screenSize(282, 1018, 1300, 812);
            QCOMPARE(size.value("widthMm").toDouble(), 325.0);
            QCOMPARE(size.value("heightMm").toDouble(), 203.0);

            backend.applyAlignment(282, 1018, 1300, 812, "DP-1");
            QVERIFY(backend.aligned());
            QTRY_VERIFY(state->taken().contains("setDisplayArea -162.5 162.5"));
            QVERIFY(QFile::exists(dir.path() + "/alignment.json"));

            // Lines too close together to be the marks are ignored.
            backend.applyAlignment(500, 510, 1300, 812, "DP-1");
            QCOMPARE(backend.markLeft(), 282.0);
        }
        state->forget();
        Backend again(fakeFactory(state), dir.path(), nullptr, 20);
        QVERIFY(again.aligned());
        QCOMPARE(again.screenName(), QString("DP-1"));
        QCOMPARE(again.markRight(), 1018.0);
        QTRY_VERIFY(state->taken().contains("setDisplayArea -162.5 162.5"));
    }

    void calibrationFollowsTobiisSequenceAndIsSaved()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(backend.connected());
        state->forget();

        QSignalSpy collected(&backend, &Backend::pointCollected), saved(&backend, &Backend::calibrationSaved);
        backend.beginCalibration();
        QVERIFY(backend.calibrating());
        QCOMPARE(backend.calibrationPointCount(), 7);
        for (int i = 0; i < 7; ++i) {
            backend.collectPoint(i);
            QTRY_COMPARE(collected.count(), i + 1);
        }
        QTRY_COMPARE(saved.count(), 1);
        QVERIFY(!backend.calibrating() && backend.calibrated());

        QCOMPARE(state->taken(), QStringList({"start", "clear", "add 0.5,0.5", "compute",
                                               "add 0.1,0.9", "add 0.5,0.1", "add 0.9,0.9", "compute",
                                               "add 0.1,0.1", "add 0.9,0.1", "add 0.5,0.9", "compute",
                                               "get", "stop"}));
        QCOMPARE(Store(dir.path()).calibration("Default"), QByteArray("new calibration"));

        // The next connection gets it back.
        state->forget();
        state->unplug = true;
        QTRY_VERIFY(state->taken().contains("setCalibration new calibration"));
    }

    void failedCalibrationPutsTheOldOneBack()
    {
        QTemporaryDir dir;
        QVERIFY(Store(dir.path()).saveCalibration("Default", "old calibration"));

        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(state->taken().contains("setCalibration old calibration"));
        state->forget();
        {
            QMutexLocker lock(&state->mutex);
            state->refuse = "add";
        }

        QSignalSpy failed(&backend, &Backend::calibrationFailed);
        backend.beginCalibration();
        backend.collectPoint(0);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed[0][0].toString(), QString("refused add"));
        QVERIFY(!backend.calibrating());
        QTRY_COMPARE(state->taken(), QStringList({"start", "clear", "add 0.5,0.5", "stop",
                                                   "setCalibration old calibration"}));
        QCOMPARE(Store(dir.path()).calibration("Default"), QByteArray("old calibration"));
    }

    void cancelledCalibrationPutsTheOldOneBack()
    {
        QTemporaryDir dir;
        QVERIFY(Store(dir.path()).saveCalibration("Default", "old calibration"));

        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(state->taken().contains("setCalibration old calibration"));
        state->forget();

        QSignalSpy collected(&backend, &Backend::pointCollected), failed(&backend, &Backend::calibrationFailed);
        backend.beginCalibration();
        backend.collectPoint(0);
        backend.cancelCalibration();
        QVERIFY(!backend.calibrating());
        QTRY_COMPARE(state->taken().value(state->taken().size() - 1), QString("setCalibration old calibration"));
        QVERIFY(state->taken().contains("stop"));
        // The answer to the point that was in flight is ignored.
        QTest::qWait(50);
        QCOMPARE(collected.count(), 0);
        QCOMPARE(failed.count(), 0);
    }

    void calibrationNeedsATrackerAndEndsWhenItLeaves()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        state->present = false;
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QSignalSpy failed(&backend, &Backend::calibrationFailed);
        backend.beginCalibration();
        QCOMPARE(failed.count(), 1);
        QVERIFY(!backend.calibrating());

        state->present = true;
        QTRY_VERIFY(backend.connected());
        backend.beginCalibration();
        QVERIFY(backend.calibrating());
        state->present = false;
        state->unplug = true;
        QTRY_COMPARE(failed.count(), 2);
        QVERIFY(!backend.calibrating());
    }

    // --- what is kept on disk ---

    void oldSingleCalibrationBecomesTheDefaultProfile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/calibration.bin", "from before profiles");
        Store store(dir.path());
        QCOMPARE(store.profiles(), QStringList({"Default"}));
        QCOMPARE(store.profile(), QString("Default"));
        QCOMPARE(store.calibration("Default"), QByteArray("from before profiles"));
        QVERIFY(!QFile::exists(dir.path() + "/calibration.bin"));

        // An existing Default profile is never overwritten by a stray old file.
        write(dir.path() + "/calibration.bin", "stray");
        Store again(dir.path());
        QCOMPARE(again.calibration("Default"), QByteArray("from before profiles"));
    }

    void profilesAreListedChosenAndNamedSafely()
    {
        QTemporaryDir dir;
        Store store(dir.path());
        QCOMPARE(store.profiles(), QStringList({"Default"}));
        QVERIFY(!store.hasCalibration("Default"));
        QCOMPARE(store.unusedProfileName(), QString("Profile 2"));

        QVERIFY(store.saveCalibration("Glasses", "g"));
        QVERIFY(store.saveCalibration("Profile 2", "p"));
        QCOMPARE(store.profile(), QString("Glasses")); // nothing chosen, no Default: the first one
        QVERIFY(store.setProfile("Profile 2"));
        QCOMPARE(Store(dir.path()).profile(), QString("Profile 2"));
        QCOMPARE(store.profiles(), QStringList({"Glasses", "Profile 2"}));
        QCOMPARE(store.unusedProfileName(), QString("Profile 3"));

        for (const char *bad : {"", " x", "x ", "../x", "a/b", ".hidden", "<img src=x>", "a\nb",
                                "0123456789012345678901234567890123"}) {
            QVERIFY2(!Store::validProfileName(bad), bad);
            QVERIFY(!store.saveCalibration(bad, "x"));
            QVERIFY(!store.setProfile(bad));
        }
        QVERIFY(Store::validProfileName("With glasses_2-b"));

        // A file with a name we would not accept is not offered as a profile.
        write(dir.path() + "/profiles/<b>bold<.bin", "x");
        QCOMPARE(store.profiles(), QStringList({"Glasses", "Profile 2"}));
    }

    void switchingProfileSendsItsCalibration()
    {
        QTemporaryDir dir;
        {
            Store store(dir.path());
            QVERIFY(store.saveCalibration("Default", "plain"));
            QVERIFY(store.saveCalibration("Glasses", "glasses"));
        }
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(state->taken().contains("setCalibration plain"));
        QCOMPARE(backend.profiles(), QStringList({"Default", "Glasses"}));

        QVERIFY(backend.useProfile("Glasses"));
        QCOMPARE(backend.profile(), QString("Glasses"));
        QTRY_VERIFY(state->taken().contains("setCalibration glasses"));
        QCOMPARE(Store(dir.path()).profile(), QString("Glasses"));

        QVERIFY(!backend.useProfile("Nobody"));
        QVERIFY(!backend.useProfile("../Default"));
        QCOMPARE(backend.profile(), QString("Glasses"));
    }

    void calibratingANewProfileCreatesAndSelectsIt()
    {
        QTemporaryDir dir;
        QVERIFY(Store(dir.path()).saveCalibration("Default", "plain"));
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        QTRY_VERIFY(backend.connected());

        QSignalSpy collected(&backend, &Backend::pointCollected), saved(&backend, &Backend::calibrationSaved);
        QSignalSpy failed(&backend, &Backend::calibrationFailed);
        backend.beginCalibration("../evil");
        QCOMPARE(failed.count(), 1);
        QVERIFY(!backend.calibrating());

        backend.beginCalibration(backend.unusedProfileName());
        QVERIFY(!backend.useProfile("Default")); // not while calibrating
        for (int i = 0; i < 7; ++i) {
            backend.collectPoint(i);
            QTRY_COMPARE(collected.count(), i + 1);
        }
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(backend.profile(), QString("Profile 2"));
        QCOMPARE(backend.profiles(), QStringList({"Default", "Profile 2"}));
        QCOMPARE(Store(dir.path()).calibration("Profile 2"), QByteArray("new calibration"));
        QCOMPARE(Store(dir.path()).calibration("Default"), QByteArray("plain"));
    }

    // --- what is showing, and the commands that drive it ---

    void sessionQuitsOnlyWhenNothingIsShowing()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        Session session(&backend, nullptr, 150);
        QSignalSpy idle(&session, &Session::nothingLeftToShow);

        session.setOverlay(true);       // arrives within the grace period
        QTest::qWait(500);
        QCOMPARE(idle.count(), 0);

        session.showWindow();
        session.setOverlay(false);
        QTest::qWait(400);
        QCOMPARE(idle.count(), 0);

        session.hideWindow();
        QTRY_COMPARE(idle.count(), 1);
    }

    void sessionSaysWhyATaskCannotStart()
    {
        QTemporaryDir dir;
        auto state = std::make_shared<FakeState>();
        state->present = false;
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        Session session(&backend);

        QVERIFY(session.startCalibration().contains("Connect"));
        state->present = true;
        QTRY_VERIFY(backend.connected());
        QVERIFY(session.startCalibration().contains("Align"));
        QCOMPARE(session.mode(), QString("home"));

        QCOMPARE(session.startAlign(), QString());
        QCOMPARE(session.mode(), QString("align"));
        QVERIFY(!session.startAlign().isEmpty());
        backend.applyAlignment(282, 1018, 1300, 812, "DP-1");
        session.finishTask();

        QVERIFY(!session.startCalibration("../x").isEmpty());
        QCOMPARE(session.startCalibration("Glasses"), QString());
        QCOMPARE(session.mode(), QString("calibrate"));
        QCOMPARE(session.calibrationProfile(), QString("Glasses"));
    }

    void commandsDriveTheSession()
    {
        QTemporaryDir dir;
        QVERIFY(Store(dir.path()).saveCalibration("Default", "plain"));
        QVERIFY(Store(dir.path()).saveCalibration("Glasses", "glasses"));
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        Session session(&backend);
        Control control(&session, &backend);
        QTRY_VERIFY(backend.connected());

        auto reply = control.handle({{"cmd", "overlay"}, {"value", "toggle"}});
        QVERIFY(reply.value("ok").toBool() && reply.value("overlay").toBool() && session.overlayOn());
        QVERIFY(!control.handle({{"cmd", "overlay"}, {"value", "sideways"}}).value("ok").toBool());
        QVERIFY(control.handle({{"cmd", "overlay"}, {"value", "off"}}).value("ok").toBool());
        QVERIFY(!session.overlayOn());

        reply = control.handle({{"cmd", "profile"}, {"name", "Glasses"}});
        QVERIFY(reply.value("ok").toBool());
        QCOMPARE(reply.value("profile").toString(), QString("Glasses"));
        QCOMPARE(reply.value("profiles").toArray().size(), 2);
        QVERIFY(!control.handle({{"cmd", "profile"}, {"name", "Nobody"}}).value("ok").toBool());

        reply = control.handle({{"cmd", "calibrate"}});
        QVERIFY(!reply.value("ok").toBool());
        QVERIFY(reply.value("error").toString().contains("Align"));

        backend.applyAlignment(282, 1018, 1300, 812, "DP-1");
        QVERIFY(control.handle({{"cmd", "calibrate"}, {"new", true}}).value("ok").toBool());
        QCOMPARE(session.mode(), QString("calibrate"));
        QCOMPARE(session.calibrationProfile(), QString("Profile 2"));

        QVERIFY(!control.handle({{"cmd", "format-disk"}}).value("ok").toBool());
        QVERIFY(!control.handle({}).value("ok").toBool());
    }

    void statusTravelsOverTheSocket()
    {
        QTemporaryDir dir;
        qputenv("EYENIX_SOCKET", (dir.path() + "/test.sock").toUtf8());
        QVERIFY(Control::ask({{"cmd", "ping"}}, 200).isEmpty()); // nobody home

        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        Session session(&backend);
        Control control(&session, &backend);
        QVERIFY(control.listen());

        // ask() blocks, so the client runs on another thread, as a separate process would.
        QJsonObject reply;
        std::unique_ptr<QThread> client(QThread::create([&reply] {
            reply = Control::ask({{"cmd", "overlay"}, {"value", "on"}});
        }));
        client->start();
        QTRY_VERIFY(client->isFinished());
        QVERIFY(reply.value("ok").toBool() && reply.value("running").toBool());
        QVERIFY(session.overlayOn());

        // A follower hears about changes without asking again.
        QLocalSocket follower;
        follower.connectToServer(Control::socketPath());
        QVERIFY(follower.waitForConnected(1000));
        follower.write("{\"cmd\":\"status\",\"follow\":true}\n");
        QTRY_VERIFY(follower.canReadLine());
        QVERIFY(QJsonDocument::fromJson(follower.readLine()).object().value("overlay").toBool());
        session.setOverlay(false);
        QTRY_VERIFY(follower.canReadLine());
        QVERIFY(!QJsonDocument::fromJson(follower.readLine()).object().value("overlay").toBool());

        // Junk is dropped rather than buffered without end.
        QLocalSocket junk;
        junk.connectToServer(Control::socketPath());
        QVERIFY(junk.waitForConnected(1000));
        junk.write(QByteArray(10000, 'x'));
        QTRY_COMPARE(junk.state(), QLocalSocket::UnconnectedState);

        QSignalSpy quit(&control, &Control::quitRequested);
        std::unique_ptr<QThread> quitter(QThread::create([] { Control::ask({{"cmd", "quit"}}); }));
        quitter->start();
        QTRY_VERIFY(quitter->isFinished());
        QTRY_COMPARE(quit.count(), 1);
        qunsetenv("EYENIX_SOCKET");
    }

    // --- the bar plugin's reading of the app's output ---

    void pluginAcceptsOnlyWellFormedStatus()
    {
        QFile source(QFINDTESTDATA("../plugin/Model.js"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QJSEngine js;
        // ".pragma library" is QML's; plain JavaScript does not know it.
        const QJSValue loaded = js.evaluate(QString::fromUtf8(source.readAll()).replace(".pragma library", ""));
        QVERIFY2(!loaded.isError(), qPrintable(loaded.toString()));
        auto parse = [&js](const QString &line) {
            return js.globalObject().property("parseStatus").call({line});
        };

        // What the app really sends.
        QTemporaryDir dir;
        QVERIFY(Store(dir.path()).saveCalibration("Default", "plain"));
        QVERIFY(Store(dir.path()).saveCalibration("With glasses", "g"));
        auto state = std::make_shared<FakeState>();
        Backend backend(fakeFactory(state), dir.path(), nullptr, 20);
        Session session(&backend);
        QTRY_VERIFY(backend.connected());
        session.setOverlay(true);
        QJSValue parsed = parse(QJsonDocument(session.status()).toJson(QJsonDocument::Compact));
        QVERIFY(parsed.isObject());
        QVERIFY(parsed.property("running").toBool() && parsed.property("connected").toBool());
        QVERIFY(parsed.property("overlay").toBool());
        QCOMPARE(parsed.property("profile").toString(), QString("Default"));
        QCOMPARE(parsed.property("profiles").property("length").toInt(), 2);
        QCOMPARE(parsed.property("device").toString(), QString("Fake Tracker"));
        QVERIFY(js.globalObject().property("tracking").call({parsed}).toBool());
        QCOMPARE(js.globalObject().property("statusKey").call({parsed, false}).toString(), QString("status.tracking"));

        // Things that are not a status at all.
        for (const char *bad : {"", "not json", "[]", "null", "42", "{}", "{\"running\":\"yes\"}"})
            QVERIFY2(parse(bad).isNull(), bad);
        QVERIFY(parse(QString(20000, 'x')).isNull());

        // Markup-shaped and oversized values are dropped or cut, never passed on.
        QJsonArray names{"Default", "<img src=\"https://example.invalid/t\">", "../etc", "a\nb", 7, "Default"};
        for (int i = 0; i < 80; ++i)
            names.append(QString("Profile %1").arg(i));
        const QJsonObject hostile{{"running", true}, {"connected", "true"}, {"mode", "<b>"},
                                  {"profile", "<img src=x>"}, {"profiles", names},
                                  {"device", QString(500, 'd')}, {"problem", "line one\nline two"}};
        parsed = parse(QJsonDocument(hostile).toJson(QJsonDocument::Compact));
        QVERIFY(parsed.isObject());
        QVERIFY(!parsed.property("connected").toBool());
        QCOMPARE(parsed.property("mode").toString(), QString("home"));
        QCOMPARE(parsed.property("profile").toString(), QString());
        QCOMPARE(parsed.property("profiles").property("length").toInt(), 50);
        QCOMPARE(parsed.property("profiles").property(0).toString(), QString("Default"));
        QCOMPARE(parsed.property("profiles").property(1).toString(), QString("Profile 0"));
        QCOMPARE(parsed.property("device").toString().size(), 64);
        QCOMPARE(parsed.property("problem").toString(), QString("line one line two"));

        // Game mode: the app's own status first, then values a status should never hold.
        QCOMPARE(parsed.property("games").property("state").toString(), QString("off"));
        auto call = [&js](const char *function, QJSValueList arguments) {
            return js.globalObject().property(function).call(arguments);
        };
        const QJsonObject serving{{"running", true}, {"connected", true}, {"eyes", true},
                                  {"games", QJsonObject{{"enabled", true}, {"state", "serving"}, {"game", "Star Citizen"},
                                                        {"head", true}, {"gaze", false}}}};
        parsed = parse(QJsonDocument(serving).toJson(QJsonDocument::Compact));
        QCOMPARE(call("statusKey", {parsed, false}).toString(), QString("status.serving"));
        QCOMPARE(call("gamesKey", {parsed.property("games")}).toString(), QString("games.servingHead"));
        QCOMPARE(call("withGame", {"Tracking for %1", parsed.property("games")}).toString(), QString("Tracking for Star Citizen"));

        const QJsonObject resting{{"running", true}, {"connected", false}, {"idle", true},
                                  {"games", QJsonObject{{"enabled", true}, {"state", "waiting"}}}};
        parsed = parse(QJsonDocument(resting).toJson(QJsonDocument::Compact));
        QCOMPARE(call("statusKey", {parsed, false}).toString(), QString("status.gamesWaiting"));
        QCOMPARE(call("gamesKey", {parsed.property("games")}).toString(), QString("games.waiting"));
        QVERIFY(!call("tracking", {parsed}).toBool());

        const QJsonObject odd{{"running", false},
                              {"games", QJsonObject{{"enabled", "yes"}, {"state", "<b>"}, {"head", 1},
                                                    {"game", "<img src=x onerror=1>\nStar " + QString(200, 'z')}}}};
        parsed = parse(QJsonDocument(odd).toJson(QJsonDocument::Compact));
        QVERIFY(!parsed.property("games").property("enabled").toBool());
        QCOMPARE(parsed.property("games").property("state").toString(), QString("off"));
        QVERIFY(!parsed.property("games").property("head").toBool());
        const QString name = parsed.property("games").property("game").toString();
        QVERIFY(name.size() <= 48 && !name.contains('<') && !name.contains('=') && !name.contains('\n'));
        for (const char *notGames : {"null", "[]", "\"on\"", "7"}) {
            parsed = parse(QString("{\"running\":true,\"games\":%1}").arg(notGames));
            QVERIFY2(parsed.isObject() && !parsed.property("games").property("enabled").toBool(), notGames);
        }

        // Game mode left on with the app gone: start it again, but only once.
        parsed = parse("{\"running\":false,\"games\":{\"enabled\":true}}");
        QVERIFY(call("shouldResumeGames", {parsed, false}).toBool());
        QVERIFY(!call("shouldResumeGames", {parsed, true}).toBool());
        parsed = parse("{\"running\":true,\"games\":{\"enabled\":true}}");
        QVERIFY(!call("shouldResumeGames", {parsed, false}).toBool());

        auto commandPath = [&js](const QString &raw) {
            return js.globalObject().property("commandPath").call({raw}).toString();
        };
        QCOMPARE(commandPath("/usr/bin/eyenix\n"), QString("/usr/bin/eyenix"));
        QCOMPARE(commandPath("eyenix"), QString());
        QCOMPARE(commandPath("/usr/bin/../bin/sh"), QString());
        QCOMPARE(commandPath("/usr/bin/eyenix; echo hi"), QString());
        QCOMPARE(commandPath("/usr/bin/$(x)"), QString());
    }

    // --- theme and language, as in the Omarchy app template ---

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n");

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Calibrate").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Calibrate": "Calibrar", "Quit": "Sair"})");
        write(dir.path() + "/pt-BR.json", R"({"Quit": "Fechar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Quit"), QString("Fechar"));
        QCOMPARE(translator.translate("Main", "Calibrate"), QString("Calibrar"));
        QVERIFY(translator.translate("Main", "Saved").isNull());
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(EyenixTests)
#include "eyenix_tests.moc"
