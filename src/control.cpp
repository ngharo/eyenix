#include "control.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>

namespace {

QByteArray line(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'; }

QJsonObject failure(const QString &error) { return {{"ok", false}, {"error", error}}; }

}

Control::Control(Session *session, Backend *backend, QObject *parent)
    : QObject(parent), m_session(session), m_backend(backend) {
    connect(&m_server, &QLocalServer::newConnection, this, &Control::accept);

    // Gaze validity flips with every blink, so gather changes for a moment
    // and send followers a new status only when it really differs.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(200);
    connect(&m_debounce, &QTimer::timeout, this, &Control::publish);
    const auto changed = [this] { if (!m_debounce.isActive()) m_debounce.start(); };
    connect(session, &Session::changed, this, changed);
    connect(backend, &Backend::connectionChanged, this, changed);
    connect(backend, &Backend::noticeChanged, this, changed);
    connect(backend, &Backend::alignmentChanged, this, changed);
    connect(backend, &Backend::calibrationChanged, this, changed);
    connect(backend, &Backend::gazeChanged, this, changed);
}

// Open connections belong to the server and are closed with it. Stop
// listening to them first, or their last signals would reach a half-destroyed
// Control.
Control::~Control() {
    m_server.close();
    const auto sockets = m_server.findChildren<QLocalSocket *>();
    for (QLocalSocket *socket : sockets)
        socket->disconnect(this);
}

QString Control::socketPath() {
    const QString override = qEnvironmentVariable("EYENIX_SOCKET");
    if (!override.isEmpty())
        return override;
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    return runtime + "/eyenix.sock";
}

QJsonObject Control::ask(const QJsonObject &request, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(500))
        return {};
    socket.write(line(request));
    while (!socket.canReadLine())
        if (!socket.waitForReadyRead(timeoutMs))
            return {};
    return QJsonDocument::fromJson(socket.readLine()).object();
}

bool Control::listen() {
    // Only the user may talk to the app.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (m_server.listen(socketPath()))
        return true;
    if (!ask({{"cmd", "ping"}}, 500).isEmpty())
        return false;
    // The socket file is left over from an instance that is gone.
    QLocalServer::removeServer(socketPath());
    return m_server.listen(socketPath());
}

void Control::accept() {
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            m_followers.removeAll(socket);
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            // Requests are small; anything long without a newline is not ours.
            if (!socket->canReadLine()) {
                if (socket->bytesAvailable() > 4096)
                    socket->abort();
                return;
            }
            const QJsonObject request = QJsonDocument::fromJson(socket->readLine(4096)).object();
            if (request.value("cmd") == "status" && request.value("follow").toBool()) {
                m_followers << socket;
                socket->write(line(m_session->status()));
                return;
            }
            const bool quit = request.value("cmd") == "quit";
            socket->write(line(handle(request)));
            socket->flush();
            if (quit)
                emit quitRequested();
        });
    }
}

void Control::publish() {
    const QByteArray status = line(m_session->status());
    if (status == m_published)
        return;
    m_published = status;
    for (QLocalSocket *follower : std::as_const(m_followers))
        follower->write(status);
}

QJsonObject Control::handle(const QJsonObject &request) {
    const QString command = request.value("cmd").toString();
    QString error;

    if (command == "show") {
        m_session->showWindow();
    } else if (command == "overlay") {
        const QString value = request.value("value").toString();
        if (value != "on" && value != "off" && value != "toggle")
            return failure("overlay takes on, off or toggle");
        m_session->setOverlay(value == "toggle" ? !m_session->overlayOn() : value == "on");
    } else if (command == "games") {
        const QString value = request.value("value").toString();
        if (value != "on" && value != "off" && value != "toggle")
            return failure("games takes on, off or toggle");
        error = m_session->setGames(value == "toggle" ? !m_session->gamesOn() : value == "on");
    } else if (command == "wake") {
        m_session->wake();
    } else if (command == "align") {
        error = m_session->startAlign();
    } else if (command == "calibrate") {
        QString profile = request.value("profile").toString();
        if (request.value("new").toBool())
            profile = m_backend->unusedProfileName();
        error = m_session->startCalibration(profile);
    } else if (command == "profile") {
        if (!m_backend->useProfile(request.value("name").toString()))
            error = tr("There is no calibrated profile with that name.");
    } else if (command != "status" && command != "ping" && command != "quit") {
        return failure("unknown command");
    }

    if (!error.isEmpty())
        return failure(error);
    QJsonObject reply = m_session->status();
    reply.insert("ok", true);
    return reply;
}
