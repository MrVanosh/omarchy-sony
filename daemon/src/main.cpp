#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <optional>
#include <chrono>
#include <deque>
#include <functional>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/signalfd.h>

#include "BluetoothManager.hpp"
#include "MDRProtocol.hpp"
#include "StateEngine.hpp"
#include "IpcServer.hpp"

namespace {

using namespace omarchy::sony;
using namespace omarchy::sony::protocol;

constexpr const char* DAEMON_VERSION = "0.3.0";

struct DaemonOptions {
    bool showHelp{false};
    bool showVersion{false};
    bool mockMode{false};
    std::string preferredMac;
    std::string stateDir;
    std::string runtimeDir;
};

void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [OPTIONS]\n\n"
              << "Headless background daemon managing Sony ULT WEAR headphones on Linux.\n\n"
              << "Options:\n"
              << "  -h, --help               Display this help message and exit\n"
              << "  -v, --version            Display version information and exit\n"
              << "  -m, --mock               Run in mock simulation mode (completely offline)\n"
              << "  -d, --device <MAC>       Target specific Bluetooth MAC address\n"
              << "      --state-dir <DIR>    Override directory for status.json\n"
              << "      --runtime-dir <DIR>  Override directory for IPC socket\n\n";
}

void printVersion() {
    std::cout << "sony-headphones-daemon " << DAEMON_VERSION << "\n";
}

std::optional<DaemonOptions> parseCommandLine(int argc, char* argv[]) {
    DaemonOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
            return opts;
        } else if (arg == "-v" || arg == "--version") {
            opts.showVersion = true;
            return opts;
        } else if (arg == "-m" || arg == "--mock") {
            opts.mockMode = true;
        } else if (arg == "-d" || arg == "--device") {
            if (i + 1 < argc) {
                opts.preferredMac = argv[++i];
            } else {
                std::cerr << "Error: --device requires a MAC address argument\n";
                return std::nullopt;
            }
        } else if (arg == "--state-dir") {
            if (i + 1 < argc) {
                opts.stateDir = argv[++i];
            } else {
                std::cerr << "Error: --state-dir requires a directory path\n";
                return std::nullopt;
            }
        } else if (arg == "--runtime-dir") {
            if (i + 1 < argc) {
                opts.runtimeDir = argv[++i];
            } else {
                std::cerr << "Error: --runtime-dir requires a directory path\n";
                return std::nullopt;
            }
        } else {
            std::cerr << "Error: Unrecognized option '" << arg << "'\n";
            return std::nullopt;
        }
    }
    return opts;
}

std::string resolveStateDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        return overrideDir;
    }
    const char* xdgState = std::getenv("XDG_STATE_HOME");
    if (xdgState && xdgState[0] != '\0') {
        return std::string(xdgState);
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.local/state";
    }
    return "/tmp";
}

