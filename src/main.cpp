// eyenix — set up a Tobii Eye Tracker 5 and see where you are looking.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>
#include <cstdio>
#include <cstring>

#include "backend.h"
#include "cli.h"
#include "control.h"
#include "gamemode.h"
#include "session.h"
#include "theme.h"
#include "translator.h"

static const char kVersion[] = "1.0.0";

static void usage() {
    std::puts("usage: eyenix                 open the app\n"
              "       eyenix dump [options]  print live gaze to the terminal (see eyenix dump --help)\n"
              "       eyenix <command>       drive the running app: overlay, align, calibrate, profile,\n"
              "                              status, quit, plugin (see eyenix status --help)\n"
              "       eyenix --version");
}

int main(int argc, char *argv[]) {
    // Terminal commands are handled before Qt's GUI starts, so they need no display.
    bool background = false;
    if (argc > 1) {
        if (!std::strcmp(argv[1], "dump"))
            return runDump(argc - 1, argv + 1);
        if (isControlCommand(argv[1]))
            return runControl(argc, argv);
        if (!std::strcmp(argv[1], "--version")) {
            std::printf("eyenix %s\n", kVersion);
            return 0;
        }
        // How the terminal commands start the app when it is not running:
        // nothing shows until a command asks for it.
        background = !std::strcmp(argv[1], "--background");
        if (!background) {
            const bool help = !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h");
            usage();
            return help ? 0 : 2;
        }
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("eyenix");
    app.setApplicationVersion(kVersion);
    // The app keeps running with only the gaze overlay up; Session decides when to quit.
    app.setQuitOnLastWindowClosed(false);

    // Matches the window to eyenix.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("eyenix");
    app.setWindowIcon(QIcon::fromTheme("eyenix"));

    // One app at a time, since only one program can hold the tracker. A
    // second start just brings the first one's window up.
    if (!Control::ask({{"cmd", "ping"}}, 500).isEmpty()) {
        if (!background)
            Control::ask({{"cmd", "show"}});
        return 0;
    }

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend(usbDeviceFactory());
    Session session(&backend);
    GameMode gameMode(&backend);
    session.setGameMode(&gameMode);
    // Game mode stays on across restarts until it is turned off.
    if (backend.store().gamesEnabled())
        session.setGames(true);
    Control control(&session, &backend);
    if (!control.listen())
        return 0; // another instance won the race to start
    // exit() rather than quit(): quit() first asks every window to close, and
    // the main window answers a close by hiding, which would cancel the quit.
    const auto leave = [] { QCoreApplication::exit(0); };
    QObject::connect(&session, &Session::nothingLeftToShow, &app, leave);
    QObject::connect(&session, &Session::quitRequested, &app, leave, Qt::QueuedConnection);
    QObject::connect(&control, &Control::quitRequested, &app, leave, Qt::QueuedConnection);
    if (!background)
        session.showWindow();

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("session", &session);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
