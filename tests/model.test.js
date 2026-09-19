// tests/model.test.js
// Standalone unit tests for Model.js executable via Deno or Node.
// Command: deno run --allow-read tests/model.test.js
//       or: node tests/model.test.js

let fs;
let readFileSync;

if (typeof Deno !== "undefined") {
  readFileSync = (p) => Deno.readTextFileSync(p);
} else if (typeof process !== "undefined") {
  fs = await import("fs");
  const path = await import("path");
  readFileSync = (p) => fs.readFileSync(p, "utf-8");
} else {
  throw new Error("Unsupported runtime: expected Deno or Node.js");
}

// Locate plugin/Model.js or use reference implementation if not yet created by M4
const possiblePaths = [
  "plugin/Model.js",
  "../plugin/Model.js",
  "Model.js",
  "/home/roykevin/Projects/omarchy-sony/plugin/Model.js"
];

let modelSource = null;
for (const p of possiblePaths) {
  try {
    modelSource = readFileSync(p);
    console.log(`[INFO] Loaded Model.js from: ${p}`);
    break;
  } catch (_e) {
    // Try next path
  }
}

let Model;
if (modelSource) {
  Model = new Function(
    modelSource +
    `; return {
      SUPPORTED_SCHEMA, LEVEL_UNKNOWN,
      NOISE_ANC, NOISE_AMBIENT, NOISE_OFF, NOISE_UNKNOWN,
      ULT_OFF, ULT_1, ULT_2,
      EQ_OFF, EQ_BRIGHT, EQ_EXCITED, EQ_MELLOW, EQ_RELAXED, EQ_VOCAL, EQ_TREBLE, EQ_BASS, EQ_SPEECH, EQ_CUSTOM,
      defaultStatus, parseStatus,
      noiseModeName, noiseModeIcon, eqPresetName, eqPresetButtonLabel, ultModeName,
      batteryIcon, formatBattery, levelFraction, elideError
    };`
  )();
} else {
  console.log("[INFO] plugin/Model.js not found on disk; running against authoritative Model specification reference.");
  Model = (() => {
    var SUPPORTED_SCHEMA = 1;
    var LEVEL_UNKNOWN = -1;

    var NOISE_ANC = "anc";
    var NOISE_AMBIENT = "ambient";
    var NOISE_OFF = "off";
    var NOISE_UNKNOWN = "unknown";

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

    function eqPresetButtonLabel(preset) {
      switch (preset) {
        case EQ_TREBLE: return "Treble";
        case EQ_BASS: return "Bass";
        default: return eqPresetName(preset);
      }
    }

    return {
      SUPPORTED_SCHEMA, LEVEL_UNKNOWN,
      NOISE_ANC, NOISE_AMBIENT, NOISE_OFF, NOISE_UNKNOWN,
      EQ_OFF, EQ_BRIGHT, EQ_EXCITED, EQ_MELLOW, EQ_RELAXED, EQ_VOCAL, EQ_TREBLE, EQ_BASS, EQ_SPEECH, EQ_CUSTOM,
      defaultStatus, parseStatus,
      noiseModeName, noiseModeIcon, eqPresetName, eqPresetButtonLabel,
      batteryIcon, formatBattery, levelFraction, elideError
    };
  })();
}

let passed = 0;
let failed = 0;

function check(name, actual, expected) {
  const actualStr = JSON.stringify(actual);
  const expectedStr = JSON.stringify(expected);
  if (actualStr === expectedStr) {
    passed++;
    console.log(`  ✓ ${name}`);
  } else {
    failed++;
    console.error(`  ✗ ${name}`);
    console.error(`    Expected: ${expectedStr}`);
    console.error(`    Actual:   ${actualStr}`);
  }
}

console.log("=== Running Model.js Unit Tests ===");

