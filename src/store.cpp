#include "store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

const char kDefaultProfile[] = "Default";

// Lines closer together than this cannot be the tracker's marks on any real
// screen, and would make the screen absurdly large.
const double kMinimumSpacingPx = 40;

}

Store::Store(const QString &directory)
    : m_directory(directory.isEmpty()
                      ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/eyenix"
                      : directory) {
    adoptOldCalibration();
}

// Before profiles existed there was a single calibration.bin. It becomes the
// Default profile. A rename either happens completely or not at all, so the
// calibration cannot be lost halfway.
void Store::adoptOldCalibration() {
    const QString old = m_directory + "/calibration.bin";
    if (!QFileInfo::exists(old) || hasCalibration(kDefaultProfile))
        return;
    if (QDir().mkpath(m_directory + "/profiles"))
        QFile::rename(old, profilePath(kDefaultProfile));
}

bool Store::validProfileName(const QString &name) {
    static const QRegularExpression valid("^[A-Za-z0-9][A-Za-z0-9 _-]{0,31}$");
    return valid.match(name).hasMatch() && name == name.trimmed();
}

QString Store::profilePath(const QString &name) const { return m_directory + "/profiles/" + name + ".bin"; }

// Written beside the target and renamed over it, so a crash never leaves half a file.
bool Store::write(const QString &relativePath, const QByteArray &data) {
    const QString path = m_directory + "/" + relativePath;
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

bool Store::loadAlignment(eyenix::Alignment &alignment, QString &screenName) const {
    QFile file(m_directory + "/alignment.json");
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    eyenix::Alignment loaded;
    loaded.screenWidthPx = json.value("widthPx").toDouble();
    loaded.screenHeightPx = json.value("heightPx").toDouble();
    loaded.leftMarkPx = json.value("leftMarkPx").toDouble();
    loaded.rightMarkPx = json.value("rightMarkPx").toDouble();
    if (loaded.rightMarkPx - loaded.leftMarkPx < kMinimumSpacingPx || loaded.screenWidthPx <= 0
        || loaded.screenHeightPx <= 0)
        return false;
    alignment = loaded;
    screenName = json.value("screen").toString();
    return true;
}

bool Store::saveAlignment(const eyenix::Alignment &alignment, const QString &screenName) {
    const QJsonObject json{{"screen", screenName},
                           {"widthPx", alignment.screenWidthPx},
                           {"heightPx", alignment.screenHeightPx},
                           {"leftMarkPx", alignment.leftMarkPx},
                           {"rightMarkPx", alignment.rightMarkPx}};
    return write("alignment.json", QJsonDocument(json).toJson());
}

QStringList Store::profiles() const {
    QStringList names;
    const auto files = QDir(m_directory + "/profiles").entryList({"*.bin"}, QDir::Files, QDir::Name);
    for (const QString &file : files) {
        const QString name = file.chopped(4);
        if (validProfileName(name))
            names << name;
    }
    const QString current = profile();
    if (!names.contains(current))
        names << current;
    names.sort(Qt::CaseInsensitive);
    return names;
}

QJsonObject Store::settings() const {
    QFile file(m_directory + "/settings.json");
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// Changes one setting and leaves the others as they are.
bool Store::setSetting(const QString &key, const QJsonValue &value) {
    QJsonObject all = settings();
    all.insert(key, value);
    return write("settings.json", QJsonDocument(all).toJson());
}

bool Store::gamesEnabled() const { return settings().value("games").toBool(); }

bool Store::setGamesEnabled(bool enabled) { return setSetting("games", enabled); }

QString Store::profile() const {
    {
        const QString name = settings().value("profile").toString();
        if (validProfileName(name))
            return name;
    }
    // Nothing chosen yet: Default if it exists or nothing does, else whichever comes first.
    if (hasCalibration(kDefaultProfile))
        return kDefaultProfile;
    const auto files = QDir(m_directory + "/profiles").entryList({"*.bin"}, QDir::Files, QDir::Name);
    for (const QString &file : files)
        if (validProfileName(file.chopped(4)))
            return file.chopped(4);
    return kDefaultProfile;
}

bool Store::setProfile(const QString &name) {
    if (!validProfileName(name))
        return false;
    return setSetting("profile", name);
}

QString Store::unusedProfileName() const {
    const QStringList taken = profiles();
    for (int n = 2;; ++n) {
        const QString name = QString("Profile %1").arg(n);
        if (!taken.contains(name, Qt::CaseInsensitive))
            return name;
    }
}

bool Store::hasCalibration(const QString &profile) const {
    return validProfileName(profile) && QFileInfo::exists(profilePath(profile));
}

QByteArray Store::calibration(const QString &profile) const {
    QFile file(profilePath(profile));
    return validProfileName(profile) && file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool Store::saveCalibration(const QString &profile, const QByteArray &blob) {
    return validProfileName(profile) && write("profiles/" + profile + ".bin", blob);
}
