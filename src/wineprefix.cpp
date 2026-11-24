// How running prefixes are recognised follows tobii-faithful v1.1.8
// (src/wine_detect/prefix.cpp, https://github.com/SciFi-Bob/tobii-faithful), Copyright its
// authors, Apache License 2.0 (licenses/Apache-2.0.txt). Rewritten for Qt.
#include "wineprefix.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <sys/stat.h>
#include <unistd.h>

namespace wineprefix {
namespace {

const char kBridgeName[] = "eyenix-bridge";
const QStringList kLibraryNames = {"tobii_gameintegration_x64.dll", "tobii_gameintegration_x86.dll"};
constexpr int kSearchDepth = 8;      // folders below drive_c
constexpr int kSearchFolders = 20000; // give up on a prefix with more than this

QByteArray readProc(const QString &path, qint64 limit = 1 << 20) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(limit) : QByteArray();
}

QString valueOf(const QByteArray &environment, const char *key) {
    const QByteArray needle = QByteArray(key) + '=';
    for (const QByteArray &entry : environment.split('\0'))
        if (entry.startsWith(needle))
            return QString::fromLocal8Bit(entry.mid(needle.size()));
    return {};
}

// Where the wineserver for a prefix keeps its socket.
QString serverDirectory(const QString &prefix) {
    struct stat st {};
    if (::stat(QFile::encodeName(prefix).constData(), &st) != 0)
        return {};
    return QStringLiteral("/tmp/.wine-%1/server-%2-%3")
        .arg(::getuid())
        .arg(qulonglong(st.st_dev), 0, 16)
        .arg(qulonglong(st.st_ino), 0, 16);
}

}

QList<Process> processes() {
    QList<Process> out;
    const auto entries = QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool number = false;
        const qint64 pid = entry.toLongLong(&number);
        if (!number)
            continue;
        const QString dir = "/proc/" + entry;
        const QString prefix = valueOf(readProc(dir + "/environ"), "WINEPREFIX");
        if (prefix.isEmpty())
            continue;
        Process process;
        process.pid = pid;
        process.prefix = QDir::cleanPath(prefix);
        process.executable = QFile::symLinkTarget(dir + "/exe");
        process.commandLine = QString::fromLocal8Bit(readProc(dir + "/cmdline", 8192).replace('\0', ' ')).trimmed();
        out << process;
    }
    return out;
}

QString wineBinaryFor(const QString &executable) {
    // Inside a Flatpak or Steam container the host's files appear under /run/host.
    QString path = executable;
    if (path.startsWith("/run/host/"))
        path.remove(0, int(qstrlen("/run/host")));

    static const QSet<QString> names = {"wine", "wine64", "wineserver", "wine-preloader", "wine64-preloader"};
    if (!names.contains(QFileInfo(path).fileName()))
        return {};
    for (const char *marker : {"/bin/", "/lib/wine/", "/lib64/wine/"}) {
        const int at = path.lastIndexOf(QLatin1String(marker));
        if (at < 0)
            continue;
        for (const char *name : {"wine", "wine64"}) {
            const QString candidate = path.left(at) + "/bin/" + name;
            if (QFileInfo(candidate).isExecutable())
                return candidate;
        }
    }
    return {};
}

bool isGame(const QString &commandLine) {
    static const QStringList services = {
        "wineserver", "services.exe", "winedevice.exe", "plugplay.exe", "explorer.exe", "svchost.exe",
        "rpcss.exe", "lsass.exe", "conhost.exe", "wineboot.exe", "tabtip.exe", "start.exe",
        "winemenubuilder.exe", "rundll32.exe", kBridgeName,
    };
    if (commandLine.isEmpty())
        return false;
    for (const QString &service : services)
        if (commandLine.contains(service, Qt::CaseInsensitive))
            return false;
    return true;
}

QList<Running> group(const QList<Process> &all) {
    QList<Running> out;
    QList<int> ranks; // how good each entry's pid is: 2 a game, 1 some wine process, 0 anything
    for (const Process &process : all) {
        if (process.prefix.isEmpty())
            continue;
        const QString wine = wineBinaryFor(process.executable);
        const bool game = !wine.isEmpty() && isGame(process.commandLine);
        const int rank = (game ? 2 : 0) + (wine.isEmpty() ? 0 : 1);

        int at = 0;
        while (at < out.size() && out[at].path != process.prefix)
            ++at;
        if (at == out.size()) {
            out << Running{process.prefix, process.pid, wine, game};
            ranks << rank;
            continue;
        }
        if (rank > ranks[at]) {
            out[at].pid = process.pid;
            ranks[at] = rank;
        }
        out[at].hasGame = out[at].hasGame || game;
        if (out[at].wineBinary.isEmpty())
            out[at].wineBinary = wine;
    }
    return out;
}

QList<Running> running() {
    QList<Running> out;
    for (const Running &prefix : group(processes())) {
        const QString server = serverDirectory(prefix.path);
        if (!server.isEmpty() && QFileInfo::exists(server))
            out << prefix;
    }
    return out;
}

QByteArray environmentOf(qint64 pid) { return readProc(QStringLiteral("/proc/%1/environ").arg(pid)); }

QString findTobiiLibrary(const QString &prefix) {
    // Level by level, so a library near the top is found before a deep tree is walked.
    QStringList level = {prefix + "/drive_c"};
    int visited = 0;
    for (int depth = 0; depth <= kSearchDepth && !level.isEmpty(); ++depth) {
        QStringList next;
        for (const QString &folder : std::as_const(level)) {
            for (const QString &name : kLibraryNames)
                if (QFileInfo(folder + "/" + name).isFile())
                    return folder + "/" + name;
            if (depth == kSearchDepth)
                continue;
            const auto children = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
            for (const QString &child : children) {
                // Windows itself and the user's documents never hold a game's libraries.
                if (depth == 0 && (child.compare("windows", Qt::CaseInsensitive) == 0
                                   || child.compare("users", Qt::CaseInsensitive) == 0))
                    continue;
                if (++visited > kSearchFolders)
                    return {};
                next << folder + "/" + child;
            }
        }
        level = next;
    }
    return {};
}

QString gameName(const QString &libraryPath) {
    static const QSet<QString> generic = {
        "bin", "bin64", "bin32", "binaries", "win64", "win32", "x64", "x86", "x86_64", "live", "ptu", "eptu",
        "hotfix", "techpreview", "plugins", "thirdparty", "tobii", "game", "release", "retail", "shipping",
    };
    const QStringList parts = QFileInfo(libraryPath).absolutePath().split('/', Qt::SkipEmptyParts);
    QString name;
    for (int i = int(parts.size()) - 1; i >= 0; --i) {
        if (parts[i].compare("drive_c", Qt::CaseInsensitive) == 0)
            break;
        if (!generic.contains(parts[i].toLower())) {
            name = parts[i];
            break;
        }
    }
    if (name.isEmpty())
        return QStringLiteral("Game");
    // "StarCitizen" -> "Star Citizen"; names with spaces already are left alone.
    if (!name.contains(' ')) {
        static const QRegularExpression boundary("(?<=[a-z])(?=[A-Z])");
        name.replace(boundary, " ");
    }
    name.replace('_', ' ');
    // It ends up on screen and in a status line: plain characters only.
    static const QRegularExpression unwanted("[^A-Za-z0-9 .'&-]");
    name.remove(unwanted);
    return name.simplified().left(48);
}

}