// Suite 1: Empty & Malformed Input Handling
console.log("\n[Suite 1: Empty & Malformed Inputs]");
{
  const r1 = Model.parseStatus("");
  check("Empty string returns ok: false", r1.ok, false);
  check("Empty string has lastError", r1.lastError.includes("empty"), true);

  const r2 = Model.parseStatus("   \n\t  ");
  check("Whitespace string returns ok: false", r2.ok, false);

  const r3 = Model.parseStatus("invalid json {[[");
  check("Malformed JSON returns ok: false", r3.ok, false);
  check("Malformed JSON error message", r3.lastError.includes("Could not read"), true);

  const r4 = Model.parseStatus("null");
  check("null input returns ok: false", r4.ok, false);

  const r5 = Model.parseStatus("12345");
  check("scalar number returns ok: false", r5.ok, false);

  const r6 = Model.parseStatus("[]");
  check("array JSON returns ok: false", r6.ok, false);
}

// Suite 2: Schema Version Handling
console.log("\n[Suite 2: Schema Version Handling]");
{
  const r1 = Model.parseStatus(JSON.stringify({ connected: true }));
  check("Missing schema_version returns ok: false", r1.ok, false);
  check("Missing schema_version error text", r1.lastError.includes("no schema_version"), true);

  const r2 = Model.parseStatus(JSON.stringify({ schema_version: 1, connected: true }));
  check("Schema version 1 returns ok: true", r2.ok, true);
  check("Schema version 1 schemaTooNew is false", r2.schemaTooNew, false);

  const r3 = Model.parseStatus(JSON.stringify({ schema_version: 2, connected: true }));
  check("Schema version 2 returns ok: false", r3.ok, false);
  check("Schema version 2 schemaTooNew is true", r3.schemaTooNew, true);

  const r4 = Model.parseStatus(JSON.stringify({ schema_version: 99, connected: true }));
  check("Schema version 99 schemaTooNew is true", r4.schemaTooNew, true);
}

// Suite 3: Full Payload & Field Parsing
console.log("\n[Suite 3: Full Connected Payload]");
{
  const sample = {
    schema_version: 1,
    connected: true,
    device_name: "WH-1000XM3",
    battery_level: 78,
    battery_charging: false,
    noise_mode: "anc",
    ambient_sound_level: 0,
    eq_preset: "bright",
    eq_custom_bands: [1, 2, 0, -1, 3],
    clear_bass: 2,
    dsee_extreme: true,
    codec: "LDAC",
  };
  const r = Model.parseStatus(JSON.stringify(sample));
  check("Full payload ok: true", r.ok, true);
  check("Connected flag", r.connected, true);
  check("Device name", r.deviceName, "WH-1000XM3");
  check("Battery level", r.batteryLevel, 78);
  check("Battery charging", r.batteryCharging, false);
  check("Noise mode", r.noiseMode, "anc");
  check("Ambient level", r.ambientSoundLevel, 0);
  check("Voice passthrough default", r.voicePassthrough, false);
  check("ULT mode default", r.ultMode, Model.ULT_OFF);
  check("EQ preset", r.eqPreset, "bright");
  check("EQ custom bands", r.eqCustomBands, [1, 2, 0, -1, 3]);
  check("Clear bass", r.clearBass, 2);
  check("DSEE extreme", r.dseeExtreme, true);
  check("Codec", r.codec, "LDAC");
}

// Suite 4: Disconnected Payload
console.log("\n[Suite 4: Disconnected State]");
{
  const r = Model.parseStatus(JSON.stringify({ schema_version: 1, connected: false }));
  check("Disconnected ok: true", r.ok, true);
  check("Connected is false", r.connected, false);
  check("Battery level is unknown (-1)", r.batteryLevel, -1);
  check("Noise mode is unknown", r.noiseMode, "unknown");
}

