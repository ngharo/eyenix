import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQml.Models

// The app has up to three windows, all driven by `session`: the main window,
// the gaze overlay, and a full-screen surface for aligning and calibrating.
// Any of them can be up without the others, which is how the bar plugin
// starts a calibration or shows the gaze point with no main window.
//
// The other two are created by Instantiators hanging off a plain QtObject.
// Declared inside the main window, or inside any Item, Qt would tie them to
// that window and refuse to show them while it is hidden.
QtObject {
    id: app

    readonly property Component overlayComponent: Qt.createComponent("qrc:/GazeOverlay.qml")
    readonly property Component taskComponent: Qt.createComponent("qrc:/TaskWindow.qml")
    // Both need the layer-shell-qt package; without it they fail to load.
    readonly property bool layerShell: overlayComponent.status === Component.Ready
                                       && taskComponent.status === Component.Ready

    // The gaze point drawn over the whole desktop. It is its own window on the
    // compositor's overlay layer, so it stays up while other apps are in use.
    readonly property Instantiator overlayWindow: Instantiator {
        active: app.layerShell && session.overlayOn
        delegate: app.overlayComponent
    }

    // Aligning and calibrating are about where things are on the physical
    // display, so they cover all of it.
    readonly property Instantiator taskWindow: Instantiator {
        active: app.layerShell && session.mode !== "home"
        delegate: app.taskComponent
    }

    // Asked for something that cannot be shown: undo it and say why.
    readonly property Connections guard: Connections {
        target: session
        function onChanged() {
            if (app.layerShell || (!session.overlayOn && session.mode === "home"))
                return
            session.setOverlay(false)
            session.finishTask()
            win.hint = qsTr("The gaze point, aligning and calibrating need the layer-shell-qt package.")
            session.showWindow()
        }
    }

    readonly property ApplicationWindow window: ApplicationWindow {

    id: win
    width: 760
    height: 680
    minimumWidth: 640
    minimumHeight: 500
    visible: session.windowVisible
    title: "Eyenix"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property string hint: ""
    readonly property bool home: session.mode === "home"

    // Closing the window leaves the overlay up, if it is on. Q quits everything.
    onClosing: (close) => { close.accepted = false; session.hideWindow() }

    function number(value, decimals) {
        // Reading the language makes bindings that call this re-render on a switch.
        translator.language
        return Number(value).toLocaleString(Qt.locale(), 'f', decimals)
    }

    function nextProfile() {
        const names = backend.profiles
        const next = names[(names.indexOf(backend.profile) + 1) % names.length]
        hint = next === backend.profile ? qsTr("There is only one profile. N calibrates a new one.") : ""
        backend.useProfile(next)
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: win.home; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: win.home; onActivated: session.quit() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: win.home; onActivated: win.hint = session.startAlign() }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: win.home; onActivated: win.hint = session.startCalibration() }
    Shortcut {
        sequence: "N"; context: Qt.ApplicationShortcut; enabled: win.home
        onActivated: win.hint = session.startCalibration(backend.unusedProfileName())
    }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.home; onActivated: win.nextProfile() }
    Shortcut {
        sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.home
        onActivated: { win.hint = ""; session.setOverlay(!session.overlayOn) }
    }
    Shortcut {
        sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible && win.home
        onActivated: win.helpVisible = false
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 560)
            spacing: 22

            Text {
                text: "Eyenix"
                color: "#f2f2f5"
                font.pixelSize: 34
                font.bold: true
            }

            RowLayout {
                spacing: 10
                Rectangle {
                    width: 10; height: 10; radius: 5
                    color: backend.connected ? "#4ade80" : "#f87171"
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#b9b9c4"
                    font.pixelSize: 15
                    text: backend.connected ? backend.deviceName
                        : backend.problem === "" ? qsTr("Looking for the tracker…")
                        : qsTr("No tracker: %1").arg(backend.problem)
                }
            }

            Text {
                visible: text !== ""
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: "#fbbf24"
                font.pixelSize: 15
                text: win.hint !== "" ? win.hint : backend.notice
            }

            GridLayout {
                columns: 3
                columnSpacing: 16
                rowSpacing: 14
                Layout.fillWidth: true

                KeyCap { text: "A" }
                Text { text: qsTr("Align the screen"); color: "#f2f2f5"; font.pixelSize: 17; Layout.fillWidth: true }
                Text {
                    color: backend.aligned ? "#b9b9c4" : "#fbbf24"
                    font.pixelSize: 15
                    text: {
                        if (!backend.aligned)
                            return qsTr("Not done")
                        const size = backend.screenSize(backend.markLeft, backend.markRight,
                                                        backend.alignedWidth, backend.alignedHeight)
                        return qsTr("%1 × %2 mm").arg(win.number(size.widthMm, 0)).arg(win.number(size.heightMm, 0))
                    }
                }

                KeyCap { text: "C" }
                Text { text: qsTr("Calibrate"); color: "#f2f2f5"; font.pixelSize: 17; Layout.fillWidth: true }
                Text {
                    color: backend.calibrated ? "#b9b9c4" : "#fbbf24"
                    font.pixelSize: 15
                    text: backend.calibrated ? qsTr("Saved") : qsTr("Not done")
                }

                KeyCap { text: "P" }
                Text { text: qsTr("Switch profile"); color: "#f2f2f5"; font.pixelSize: 17; Layout.fillWidth: true }
                Text {
                    textFormat: Text.PlainText
                    color: "#b9b9c4"
                    font.pixelSize: 15
                    text: backend.profile
                }

                KeyCap { text: "Space" }
                Text { text: qsTr("Show gaze on screen"); color: "#f2f2f5"; font.pixelSize: 17; Layout.fillWidth: true }
                Text {
                    color: session.overlayOn ? theme.accent : "#b9b9c4"
                    font.pixelSize: 15
                    text: session.overlayOn ? qsTr("On") : qsTr("Off")
                }
            }

            Text {
                Layout.fillWidth: true
                color: "#8a8a96"
                font.pixelSize: 14
                text: !backend.connected ? ""
                    : backend.gazeValid ? qsTr("Looking at %1, %2 from %3 cm away")
                          .arg(win.number(backend.gazeX, 2)).arg(win.number(backend.gazeY, 2))
                          .arg(win.number(backend.distanceCm, 0))
                    : qsTr("No eyes in view")
            }

            RowLayout {
                spacing: 10
                KeyCap { text: "?" }
                Text { text: qsTr("Shortcuts"); color: "#8a8a96"; font.pixelSize: 14 }
                Item { width: 12 }
                KeyCap { text: "Q" }
                Text { text: qsTr("Quit"); color: "#8a8a96"; font.pixelSize: 14 }
            }
        }

        // Every shortcut, over whatever is showing.
        Rectangle {
            visible: win.helpVisible
            anchors.fill: parent
            color: "#e6000000"

            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: helpGrid.implicitWidth + 56
                height: helpGrid.implicitHeight + 56
                // The list is long; shrink it rather than cut it off in a small window.
                scale: Math.min(1, (parent.width - 32) / width, (parent.height - 32) / height)
                radius: theme.radius
                color: "#17171b"
                border.color: "#2c2c34"

                GridLayout {
                    id: helpGrid
                    anchors.centerIn: parent
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 6

                    component Row: Text { color: "#d8d8e0"; font.pixelSize: 15 }
                    component Head: Text {
                        Layout.columnSpan: 2; Layout.topMargin: 8
                        color: theme.accent; font.pixelSize: 13; font.bold: true
                    }

                    Head { text: qsTr("On the main screen"); Layout.topMargin: 0 }
                    KeyCap { text: "A" }            Row { text: qsTr("Align the screen with the tracker") }
                    KeyCap { text: "C" }            Row { text: qsTr("Calibrate the current profile") }
                    KeyCap { text: "N" }            Row { text: qsTr("Calibrate a new profile") }
                    KeyCap { text: "P" }            Row { text: qsTr("Switch to the next profile") }
                    KeyCap { text: "Space" }        Row { text: qsTr("Show or hide the gaze point over the desktop") }
                    KeyCap { text: "?" }            Row { text: qsTr("Show or hide this list") }
                    KeyCap { text: "Q" }            Row { text: qsTr("Quit, and turn the gaze point off") }

                    Head { text: qsTr("While aligning") }
                    KeyCap { text: "← →" }          Row { text: qsTr("Move both lines") }
                    KeyCap { text: "↑ ↓" }          Row { text: qsTr("Spread the lines apart or bring them together") }
                    KeyCap { text: "Shift" }        Row { text: qsTr("Hold for small steps") }
                    KeyCap { text: "Enter" }        Row { text: qsTr("Save") }
                    KeyCap { text: "Esc" }          Row { text: qsTr("Leave without saving") }

                    Head { text: qsTr("While calibrating") }
                    KeyCap { text: "Enter" }        Row { text: qsTr("Try again after a failure") }
                    KeyCap { text: "Esc" }          Row { text: qsTr("Stop and keep the old calibration") }
                }
            }
        }
    }
}
}
