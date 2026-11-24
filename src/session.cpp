#include "session.h"

#include <QJsonArray>

#include "gamemode.h"

Session::Session(Backend *backend, QObject *parent, int graceMs) : QObject(parent), m_backend(backend) {
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        if (!busy() && !gamesOn())
            emit nothingLeftToShow();
    });
    m_wake.setSingleShot(true);
    connect(&m_wake, &QTimer::timeout, this, &Session::updateTracker);
    // Whatever started us is about to ask for something; have the tracker ready.
    m_wake.start(graceMs);
    // Started in the background, there is nothing to show until the command
    // that started us arrives. Give it a moment before giving up.
    m_idle.start(graceMs);
}

// A short delay, so moving from one thing straight to the next (closing the
// window as a calibration opens) does not look like having nothing to do.
void Session::settle() {
    updateTracker();
    emit changed();
    m_idle.start(300);
}

// The tracker is held while something on screen uses it, a game does, or a
// command that needs it is expected.
void Session::updateTracker() {
    m_backend->setTrackerWanted(busy() || m_wake.isActive() || (m_gameMode && m_gameMode->active()));
}

void Session::wake() {
    m_wake.start(5000);
    updateTracker();
}

void Session::setGameMode(GameMode *gameMode) {
    m_gameMode = gameMode;
    connect(gameMode, &GameMode::changed, this, &Session::changed);
    connect(gameMode, &GameMode::activeChanged, this, &Session::updateTracker);
}

bool Session::gamesOn() const { return m_gameMode && m_gameMode->enabled(); }

QString Session::setGames(bool on) {
    if (!m_gameMode)
        return tr("Game mode is not available.");
    const QString error = m_gameMode->setEnabled(on);
    if (!error.isEmpty())
        return error;
    m_backend->store().setGamesEnabled(on);
    settle();
    return {};
}

void Session::showWindow() {
    if (m_windowVisible)
        return;
    m_windowVisible = true;
    settle();
}

void Session::hideWindow() {
    if (!m_windowVisible)
        return;
    m_windowVisible = false;
    settle();
}

void Session::setOverlay(bool on) {
    if (on == m_overlayOn)
        return;
    m_overlayOn = on;
    settle();
}

QString Session::startAlign() {
    if (m_mode != "home")
        return tr("Finish what is on screen first.");
    m_mode = "align";
    settle();
    return {};
}

QString Session::startCalibration(const QString &profile) {
    if (m_mode != "home")
        return tr("Finish what is on screen first.");
    if (!profile.isEmpty() && !Store::validProfileName(profile))
        return tr("A profile name can have letters, digits, spaces, dashes and underscores.");
    if (!m_backend->connected())
        return m_backend->idle() ? tr("The tracker is waking up. Try again in a moment.")
                                 : tr("Connect the tracker before calibrating.");
    if (!m_backend->aligned())
        return tr("Align the screen before calibrating.");
    m_calibrationProfile = profile;
    m_mode = "calibrate";
    settle();
    return {};
}

void Session::finishTask() {
    if (m_mode == "home")
        return;
    m_mode = "home";
    settle();
}

QJsonObject Session::status() const {
    return {{"running", true},
            {"connected", m_backend->connected()},
            {"device", m_backend->deviceName()},
            {"idle", m_backend->idle()},
            {"problem", m_backend->problem()},
            {"notice", m_backend->notice()},
            {"eyes", m_backend->gazeValid()},
            {"overlay", m_overlayOn},
            {"mode", m_mode},
            {"aligned", m_backend->aligned()},
            {"calibrated", m_backend->calibrated()},
            {"profile", m_backend->profile()},
            {"profiles", QJsonArray::fromStringList(m_backend->profiles())},
            {"games", m_gameMode ? m_gameMode->status()
                                 : QJsonObject{{"enabled", false}, {"state", "off"}, {"game", ""},
                                               {"head", false}, {"gaze", false}, {"problem", ""}}}};
}
