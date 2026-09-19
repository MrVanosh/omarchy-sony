# omarchy-sony

> **Fork for Sony ULT WEAR (WH-ULT900N).** It uses Sony's MDR **v2** RFCOMM
> service and model-specific commands verified against a real ULT WEAR headset.
> The protocol details are documented in
> [`docs/ult-wear-protocol.md`](docs/ult-wear-protocol.md).

A lightweight Omarchy bar widget and C++20 background daemon for controlling
**Sony ULT WEAR** headphones on Linux.

---

## Features

- 🔋 **Live Battery & Codec Monitoring:** Real-time battery percentage, charging state, and active Bluetooth audio codec (e.g. LDAC, AAC, SBC).
- 🎧 **Noise Control Modes:** Seamless hardware switching between **ANC (Noise Canceling)**, **Ambient Sound**, and **Off** (passive).
- 🗣️ **Focus on Voice:** Toggle voice emphasis while Ambient Sound is active.
- 🔊 **ULT Power Sound:** Switch between Off, ULT 1, and ULT 2 bass modes.
- ⚙️ **DSEE:** Toggle compressed-audio upscaling.
- 🎚️ **Model-correct controls:** ULT WEAR has binary ANC/Ambient modes, so the widget does not show the ineffective 0–20 ambient slider used by XM5.
- ⌨️ **Keyboard Navigation:** Full vim-style navigation (`h`/`j`/`k`/`l`, `Enter`, `Esc`) inside the panel dropdown.
- 💻 **Standalone CLI (`sony-ctl`):** Full terminal and scripting interface for all headphone controls.
- ⚡ **Zero Polling & Lightweight:** Native BlueZ RFCOMM transport with reactive file-view event updates.

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   Omarchy Shell (QML)                  │
│   ┌───────────────┐ ┌─────────────┐ ┌──────────────┐   │
│   │   SonyIcon    │ │  Service    │ │    Panel     │   │
│   └───────▲───────┘ └──────▲──────┘ └──────▲───────┘   │
│           │                │               │           │
│           └────────────────┼───────────────┘           │
│                            │ watches                   │
│                            │ (FileView)                │
│                 ~/.local/state/sony-headphones/        │
│                           status.json                  │
│                                ▲                       │
│                                │ writes                │
└────────────────────────────────┼───────────────────────┘
                                 │
┌────────────────────────────────┼───────────────────────┐
│  Headless Daemon               │   Companion CLI       │
│  (sony-headphones-daemon)      │   (sony-ctl)          │
│                                │           │           │
│   UNIX Domain Socket ◄─────────┴───────────┘           │
│   (/run/user/$UID/sony-headphones.sock)                │
│                 │                                      │
│                 ▼                                      │
│   Bluetooth RFCOMM Stack (MDR v2 Protocol)             │
│                 │ (channel resolved over SDP)          │
│                 ▼                                      │
│       Sony ULT WEAR (WH-ULT900N)                       │
└────────────────────────────────────────────────────────┘
```

- **`plugin/`**: Quickshell / QML plugin conforming to Omarchy manifest specification version 1.
- **`daemon/`**: Headless C++20 background daemon managing Bluetooth RFCOMM connection and UNIX socket IPC.
- **`cli/`**: Lightweight CLI tool (`sony-ctl`) for terminal queries and control scripts.

---

## Prerequisites

On Arch Linux / Omarchy:
```bash
sudo pacman -S --needed base-devel cmake ninja bluez bluez-libs dbus jq
```

On Debian / Ubuntu:
```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build libbluetooth-dev libdbus-1-dev jq
```

---

## Installation & Setup

Clone the repository and run the automated setup script:

```bash
git clone https://github.com/FelixDaCraft/omarchy-sony.git
cd omarchy-sony
./setup
```

The `./setup` script will:
1. Verify system dependencies.
2. Build the daemon and CLI binaries with CMake and Ninja.
3. Install `sony-headphones-daemon` and `sony-ctl` to `~/.local/bin/`.
4. Register and start the `sony-headphones.service` user systemd unit.
5. Deploy the QML plugin to `~/.config/omarchy/plugins/io.github.felixdacraft.omasony`.

---

## Uninstalling

`setup` only ever writes inside your home directory. Removing it is the exact
reverse, and touches nothing else:

```bash
# 1. Take the widget out of the bar
omarchy plugin disable io.github.felixdacraft.omasony

