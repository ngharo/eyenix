.pragma library

// Everything the widget learns comes from the eyenix command's output. It is
// parsed and checked here before any of it reaches the screen, so a damaged
// or unexpected line can change what the panel says but never how it behaves.

var PROFILE_NAME = /^[A-Za-z0-9][A-Za-z0-9 _-]{0,31}$/
var MAX_PROFILES = 50
var MAX_TEXT = 200
var MAX_LINE = 16384

function empty() {
  return {
    running: false, connected: false, eyes: false, overlay: false,
    aligned: false, calibrated: false, mode: "home",
    device: "", problem: "", notice: "", profile: "", profiles: [],
    // The tracker was let go because nothing needs it; it comes back by itself.
    idle: false,
    games: { enabled: false, state: "off", game: "", head: false, gaze: false, problem: "" }
  }
}

var GAME_STATES = ["off", "waiting", "starting", "serving"]

// Game names come from folder names on disk: keep them short and plain.
function gameName(value) {
  return text(value, 48).replace(/[^A-Za-z0-9 .'&-]/g, "").trim()
}

function parseGames(raw) {
  var games = empty().games
  if (!raw || typeof raw !== "object" || Array.isArray(raw)) return games
  games.enabled = raw.enabled === true
  games.state = GAME_STATES.indexOf(raw.state) !== -1 ? raw.state : "off"
  games.game = gameName(raw.game)
  games.head = raw.head === true
  games.gaze = raw.gaze === true
  games.problem = text(raw.problem)
  return games
}

function text(value, limit) {
  if (typeof value !== "string") return ""
  // Control characters have no business in a status line.
  return value.replace(/[\u0000-\u001f\u007f]/g, " ").slice(0, limit || MAX_TEXT)
}

function validProfileName(name) {
  return typeof name === "string" && PROFILE_NAME.test(name)
}

// One line of `eyenix status --follow` -> a state object, or null if the line
// is not a status.
function parseStatus(line) {
  if (typeof line !== "string" || line.length === 0 || line.length > MAX_LINE) return null
  var raw
  try { raw = JSON.parse(line) } catch (e) { return null }
  if (!raw || typeof raw !== "object" || Array.isArray(raw) || typeof raw.running !== "boolean") return null

  var state = empty()
  state.running = raw.running
  state.connected = raw.connected === true
  state.eyes = raw.eyes === true
  state.overlay = raw.overlay === true
  state.aligned = raw.aligned === true
  state.calibrated = raw.calibrated === true
  state.mode = raw.mode === "align" || raw.mode === "calibrate" ? raw.mode : "home"
  state.device = text(raw.device, 64)
  state.problem = text(raw.problem)
  state.notice = text(raw.notice)
  state.idle = raw.idle === true
  state.games = parseGames(raw.games)

  var names = Array.isArray(raw.profiles) ? raw.profiles : []
  for (var i = 0; i < names.length && state.profiles.length < MAX_PROFILES; i++) {
    if (validProfileName(names[i]) && state.profiles.indexOf(names[i]) === -1) state.profiles.push(names[i])
  }
  state.profile = state.profiles.indexOf(raw.profile) !== -1 ? raw.profile : ""
  return state
}

// The path the installer recorded for the eyenix binary, or "" if the file's
// content is not a plain absolute path.
function commandPath(raw) {
  var path = typeof raw === "string" ? raw.trim() : ""
  return /^\/[A-Za-z0-9._\/ -]{1,255}$/.test(path) && path.indexOf("..") === -1 ? path : ""
}

// Whether the bar icon should be lit: the app is running and has the tracker.
function tracking(state) {
  return state.running && state.connected
}

function statusKey(state, missing) {
  if (missing) return "status.missing"
  if (!state.running) return "status.off"
  if (!state.connected && state.idle) return state.games.enabled ? "status.gamesWaiting" : "status.idle"
  if (!state.connected) return state.problem === "" ? "status.searching" : "status.noTracker"
  if (state.mode === "align") return "status.aligning"
  if (state.mode === "calibrate") return "status.calibrating"
  if (state.games.state === "serving") return "status.serving"
  return state.eyes ? "status.tracking" : "status.noEyes"
}

// What the game mode switch says underneath its label.
function gamesKey(games) {
  if (!games.enabled) return "games.description"
  if (games.state === "serving") {
    if (games.head && games.gaze) return "games.servingBoth"
    return games.head ? "games.servingHead" : "games.servingGaze"
  }
  return games.state === "starting" ? "games.starting" : "games.waiting"
}

// Puts a game's name into a sentence that has a %1 in it.
function withGame(sentence, games) {
  return String(sentence).split("%1").join(games.game !== "" ? games.game : "?")
}

// Game mode was left on, but the app is not running: it should be started
// again, once.
function shouldResumeGames(state, alreadyTried) {
  return !alreadyTried && !state.running && state.games.enabled
}
