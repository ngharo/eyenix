import QtQuick

// Shows one target at a time and has the tracker measure the eyes while the
// user looks at it. Must fill a full-screen window: targets are placed as
// fractions of the screen.
Item {
    id: view

    property bool active: false
    signal finished()

    // "idle", "settle" (eyes travelling to the target), "collect" (tracker
    // measuring), "done" or "failed".
    property string phase: "idle"
    property int index: 0
    property string message: ""
    readonly property point target: backend.calibrationPoint(index)

    function start() {
        message = ""
        index = 0
        phase = "settle"
        backend.beginCalibration(session.calibrationProfile)
        settle.restart()
    }

    function stop() {
        settle.stop()
        leave.stop()
        backend.cancelCalibration()
        phase = "idle"
        finished()
    }

    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: view.active; onActivated: view.stop() }
    Shortcut {
        sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut
        enabled: view.active && view.phase === "failed"
        onActivated: view.start()
    }

    // Time for the eyes to reach the target and rest on it before measuring.
    Timer {
        id: settle
        interval: 1300
        onTriggered: {
            view.phase = "collect"
            backend.collectPoint(view.index)
        }
    }
    Timer { id: leave; interval: 1400; onTriggered: { view.phase = "idle"; view.finished() } }

    Connections {
        target: backend
        function onPointCollected(index) {
            if (view.phase !== "collect" || index !== view.index)
                return
            if (index + 1 < backend.calibrationPointCount) {
                view.index = index + 1
                view.phase = "settle"
                settle.restart()
            }
        }
        function onCalibrationSaved() {
            view.phase = "done"
            leave.restart()
        }
        function onCalibrationFailed(message) {
            if (view.phase === "idle")
                return
            settle.stop()
            view.message = message
            view.phase = "failed"
        }
    }

    Item {
        id: dot
        visible: view.phase === "settle" || view.phase === "collect"
        x: view.target.x * view.width
        y: view.target.y * view.height
        Behavior on x { NumberAnimation { duration: 450; easing.type: Easing.InOutCubic } }
        Behavior on y { NumberAnimation { duration: 450; easing.type: Easing.InOutCubic } }

        // The ring closes in on the centre, which draws the eye to one spot.
        Rectangle {
            id: ring
            anchors.centerIn: parent
            width: view.phase === "collect" ? 18 : 64
            height: width
            radius: width / 2
            color: "transparent"
            border.color: theme.accent
            border.width: 3
            Behavior on width { NumberAnimation { duration: 600; easing.type: Easing.OutCubic } }
        }
        Rectangle {
            anchors.centerIn: parent
            width: 10; height: 10; radius: 5
            color: "#f2f2f5"
        }
    }

    Column {
        anchors.centerIn: parent
        // Clear of the centre target while it is showing.
        anchors.verticalCenterOffset: dot.visible ? 140 : 0
        width: Math.min(parent.width - 120, 720)
        spacing: 14

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: view.phase === "failed" ? "#f87171" : "#f2f2f5"
            font.pixelSize: view.phase === "done" || view.phase === "failed" ? 28 : 18
            font.bold: view.phase === "done" || view.phase === "failed"
            text: view.phase === "done" ? qsTr("Calibration saved")
                : view.phase === "failed" ? qsTr("Calibration did not work")
                : qsTr("Look at the dot and keep your head still. %1 of %2")
                      .arg(view.index + 1).arg(backend.calibrationPointCount)
        }
        Text {
            visible: view.phase === "failed"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: "#b9b9c4"
            font.pixelSize: 17
            text: view.message + "\n" + qsTr("Enter tries again, Esc leaves.")
        }
    }
}
