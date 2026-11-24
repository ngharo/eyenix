// Prints tracker information and live gaze. Reconnects if the tracker is replugged.
#include "cli.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <memory>
#include <optional>

#include "eyenix/headpose.hpp"
#include "eyenix/tracker.hpp"
#include "models.h"
#include "store.h"

using namespace eyenix;

static volatile std::sig_atomic_t stop = 0;

static void usage() {
    std::puts("usage: eyenix dump [--seconds N] [--once] [--head]\n"
              "  --seconds N   stop after N seconds (default: run until Ctrl-C)\n"
              "  --head        print head position and rotation instead of gaze\n"
              "  --once        exit instead of waiting for the tracker to come back");
}

// Head pose relative to the aligned screen, one line per camera frame.
static void headSession(Tracker& t, double seconds) {
    HeadTracker head(modelDirectory().toStdString()); // runDump has checked the models exist

    Alignment alignment;
    QString screen;
    if (Store().loadAlignment(alignment, screen)) {
        const auto area = displayAreaFromAlignment(alignment);
        t.setDisplayArea(area);
        head.setDisplayArea(area);
        std::printf("# measuring against the aligned screen %s\n", qPrintable(screen));
    } else {
        head.setDisplayArea(t.displayArea());
        std::puts("# no alignment saved (run `eyenix align`); using the tracker's default screen");
    }
    std::puts("# time_s | position: valid x y z mm from screen centre | pitch yaw roll deg | face 0..1 | ms spent");

    t.startGaze();
    t.startImages();
    auto begin = std::chrono::steady_clock::now();
    std::uint64_t first = 0;
    bool lost = false;
    while (!stop) {
        if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() >= seconds) {
            stop = 1;
            break;
        }
        auto s = t.readGaze(250);
        if (!s) continue;
        head.onGaze(*s);
        auto frame = t.takeImage();
        if (!frame) continue;
        auto before = std::chrono::steady_clock::now();
        auto pose = head.onImage(*frame);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
        if (!pose) {
            if (!lost) std::puts("# no face in view");
            lost = true;
            std::fflush(stdout);
            continue;
        }
        lost = false;
        if (!first) first = pose->timestampUs;
        constexpr double deg = 57.29577951308232;
        std::printf("%8.3f | %d %7.1f %7.1f %7.1f | %6.1f %6.1f %6.1f | %.2f | %.1f\n",
                    (pose->timestampUs - first) / 1e6, pose->positionValid, pose->positionMm.x, pose->positionMm.y,
                    pose->positionMm.z, pose->pitch * deg, pose->yaw * deg, pose->roll * deg, pose->presence, ms);
        std::fflush(stdout);
    }
    t.stopImages();
    t.stopGaze();
}

static void session(double seconds, bool headPose) {
    Tracker t;
    auto strings = t.deviceStrings();
    std::printf("# connected:");
    for (std::size_t i = 0; i < strings.size() && i < 2; ++i) std::printf(" %s", strings[i].c_str());
    std::printf("\n# device clock: %.3f s\n", t.clockUs() / 1e6);
    auto d = t.displayArea();
    std::printf("# display area mm: top-left (%.1f, %.1f, %.1f) top-right (%.1f, %.1f, %.1f) bottom-left (%.1f, %.1f, %.1f)\n",
                d.topLeft.x, d.topLeft.y, d.topLeft.z, d.topRight.x, d.topRight.y, d.topRight.z,
                d.bottomLeft.x, d.bottomLeft.y, d.bottomLeft.z);
    if (headPose) {
        headSession(t, seconds);
        return;
    }
    std::puts("# time_s valid gaze_x gaze_y | left: valid x y z mm | right: valid x y z mm");

    t.startGaze();
    auto begin = std::chrono::steady_clock::now();
    std::uint64_t first = 0;
    while (!stop) {
        if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() >= seconds) {
            stop = 1;
            break;
        }
        auto s = t.readGaze(250);
        if (!s) continue;
        if (!first) first = s->timestampUs;
        std::printf("%8.3f %d %6.3f %6.3f | %d %7.1f %7.1f %7.1f | %d %7.1f %7.1f %7.1f\n",
                    (s->timestampUs - first) / 1e6, s->valid, s->gaze.x, s->gaze.y,
                    s->left.originValid, s->left.originMm.x, s->left.originMm.y, s->left.originMm.z,
                    s->right.originValid, s->right.originMm.x, s->right.originMm.y, s->right.originMm.z);
        std::fflush(stdout);
    }
    t.stopGaze();
}

