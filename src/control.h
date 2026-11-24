#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include "session.h"

// How the terminal commands and the bar plugin reach the running app: a
// socket in the user's runtime directory carrying one JSON object per line.
// A request gets one reply, except {"cmd":"status","follow":true}, which gets
// the status again every time it changes.
class Control : public QObject {
    Q_OBJECT

public:
    Control(Session *session, Backend *backend, QObject *parent = nullptr);
    ~Control() override;

    // $XDG_RUNTIME_DIR/eyenix.sock, or $EYENIX_SOCKET when set (tests use it).
    static QString socketPath();
    // Sends one request to a running app and waits for the reply. An empty
    // object means no app answered.
    static QJsonObject ask(const QJsonObject &request, int timeoutMs = 3000);

    // False when another instance already owns the socket.
    bool listen();

    QJsonObject handle(const QJsonObject &request);

signals:
    void quitRequested();

private:
    void accept();
    void publish();

    Session *m_session;
    Backend *m_backend;
    QLocalServer m_server;
    QList<QLocalSocket *> m_followers;
    QTimer m_debounce;
    QByteArray m_published;
};