std::string resolveRuntimeDir(const std::string& overrideDir) {
    if (!overrideDir.empty()) {
        return overrideDir;
    }
    const char* xdgRun = std::getenv("XDG_RUNTIME_DIR");
    if (xdgRun && xdgRun[0] != '\0') {
        return std::string(xdgRun);
    }
    return "/tmp/run-" + std::to_string(::getuid());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Main Daemon Entry Point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Restrict default permissions on all newly created files and directories
    ::umask(0077);

    auto optsOpt = parseCommandLine(argc, argv);
    if (!optsOpt) {
        return 1;
    }
    const auto& opts = *optsOpt;

    if (opts.showHelp) {
        printHelp(argv[0]);
        return 0;
    }
    if (opts.showVersion) {
        printVersion();
        return 0;
    }

    // 1. Resolve paths
    std::string stateDir = resolveStateDir(opts.stateDir);
    std::string runtimeDir = resolveRuntimeDir(opts.runtimeDir);

    // 2. Setup Linux signalfd (ignoring SIGPIPE, blocking SIGINT/TERM/USR1/USR2)
    ::signal(SIGPIPE, SIG_IGN);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);

    if (::sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        std::cerr << "Fatal: Failed to mask signals: " << std::strerror(errno) << "\n";
        return 1;
    }

    int sigFd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigFd < 0) {
        std::cerr << "Fatal: Failed to create signalfd: " << std::strerror(errno) << "\n";
        return 1;
    }

    // 3. Initialize StateEngine
    StateEngine stateEngine(stateDir);
    if (!stateEngine.initialize(opts.mockMode ? true : false)) {
        std::cerr << "Fatal: Failed to initialize StateEngine at " << stateDir << "\n";
        ::close(sigFd);
        return 1;
    }

    // In mock mode, ensure initial standard state
    if (opts.mockMode) {
        stateEngine.setConnected(true, "ULT WEAR");
        stateEngine.setBattery(85, false);
        stateEngine.updateNoiseMode("anc", 0, false);
        stateEngine.updateUltMode(0);
        stateEngine.updateDsee(true);
        stateEngine.save();
    }

    // 4. Initialize BluetoothManager
    BluetoothConfig btConfig;
    btConfig.preferredMac = opts.preferredMac;
    btConfig.autoReconnect = true;
    btConfig.mockMode = opts.mockMode;

    int mockPeerFd = -1;
    std::unique_ptr<BluetoothManager> btManager;
    if (opts.mockMode) {
        btManager = BluetoothManager::createMock(btConfig, &mockPeerFd);
    } else {
        btManager = BluetoothManager::createLinux(btConfig);
    }

    if (!btManager) {
        std::cerr << "Fatal: Failed to create BluetoothManager\n";
        stateEngine.cleanup();
        ::close(sigFd);
        return 1;
    }

    // Protocol stream framer
    StreamFramer streamFramer;

    uint8_t txSeq = 0;
    auto nextSeq = [&]() -> uint8_t {
        uint8_t s = txSeq;
        txSeq ^= 1;
        return s;
    };

    // MDR is strictly sequenced: the headset acknowledges one frame before it is
    // ready for the next. Writing frames back to back makes it drop some of them
    // with no error of any kind — four control commands in a row reliably lose
    // one — so every outbound frame goes through this queue and the next is
    // released only once the previous one has been acknowledged.
    //
    // Frames are built lazily because the sequence bit has to alternate in the
    // order frames actually reach the headset, not the order they were queued.
    //
    // Our own ACKs bypass the queue: they are replies, not commands, and the
    // headset retransmits until it gets one.
    std::deque<std::function<std::vector<uint8_t>()>> txQueue;
    bool txPending = false;
    std::chrono::steady_clock::time_point txSentAt{};

    auto pumpTx = [&]() {
        if (txQueue.empty()) {
            txPending = false;
            return;
        }
        auto buildFrame = txQueue.front();
        txQueue.pop_front();
        auto frame = buildFrame();
        fprintf(stderr, "[DAEMON] TX frame (%zu bytes, %zu still queued)\n",
                frame.size(), txQueue.size());
        fflush(stderr);
        btManager->sendPacket(frame);
        txPending = true;
        txSentAt = std::chrono::steady_clock::now();
    };

    auto enqueueTx = [&](std::function<std::vector<uint8_t>()> buildFrame) {
        txQueue.push_back(std::move(buildFrame));
        if (!txPending) {
            pumpTx();
        }
    };

    BluetoothCallbacks callbacks;
    callbacks.onConnected = [&]() {
        txSeq = 0;
        const auto& dev = btManager->getCurrentDevice();
        std::string name = dev.name.empty() ? "ULT WEAR" : dev.name;
        stateEngine.setConnected(true, name);
        if (dev.batteryLevel >= 0) {
            stateEngine.setBattery(dev.batteryLevel, false);
        }
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Headset connected (%s, battery: %d%%)\n",
                name.c_str(), dev.batteryLevel);
        fflush(stderr);

        txQueue.clear();
        txPending = false;

        if (!opts.mockMode) {
            // Open the Sony MDR v2 session, then query the ULT WEAR state.
            enqueueTx([&]() { return serializeHandshake(nextSeq()); });
            enqueueTx([&]() { return serializeQueryBattery(nextSeq()); });
            enqueueTx([&]() { return serializeQueryNoiseMode(nextSeq()); });
            enqueueTx([&]() { return serializeQueryUltMode(nextSeq()); });
            enqueueTx([&]() { return serializeQueryDsee(nextSeq()); });
        }
    };

    callbacks.onDisconnected = [&](const std::string& reason) {
        txSeq = 0;
        txQueue.clear();
        txPending = false;
        streamFramer.reset();
        stateEngine.setConnected(false);
        stateEngine.save();
        fprintf(stderr, "[DAEMON] Headset disconnected: %s\n", reason.c_str());
        fflush(stderr);
    };

    callbacks.onDataReceived = [&](const uint8_t* data, size_t length) {
        streamFramer.append(std::span<const uint8_t>(data, length));
        while (auto frameOpt = streamFramer.nextFrame()) {
            auto unpackedOpt = unpackFrame(*frameOpt);
            if (!unpackedOpt) continue;

            const auto& unpacked = *unpackedOpt;
            if (unpacked.type == PacketType::DATA_MDR || unpacked.type == PacketType::DATA_MDR_NO2) {
                // Reply with ACK packet
                auto ack = serializeACK(unpacked.seq);
                btManager->sendPacket(ack);

                fprintf(stderr, "[DAEMON] RX payload (cmd=0x%02x, size=%zu): ",
                        unpacked.payload.empty() ? 0 : unpacked.payload[0], unpacked.payload.size());
                for (uint8_t b : unpacked.payload) {
                    fprintf(stderr, "%02x ", b);
                }
                fprintf(stderr, "\n");
                fflush(stderr);

                // Update state from payload
                bool changed = protocol::parseInboundPayload(unpacked.payload, stateEngine.getStateUnsafe());
                if (changed) {
                    stateEngine.save();
                    fprintf(stderr, "[DAEMON] Updated state: mode=%s, ambient=%d, voice=%d\n",
                            stateEngine.getState().noise_mode.c_str(),
                            stateEngine.getState().ambient_sound_level,
                            stateEngine.getState().voice_passthrough ? 1 : 0);
                    fflush(stderr);
                }
            } else if (unpacked.type == PacketType::ACK) {
                // Exactly one acknowledgement comes back per frame we send, even
                // for a command the headset chooses not to answer, so this is the
                // signal to release the next queued frame.
                pumpTx();
            }
        }
    };

    btManager->setCallbacks(std::move(callbacks));
    btManager->start();

    // 5. Initialize UNIX Domain Socket IPC Server
    std::string socketPath = runtimeDir + "/sony-headphones.sock";
    IpcServer ipcServer(socketPath);

    IpcCallbacks ipcCb;
    ipcCb.getStatusJson = [&]() {
        return stateEngine.getStatusJson();
    };
    ipcCb.setNoiseMode = [&](NoiseMode mode, uint8_t ambientLevel, std::string& /*err*/) {
        const uint8_t finalLevel = 0;
        stateEngine.updateNoiseMode(noiseModeToString(mode), 0, stateEngine.getState().voice_passthrough);
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            bool voice = stateEngine.getState().voice_passthrough;
            fprintf(stderr, "[DAEMON] Queueing noise mode: %s (level %u)\n",
                    noiseModeToString(mode).c_str(), (unsigned)finalLevel);
            fflush(stderr);
            enqueueTx([&, mode, finalLevel, voice]() {
                return serializeNoiseMode(mode, finalLevel, voice, nextSeq());
            });
        } else {
            fprintf(stderr, "[DAEMON] Warning: BT not connected, packet queued/skipped\n");
            fflush(stderr);
        }
        return true;
    };
    ipcCb.setVoiceFocus = [&](bool enabled, std::string& /*err*/) {
        stateEngine.updateNoiseMode("ambient", 0, enabled);
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            fprintf(stderr, "[DAEMON] Queueing Voice Focus: %s\n", enabled ? "on" : "off");
            fflush(stderr);
            enqueueTx([&, enabled]() {
                return serializeNoiseMode(NoiseMode::AMBIENT, 0, enabled, nextSeq());
            });
        }
        return true;
    };
    ipcCb.setUltMode = [&](UltMode mode, std::string& /*err*/) {
        stateEngine.updateUltMode(static_cast<int>(mode));
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            fprintf(stderr, "[DAEMON] Queueing ULT mode: %u\n", static_cast<unsigned>(mode));
            fflush(stderr);
            enqueueTx([&, mode]() { return serializeUltMode(mode, nextSeq()); });
        }
        return true;
    };
    ipcCb.setAmbientLevel = [&](uint8_t level, std::string& /*err*/) {
        stateEngine.updateAmbientLevel(level);
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            bool voice = stateEngine.getState().voice_passthrough;
            fprintf(stderr, "[DAEMON] Queueing ambient level: %u\n", (unsigned)level);
            fflush(stderr);
            enqueueTx([&, level, voice]() {
                return serializeAmbientLevel(level, voice, nextSeq());
            });
        }
        return true;
    };
    ipcCb.setEqPreset = [&](EqPreset preset, std::string& /*err*/) {
        stateEngine.updateEqPreset(eqPresetToString(preset));
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            fprintf(stderr, "[DAEMON] Queueing EQ preset: %s\n", eqPresetToString(preset).c_str());
            fflush(stderr);
            enqueueTx([&, preset]() { return serializeEqPreset(preset, nextSeq()); });
        }
        return true;
    };
    ipcCb.setCustomEq = [&](const std::array<int, 5>& bands, int clearBass, std::string& /*err*/) {
        stateEngine.updateCustomEq(bands, clearBass);
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            fprintf(stderr, "[DAEMON] Queueing Custom EQ\n");
            fflush(stderr);
            enqueueTx([&, bands, clearBass]() { return serializeCustomEq(bands, clearBass, nextSeq()); });
        }
        return true;
    };
    ipcCb.setDsee = [&](bool enabled, std::string& /*err*/) {
        stateEngine.updateDsee(enabled);
        stateEngine.save();
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            fprintf(stderr, "[DAEMON] Queueing DSEE: %s\n", enabled ? "on" : "off");
            fflush(stderr);
            enqueueTx([&, enabled]() { return serializeDsee(enabled, nextSeq()); });
        }
        return true;
    };
    ipcCb.sendPacket = [&](const std::vector<uint8_t>& packet) {
        if (btManager && btManager->getState() == ConnectionState::CONNECTED) {
            btManager->sendPacket(packet);
        }
        return true;
    };
    ipcCb.onTestSetBattery = [&](int level, bool charging) {
        stateEngine.setBattery(level, charging);
        stateEngine.save();
    };
    ipcCb.onTestDisconnect = [&]() {
        stateEngine.setConnected(false);
        stateEngine.save();
    };
    ipcCb.onTestReconnect = [&]() {
        stateEngine.setConnected(true);
        stateEngine.save();
    };

    ipcServer.setCallbacks(std::move(ipcCb));

    if (!ipcServer.start()) {
        std::cerr << "Fatal: Failed to start IPC server at " << socketPath << "\n";
        btManager->stop();
        stateEngine.cleanup();
        ::close(sigFd);
        return 1;
    }

    // 6. Signal daemon readiness
    if (opts.mockMode) {
        std::cout << "[MOCK_DAEMON] Ready PID=" << ::getpid() << std::endl;
    } else {
        std::cout << "[DAEMON] Ready PID=" << ::getpid() << std::endl;
    }

    // 7. Unified Event Loop
    bool running = true;

    while (running) {
        std::vector<struct pollfd> pfds;

        // Entry 0: Signal descriptor
        pfds.push_back({sigFd, POLLIN, 0});

        // Entry 1 (Optional): Bluetooth transport descriptor
        int btFd = btManager->getPollFd();
        short btEvents = btManager->getPollEvents();
        int btIndex = -1;
        if (btFd >= 0 && btEvents != 0) {
            btIndex = static_cast<int>(pfds.size());
            pfds.push_back({btFd, btEvents, 0});
        }

        // Entries 2+: IPC listen socket and connected client sockets
        size_t ipcStartIndex = pfds.size();
        ipcServer.appendPollFds(pfds);

        // Poll with 100ms timeout for periodic BluetoothManager tick
        int pollRc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
        if (pollRc < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Event loop poll error: " << std::strerror(errno) << "\n";
            break;
        }

        // Check Signal Descriptor
        if (pfds[0].revents & POLLIN) {
            struct signalfd_siginfo fdsi{};
            ssize_t s = ::read(sigFd, &fdsi, sizeof(fdsi));
            if (s == sizeof(fdsi)) {
                if (fdsi.ssi_signo == SIGINT || fdsi.ssi_signo == SIGTERM) {
                    running = false;
                    break;
                } else if (fdsi.ssi_signo == SIGUSR1) {
                    // Simulated disconnect
                    stateEngine.setConnected(false);
                    stateEngine.save();
                    if (!opts.mockMode) {
                        btManager->disconnect();
                    }
                } else if (fdsi.ssi_signo == SIGUSR2) {
                    // Simulated reconnect
                    stateEngine.setConnected(true);
                    stateEngine.save();
                    if (!opts.mockMode) {
                        btManager->start();
                    }
                }
            }
        }

        // Check Bluetooth Transport Descriptor
        if (btIndex >= 0 && (pfds[btIndex].revents != 0)) {
            btManager->handleSocketEvent(pfds[btIndex].revents);
        }

        // Check IPC Descriptors
        for (size_t i = ipcStartIndex; i < pfds.size(); ++i) {
            if (pfds[i].revents != 0) {
                ipcServer.handleSocketEvent(pfds[i].fd, pfds[i].revents);
            }
        }

        // Subsystem periodic tick (timeouts and reconnect backoff)
        btManager->tick();

        // Never let a silent headset stall the queue: if an acknowledgement does
        // not arrive, move on rather than wedging every later command.
        if (txPending && !txQueue.empty()) {
            auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - txSentAt).count();
            if (waitedMs > 1500) {
                fprintf(stderr, "[DAEMON] Frame unacknowledged after %lldms; releasing next\n",
                        static_cast<long long>(waitedMs));
                fflush(stderr);
                pumpTx();
            }
        }
    }

    // 8. Graceful Shutdown & Resource Cleanup
    if (opts.mockMode) {
        std::cout << "[MOCK_DAEMON] Shutdown cleanly\n";
    } else {
        std::cout << "[DAEMON] Shutdown cleanly\n";
    }
    std::cout.flush();

    btManager->stop();
    ipcServer.stop();
    stateEngine.cleanup();

    if (mockPeerFd >= 0) {
        ::close(mockPeerFd);
        mockPeerFd = -1;
    }
    if (sigFd >= 0) {
        ::close(sigFd);
        sigFd = -1;
    }

    return 0;
}
