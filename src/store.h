#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "eyenix/display.hpp"

// What Eyenix keeps on disk, in one directory:
//
//   alignment.json        where the screen is relative to the tracker
//   settings.json         which calibration profile is in use; whether game mode is on
//   profiles/<name>.bin   one calibration per profile
//
// Both the app and the terminal commands read it, so the bar plugin can list
// profiles and switch between them while the app is not running.
class Store {
public:
    // An empty directory means the app's config directory (~/.config/eyenix).
    explicit Store(const QString &directory = {});

    QString directory() const { return m_directory; }

    // Letters, digits, spaces, dashes and underscores, at most 32 characters.
    // Names become file names and travel through the bar plugin, so nothing
    // else is accepted.
    static bool validProfileName(const QString &name);

    bool loadAlignment(eyenix::Alignment &alignment, QString &screenName) const;
    bool saveAlignment(const eyenix::Alignment &alignment, const QString &screenName);

    // Calibrated profiles in alphabetical order, plus the current one if it
    // has not been calibrated yet. Never empty.
    QStringList profiles() const;
    QString profile() const;
    bool setProfile(const QString &name);
    // The first of "Profile 2", "Profile 3", … that is not taken.
    QString unusedProfileName() const;

    // Whether Eyenix should serve games (see GameMode). Off until turned on.
    bool gamesEnabled() const;
    bool setGamesEnabled(bool enabled);

    bool hasCalibration(const QString &profile) const;
    QByteArray calibration(const QString &profile) const;
    bool saveCalibration(const QString &profile, const QByteArray &blob);

private:
    QString profilePath(const QString &name) const;
    QJsonObject settings() const;
    bool setSetting(const QString &key, const QJsonValue &value);
    bool write(const QString &relativePath, const QByteArray &data);
    void adoptOldCalibration();

    QString m_directory;
};
