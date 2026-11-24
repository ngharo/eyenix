#pragma once

#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include "backend.h"

class GameMode;

// What the app is showing: the main window, the gaze overlay, and the
// full-screen alignment or calibration. The window, the terminal commands and
// the bar plugin all drive it through here, so they always agree.
//
// The app only runs while it has something to do. With the window closed,
// the overlay off, no task open and game mode off, it asks to quit, which
// also releases the tracker and lets it go idle.
//
// With only game mode on, the app stays but lets go of the tracker until a
// game that can use it is running.
class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool windowVisible READ windowVisible NOTIFY changed)
    Q_PROPERTY(bool overlayOn READ overlayOn NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QString calibrationProfile READ calibrationProfile NOTIFY changed)
    Q_PROPERTY(bool gamesOn READ gamesOn NOTIFY changed)

public:
    // graceMs is how long a background start may wait for its first command.
    explicit Session(Backend *backend, QObject *parent = nullptr, int graceMs = 5000);

    // Optional. Without it, game mode cannot be turned on.
    void setGameMode(GameMode *gameMode);
    bool gamesOn() const;
    // Turns game mode on or off and remembers the choice. Returns why it could not, or an empty string.
    Q_INVOKABLE QString setGames(bool on);
    // Asks for the tracker for a few seconds, so a command that needs it can follow.
    Q_INVOKABLE void wake();

    bool windowVisible() const { return m_windowVisible; }
    bool overlayOn() const { return m_overlayOn; }
    QString mode() const { return m_mode; } // "home", "align" or "calibrate"
    QString calibrationProfile() const { return m_calibrationProfile; }

    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void setOverlay(bool on);
    // Both return why they could not start, or an empty string.
    Q_INVOKABLE QString startAlign();
    Q_INVOKABLE QString startCalibration(const QString &profile = {});
    Q_INVOKABLE void finishTask();
    // Stops the app whatever is showing.
    Q_INVOKABLE void quit() { emit quitRequested(); }

    // Everything the bar plugin shows, as one object.
    QJsonObject status() const;

signals:
    void changed();
    void nothingLeftToShow();
    void quitRequested();

private:
    void settle();
    void updateTracker();
    bool busy() const { return m_windowVisible || m_overlayOn || m_mode != "home"; }

    GameMode *m_gameMode = nullptr;
    QTimer m_wake;

    Backend *m_backend;
    bool m_windowVisible = false, m_overlayOn = false;
    QString m_mode = "home", m_calibrationProfile;
    QTimer m_idle;
};
