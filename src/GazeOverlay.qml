import QtQuick
import QtQuick.Window
import org.kde.layershell as LayerShell

// A see-through window over the whole screen that draws where the user is
// looking. It sits on the compositor's overlay layer and takes no input, so
// clicks and keys go to whatever is underneath.
Window {
    id: overlay
    visible: true
    color: "transparent"
    // The screen that was aligned, since that is the one the tracker knows about.
    screen: {
        for (const candidate of Qt.application.screens)
            if (candidate.name === backend.screenName)
                return candidate
        return Qt.application.screens[0]
    }
    flags: Qt.FramelessWindowHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus

    LayerShell.Window.scope: "eyenix-gaze"
    LayerShell.Window.layer: LayerShell.Window.LayerOverlay
    LayerShell.Window.anchors: LayerShell.Window.AnchorTop | LayerShell.Window.AnchorBottom
                               | LayerShell.Window.AnchorLeft | LayerShell.Window.AnchorRight
    // -1 covers bars and panels too, so the window matches the aligned screen.
    LayerShell.Window.exclusionZone: -1
    LayerShell.Window.keyboardInteractivity: LayerShell.Window.KeyboardInteractivityNone

    Rectangle {
        width: 72
        height: 72
        radius: 36
        x: backend.gazeX * overlay.width - width / 2
        y: backend.gazeY * overlay.height - height / 2
        color: Qt.rgba(border.color.r, border.color.g, border.color.b, 0.18)
        border.color: theme.accent
        border.width: 3
        opacity: backend.connected && backend.gazeValid ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        // Samples arrive about 33 times a second; gliding between them hides the steps.
        Behavior on x { NumberAnimation { duration: 30 } }
        Behavior on y { NumberAnimation { duration: 30 } }
    }
}
