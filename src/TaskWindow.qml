import QtQuick
import QtQuick.Controls.Material
import QtQuick.Window
import org.kde.layershell as LayerShell

// The full-screen surface for aligning and calibrating. It is a layer-shell
// surface rather than a full-screen window, so it covers the whole output,
// bars included, whatever workspace or scratchpad the app's window is on.
// Both tasks place things as fractions of the physical screen, so anything
// less than the whole output would put them in the wrong spot.
Window {
    id: task
    visible: true
    color: "#0e0e10"
    flags: Qt.FramelessWindowHint

    Material.theme: Material.Dark
    Material.accent: theme.accent

    readonly property bool aligning: session.mode === "align"

    // Calibration belongs on the screen that was aligned. Alignment itself
    // goes wherever the compositor puts it, which is the screen in use.
    screen: {
        if (!aligning)
            for (const candidate of Qt.application.screens)
                if (candidate.name === backend.screenName)
                    return candidate
        return Qt.application.screens[0]
    }

    LayerShell.Window.scope: "eyenix-task"
    LayerShell.Window.layer: LayerShell.Window.LayerOverlay
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorBottom
                               | LayerShell.Window.AnchorLeft | LayerShell.Window.AnchorRight
    LayerShell.Window.exclusionZone: -1
    LayerShell.Window.keyboardInteractivity: LayerShell.Window.KeyboardInteractivityExclusive
    LayerShell.Window.wantsToBeOnActiveScreen: aligning

    Component.onCompleted: aligning ? alignView.start() : calibrateView.start()

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        AlignView {
            id: alignView
            anchors.fill: parent
            visible: task.aligning
            active: visible
            screenName: task.screen ? task.screen.name : ""
            onFinished: session.finishTask()
        }

        CalibrateView {
            id: calibrateView
            anchors.fill: parent
            visible: session.mode === "calibrate"
            active: visible
            onFinished: session.finishTask()
        }
    }
}