# 2. Stop and remove the background daemon
systemctl --user disable --now sony-headphones.service
rm -f ~/.config/systemd/user/sony-headphones.service
systemctl --user daemon-reload

# 3. Remove the binaries
rm -f ~/.local/bin/sony-headphones-daemon ~/.local/bin/sony-ctl

# 4. Remove the plugin and its runtime state
rm -rf ~/.config/omarchy/plugins/io.github.felixdacraft.omasony
rm -rf ~/.local/state/sony-headphones
```

Step 1 tells the Omarchy shell to stop loading the widget; do it before deleting
the files so the shell is not left referencing a plugin that no longer exists.

`omarchy plugin remove io.github.felixdacraft.omasony` is a shortcut for step 1
plus the plugin directory in step 4: it disables the plugin, deletes
`~/.config/omarchy/plugins/<id>` and rescans. It does not touch the daemon, the
systemd unit, the binaries or the runtime state, so steps 2 and 3 and the
`~/.local/state` line still apply.

Nothing is installed system-wide, and no configuration outside the paths above
is modified. The build dependencies installed from the Prerequisites section are
ordinary system packages and are left alone.

---

## CLI Usage (`sony-ctl`)

You can query or control your headphones from anywhere via `sony-ctl`:

```bash
# Query full headphone status as JSON
sony-ctl status

# Switch Noise Cancellation Modes
sony-ctl noise anc          # Turn on Active Noise Cancellation
sony-ctl noise ambient      # Switch to Ambient Sound mode
sony-ctl noise off          # Turn off noise processing

# Focus on Voice (switches to Ambient Sound)
sony-ctl voice-focus on

# ULT Power Sound
sony-ctl ult off
sony-ctl ult 1
sony-ctl ult 2

# Toggle Smart Features
sony-ctl dsee on
```

---

## Systemd Service Management

The background daemon is managed automatically via user systemd:

```bash
# Check daemon status and logs
systemctl --user status sony-headphones.service
journalctl --user -u sony-headphones.service -f

# Restart daemon
systemctl --user restart sony-headphones.service
```

---

## Testing

Run unit tests and end-to-end integration tests:

```bash
# C++ Protocol Unit Tests
cmake --build build --target test

# JavaScript Model Unit Tests (requires Deno or Node.js)
deno run --allow-read tests/model.test.js

# End-to-End Simulation Tests
./tests/integration.sh
```

---

## Documentation

The verified **ULT WEAR MDR v2 wire protocol** — UUID, RFCOMM channel, framing,
queries and controls — is documented in
[`docs/ult-wear-protocol.md`](docs/ult-wear-protocol.md). The older XM3 and XM5
notes remain under `docs/` for historical reference.

---

## Acknowledgments & References

This project builds upon and draws inspiration from these open-source projects:

1. **[thisisgm/omarchy-pods](https://github.com/thisisgm/omarchy-pods)**:
   - Architecture reference for the Omarchy bar widget + headless background daemon + atomic state file design.
2. **[mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient)**:
   - Reverse-engineered protocol definitions and implementation reference for Sony MDR Bluetooth RFCOMM communication.
3. **[andROYdified/omarchy-sony](https://github.com/andROYdified/omarchy-sony)** by Roy Kevin De Jesus:
   - The upstream project this fork is based on (Omarchy plugin, daemon, CLI and test suite), MIT licensed.
4. **[Leonard013/sony-ult-ctl](https://github.com/Leonard013/sony-ult-ctl)**:
   - Independently captured ULT WEAR commands used to cross-check the live hardware responses.

---

## Disclaimer

This is an unofficial, independent community project developed for Linux desktop integration. It is not affiliated with, authorized, maintained, sponsored, or endorsed by Sony Corporation or any of its subsidiaries. "Sony", "ULT WEAR", "WH-ULT900N", and related marks are trademarks of Sony Corporation.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