int runDump(int argc, char *argv[]) {
    double seconds = 0;
    bool once = false, headPose = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--once")) once = true;
        else if (!std::strcmp(argv[i], "--head")) headPose = true;
        else { usage(); return std::strcmp(argv[i], "--help") ? 2 : 0; }
    }
    if (headPose && modelDirectory().isEmpty()) {
        std::fputs("eyenix: the face models are not installed (see tools/convert_models.py)\n", stderr);
        return 1;
    }
    std::signal(SIGINT, [](int) { stop = 1; });
    std::signal(SIGTERM, [](int) { stop = 1; });

    bool waiting = false;
    while (!stop) {
        try {
            session(seconds, headPose);
            waiting = false;
        } catch (const DeviceError& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        } catch (const std::exception& e) {
            if (once) { std::fprintf(stderr, "error: %s\n", e.what()); return 1; }
            if (!waiting) std::fprintf(stderr, "# %s; waiting for the tracker...\n", e.what());
            waiting = true;
            for (int i = 0; i < 10 && !stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Commands that drive the running app.

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <unistd.h>

#include "control.h"
#include "store.h"

namespace {

const char kPluginId[] = "ngharo.eyenix";

void controlUsage() {
    std::puts("usage: eyenix overlay on|off|toggle     show or hide the gaze point over the desktop\n"
              "       eyenix align                     align the screen with the tracker\n"
              "       eyenix calibrate                 calibrate the current profile\n"
              "       eyenix calibrate --profile NAME  calibrate that profile, creating it if new\n"
              "       eyenix calibrate --new           calibrate a new profile\n"
              "       eyenix games on|off|toggle       let games that support Tobii trackers use this one\n"
              "       eyenix profile list              list calibration profiles\n"
              "       eyenix profile use NAME          switch profile\n"
              "       eyenix status [--follow]         print the app's state as JSON, again on every change\n"
              "       eyenix quit                      stop the app\n"
              "       eyenix plugin install [--enable] install the Omarchy bar plugin");
}

int fail(const QString &message) {
    std::fprintf(stderr, "eyenix: %s\n", qPrintable(message));
    return 1;
}

void printJson(const QJsonObject &object) {
    std::puts(QJsonDocument(object).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}

// What can be said about Eyenix while the app is not running.
QJsonObject offlineStatus() {
    const Store store;
    eyenix::Alignment alignment;
    QString screen;
    const QString profile = store.profile();
    return {{"running", false},
            {"connected", false},
            {"device", ""},
            {"problem", ""},
            {"notice", ""},
            {"eyes", false},
            {"overlay", false},
            {"mode", "home"},
            {"aligned", store.loadAlignment(alignment, screen)},
            {"calibrated", store.hasCalibration(profile)},
            {"profile", profile},
            {"profiles", QJsonArray::fromStringList(store.profiles())},
            {"idle", false},
            // "enabled" is the saved choice: it applies again the next time the app starts.
            {"games", QJsonObject{{"enabled", store.gamesEnabled()}, {"state", "off"}, {"game", ""},
                                  {"head", false}, {"gaze", false}, {"problem", ""}}}};
}

// Starts the app with nothing showing and waits until it answers.
bool startApp() {
    if (!Control::ask({{"cmd", "ping"}}, 500).isEmpty())
        return true;
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {"--background"}))
        return false;
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 5000) {
        QThread::msleep(50);
        if (!Control::ask({{"cmd", "ping"}}, 500).isEmpty())
            return true;
    }
    return false;
}

// A freshly started app needs a moment to find the tracker. Wait until it
// has, or says why it can't, so a command is not refused for being early.
void waitForTracker() {
    Control::ask({{"cmd", "wake"}}); // it may have been let go while only game mode was on
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 3000) {
        const QJsonObject status = Control::ask({{"cmd", "status"}});
        if (status.value("connected").toBool() || !status.value("problem").toString().isEmpty())
            return;
        QThread::msleep(50);
    }
}

int send(const QJsonObject &request) {
    const QJsonObject reply = Control::ask(request);
    if (reply.isEmpty())
        return fail("the app did not answer");
    if (!reply.value("ok").toBool())
        return fail(reply.value("error").toString());
    return 0;
}

int follow() {
    QByteArray printed;
    const auto print = [&printed](const QByteArray &status) {
        if (status == printed)
            return;
        printed = status;
        std::puts(status.constData());
        std::fflush(stdout);
    };

    // The bar plugin runs this for as long as the shell lives. If the shell
    // goes away we are handed to another parent; stop then, rather than
    // linger with nobody reading.
    const pid_t parent = getppid();
    const auto orphaned = [parent] { return getppid() != parent; };

    while (!orphaned()) {
        QLocalSocket socket;
        socket.connectToServer(Control::socketPath());
        if (socket.waitForConnected(500)) {
            socket.write("{\"cmd\":\"status\",\"follow\":true}\n");
            while (socket.state() == QLocalSocket::ConnectedState && !orphaned()) {
                socket.waitForReadyRead(1000);
                while (socket.canReadLine())
                    print(socket.readLine().trimmed());
            }
            continue;
        }
        // Not running: say so, and keep looking for it.
        print(QJsonDocument(offlineStatus()).toJson(QJsonDocument::Compact));
        QThread::msleep(1000);
    }
    return 0;
}

int installPlugin(bool enable) {
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omarchy";
    if (!QFileInfo(config).isDir())
        return fail(QString("%1 does not exist; is this an Omarchy system?").arg(config));
    const QString target = config + "/plugins/" + kPluginId;

    QDirIterator files(":/plugin", QDir::Files, QDirIterator::Subdirectories);
    int count = 0;
    while (files.hasNext()) {
        const QString source = files.next();
        const QString path = target + source.mid(int(std::strlen(":/plugin")));
        QFile in(source);
        QSaveFile out(path);
        if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !in.open(QIODevice::ReadOnly)
            || !out.open(QIODevice::WriteOnly) || out.write(in.readAll()) < 0 || !out.commit())
            return fail(QString("could not write %1").arg(path));
        ++count;
    }
    if (count == 0)
        return fail("this build carries no plugin files");

    // The plugin runs whichever eyenix installed it, so a build that is not
    // on the PATH works too.
    QSaveFile command(target + "/command");
    const QByteArray self = QCoreApplication::applicationFilePath().toUtf8() + '\n';
    if (!command.open(QIODevice::WriteOnly) || command.write(self) < 0 || !command.commit())
        return fail(QString("could not write %1/command").arg(target));

    std::printf("Installed the plugin in %s\n", qPrintable(target));
    if (!enable) {
        std::printf("Add it to the bar with: omarchy plugin enable %s\n", kPluginId);
        return 0;
    }
    // The shell has to notice the new folder before it will enable it.
    QProcess::execute("omarchy-shell", {"shell", "rescanPlugins"});
    for (int attempt = 0; attempt < 5; ++attempt) {
        QThread::msleep(500);
        QProcess enabling;
        enabling.start("omarchy", {"plugin", "enable", kPluginId});
        if (enabling.waitForFinished(10000) && enabling.exitStatus() == QProcess::NormalExit && enabling.exitCode() == 0) {
            std::printf("Added it to the bar. Remove it with: omarchy plugin disable %s\n", kPluginId);
            return 0;
        }
    }
    return fail(QString("the plugin is installed but could not be enabled; try: omarchy plugin enable %1").arg(kPluginId));
}

}

