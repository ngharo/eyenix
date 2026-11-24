#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Finding games that run under Wine or Proton, so the bridge can be started
// next to them. Everything here only looks at the user's own processes and
// files.
namespace wineprefix {

// A process that has WINEPREFIX in its environment.
struct Process {
    qint64 pid = 0;
    QString prefix;      // the WINEPREFIX it runs in
    QString executable;  // what /proc/<pid>/exe points at
    QString commandLine; // arguments joined by spaces
};

// A Wine prefix with something running in it.
struct Running {
    QString path;
    qint64 pid = 0;      // a process to copy the environment from; a game's if there is one
    QString wineBinary;  // the wine that prefix is being run with, or empty if unknown
    bool hasGame = false; // something other than Wine's own services is running
};

// Every process of this user that runs in a Wine prefix.
QList<Process> processes();

// The wine binary belonging to a process's executable, if it is part of a
// Wine installation: "<root>/bin/wine" for ".../bin/wine-preloader" and the like.
QString wineBinaryFor(const QString &executable);

// False for Wine's own services (wineserver, explorer.exe, …) and for our bridge.
bool isGame(const QString &commandLine);

QList<Running> group(const QList<Process> &processes);

// processes() grouped, keeping only prefixes whose wineserver is up.
QList<Running> running();

// The environment of a process, for starting another one beside it. Empty if unreadable.
QByteArray environmentOf(qint64 pid);

// Tobii's game integration library inside the prefix, or an empty string.
// Looks through drive_c a limited number of folders deep.
QString findTobiiLibrary(const QString &prefix);

// A name for the game the library belongs to, from the folders above it:
// ".../StarCitizen/LIVE/Bin64/tobii_gameintegration_x64.dll" -> "Star Citizen".
QString gameName(const QString &libraryPath);

}
