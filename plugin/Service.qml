import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

// The widget's link to the eyenix app. It does nothing itself: it follows the
// app's state through `eyenix status --follow`, and asks for changes by
// running eyenix commands. Arguments are passed as a list, never through a
// shell.
Item {
  id: root

  // The latest state the app reported, already validated by Model.parseStatus.
  property var state: Model.empty()
  // True when the eyenix command could not be run at all.
  property bool missing: false
  // What the last command said when it failed, for the panel to show.
  property string lastError: ""
  readonly property bool busy: action.running
  readonly property bool tracking: Model.tracking(state)

  // `eyenix plugin install` records which binary installed the plugin in the
  // `command` file, so a build that is not on the PATH works too.
  readonly property string command: {
    var recorded = Model.commandPath(commandFile.text())
    return recorded !== "" ? recorded : "eyenix"
  }

  function run(args) {
    if (action.running) return
    lastError = ""
    action.command = [command].concat(args)
    action.running = true
  }

  // Game mode stays on until it is turned off, so after a login or a shell
  // restart the app is started again for it. Once only: if that fails, the
  // panel shows why rather than trying for ever.
  property bool resumedGames: false
  onStateChanged: {
    if (Model.shouldResumeGames(state, resumedGames) && !action.running) {
      resumedGames = true
      run(["games", "on"])
    }
  }

  function toggleGames() { run(["games", "toggle"]) }
  function setOverlay(on) { run(["overlay", on ? "on" : "off"]) }
  function toggleOverlay() { run(["overlay", "toggle"]) }
  function useProfile(name) { if (Model.validProfileName(name)) run(["profile", "use", name]) }
  function calibrate() { run(["calibrate"]) }
  function calibrateNew() { run(["calibrate", "--new"]) }
  function align() { run(["align"]) }

  FileView {
    id: commandFile
    path: String(Qt.resolvedUrl("command")).replace(/^file:\/\//, "")
    watchChanges: true
    printErrors: false
    blockLoading: true
    onFileChanged: reload()
  }

  // One long-running command that prints the state again whenever it changes,
  // including when the app starts and stops.
  Process {
    id: follower
    command: [root.command, "status", "--follow"]
    running: true
    stdout: SplitParser {
      onRead: function(line) {
        var parsed = Model.parseStatus(line)
        if (!parsed) return
        root.missing = false
        root.state = parsed
      }
    }
    onExited: function(exitCode) {
      // It is meant to run for ever. If it stopped, the binary is gone or
      // broken; say so and look again in a while.
      root.state = Model.empty()
      root.missing = true
      retry.restart()
    }
  }

  Timer {
    id: retry
    interval: 5000
    onTriggered: follower.running = true
  }

  Process {
    id: action
    stderr: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var said = Model.text(String(text || "").trim().replace(/^eyenix: /, ""))
        if (said !== "") root.lastError = said
      }
    }
  }
}
