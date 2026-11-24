#include "gamemode.h"

#include <QHostAddress>
#include <QProcessEnvironment>
#include <QtEndian>
#include <cmath>
#include <cstring>

#include "models.h"

namespace {

constexpr int kScanMs = 2000;
constexpr int kHeartbeatMs = 1000;    // the bridge exits after ten seconds of silence
constexpr int kPresenceMs = 500;      // presence is repeated this often even when unchanged
constexpr int kMaxFailures = 3;       // a bridge that dies this often in a row is left alone for a while
constexpr int kRetryMs = 30000;
constexpr int kEarlyExitMs = 5000;    // a bridge that lives shorter than this counts as failed

}

GameMode::GameMode(Backend *backend, QObject *parent, quint16 gazePort, Scanner scanner)
    : QObject(parent), m_backend(backend), m_scanner(std::move(scanner)), m_gazePort(gazePort) {
    m_scanTimer.setInterval(kScanMs);
    connect(&m_scanTimer, &QTimer::timeout, this, &GameMode::scan);
    m_heartbeat.setInterval(kHeartbeatMs);
    connect(&m_heartbeat, &QTimer::timeout, this, [this] {
        m_udp.writeDatagram(posePacket({}, false), QHostAddress::LocalHost, kPosePort);
    });

    connect(&m_server, &GameServer::clientsChanged, this, [this] {
        updateActive();
        emit changed();
    });
    connect(&m_server, &GameServer::licenceOffered, this, [this] {
        m_licence = true;
        emit changed();
    });

    connect(backend, &Backend::described, this, [this] {
        applyDescriptions();
        scan(); // the bridge may have been waiting for the serial number
    });
    connect(backend, &Backend::alignmentChanged, this, &GameMode::applyDisplay);
    connect(backend, &Backend::sampled, this, [this](const eyenix::GazeSample &sample) {
        if (!m_enabled)
            return;
        m_server.publishGaze(sample.gaze.x, sample.gaze.y, sample.valid);
        m_eyesPresent = sample.left.originValid || sample.right.originValid;
        const bool present = m_eyesPresent || m_facePresent;
        if (present != m_presenceSent || !m_presenceAge.isValid() || m_presenceAge.elapsed() >= kPresenceMs) {
            m_server.publishPresence(present);
            m_presenceSent = present;
            m_presenceAge.start();
        }
    });
    connect(backend, &Backend::headPose, this, [this](const eyenix::HeadPose &pose, bool found) {
        m_facePresent = found;
        if (m_bridge)
            m_udp.writeDatagram(posePacket(pose, found), QHostAddress::LocalHost, kPosePort);
    });
    applyDescriptions();
    applyDisplay();
}

GameMode::~GameMode() { stopBridge(); }

// What the game is told about the tracker comes from the tracker itself where possible.
void GameMode::applyDescriptions() {
    const auto descriptions = m_backend->descriptions();
    for (auto it = descriptions.begin(); it != descriptions.end(); ++it)
        m_server.setReply(it.key(), it.value());
}

void GameMode::applyDisplay() {
    if (m_backend->aligned())
        m_server.setDisplaySize(int(m_backend->alignedWidth()), int(m_backend->alignedHeight()));
}

// Turns head tracking on with the first need for the tracker and off with the
// last, and tells the session, which opens or releases the tracker.
void GameMode::updateActive() {
    const bool active = m_enabled && (!m_game.isEmpty() || m_server.clientCount() > 0);
    if (active == m_active)
        return;
    m_active = active;
    if (active && !m_backend->setHeadTracking(true))
        setProblem(tr("Head tracking is not installed; games get gaze only."));
    if (!active)
        m_backend->setHeadTracking(false);
    emit activeChanged();
}

void GameMode::setProblem(const QString &problem) {
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit changed();
}

QString GameMode::setEnabled(bool enabled) {
    if (enabled == m_enabled)
        return {};
    if (enabled) {
        if (!m_backend->aligned())
            return tr("Align the screen first: games need to know which screen the tracker is on.");
        if (!m_server.listen(m_gazePort))
            return tr("Port %1 is taken. Is another Tobii runtime running?").arg(m_gazePort);
        m_enabled = true;
        m_problem.clear();
        m_failures = 0;
        m_retryAfter.invalidate();
        m_scanTimer.start();
        emit changed();
        scan();
        return {};
    }
    m_enabled = false;
    m_scanTimer.stop();
    stopBridge();
    m_server.close();
    m_game.clear();
    m_prefix.clear();
    m_problem.clear();
    updateActive();
    emit changed();
    return {};
}

void GameMode::scan() {
    if (!m_enabled)
        return;

    // The first running prefix that holds a game with Tobii's library.
    wineprefix::Running found;
    QString library;
    for (const auto &prefix : m_scanner()) {
        if (!prefix.hasGame || prefix.wineBinary.isEmpty())
            continue;
        auto known = m_libraries.constFind(prefix.path);
        if (known == m_libraries.constEnd())
            known = m_libraries.insert(prefix.path, wineprefix::findTobiiLibrary(prefix.path));
        if (known->isEmpty())
            continue;
        found = prefix;
        library = *known;
        break;
    }

    if (found.path != m_prefix) {
        stopBridge();
        m_prefix = found.path;
        m_game = found.path.isEmpty() ? QString() : wineprefix::gameName(library);
        m_failures = 0;
        m_retryAfter.invalidate();
        m_licence = false;
        if (found.path.isEmpty())
            m_problem.clear();
        emit changed();
    }
    updateActive();

    // The bridge needs the tracker's serial number, which arrives once the tracker is open.
    if (m_game.isEmpty() || m_bridge || m_backend->serial().isEmpty())
        return;
    if (m_retryAfter.isValid() && m_retryAfter.elapsed() < kRetryMs)
        return;
    startBridge(found);
}

