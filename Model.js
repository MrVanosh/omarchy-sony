// plugin/Model.js
// Pure ECMAScript model library for Sony ULT WEAR headphone management.
// Zero QML dependencies; runnable in QML, Deno, and Node.js runtimes.

var SUPPORTED_SCHEMA = 1;
var LEVEL_UNKNOWN = -1;

var NOISE_ANC = "anc";
var NOISE_AMBIENT = "ambient";
var NOISE_OFF = "off";
var NOISE_UNKNOWN = "unknown";

var ULT_OFF = 0;
var ULT_1 = 1;
var ULT_2 = 2;

var EQ_OFF = "off";
var EQ_BRIGHT = "bright";
var EQ_EXCITED = "excited";
var EQ_MELLOW = "mellow";
var EQ_RELAXED = "relaxed";
var EQ_VOCAL = "vocal";
var EQ_TREBLE = "treble";
var EQ_BASS = "bass";
var EQ_SPEECH = "speech";
var EQ_CUSTOM = "custom";

var MAX_ERROR_CHARS = 140;
var ELIDED_ERROR_CHARS = 137;

var NOISE_MODES = [NOISE_ANC, NOISE_AMBIENT, NOISE_OFF];
var EQ_PRESETS = [
  EQ_OFF, EQ_BRIGHT, EQ_EXCITED, EQ_MELLOW, EQ_RELAXED,
  EQ_VOCAL, EQ_TREBLE, EQ_BASS, EQ_SPEECH, EQ_CUSTOM
];

function defaultStatus() {
  return {
    ok: false,
    lastError: "",
    schemaVersion: 0,
    schemaTooNew: false,
    connected: false,
    deviceName: "",
    batteryLevel: LEVEL_UNKNOWN,
    batteryCharging: false,
    noiseMode: NOISE_UNKNOWN,
    ambientSoundLevel: 0,
    voicePassthrough: false,
    ultMode: ULT_OFF,
    eqPreset: EQ_OFF,
    eqCustomBands: [0, 0, 0, 0, 0],
    clearBass: 0,
    dseeExtreme: false,
    codec: ""
  };
}

function clamp(val, min, max, def) {
  var n = Number(val);
  if (isNaN(n)) return def;
  return Math.max(min, Math.min(max, Math.round(n)));
}

function parseStatus(raw) {
  if (raw === null || raw === undefined) {
    var res = defaultStatus();
    res.lastError = "The sony status file is empty";
    return res;
  }
  var text = String(raw).trim();
  if (!text) {
    var res = defaultStatus();
    res.lastError = "The sony status file is empty";
    return res;
  }

  var parsed;
  try {
    parsed = JSON.parse(text);
  } catch (_e) {
    var res = defaultStatus();
    res.lastError = "Could not read the sony status file";
    return res;
  }

  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    var res = defaultStatus();
    res.lastError = "The sony status file is invalid";
    return res;
  }

  if (parsed.schema_version === undefined || parsed.schema_version === null) {
    var res = defaultStatus();
    res.lastError = "The sony status file carried no schema_version";
    return res;
  }

  var version = Number(parsed.schema_version);
  if (isNaN(version) || version > SUPPORTED_SCHEMA) {
    var res = defaultStatus();
    res.schemaTooNew = true;
    res.schemaVersion = isNaN(version) ? 0 : version;
    res.lastError = "sony daemon speaks status schema " + version + ", this panel reads " + SUPPORTED_SCHEMA;
    return res;
  }

  var res = defaultStatus();
  res.ok = true;
  res.schemaVersion = version;
  res.connected = parsed.connected === true;

  if (!res.connected) {
    return res;
  }

  res.deviceName = String(parsed.device_name || "");

  if (parsed.battery_level !== undefined && parsed.battery_level !== null) {
    var bl = Number(parsed.battery_level);
    if (!isNaN(bl) && bl >= 0) {
      res.batteryLevel = Math.min(100, Math.round(bl));
    } else {
      res.batteryLevel = LEVEL_UNKNOWN;
    }
  }

  res.batteryCharging = parsed.battery_charging === true || parsed.charging === true;

  var validModes = [NOISE_ANC, NOISE_AMBIENT, NOISE_OFF];
  var nm = String(parsed.noise_mode || "").toLowerCase();
  res.noiseMode = validModes.indexOf(nm) !== -1 ? nm : NOISE_UNKNOWN;

  var rawAmbient = parsed.ambient_sound_level !== undefined ? parsed.ambient_sound_level : parsed.ambient_level;
  res.ambientSoundLevel = clamp(rawAmbient, 0, 20, 0);
  res.voicePassthrough = parsed.voice_passthrough === true;
  res.ultMode = clamp(parsed.ult_mode, ULT_OFF, ULT_2, ULT_OFF);

  var validPresets = [EQ_OFF, EQ_BRIGHT, EQ_EXCITED, EQ_MELLOW, EQ_RELAXED, EQ_VOCAL, EQ_TREBLE, EQ_BASS, EQ_SPEECH, EQ_CUSTOM];
  var ep = String(parsed.eq_preset || "").toLowerCase();
  res.eqPreset = validPresets.indexOf(ep) !== -1 ? ep : EQ_OFF;

  var rawBands = Array.isArray(parsed.eq_custom_bands) ? parsed.eq_custom_bands : (Array.isArray(parsed.eq_bands) ? parsed.eq_bands : []);
  var bands = [];
  for (var i = 0; i < 5; i++) {
    var b = rawBands[i];
    bands.push(clamp(b, -10, 10, 0));
  }
  res.eqCustomBands = bands;
  res.clearBass = clamp(parsed.clear_bass, -10, 10, 0);

  res.dseeExtreme = parsed.dsee_extreme === true || parsed.dsee === true;
  res.codec = String(parsed.codec || "");

  return res;
}