bool isControlCommand(const char *name) {
    for (const char *known : {"overlay", "games", "align", "calibrate", "profile", "status", "quit", "plugin"})
        if (!std::strcmp(name, known))
            return true;
    return false;
}

int runControl(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    const QString command = args.value(0), first = args.value(1), second = args.value(2);

    if (args.contains("--help") || args.contains("-h")) {
        controlUsage();
        return 0;
    }

    if (command == "overlay" && (first == "on" || first == "off" || first == "toggle") && args.size() == 2) {
        // Turning it off when nothing is running is already done.
        if (first == "off" && Control::ask({{"cmd", "ping"}}, 500).isEmpty())
            return 0;
        if (!startApp())
            return fail("could not start the app");
        return send({{"cmd", "overlay"}, {"value", first}});
    }

    if (command == "games" && (first == "on" || first == "off" || first == "toggle") && args.size() == 2) {
        if (first == "off" && Control::ask({{"cmd", "ping"}}, 500).isEmpty())
            return Store().setGamesEnabled(false) ? 0 : fail("could not save the choice");
        if (!startApp())
            return fail("could not start the app");
        return send({{"cmd", "games"}, {"value", first}});
    }

    if (command == "align" && args.size() == 1) {
        if (!startApp())
            return fail("could not start the app");
        return send({{"cmd", "align"}});
    }

    if (command == "calibrate") {
        QJsonObject request{{"cmd", "calibrate"}};
        if (first == "--new" && args.size() == 2) {
            request.insert("new", true);
        } else if (first == "--profile" && args.size() == 3) {
            if (!Store::validProfileName(second))
                return fail("a profile name can have letters, digits, spaces, dashes and underscores, up to 32");
            request.insert("profile", second);
        } else if (args.size() != 1) {
            controlUsage();
            return 2;
        }
        if (!startApp())
            return fail("could not start the app");
        waitForTracker();
        return send(request);
    }

    if (command == "profile" && first == "list" && args.size() == 2) {
        const Store store;
        const QString current = store.profile();
        for (const QString &name : store.profiles())
            std::printf("%s %s\n", name == current ? "*" : " ", qPrintable(name));
        return 0;
    }

    if (command == "profile" && first == "use" && args.size() == 3) {
        if (!Control::ask({{"cmd", "ping"}}, 500).isEmpty())
            return send({{"cmd", "profile"}, {"name", second}});
        Store store;
        if (!store.hasCalibration(second))
            return fail("there is no calibrated profile with that name");
        return store.setProfile(second) ? 0 : fail("could not save the profile choice");
    }

    if (command == "status" && args.size() == 1) {
        const QJsonObject status = Control::ask({{"cmd", "status"}});
        printJson(status.isEmpty() ? offlineStatus() : status);
        return 0;
    }
    if (command == "status" && first == "--follow" && args.size() == 2)
        return follow();

    if (command == "quit" && args.size() == 1) {
        Control::ask({{"cmd", "quit"}});
        return 0;
    }

    if (command == "plugin" && first == "install" && (args.size() == 2 || (args.size() == 3 && second == "--enable")))
        return installPlugin(args.size() == 3);

    controlUsage();
    return 2;
}
