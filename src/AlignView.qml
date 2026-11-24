import QtQuick
import QtQuick.Window

// Tells the tracker where the screen is. The user moves two lines until they
// sit directly above the two marks on the tracker; the marks are a known
// distance apart, so the lines give the size of a pixel and with it the size
// and position of the whole screen. Must fill a full-screen window.
Item {
    id: view

    property bool active: false
    property string screenName: ""
    signal finished()

    // Kept as fractions of the width, so they hold while the window is still
    // growing to full screen.
    property real center: 0.5
    property real spacing: 0.3
    readonly property real leftX: (center - spacing / 2) * width
    readonly property real rightX: (center + spacing / 2) * width
    readonly property var size: backend.screenSize(leftX, rightX, width, height)

    function number(value, decimals) {
        // Reading the language makes bindings that call this re-render on a switch.
        translator.language
        return Number(value).toLocaleString(Qt.locale(), 'f', decimals)
    }

    function start() {
        if (backend.aligned) {
            center = (backend.markLeft + backend.markRight) / 2 / backend.alignedWidth
            spacing = (backend.markRight - backend.markLeft) / backend.alignedWidth
        } else {
            // Start from the size the monitor reports, which is usually close.
            center = 0.5
            spacing = Math.min(0.9, backend.defaultMarkSpacing(Screen.pixelDensity) / Screen.width)
        }
    }

    function move(pixels) {
        center = Math.max(spacing / 2, Math.min(1 - spacing / 2, center + pixels / width))
    }

    function spread(pixels) {
        const widest = 2 * Math.min(center, 1 - center)
        spacing = Math.max(60 / width, Math.min(widest, spacing + pixels / width))
    }

    function save() {
        backend.applyAlignment(leftX, rightX, width, height, screenName)
        finished()
    }

    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.move(-5) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.move(5) }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.move(-1) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.move(1) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.spread(10) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.spread(-10) }
    Shortcut { sequence: "Shift+Up"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.spread(2) }
    Shortcut { sequence: "Shift+Down"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.spread(-2) }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.save() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.finished() }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -60
        width: Math.min(parent.width - 120, 760)
        spacing: 18

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#f2f2f5"
            font.pixelSize: 28
            font.bold: true
            text: qsTr("Line up the two lines with the marks on the tracker")
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#b9b9c4"
            font.pixelSize: 17
            text: qsTr("Move them with ← and →, spread them with ↑ and ↓, or drag them. Hold Shift for small steps. Enter saves, Esc leaves.")
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            color: theme.accent
            font.pixelSize: 20
            // A wrong size here means the lines are not on the marks yet.
            text: view.size.widthMm === undefined ? ""
                : qsTr("This makes your screen %1 × %2 mm (%3″)")
                      .arg(view.number(view.size.widthMm, 0)).arg(view.number(view.size.heightMm, 0))
                      .arg(view.number(view.size.diagonalInches, 1))
        }
    }

    component Line: Item {
        id: line
        property bool isLeft: true
        x: (isLeft ? view.leftX : view.rightX) - width / 2
        y: view.height - height
        width: 44
        height: 260

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: 2
            height: parent.height - 22
            color: theme.accent
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 22; height: 22; radius: 11
            color: theme.accent
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            preventStealing: true
            // Dragging one line leaves the other where it is.
            onPositionChanged: (mouse) => {
                const pointer = line.x + mouse.x
                const other = line.isLeft ? view.rightX : view.leftX
                const low = line.isLeft ? Math.max(0, Math.min(pointer, other - 60)) : other
                const high = line.isLeft ? other : Math.min(view.width, Math.max(pointer, other + 60))
                view.center = (low + high) / 2 / view.width
                view.spacing = (high - low) / view.width
            }
        }
    }

    Line { isLeft: true }
    Line { isLeft: false }
}