function noiseModeName(mode) {
  switch (mode) {
    case NOISE_ANC: return "Noise Cancelling";
    case NOISE_AMBIENT: return "Ambient Sound";
    case NOISE_OFF: return "Off";
    default: return "Unknown";
  }
}

function noiseModeIcon(mode) {
  switch (mode) {
    case NOISE_ANC: return "\uDB80\uDF4B";
    case NOISE_AMBIENT: return "\uDB80\uDE26";
    case NOISE_OFF: return "\uDB80\uDF4C";
    default: return "\uDB80\uDF4B";
  }
}

function eqPresetName(preset) {
  switch (preset) {
    case EQ_OFF: return "Off";
    case EQ_BRIGHT: return "Bright";
    case EQ_EXCITED: return "Excited";
    case EQ_MELLOW: return "Mellow";
    case EQ_RELAXED: return "Relaxed";
    case EQ_VOCAL: return "Vocal";
    case EQ_TREBLE: return "Treble Boost";
    case EQ_BASS: return "Bass Boost";
    case EQ_SPEECH: return "Speech";
    case EQ_CUSTOM: return "Custom";
    default: return "Unknown";
  }
}

function eqPresetButtonLabel(preset) {
  switch (preset) {
    case EQ_TREBLE: return "Treble";
    case EQ_BASS: return "Bass";
    default: return eqPresetName(preset);
  }
}

function ultModeName(mode) {
  if (mode === ULT_1) return "ULT 1";
  if (mode === ULT_2) return "ULT 2";
  return "Off";
}

function formatBattery(level) {
  if (level === undefined || level === null || level < 0) return "—";
  return Math.round(level) + "%";
}

function levelFraction(level) {
  if (level === undefined || level === null || level < 0) return 0.0;
  return Math.max(0.0, Math.min(1.0, level / 100.0));
}

function batteryIcon(level, charging) {
  if (charging) return "\uDB80\uDC84";
  if (level < 0) return "\uDB80\uDC83";
  if (level >= 95) return "\uDB80\uDC79";
  if (level >= 85) return "\uDB80\uDC82";
  if (level >= 75) return "\uDB80\uDC81";
  if (level >= 65) return "\uDB80\uDC80";
  if (level >= 55) return "\uDB80\uDC7F";
  if (level >= 45) return "\uDB80\uDC7E";
  if (level >= 35) return "\uDB80\uDC7D";
  if (level >= 25) return "\uDB80\uDC7C";
  if (level >= 15) return "\uDB80\uDC7B";
  return "\uDB80\uDC8E";
}

function elideError(text) {
  if (!text) return "";
  var cleaned = String(text).replace(/\s+/g, " ").trim();
  if (cleaned.length > MAX_ERROR_CHARS) {
    return cleaned.substring(0, ELIDED_ERROR_CHARS) + "…";
  }
  return cleaned;
}

function cycleNoiseMode(currentMode) {
  if (currentMode === NOISE_ANC) return NOISE_AMBIENT;
  if (currentMode === NOISE_AMBIENT) return NOISE_OFF;
  return NOISE_ANC;
}