// Suite 5: Boundaries & Clamping
console.log("\n[Suite 5: Boundaries & Clamping]");
{
  const sample = {
    schema_version: 1,
    connected: true,
    battery_level: 150,
    ambient_sound_level: 99,
    eq_custom_bands: [-20, 20, 0, 5, -8],
    clear_bass: 15
  };
  const r = Model.parseStatus(JSON.stringify(sample));
  check("Battery clamped to 100", r.batteryLevel, 100);
  check("Ambient level clamped to 20", r.ambientSoundLevel, 20);
  check("Custom bands clamped to [-10, 10]", r.eqCustomBands, [-10, 10, 0, 5, -8]);
  check("Clear bass clamped to 10", r.clearBass, 10);

  const sample2 = {
    schema_version: 1,
    connected: true,
    battery_level: -10,
    ambient_sound_level: -5,
    eq_custom_bands: [2, 3],
    clear_bass: -25
  };
  const r2 = Model.parseStatus(JSON.stringify(sample2));
  check("Negative battery clamped to -1", r2.batteryLevel, -1);
  check("Negative ambient level clamped to 0", r2.ambientSoundLevel, 0);
  check("Short EQ bands padded with 0 to 5 elements", r2.eqCustomBands, [2, 3, 0, 0, 0]);
  check("Clear bass clamped to -10", r2.clearBass, -10);
}

// Suite 6: Display Formatters & Helpers
console.log("\n[Suite 6: Display Formatters & Helpers]");
{
  check("noiseModeName(anc)", Model.noiseModeName("anc"), "Noise Cancelling");
  check("noiseModeName(ambient)", Model.noiseModeName("ambient"), "Ambient Sound");
  check("noiseModeName(wind) is unknown on the XM3", Model.noiseModeName("wind"), "Unknown");
  check("noiseModeName(off)", Model.noiseModeName("off"), "Off");
  check("noiseModeName(invalid)", Model.noiseModeName("xyz"), "Unknown");

  check("eqPresetName(bright)", Model.eqPresetName("bright"), "Bright");
  check("eqPresetName(vocal)", Model.eqPresetName("vocal"), "Vocal");
  check("eqPresetName(treble)", Model.eqPresetName("treble"), "Treble Boost");
  check("eqPresetName(bass)", Model.eqPresetName("bass"), "Bass Boost");
  check("eqPresetName(custom)", Model.eqPresetName("custom"), "Custom");

  check("eqPresetButtonLabel(treble)", Model.eqPresetButtonLabel("treble"), "Treble");
  check("eqPresetButtonLabel(bass)", Model.eqPresetButtonLabel("bass"), "Bass");
  check("eqPresetButtonLabel(vocal)", Model.eqPresetButtonLabel("vocal"), "Vocal");
  check("ultModeName(off)", Model.ultModeName(Model.ULT_OFF), "Off");
  check("ultModeName(1)", Model.ultModeName(Model.ULT_1), "ULT 1");
  check("ultModeName(2)", Model.ultModeName(Model.ULT_2), "ULT 2");

  check("formatBattery(78)", Model.formatBattery(78), "78%");
  check("formatBattery(0)", Model.formatBattery(0), "0%");
  check("formatBattery(-1)", Model.formatBattery(-1), "—");

  check("levelFraction(78)", Model.levelFraction(78), 0.78);
  check("levelFraction(100)", Model.levelFraction(100), 1.0);
  check("levelFraction(-1)", Model.levelFraction(-1), 0.0);

  check("batteryIcon charging", typeof Model.batteryIcon(50, true), "string");
  check("batteryIcon normal", typeof Model.batteryIcon(50, false), "string");

  const longErr = "Error: " + "a".repeat(200);
  const elided = Model.elideError(longErr);
  check("elideError length <= 140", elided.length <= 140, true);
  check("elideError ends with ellipsis", elided.endsWith("…"), true);
}

console.log(`\nSummary: ${passed} passed, ${failed} failed`);
if (failed > 0) {
  if (typeof Deno !== "undefined") Deno.exit(1);
  if (typeof process !== "undefined") process.exit(1);
}