void GameMode::startBridge(const wineprefix::Running &prefix) {
    const QString bridge = bridgeExecutable();
    if (bridge.isEmpty()) {
        setProblem(tr("The game bridge is not installed; games get gaze only."));
        return;
    }
    // The game's own environment, so the helper runs under exactly the same Wine setup.
    const QByteArray raw = wineprefix::environmentOf(prefix.pid);
    if (raw.isEmpty()) {
        setProblem(tr("Could not read the game's environment."));
        return;
    }
    QProcessEnvironment environment;
    for (const QByteArray &entry : raw.split('\0')) {
        const int equals = entry.indexOf('=');
        if (equals > 0)
            environment.insert(QString::fromLocal8Bit(entry.left(equals)), QString::fromLocal8Bit(entry.mid(equals + 1)));
    }
    environment.insert("WINEPREFIX", prefix.path);
    environment.insert("WINEDEBUG", "-all");

    m_bridge = new QProcess(this);
    m_bridge->setProcessEnvironment(environment);
    m_bridge->setProgram(prefix.wineBinary);
    m_bridge->setArguments({bridge,
                            "-serial", m_backend->serial(),
                            "-display-width", QString::number(int(m_backend->alignedWidth())),
                            "-display-height", QString::number(int(m_backend->alignedHeight())),
                            "-pose-port", QString::number(kPosePort)});
    m_bridge->setStandardErrorFile(QProcess::nullDevice());
    m_bridgeReady = m_headTaken = false;

    QProcess *process = m_bridge;
    auto *lifetime = new QElapsedTimer;
    lifetime->start();
    connect(process, &QProcess::readyReadStandardOutput, this, &GameMode::readBridge);
    connect(process, &QProcess::finished, this, [this, process, lifetime] {
        const bool early = lifetime->elapsed() < kEarlyExitMs;
        delete lifetime;
        process->deleteLater();
        if (process != m_bridge)
            return; // stopped on purpose
        m_bridge = nullptr;
        m_bridgeReady = m_headTaken = false;
        m_heartbeat.stop();
        if (early && ++m_failures >= kMaxFailures) {
            m_retryAfter.start();
            m_failures = 0;
            setProblem(tr("The game bridge keeps stopping; head tracking is off for now."));
        }
        emit changed();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process == m_bridge) {
            m_bridge = nullptr;
            process->deleteLater();
            m_retryAfter.start();
            setProblem(tr("The game bridge could not be started."));
        }
    });
    process->start();
    m_heartbeat.start();
    emit changed();
}

void GameMode::stopBridge() {
    m_heartbeat.stop();
    if (!m_bridge)
        return;
    QProcess *process = m_bridge;
    m_bridge = nullptr;
    m_bridgeReady = m_headTaken = false;
    process->disconnect(this);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    process->terminate();
    QTimer::singleShot(2000, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
}

// The bridge reports what happens to it, one word per line.
void GameMode::readBridge() {
    if (!m_bridge)
        return;
    while (m_bridge->canReadLine()) {
        const QByteArray word = m_bridge->readLine(256).trimmed();
        if (word == "ready") {
            m_bridgeReady = true;
            m_failures = 0;
            if (m_problem == tr("The game bridge keeps stopping; head tracking is off for now."))
                m_problem.clear();
        } else if (word == "headpose") {
            m_headTaken = true;
        } else if (word == "licence") {
            m_licence = true;
        }
    }
    emit changed();
}

QJsonObject GameMode::status() const {
    QString state = "off";
    if (m_enabled)
        state = (m_headTaken || m_server.subscriberCount() > 0) ? "serving" : m_game.isEmpty() ? "waiting" : "starting";
    return {{"enabled", m_enabled},
            {"state", state},
            {"game", m_game},
            {"head", m_headTaken},
            {"gaze", m_server.subscriberCount() > 0},
            {"problem", m_problem}};
}

QByteArray GameMode::posePacket(const eyenix::HeadPose &pose, bool found) {
    QByteArray out(32, '\0');
    auto *raw = reinterpret_cast<uchar *>(out.data());
    std::memcpy(raw, "EYNX", 4);
    const quint32 flags = !found ? 0 : (pose.positionValid ? 1 : 0) | 2;
    qToLittleEndian(flags, raw + 4);
    // Games count x the other way round from the screen's axis.
    const float values[6] = {float(-pose.positionMm.x), float(pose.positionMm.y), float(pose.positionMm.z),
                             float(pose.pitch), float(pose.yaw), float(pose.roll)};
    for (int i = 0; i < 6; ++i) {
        quint32 bits;
        const float value = std::isfinite(values[i]) ? values[i] : 0.0f;
        std::memcpy(&bits, &value, 4);
        qToLittleEndian(bits, raw + 8 + 4 * i);
    }
    return out;
}
