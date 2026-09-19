#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <limits>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

std::string to_lower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool parse_int(const std::string& str, int& out) {
    if (str.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    long val = std::strtol(str.c_str(), &end, 10);
    if (errno != 0 || end == str.c_str() || *end != '\0') {
        return false;
    }
    if (val < std::numeric_limits<int>::min() || val > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(val);
    return true;
}

bool is_negative_number(const std::string& str) {
    if (str.size() < 2 || str[0] != '-') {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(str[1])) != 0;
}

void print_usage(std::ostream& os) {
    os << "Usage: sony-ctl [-s <socket>] <subcommand> [args...]\n"
       << "Subcommands: status, noise, voice-focus, ult, dsee\n\n"
       << "Options:\n"
       << "  -s, --socket <path>  Override socket path\n"
       << "  -h, --help           Show help\n"
       << "  -v, --version        Show version\n";
}

std::string get_default_socket_path() {
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != nullptr && *xdg_runtime != '\0') {
        return std::string(xdg_runtime) + "/sony-headphones.sock";
    }
    return "/tmp/run-" + std::to_string(::getuid()) + "/sony-headphones.sock";
}

int send_command(const std::string& socket_path, const std::string& command_str, bool is_status) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "Error: Cannot create socket (" << std::strerror(errno) << ")\n";
        return 1;
    }

    // Configure 2.0 second socket timeouts
    struct timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "Error: Socket path too long: " << socket_path << "\n";
        ::close(fd);
        return 1;
    }
    std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());
    addr.sun_path[socket_path.size()] = '\0';

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Error: Cannot connect to daemon socket (" << std::strerror(errno) << ")\n";
        ::close(fd);
        return 2;
    }

    std::string payload = command_str;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    size_t total_sent = 0;
    while (total_sent < payload.size()) {
        ssize_t sent = ::send(fd, payload.data() + total_sent, payload.size() - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Socket communication failed (" << std::strerror(errno) << ")\n";
            ::close(fd);
            return 1;
        }
        if (sent == 0) {
            std::cerr << "Error: Socket communication failed (connection closed)\n";
            ::close(fd);
            return 1;
        }
        total_sent += static_cast<size_t>(sent);
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            response.append(buf, static_cast<size_t>(n));
            if (response.find('\n') != std::string::npos) {
                break;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Error: Socket communication failed (" << std::strerror(errno) << ")\n";
            ::close(fd);
            return 1;
        }
    }
    ::close(fd);

    size_t newline_pos = response.find('\n');
    if (newline_pos != std::string::npos) {
        response = response.substr(0, newline_pos);
    }
    while (!response.empty() && (response.back() == '\r' || response.back() == ' ' || response.back() == '\t')) {
        response.pop_back();
    }

    if (response.empty()) {
        std::cerr << "Error: Empty response from daemon\n";
        return 1;
    }

    if (is_status) {
        if (response.rfind("ERR", 0) == 0) {
            std::cerr << "Error from daemon: " << response << "\n";
            return 1;
        }
        std::cout << response << "\n";
        return 0;
    }

    if (response.rfind("OK", 0) == 0) {
        std::cout << "OK\n";
        return 0;
    }

    std::cerr << "Error from daemon: " << response << "\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string socket_path;
    bool show_help = false;
    bool show_version = false;
    std::vector<std::string> remaining;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_help = true;
        } else if (arg == "-v" || arg == "--version") {
            show_version = true;
        } else if (arg == "-s" || arg == "--socket") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a socket path argument\n";
                return 1;
            }
            socket_path = argv[++i];
        } else if (arg.rfind("--socket=", 0) == 0) {
            socket_path = arg.substr(9);
        } else if (arg == "--") {
            for (++i; i < argc; ++i) {
                remaining.emplace_back(argv[i]);
            }
            break;
        } else if (!arg.empty() && arg[0] == '-' && !is_negative_number(arg) && remaining.empty()) {
            std::cerr << "Error: Unknown option '" << arg << "'\n";
            print_usage(std::cerr);
            return 1;
        } else {
            remaining.push_back(arg);
        }
    }

    if (show_help) {
        print_usage(std::cout);
        return 0;
    }

    if (show_version) {
        std::cout << "sony-ctl 0.3.0\n";
        return 0;
    }

    if (remaining.empty()) {
        print_usage(std::cerr);
        return 1;
    }

    if (socket_path.empty()) {
        socket_path = get_default_socket_path();
    }

    std::string subcmd = to_lower(remaining[0]);

    if (subcmd == "status") {
        return send_command(socket_path, "status\n", true);
    }

    if (subcmd == "noise") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'noise' requires a mode: anc, ambient, off\n";
            return 1;
        }
        std::string mode = to_lower(remaining[1]);
        if (mode != "anc" && mode != "ambient" && mode != "off") {
            std::cerr << "Error: Invalid noise mode '" << remaining[1] << "'\n";
            return 1;
        }
        return send_command(socket_path, "noise " + mode + "\n", false);
    }

    if (subcmd == "voice-focus") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'voice-focus' requires 'on' or 'off'\n";
            return 1;
        }
        std::string val = to_lower(remaining[1]);
        if (val != "on" && val != "off") {
            std::cerr << "Error: 'voice-focus' requires 'on' or 'off'\n";
            return 1;
        }
        return send_command(socket_path, "voice-focus " + val + "\n", false);
    }

    if (subcmd == "ult") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'ult' requires off, 1, or 2\n";
            return 1;
        }
        std::string val = to_lower(remaining[1]);
        if (val != "off" && val != "0" && val != "1" && val != "2" &&
            val != "ult1" && val != "ult2") {
            std::cerr << "Error: 'ult' requires off, 1, or 2\n";
            return 1;
        }
        return send_command(socket_path, "ult " + val + "\n", false);
    }

    if (subcmd == "ambient-level") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'ambient-level' requires an integer level between 0 and 20\n";
            return 1;
        }
        int level = 0;
        if (!parse_int(remaining[1], level)) {
            std::cerr << "Error: Invalid integer '" << remaining[1] << "'\n";
            return 1;
        }
        if (level < 0 || level > 20) {
            std::cerr << "Error: Level " << level << " out of range [0, 20]\n";
            return 1;
        }
        return send_command(socket_path, "ambient-level " + std::to_string(level) + "\n", false);
    }

    if (subcmd == "eq") {
        if (remaining.size() < 2) {
            std::cerr << "Error: 'eq' requires a preset or 'custom' with 6 parameters\n";
            return 1;
        }
        std::string preset = to_lower(remaining[1]);
        if (preset == "custom") {
            if (remaining.size() < 8) {
                std::cerr << "Error: 'eq custom' requires 5 bands and clear bass (6 integers between -10 and 10)\n";
                return 1;
            }
            std::array<int, 5> bands{};
            for (size_t i = 0; i < 5; ++i) {
                int b = 0;
                if (!parse_int(remaining[2 + i], b)) {
                    std::cerr << "Error: EQ parameters must be valid integers\n";
                    return 1;
                }
                if (b < -10 || b > 10) {
                    std::cerr << "Error: Band values must be between -10 and 10\n";
                    return 1;
                }
                bands[i] = b;
            }
            int cb = 0;
            if (!parse_int(remaining[7], cb)) {
                std::cerr << "Error: EQ parameters must be valid integers\n";
                return 1;
            }
            if (cb < -10 || cb > 10) {
                std::cerr << "Error: Clear Bass must be between -10 and 10\n";
                return 1;
            }
            std::string cmd = "eq custom " + std::to_string(bands[0]) + " "
                                           + std::to_string(bands[1]) + " "
                                           + std::to_string(bands[2]) + " "
                                           + std::to_string(bands[3]) + " "
                                           + std::to_string(bands[4]) + " "
                                           + std::to_string(cb) + "\n";
            return send_command(socket_path, cmd, false);
        }

        static const std::vector<std::string> valid_presets = {
            "off", "bright", "excited", "mellow", "relaxed", "vocal", "treble", "bass", "speech"
        };
        if (std::find(valid_presets.begin(), valid_presets.end(), preset) == valid_presets.end()) {
            std::cerr << "Error: Unknown EQ preset '" << remaining[1] << "'\n";
            return 1;
        }
        return send_command(socket_path, "eq " + preset + "\n", false);
    }

    if (subcmd == "dsee") {
        if (remaining.size() < 2) {
            std::cerr << "Error: '" << remaining[0] << "' requires 'on' or 'off'\n";
            return 1;
        }
        std::string val = to_lower(remaining[1]);
        if (val != "on" && val != "off") {
            std::cerr << "Error: '" << remaining[0] << "' requires 'on' or 'off'\n";
            return 1;
        }
        return send_command(socket_path, subcmd + " " + val + "\n", false);
    }

    std::cerr << "Error: Unknown subcommand '" << remaining[0] << "'\n";
    return 1;
}
