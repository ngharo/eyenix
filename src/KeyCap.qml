import QtQuick

// A key as it looks on the keyboard, for naming shortcuts.
Rectangle {
    property alias text: label.text

    implicitWidth: Math.max(34, label.implicitWidth + 18)
    implicitHeight: 30
    radius: Math.min(4, theme.radius)
    color: "#1c1c21"
    border.color: "#3a3a44"

    Text {
        id: label
        anchors.centerIn: parent
        color: "#e8e8ee"
        font.pixelSize: 14
        font.bold: true
    }
}
