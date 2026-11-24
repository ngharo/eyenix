import QtQuick
import QtQuick.Layouts
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// Eyenix in the bar. The icon lights up while eye tracking is running; the
// panel turns the gaze point and game mode on and off, switches calibration
// profile, and starts calibration or alignment. All of the work happens in the eyenix app;
// see Service.qml.
Panel {
  id: root
  moduleName: "ngharo.eyenix"
  ipcTarget: "ngharo.eyenix"

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property var state: service.state
  readonly property string statusText: Model.withGame(strings.t(Model.statusKey(state, service.missing)), state.games)
  // The app's own complaint first, then whatever the last command said.
  readonly property string problemText: service.missing ? ""
      : state.notice !== "" ? state.notice
      : service.lastError !== "" ? service.lastError
      : state.games.problem !== "" ? state.games.problem
      : !state.connected ? state.problem : ""

  // Keyboard cursor: 0 gaze toggle, 1 game mode toggle, 2 profile dropdown,
  // 3 calibrate, 4 new profile, 5 align.
  property int cursor: 0
  property bool cursorActive: false
  readonly property int cursorCount: 6

  function moveCursor(step) {
    if (!cursorActive) { cursorActive = true; return }
    cursor = Math.max(0, Math.min(cursorCount - 1, cursor + step))
  }

  function setCursor(index) {
    cursorActive = true
    cursor = index
  }

  // Calibration and alignment take over the screen, so the panel gets out of the way.
  function startTask(start) {
    start()
    root.close()
  }

  function activateCursor() {
    if (cursor === 0) service.toggleOverlay()
    else if (cursor === 1) service.toggleGames()
    else if (cursor === 2 && panel.loadedContent) panel.loadedContent.profileDropdown.toggle()
    else if (cursor === 3) startTask(service.calibrate)
    else if (cursor === 4) startTask(service.calibrateNew)
    else if (cursor === 5) startTask(service.align)
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: if (opened) {
    cursorActive = false
    cursor = 0
    Qt.callLater(function() { if (panel.loadedContent) panel.loadedContent.forceActiveFocus() })
  }

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  Service { id: service }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: service.tracking ? "󰈈" : "󰈉"
    // Lit in the accent color while the tracker is running, dim otherwise.
    foreground: service.tracking ? Commons.Color.accent : root.barForeground
    dimmed: !service.tracking
    tooltipText: strings.t(service.missing ? "tooltip.missing" : service.tracking ? "tooltip.on" : "tooltip.off")
    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) service.toggleOverlay()
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(340))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(520))

    content: PanelKeyCatcher {
      id: keyCatcher
      property alias profileDropdown: profileDropdown
      implicitHeight: column.implicitHeight
      anchors.fill: parent
      // While the profile list is open, its own keys choose from it.
      blocked: profileDropdown.popupOpen
      onMoveRequested: function(dx, dy) { root.moveCursor(dy !== 0 ? dy : dx) }
      onActivateRequested: if (root.cursorActive) root.activateCursor()
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: column
        width: parent.width
        spacing: Style.space(12)

        PanelHero {
          width: parent.width
          title: strings.t("title")
          meta: root.statusText
          foreground: root.foreground
          fontFamily: root.fontFamily
          iconOpacity: service.tracking ? 1.0 : 0.5
          iconComponent: Component {
            Text {
              textFormat: Text.PlainText
              text: service.tracking ? "󰈈" : "󰈉"
              color: service.tracking ? Commons.Color.accent : root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.display
            }
          }
        }

        // Without the app this panel can do nothing: say how to get it.
        Text {
          textFormat: Text.PlainText
          visible: service.missing
          width: parent.width
          text: strings.t("missing.help")
          color: root.urgent
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          wrapMode: Text.WordWrap
        }

        Text {
          textFormat: Text.PlainText
          visible: text !== ""
          width: parent.width
          text: root.problemText
          color: root.urgent
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          wrapMode: Text.WordWrap
        }

        Toggle {
          width: parent.width
          label: strings.t("gaze.label")
          description: strings.t("gaze.description")
          checked: root.state.overlay
          hasCursor: root.cursorActive && root.cursor === 0
          foreground: root.foreground
          fontFamily: root.fontFamily
          enabled: !service.missing
          onHovered: function(on) { if (on) root.setCursor(0) }
          onClicked: service.toggleOverlay()
        }

        Toggle {
          width: parent.width
          label: strings.t("games.label")
          description: Model.withGame(strings.t(Model.gamesKey(root.state.games)), root.state.games)
          checked: root.state.games.enabled
          hasCursor: root.cursorActive && root.cursor === 1
          foreground: root.foreground
          fontFamily: root.fontFamily
          enabled: !service.missing && !service.busy
          onHovered: function(on) { if (on) root.setCursor(1) }
          onClicked: service.toggleGames()
        }

        PanelSeparator { foreground: root.foreground }

        PanelSectionHeader {
          text: strings.t("profile.header")
          foreground: root.foreground
          fontFamily: root.fontFamily
        }

        Dropdown {
          id: profileDropdown
          width: parent.width
          showLabel: false
          fontFamily: root.fontFamily
          options: root.state.profiles
          value: root.state.profile
          enabled: !service.missing && root.state.mode === "home"
          hasCursor: root.cursorActive && root.cursor === 2
          onHovered: function(on) { if (on) root.setCursor(2) }
          onChanged: function(name) { if (name !== root.state.profile) service.useProfile(name) }
        }

        Text {
          textFormat: Text.PlainText
          visible: !root.state.calibrated && !service.missing
          width: parent.width
          text: strings.t("profile.notCalibrated")
          color: root.dim
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          wrapMode: Text.WordWrap
        }

        RowLayout {
          width: parent.width
          spacing: Style.space(8)

          ActionButton {
            index: 3
            text: strings.t("action.calibrate")
            tooltipText: strings.t("action.calibrate.tip")
            onClicked: root.startTask(service.calibrate)
          }
          ActionButton {
            index: 4
            text: strings.t("action.new")
            tooltipText: strings.t("action.new.tip")
            onClicked: root.startTask(service.calibrateNew)
          }
          ActionButton {
            index: 5
            text: strings.t("action.align")
            tooltipText: strings.t("action.align.tip")
            onClicked: root.startTask(service.align)
          }
        }
      }
    }
  }

  component ActionButton: Button {
    property int index: 0
    Layout.fillWidth: true
    Layout.preferredWidth: 1
    bordered: true
    fontSize: Style.font.bodySmall
    foreground: root.foreground
    fontFamily: root.fontFamily
    enabled: !service.missing && !service.busy && root.state.mode === "home"
    opacity: enabled ? 1 : 0.5
    hasCursor: root.cursorActive && root.cursor === index
    onHovered: function(on) { if (on) root.setCursor(index) }
  }
}
