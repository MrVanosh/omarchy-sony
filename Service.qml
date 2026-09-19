// plugin/Service.qml
// Reactive singleton / manager service for Sony ULT WEAR headphones.
import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

Item {
  id: root

  property var settings: ({})

  readonly property string cliBinary: {
    var home = Quickshell.env("HOME")
    return home ? (home + "/.local/bin/sony-ctl") : "sony-ctl"
  }

  // Path to status.json ($XDG_STATE_HOME/sony-headphones/status.json)
  readonly property string statusPath: {
    var xdg = Quickshell.env("XDG_STATE_HOME")
    var home = Quickshell.env("HOME")
    var base = xdg ? xdg : (home ? home + "/.local/state" : "/tmp")
    return base + "/sony-headphones/status.json"
  }

  // Reactive state properties
  property bool ok: false
  property string lastError: ""
  property int schemaVersion: 0
  property bool schemaTooNew: false
  property bool connected: false
  property string deviceName: ""
  property int batteryLevel: Model.LEVEL_UNKNOWN
  property bool batteryCharging: false
  property string codec: ""
  property var eqCustomBands: [0, 0, 0, 0, 0]
  property int clearBass: 0

  // Real internal states reported by daemon
  property string _realNoiseMode: Model.NOISE_UNKNOWN
  property bool _realVoicePassthrough: false
  property int _realUltMode: Model.ULT_OFF
  property int _realAmbientSoundLevel: 0
  property string _realEqPreset: Model.EQ_OFF
  property bool _realDseeExtreme: false

  // Optimistic desired states
  property string _desiredNoiseMode: ""
  property var _desiredVoicePassthrough: null
  property int _desiredUltMode: -1
  property int _desiredAmbientLevel: -1
  property string _desiredEqPreset: ""
  property var _desiredDsee: null

  // Exposed effective properties (optimistic value if pending, otherwise real value)
  readonly property string noiseMode: _desiredNoiseMode !== "" ? _desiredNoiseMode : _realNoiseMode
  readonly property bool voicePassthrough: _desiredVoicePassthrough !== null ? _desiredVoicePassthrough : _realVoicePassthrough
  readonly property int ultMode: _desiredUltMode !== -1 ? _desiredUltMode : _realUltMode
  readonly property int ambientSoundLevel: _desiredAmbientLevel !== -1 ? _desiredAmbientLevel : _realAmbientSoundLevel
  readonly property string eqPreset: _desiredEqPreset !== "" ? _desiredEqPreset : _realEqPreset
  readonly property bool dseeExtreme: _desiredDsee !== null ? _desiredDsee : _realDseeExtreme

  // 4000ms optimistic state settlement timer
  Timer {
    id: settleTimer
    interval: 4000
    repeat: false
    onTriggered: root.clearOptimisticOverrides()
  }

  function clearOptimisticOverrides() {
    _desiredNoiseMode = ""
    _desiredVoicePassthrough = null
    _desiredUltMode = -1
    _desiredAmbientLevel = -1
    _desiredEqPreset = ""
    _desiredDsee = null
  }

  // Command dispatch queue
  property var commandQueue: []

  function runCommand(args) {
    var cmd = [cliBinary].concat(args)
    commandQueue.push(cmd)
    dispatchNext()
  }

  function dispatchNext() {
    if (ctlProcess.running || commandQueue.length === 0) return
    var nextCmd = commandQueue.shift()
    ctlProcess.command = nextCmd
    ctlProcess.running = true
  }

  Process {
    id: ctlProcess
    running: false
    stdout: StdioCollector { id: ctlStdout; waitForEnd: true }
    stderr: StdioCollector { id: ctlStderr; waitForEnd: true }
    onExited: function(exitCode) {
      if (exitCode !== 0) {
        var err = String(ctlStderr.text || ctlStdout.text || "").trim()
        if (err) {
          root.lastError = Model.elideError(err)
        }
      }
      dispatchNext()
    }
  }

  // Reactive FileView watcher (zero periodic polling timers for file reading)
  FileView {
    id: fileView
    path: root.statusPath
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onLoaded: {
      var content = typeof text === "function" ? text() : (fileView.text || "")
      root.applyStatus(content)
    }
    onLoadFailed: root.applyStatus("")
    onFileChanged: reload()
  }

  function applyStatus(raw) {
    var content = raw
    if (content === undefined || content === null) {
      content = typeof fileView.text === "function" ? fileView.text() : (fileView.text || "")
    }
    var parsed = Model.parseStatus(content)

    ok = parsed.ok === true
    lastError = parsed.lastError || ""
    schemaVersion = parsed.schemaVersion || 0
    schemaTooNew = parsed.schemaTooNew === true
    connected = parsed.connected === true
    deviceName = parsed.deviceName || ""
    batteryLevel = parsed.batteryLevel !== undefined ? parsed.batteryLevel : Model.LEVEL_UNKNOWN
    batteryCharging = parsed.batteryCharging === true
    codec = parsed.codec || ""
    eqCustomBands = parsed.eqCustomBands || [0, 0, 0, 0, 0]
    clearBass = parsed.clearBass !== undefined ? parsed.clearBass : 0

    _realNoiseMode = parsed.noiseMode || Model.NOISE_UNKNOWN
    _realVoicePassthrough = parsed.voicePassthrough === true
    _realUltMode = parsed.ultMode !== undefined ? parsed.ultMode : Model.ULT_OFF
    _realAmbientSoundLevel = parsed.ambientSoundLevel !== undefined ? parsed.ambientSoundLevel : 0
    _realEqPreset = parsed.eqPreset || Model.EQ_OFF
    _realDseeExtreme = parsed.dseeExtreme === true

    // Reconcile optimistic values with settled daemon updates
    if (_desiredNoiseMode !== "" && _realNoiseMode === _desiredNoiseMode) _desiredNoiseMode = ""
    if (_desiredVoicePassthrough !== null && _realVoicePassthrough === _desiredVoicePassthrough) _desiredVoicePassthrough = null
    if (_desiredUltMode !== -1 && _realUltMode === _desiredUltMode) _desiredUltMode = -1
    if (_desiredAmbientLevel !== -1 && _realAmbientSoundLevel === _desiredAmbientLevel) _desiredAmbientLevel = -1
    if (_desiredEqPreset !== "" && _realEqPreset === _desiredEqPreset) _desiredEqPreset = ""
    if (_desiredDsee !== null && _realDseeExtreme === _desiredDsee) _desiredDsee = null

    if (_desiredNoiseMode === "" && _desiredVoicePassthrough === null && _desiredUltMode === -1 &&
        _desiredAmbientLevel === -1 && _desiredEqPreset === "" &&
        _desiredDsee === null) {
      settleTimer.stop()
    }
  }

  function setNoiseMode(mode) {
    var valid = [Model.NOISE_ANC, Model.NOISE_AMBIENT, Model.NOISE_OFF]
    if (valid.indexOf(mode) === -1) return
    _desiredNoiseMode = mode
    settleTimer.restart()
    runCommand(["noise", mode])
  }

  function setVoiceFocus(enabled) {
    _desiredNoiseMode = Model.NOISE_AMBIENT
    _desiredVoicePassthrough = enabled === true
    settleTimer.restart()
    runCommand(["voice-focus", enabled ? "on" : "off"])
  }

  function setUltMode(mode) {
    var clamped = Model.clamp(mode, Model.ULT_OFF, Model.ULT_2, Model.ULT_OFF)
    _desiredUltMode = clamped
    settleTimer.restart()
    runCommand(["ult", clamped === Model.ULT_OFF ? "off" : String(clamped)])
  }

  function setAmbientLevel(level) {
    var clamped = Model.clamp(level, 0, 20, 0)
    _desiredAmbientLevel = clamped
    settleTimer.restart()
    runCommand(["ambient-level", String(clamped)])
  }

  function setEqPreset(preset) {
    var valid = [
      Model.EQ_OFF, Model.EQ_BRIGHT, Model.EQ_EXCITED, Model.EQ_MELLOW,
      Model.EQ_RELAXED, Model.EQ_VOCAL, Model.EQ_TREBLE, Model.EQ_BASS,
      Model.EQ_SPEECH, Model.EQ_CUSTOM
    ]
    if (valid.indexOf(preset) === -1) return
    _desiredEqPreset = preset
    settleTimer.restart()
    runCommand(["eq", preset])
  }

  function setEqCustom(b1, b2, b3, b4, b5, cb) {
    _desiredEqPreset = Model.EQ_CUSTOM
    settleTimer.restart()
    runCommand([
      "eq", "custom",
      String(Model.clamp(b1, -10, 10, 0)),
      String(Model.clamp(b2, -10, 10, 0)),
      String(Model.clamp(b3, -10, 10, 0)),
      String(Model.clamp(b4, -10, 10, 0)),
      String(Model.clamp(b5, -10, 10, 0)),
      String(Model.clamp(cb, -10, 10, 0))
    ])
  }

  function setDsee(enabled) {
    _desiredDsee = enabled === true
    settleTimer.restart()
    runCommand(["dsee", enabled ? "on" : "off"])
  }

  function cycleNoiseMode() {
    var next = Model.cycleNoiseMode(noiseMode)
    setNoiseMode(next)
  }

  function refresh() {
    fileView.reload()
  }

  Component.onCompleted: {
    fileView.reload()
  }
}
