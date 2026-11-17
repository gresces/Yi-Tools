// yi-niri — turn niri session lifecycle transitions into Yi events.
//
// Event types:
//   * session start   -> system.boot
//   * system shutdown -> system.shutdown  (logind PrepareForShutdown)
//   * screen lock     -> system.lock / system.unlock (logind LockedHint)
//
// niri itself is the one setting LockedHint on the logind session object
// (src/niri.rs calls SetLockedHint when an ext-session-lock client locks the
// session), so logind is the authoritative source and no private niri IPC is
// needed. niri's own event-stream has no lock event.
//
// Events are published through lib/cpp/yi.hpp.

#include <curl/curl.h>
#include <systemd/sd-bus.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "yi.hpp"

namespace {

constexpr const char *kVersion = "0.1.0";

constexpr const char *kLogin1 = "org.freedesktop.login1";
constexpr const char *kManagerPath = "/org/freedesktop/login1";
constexpr const char *kManagerIface = "org.freedesktop.login1.Manager";
constexpr const char *kSessionIface = "org.freedesktop.login1.Session";
constexpr const char *kPropertiesIface = "org.freedesktop.DBus.Properties";
constexpr const char *kAutoSessionPath = "/org/freedesktop/login1/session/auto";

volatile std::sig_atomic_t g_stop = 0;

void handle_stop(int) { g_stop = 1; }

void log_line(const std::string &message) {
    std::fprintf(stderr, "yi-niri: %s\n", message.c_str());
}

std::string env_or(const char *name, const std::string &fallback = {}) {
    const char *value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

// One-line label shown by the Yi frontend in the content column.
std::string event_text(const std::string &type, const std::string &reason) {
    if (type == "system.boot") {
        return "开机";
    }
    if (type == "system.lock") {
        return "锁屏";
    }
    if (type == "system.unlock") {
        return "解锁";
    }
    if (type == "system.shutdown") {
        return reason == "agent-stop" ? "服务停止" : "关机";
    }
    return type;
}

std::string host_name() {
    char buffer[256];
    if (::gethostname(buffer, sizeof buffer) != 0) {
        return {};
    }
    buffer[sizeof buffer - 1] = '\0';
    return buffer;
}

std::string read_first_line(const char *path) {
    std::FILE *file = std::fopen(path, "r");
    if (file == nullptr) {
        return {};
    }
    char buffer[256];
    const char *line = std::fgets(buffer, sizeof buffer, file);
    std::fclose(file);
    if (line == nullptr) {
        return {};
    }

    std::string value(line);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

std::string kernel_release() {
    utsname info{};
    if (::uname(&info) != 0) {
        return {};
    }
    return info.release;
}

long long uptime_seconds() {
    std::FILE *file = std::fopen("/proc/uptime", "r");
    if (file == nullptr) {
        return -1;
    }
    double seconds = 0.0;
    const int ok = std::fscanf(file, "%lf", &seconds);
    std::fclose(file);
    return ok == 1 ? static_cast<long long>(seconds) : -1;
}

// Ask the running niri for its version over the IPC socket. The request and
// reply are newline-terminated JSON; we only need the version string.
std::string niri_version() {
    const std::string socket_path = env_or("NIRI_SOCKET");
    if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
        return {};
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return {};
    }

    timeval timeout{};
    timeout.tv_usec = 500 * 1000; // never let a wedged compositor block us
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);

    std::string version;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0) {
        const char request[] = "\"Version\"\n";
        if (::send(fd, request, sizeof request - 1, 0) > 0) {
            char reply[512];
            const ssize_t size = ::recv(fd, reply, sizeof reply - 1, 0);
            if (size > 0) {
                reply[size] = '\0';
                const std::string body(reply);
                const std::string key = "\"Version\":\"";
                auto begin = body.find(key);
                if (begin != std::string::npos) {
                    begin += key.size();
                    const auto end = body.find('"', begin);
                    if (end != std::string::npos) {
                        version = body.substr(begin, end - begin);
                    }
                }
            }
        }
    }
    ::close(fd);
    return version;
}

// Static facts about this machine, collected once at startup.
struct SystemInfo {
    std::string host;
    std::string user;
    std::string session;
    std::string kernel;
    std::string boot_id;
    std::string niri;
};

SystemInfo collect_info() {
    SystemInfo info;
    info.host = host_name();
    info.user = env_or("USER");
    info.session = env_or("XDG_SESSION_ID");
    info.kernel = kernel_release();
    info.boot_id = read_first_line("/proc/sys/kernel/random/boot_id");
    info.niri = niri_version();
    return info;
}

// Built-in defaults. Real deployments get their endpoint from
// ~/.config/yi/bus.conf; these values only make a fresh install usable.
constexpr const char *kDefaultUrl = "http://127.0.0.1:9092";
constexpr const char *kDefaultToken = "";
// The compositor, not a machine identity. Override per host with --source or
// `source = ...` in niri.conf when several machines report to one bus.
constexpr const char *kDefaultSource = "niri";
constexpr int kDefaultTimeoutMs = 5000;

std::string trim_copy(const std::string &value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lowercase(std::string value) {
    for (char &character : value) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

// $XDG_CONFIG_HOME/yi, falling back to $HOME/.config/yi.
std::string config_dir() {
    const std::string xdg = env_or("XDG_CONFIG_HOME");
    if (!xdg.empty()) {
        return xdg + "/yi";
    }
    const std::string home = env_or("HOME");
    return home.empty() ? std::string() : home + "/.config/yi";
}

struct IniEntry {
    std::string key;
    std::string value;
};

// Minimal INI reader: `key = value` lines, '#' and ';' comments. Unknown keys
// are returned too; callers simply ignore the ones they do not know.
bool read_ini(const std::string &path, std::vector<IniEntry> &entries) {
    entries.clear();
    if (path.empty()) {
        return false;
    }
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        return false;
    }
    char buffer[1024];
    while (std::fgets(buffer, sizeof buffer, file) != nullptr) {
        const std::string line = trim_copy(buffer);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const std::size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        IniEntry entry;
        entry.key = lowercase(trim_copy(line.substr(0, equal)));
        entry.value = trim_copy(line.substr(equal + 1));
        if (entry.value.size() >= 2) {
            const char quote = entry.value.front();
            if ((quote == '"' || quote == '\'') && entry.value.back() == quote) {
                entry.value = entry.value.substr(1, entry.value.size() - 2);
            }
        }
        if (!entry.key.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    std::fclose(file);
    return true;
}

bool parse_port(const std::string &text, int &port) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < 1 ||
        value > 65535) {
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

bool parse_positive_int(const std::string &text, int &number) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < 1 ||
        value > INT_MAX) {
        return false;
    }
    number = static_cast<int>(value);
    return true;
}

std::string scheme_host_port_url(const std::string &scheme,
                                 const std::string &host, int port) {
    char suffix[32];
    std::snprintf(suffix, sizeof suffix, ":%d", port);
    return scheme + "://" + host + suffix;
}

struct BusConfig {
    std::string scheme = "http";
    std::string host = "127.0.0.1";
    int port = 9092;
    std::string token;
    std::string url; // complete override, empty when not configured
    std::string path;
    bool found = false;
    bool has_url = false;
    bool has_token = false;
};

void split_authority(const std::string &authority, BusConfig &config) {
    if (authority.empty()) {
        return;
    }
    if (authority.front() == '[') { // [IPv6]:port
        const std::size_t close = authority.find(']');
        if (close == std::string::npos || close <= 1) {
            return;
        }
        config.host = authority.substr(1, close - 1);
        if (close + 2 <= authority.size() && authority[close + 1] == ':') {
            int port = 0;
            if (parse_port(authority.substr(close + 2), port)) {
                config.port = port;
            }
        }
        return;
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && colon > 0) {
        int port = 0;
        if (parse_port(authority.substr(colon + 1), port)) {
            config.host = authority.substr(0, colon);
            config.port = port;
            return;
        }
    }
    config.host = authority;
}

// `url = ...` overrides scheme/host/port as a whole.
bool apply_bus_url(BusConfig &config, const std::string &url) {
    const std::size_t separator = url.find("://");
    if (separator == std::string::npos || separator == 0) {
        return false;
    }
    const std::string scheme = url.substr(0, separator);
    std::string rest = url.substr(separator + 3);
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        rest.resize(slash);
    }
    if (rest.empty()) {
        return false;
    }
    config.scheme = scheme;
    split_authority(rest, config);
    config.url = url;
    config.has_url = true;
    return true;
}

BusConfig load_bus_config() {
    BusConfig config;
    const std::string dir = config_dir();
    config.path = dir.empty() ? std::string() : dir + "/bus.conf";

    std::vector<IniEntry> entries;
    if (!read_ini(config.path, entries)) {
        return config;
    }
    config.found = true;

    std::string url;
    for (const IniEntry &entry : entries) {
        if (entry.key == "scheme") {
            if (!entry.value.empty()) {
                config.scheme = entry.value;
                config.has_url = true;
            }
        } else if (entry.key == "host") {
            if (!entry.value.empty()) {
                config.host = entry.value;
                config.has_url = true;
            }
        } else if (entry.key == "port") {
            int port = 0;
            if (parse_port(entry.value, port)) {
                config.port = port;
                config.has_url = true;
            }
        } else if (entry.key == "token") {
            config.token = entry.value;
            config.has_token = true;
        } else if (entry.key == "url") {
            url = entry.value;
        }
    }
    if (!url.empty()) {
        apply_bus_url(config, url);
    }
    return config;
}

struct ToolConfig {
    std::string source = kDefaultSource;
    int timeout_ms = kDefaultTimeoutMs;
    // url/token are deprecated here, kept for compatibility (bus.conf wins).
    std::string url;
    std::string token;
    std::string path;
    bool found = false;
    bool has_url = false;
    bool has_token = false;
};

ToolConfig load_tool_config(const std::string &override_path) {
    ToolConfig config;
    if (!override_path.empty()) {
        config.path = override_path;
    } else {
        const std::string dir = config_dir();
        config.path = dir.empty() ? std::string() : dir + "/niri.conf";
    }

    std::vector<IniEntry> entries;
    if (!read_ini(config.path, entries)) {
        return config;
    }
    config.found = true;

    for (const IniEntry &entry : entries) {
        if (entry.key == "source") {
            if (!entry.value.empty()) {
                config.source = entry.value;
            }
        } else if (entry.key == "timeout_ms") {
            int timeout = 0;
            if (parse_positive_int(entry.value, timeout)) {
                config.timeout_ms = timeout;
            }
        } else if (entry.key == "url") {
            if (!entry.value.empty()) {
                config.url = entry.value;
                config.has_url = true;
            }
        } else if (entry.key == "token") {
            config.token = entry.value;
            config.has_token = true;
        }
    }
    return config;
}

struct Options {
    std::string url = kDefaultUrl;
    std::string token = kDefaultToken;
    std::string source = kDefaultSource;
    int timeout_ms = kDefaultTimeoutMs;
    std::string bus_path;
    std::string tool_path;
};

class Agent {
public:
    explicit Agent(Options options)
        : options_(std::move(options)),
          client_(options_.url, client_options(options_)),
          info_(collect_info()) {}

    // Daemon mode: boot event, then follow logind until SIGTERM.
    int run();

    // One-shot mode for manual hooks and testing.
    bool notify(const std::string &type);

private:
    struct Pending {
        std::string type;
        std::string reason;
        long long locked_for_sec = -1;
    };

    static yi::ClientOptions client_options(const Options &options) {
        yi::ClientOptions client;
        client.token = options.token;
        client.request_timeout = std::chrono::milliseconds(options.timeout_ms);
        return client;
    }

    yi::json base_data() const;
    bool publish(const std::string &type, const std::string &reason,
                 long long locked_for_sec = -1);
    bool publish_retry(const std::string &type, const std::string &reason,
                       int attempts);

    std::string resolve_session_path(sd_bus *bus) const;
    bool read_locked_hint(sd_bus *bus, const std::string &session_path) const;
    bool watch(sd_bus *bus, const std::string &session_path);

    void on_shutdown(bool going_down);
    void on_locked_hint(bool locked);
    void drain_pending();

    static int on_manager_message(sd_bus_message *message, void *userdata,
                                  sd_bus_error *error);
    static int on_properties_changed(sd_bus_message *message, void *userdata,
                                     sd_bus_error *error);

    Options options_;
    yi::Client client_;
    SystemInfo info_;

    std::vector<Pending> pending_;
    std::optional<std::chrono::steady_clock::time_point> locked_at_;
    bool locked_ = false;
    bool shutdown_sent_ = false;
};

// Context that is not already implied by source/type/time: which user and
// logind session, which kernel boot, which niri, and how long it has been up.
yi::json Agent::base_data() const {
    yi::json data = yi::json::object();

    const auto put = [&data](const char *key, const std::string &value) {
        if (!value.empty()) {
            data[key] = value;
        }
    };
    put("host", info_.host);
    put("user", info_.user);
    put("session", info_.session);
    put("kernel", info_.kernel);
    put("boot_id", info_.boot_id);
    put("niri", info_.niri);

    const long long uptime = uptime_seconds();
    if (uptime >= 0) {
        data["uptime_sec"] = uptime;
    }
    return data;
}

bool Agent::publish(const std::string &type, const std::string &reason,
                    long long locked_for_sec) {
    yi::json data = base_data();
    data["text"] = event_text(type, reason);
    if (!reason.empty()) {
        data["reason"] = reason;
    }
    if (locked_for_sec >= 0) {
        data["locked_for_sec"] = locked_for_sec;
    }

    try {
        const yi::Event event =
            yi::Event::create(options_.source, {type}, std::move(data));
        const yi::PublishResult result = client_.publish(event);
        log_line("published " + type + " id=" + event.id +
                 " matched=" + std::to_string(result.matched));
        return true;
    } catch (const std::exception &error) {
        log_line("publish " + type + " failed: " + error.what());
        return false;
    }
}

bool Agent::publish_retry(const std::string &type, const std::string &reason,
                          int attempts) {
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (publish(type, reason)) {
            return true;
        }
        if (attempt < attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400 * attempt));
        }
    }
    return false;
}

bool Agent::notify(const std::string &type) {
    // Only shutdown needs a reason (real power-off vs. manual/agent stop).
    const std::string reason = type == "system.shutdown" ? "manual" : "";
    return publish_retry(type, reason, 3);
}

std::string Agent::resolve_session_path(sd_bus *bus) const {
    const std::string session_id = env_or("XDG_SESSION_ID");
    if (session_id.empty()) {
        return kAutoSessionPath;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    const int r = sd_bus_call_method(bus, kLogin1, kManagerPath, kManagerIface,
                                     "GetSession", &error, &reply, "s",
                                     session_id.c_str());
    std::string path;
    if (r >= 0) {
        const char *object = nullptr;
        if (sd_bus_message_read(reply, "o", &object) > 0) {
            path = object;
        }
        sd_bus_message_unref(reply);
    } else {
        log_line(std::string("GetSession failed: ") +
                 (error.message != nullptr ? error.message : std::strerror(-r)));
    }
    sd_bus_error_free(&error);

    return path.empty() ? std::string(kAutoSessionPath) : path;
}

bool Agent::read_locked_hint(sd_bus *bus,
                             const std::string &session_path) const {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    const int r = sd_bus_get_property(
        bus, kLogin1, session_path.c_str(), kSessionIface, "LockedHint", &error,
        &reply, "b");

    bool locked = false;
    if (r >= 0) {
        int value = 0;
        if (sd_bus_message_read(reply, "b", &value) > 0) {
            locked = value != 0;
        }
        sd_bus_message_unref(reply);
    } else {
        log_line(std::string("LockedHint unavailable: ") +
                 (error.message != nullptr ? error.message : std::strerror(-r)));
    }
    sd_bus_error_free(&error);
    return locked;
}

bool Agent::watch(sd_bus *bus, const std::string &session_path) {
    const std::string properties = std::string("type='signal',sender='") +
                                   kLogin1 + "',path='" + session_path +
                                   "',interface='" + kPropertiesIface +
                                   "',member='PropertiesChanged'";
    const std::string shutdown = std::string("type='signal',sender='") +
                                 kLogin1 + "',path='" + kManagerPath +
                                 "',interface='" + kManagerIface +
                                 "',member='PrepareForShutdown'";

    // Passing a null slot lets the bus own the match and free it on unref.
    int r = sd_bus_add_match(bus, nullptr, properties.c_str(),
                             &Agent::on_properties_changed, this);
    if (r < 0) {
        log_line(std::string("match PropertiesChanged failed: ") + std::strerror(-r));
        return false;
    }
    r = sd_bus_add_match(bus, nullptr, shutdown.c_str(),
                         &Agent::on_manager_message, this);
    if (r < 0) {
        log_line(std::string("match PrepareForShutdown failed: ") + std::strerror(-r));
        return false;
    }
    return true;
}

int Agent::on_manager_message(sd_bus_message *message, void *userdata,
                              sd_bus_error * /*error*/) {
    auto *self = static_cast<Agent *>(userdata);
    if (!sd_bus_message_is_signal(message, kManagerIface, "PrepareForShutdown")) {
        return 0;
    }

    int going_down = 0;
    const int r = sd_bus_message_read(message, "b", &going_down);
    if (r < 0) {
        return r;
    }
    if (going_down != 0) {
        self->on_shutdown(true);
    }
    return 0;
}

int Agent::on_properties_changed(sd_bus_message *message, void *userdata,
                                 sd_bus_error * /*error*/) {
    auto *self = static_cast<Agent *>(userdata);
    if (!sd_bus_message_is_signal(message, kPropertiesIface,
                                  "PropertiesChanged")) {
        return 0;
    }

    const char *interface = nullptr;
    int r = sd_bus_message_read(message, "s", &interface);
    if (r < 0) {
        return r;
    }
    if (std::strcmp(interface, kSessionIface) != 0) {
        return 0;
    }

    r = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0) {
        return r;
    }
    while ((r = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY,
                                               "sv")) > 0) {
        const char *key = nullptr;
        r = sd_bus_message_read(message, "s", &key);
        if (r < 0) {
            return r;
        }

        r = sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, nullptr);
        if (r < 0) {
            return r;
        }
        if (r > 0) {
            const char *signature = sd_bus_message_get_signature(message, false);
            if (signature != nullptr && std::strcmp(signature, "b") == 0 &&
                std::strcmp(key, "LockedHint") == 0) {
                int locked = 0;
                r = sd_bus_message_read(message, "b", &locked);
                if (r < 0) {
                    return r;
                }
                self->on_locked_hint(locked != 0);
            }
            r = sd_bus_message_exit_container(message);
            if (r < 0) {
                return r;
            }
        }

        r = sd_bus_message_exit_container(message);
        if (r < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    return sd_bus_message_exit_container(message) < 0 ? r : 0;
}

void Agent::on_shutdown(bool going_down) {
    if (!going_down || shutdown_sent_) {
        return;
    }
    pending_.push_back({"system.shutdown", "prepare-for-shutdown"});
}

void Agent::on_locked_hint(bool locked) {
    if (locked == locked_) {
        return;
    }
    locked_ = locked;

    if (locked) {
        locked_at_ = std::chrono::steady_clock::now();
        pending_.push_back({"system.lock", "", -1});
        return;
    }

    long long seconds = -1;
    if (locked_at_.has_value()) {
        seconds = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now() - *locked_at_)
                      .count();
        locked_at_.reset();
    }
    pending_.push_back({"system.unlock", "", seconds});
}

void Agent::drain_pending() {
    for (const Pending &item : pending_) {
        if (item.type == "system.shutdown") {
            if (shutdown_sent_) {
                continue;
            }
            shutdown_sent_ = true;
        }
        publish(item.type, item.reason, item.locked_for_sec);
    }
    pending_.clear();
}

int Agent::run() {
    std::signal(SIGTERM, handle_stop);
    std::signal(SIGINT, handle_stop);
    std::signal(SIGPIPE, SIG_IGN);

    publish_retry("system.boot", "", 3);

    sd_bus *bus = nullptr;
    int r = sd_bus_default_system(&bus);
    if (r < 0) {
        log_line(std::string("system bus: ") + std::strerror(-r));
        return 1;
    }

    const std::string session_path = resolve_session_path(bus);
    locked_ = read_locked_hint(bus, session_path);
    if (!watch(bus, session_path)) {
        sd_bus_unref(bus);
        return 1;
    }

    log_line("watching " + session_path +
             (locked_ ? " (currently locked)" : " (currently unlocked)"));

    while (g_stop == 0) {
        r = sd_bus_process(bus, nullptr);
        if (r < 0) {
            log_line(std::string("bus process: ") + std::strerror(-r));
            sd_bus_unref(bus);
            return 1;
        }
        drain_pending();
        if (r > 0) {
            continue;
        }

        r = sd_bus_wait(bus, 500 * 1000); // 500ms, also a stop-flag tick
        if (r < 0 && r != -EINTR) {
            log_line(std::string("bus wait: ") + std::strerror(-r));
            sd_bus_unref(bus);
            return 1;
        }
    }

    drain_pending();
    if (!shutdown_sent_) {
        shutdown_sent_ = true;
        publish("system.shutdown", "agent-stop");
    }

    sd_bus_unref(bus);
    return 0;
}

void print_usage(FILE *out) {
    std::fprintf(
        out,
        "yi-niri %s - niri session events for Yi\n"
        "\n"
        "Usage: yi-niri [options] [command]\n"
        "\n"
        "Commands:\n"
        "  run         follow logind and publish boot/lock/unlock/shutdown\n"
        "              (default)\n"
        "  boot        publish system.boot and exit\n"
        "  lock        publish system.lock and exit\n"
        "  unlock      publish system.unlock and exit\n"
        "  shutdown    publish system.shutdown and exit\n"
        "  init        write config; optionally install a systemd user service\n"
        "\n"
        "Options:\n"
        "  -u, --url URL        Yi service URL (default %s)\n"
        "  -t, --token TOKEN    bearer token (default from config)\n"
        "  -s, --source NAME    event source (default niri)\n"
        "      --timeout-ms N   HTTP request timeout (default 5000)\n"
        "      --config PATH    tool config (default ~/.config/yi/niri.conf)\n"
        "      --check          print effective configuration and exit\n"
        "  -V, --version        print version and exit\n"
        "  -h, --help           print this help and exit\n"
        "\n"
        "Environment:\n"
        "  YI_URL, YI_TOKEN     override bus.conf\n"
        "\n"
        "Config:\n"
        "  $XDG_CONFIG_HOME/yi/bus.conf or ~/.config/yi/bus.conf\n",
        kVersion, kDefaultUrl);
}

// ---- init ----------------------------------------------------------------

bool stdin_is_tty() { return ::isatty(STDIN_FILENO) != 0; }

// Reads one line. Returns false when EOF arrives before any byte; a final
// unterminated line is still returned. `hidden` disables echo on a terminal.
bool read_line(std::string &line, bool hidden) {
    line.clear();

    termios saved{};
    bool restore = false;
    if (hidden && stdin_is_tty() && ::tcgetattr(STDIN_FILENO, &saved) == 0) {
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        restore = ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet) == 0;
    }

    int character = EOF;
    while ((character = std::fgetc(stdin)) != EOF) {
        if (character == '\n') {
            break;
        }
        if (character != '\r') {
            line.push_back(static_cast<char>(character));
        }
    }
    const bool eof = character == EOF && line.empty();

    if (restore) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return !eof;
}

// Prints the prompt and updates `value` when the answer is non-empty. On EOF
// `value` keeps its previous contents, which is the documented default.
bool ask(const std::string &prompt, std::string &value, bool hidden = false) {
    std::fputs(prompt.c_str(), stdout);
    std::fflush(stdout);
    std::string line;
    if (!read_line(line, hidden)) {
        return false;
    }
    if (!line.empty()) {
        value = line;
    }
    return true;
}

std::string normalize_scheme(const std::string &value,
                             const std::string &fallback) {
    std::string scheme = trim_copy(value);
    const std::size_t separator = scheme.find("://");
    if (separator != std::string::npos) {
        scheme.resize(separator);
    }
    while (!scheme.empty() && (scheme.back() == ':' || scheme.back() == '/')) {
        scheme.pop_back();
    }
    return scheme.empty() ? fallback : scheme;
}

std::string normalize_host(const std::string &value,
                           const std::string &fallback) {
    std::string host = trim_copy(value);
    while (!host.empty() && host.back() == '/') {
        host.pop_back();
    }
    return host.empty() ? fallback : host;
}

struct ProbeResult {
    bool ok = false;
    std::string reason;
};

std::size_t discard_body(char * /*data*/, std::size_t size, std::size_t count,
                         void * /*userdata*/) {
    return size * count;
}

ProbeResult probe_health(const std::string &url) {
    static const bool curl_ready =
        ::curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready) {
        return {false, "curl 初始化失败"};
    }
    CURL *handle = ::curl_easy_init();
    if (handle == nullptr) {
        return {false, "curl_easy_init 失败"};
    }
    ::curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 3000L);
    ::curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    ::curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    ::curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &discard_body);

    const CURLcode code = ::curl_easy_perform(handle);
    long status = 0;
    ::curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    ::curl_easy_cleanup(handle);

    if (code != CURLE_OK) {
        return {false, ::curl_easy_strerror(code)};
    }
    if (status < 200 || status > 299) {
        return {false, "HTTP " + std::to_string(status)};
    }
    return {true, {}};
}

bool make_directories(const std::string &path, mode_t mode = 0700) {
    if (path.empty()) {
        return false;
    }
    std::string current;
    std::size_t position = 0;
    if (path.front() == '/') {
        current = "/";
        position = 1;
    }
    while (position <= path.size()) {
        const std::size_t slash = path.find('/', position);
        const std::string part = slash == std::string::npos
                                     ? path.substr(position)
                                     : path.substr(position, slash - position);
        if (!part.empty()) {
            if (current.empty()) {
                current = part;
            } else if (current.back() == '/') {
                current += part;
            } else {
                current += "/" + part;
            }
            if (::mkdir(current.c_str(), mode) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        position = slash + 1;
    }
    return true;
}

// Temp file + rename, mode given by the caller. Never leaves a half-written
// file behind.
bool write_file_atomic(const std::string &path, const std::string &content,
                       mode_t mode, std::string &error) {
    if (path.empty()) {
        error = "配置路径为空 (HOME 未设置?)";
        return false;
    }

    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string directory = path.substr(0, slash);
        if (!make_directories(directory)) {
            error = "无法创建目录 " + directory + ": " +
                    std::strerror(errno);
            return false;
        }
    }

    std::string temporary = path + ".tmp.XXXXXX";
    const int descriptor = ::mkstemp(&temporary[0]);
    if (descriptor < 0) {
        error = "无法创建临时文件: " + std::string(std::strerror(errno));
        return false;
    }

    std::FILE *file = ::fdopen(descriptor, "w");
    if (file == nullptr) {
        error = "fdopen: " + std::string(std::strerror(errno));
        ::close(descriptor);
        ::unlink(temporary.c_str());
        return false;
    }

    bool ok = std::fwrite(content.data(), 1, content.size(), file) ==
              content.size();
    int saved_errno = ok ? 0 : errno;
    if (ok && std::fflush(file) != 0) {
        ok = false;
        saved_errno = errno;
    }
    if (ok && ::fsync(descriptor) != 0) {
        ok = false;
        saved_errno = errno;
    }
    if (std::fclose(file) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (ok && ::chmod(temporary.c_str(), mode) != 0) {
        ok = false;
        saved_errno = errno;
    }
    if (ok && ::rename(temporary.c_str(), path.c_str()) != 0) {
        ok = false;
        saved_errno = errno;
    }

    if (!ok) {
        error = "写入 " + path + " 失败: " + std::strerror(saved_errno);
        ::unlink(temporary.c_str());
        return false;
    }
    if (::chmod(path.c_str(), mode) != 0) {
        error = "无法设置权限: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

std::string bus_file_content(const std::string &scheme,
                             const std::string &host, int port,
                             const std::string &token) {
    std::string content =
        "# Yi shared bus configuration (written by yi-niri init).\n"
        "# $XDG_CONFIG_HOME/yi/bus.conf; mode 0600.\n";
    content += "scheme = " + scheme + "\n";
    content += "host = " + host + "\n";
    content += "port = " + std::to_string(port) + "\n";
    content += "token = " + token + "\n";
    return content;
}

std::string current_executable() {
    char buffer[4096];
    const ssize_t size =
        ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (size <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(size));
}

bool copy_file(const std::string &source, const std::string &target,
               std::string &error) {
    std::FILE *input = std::fopen(source.c_str(), "rb");
    if (input == nullptr) {
        error = "无法读取 " + source + ": " + std::strerror(errno);
        return false;
    }
    std::FILE *output = std::fopen(target.c_str(), "wb");
    if (output == nullptr) {
        error = "无法写入 " + target + ": " + std::strerror(errno);
        std::fclose(input);
        return false;
    }

    bool ok = true;
    int saved_errno = 0;
    char buffer[64 * 1024];
    for (;;) {
        const std::size_t bytes = std::fread(buffer, 1, sizeof buffer, input);
        if (bytes > 0 &&
            std::fwrite(buffer, 1, bytes, output) != bytes) {
            ok = false;
            saved_errno = errno;
            break;
        }
        if (bytes < sizeof buffer) {
            if (std::ferror(input) != 0) {
                ok = false;
                saved_errno = errno;
            }
            break;
        }
    }
    if (ok && std::fflush(output) != 0) {
        ok = false;
        saved_errno = errno;
    }
    if (std::fclose(output) != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    std::fclose(input);

    if (ok && ::chmod(target.c_str(), 0755) != 0) {
        ok = false;
        saved_errno = errno;
    }
    if (!ok) {
        error = "复制到 " + target + " 失败: " + std::strerror(saved_errno);
        ::unlink(target.c_str());
        return false;
    }
    return true;
}

bool install_binary(const std::string &home, std::string &target,
                    std::string &error) {
    target = home + "/.local/bin/yi-niri";
    const std::string executable = current_executable();
    if (executable.empty()) {
        error = "无法定位当前可执行文件 (/proc/self/exe)";
        return false;
    }
    if (executable == target) {
        return true;
    }

    const std::string directory = home + "/.local/bin";
    if (!make_directories(directory, 0755)) {
        error = "无法创建目录 " + directory + ": " + std::strerror(errno);
        return false;
    }
    if (!copy_file(executable, target, error)) {
        return false;
    }
    std::printf("已安装二进制到 %s\n", target.c_str());
    return true;
}

std::string join_command(const std::vector<std::string> &arguments) {
    std::string line;
    for (const std::string &argument : arguments) {
        if (!line.empty()) {
            line += " ";
        }
        line += argument;
    }
    return line;
}

// fork/execvp so no argument ever goes through a shell. Returns false when the
// process could not be run at all; otherwise `status` holds the exit code.
bool run_command(const std::vector<std::string> &arguments, int &status,
                 std::string &error) {
    if (arguments.empty()) {
        error = "空命令";
        return false;
    }
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child < 0) {
        error = "fork: " + std::string(std::strerror(errno));
        return false;
    }
    if (child == 0) {
        ::execvp(argv[0], argv.data());
        std::fprintf(stderr, "yi-niri: exec %s: %s\n", argv[0],
                     std::strerror(errno));
        ::_exit(127);
    }

    int wait_status = 0;
    while (::waitpid(child, &wait_status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        error = "waitpid: " + std::string(std::strerror(errno));
        return false;
    }
    if (WIFEXITED(wait_status)) {
        status = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        status = 128 + WTERMSIG(wait_status);
    } else {
        status = -1;
    }
    return true;
}

bool deploy_systemd_service(std::string &error) {
    const std::string home = env_or("HOME");
    if (home.empty()) {
        error = "HOME 未设置,无法部署 systemd 服务";
        return false;
    }

    std::string binary;
    if (!install_binary(home, binary, error)) {
        return false;
    }

    const std::string xdg = env_or("XDG_CONFIG_HOME");
    const std::string unit_dir =
        xdg.empty() ? home + "/.config/systemd/user" : xdg + "/systemd/user";
    const std::string unit_path = unit_dir + "/yi-niri.service";

    std::string content =
        "[Unit]\n"
        "Description=Yi niri session events\n"
        "Documentation=https://github.com/gresces/Yi-Tools/blob/main/tools/yi-niri/README.md\n"
        "PartOf=niri.service\n"
        "After=niri.service\n"
        "Wants=niri.service\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=" +
        binary +
        " run\n"
        "Restart=on-failure\n"
        "RestartSec=2s\n"
        "\n"
        "[Install]\n"
        "WantedBy=niri.service\n";
    if (!write_file_atomic(unit_path, content, 0644, error)) {
        return false;
    }
    std::printf("已写入 %s\n", unit_path.c_str());

    const std::vector<std::vector<std::string>> steps = {
        {"systemctl", "--user", "daemon-reload"},
        {"systemctl", "--user", "enable", "yi-niri.service"},
        {"systemctl", "--user", "restart", "yi-niri.service"},
    };
    for (const std::vector<std::string> &step : steps) {
        int status = 0;
        if (!run_command(step, status, error)) {
            return false;
        }
        if (status != 0) {
            error = join_command(step) + " 失败 (退出码 " +
                    std::to_string(status) + ")";
            return false;
        }
    }
    return true;
}

int run_init(const BusConfig &bus, const ToolConfig &tool) {
    const bool interactive = stdin_is_tty();

    std::string scheme = bus.scheme;
    std::string host = bus.host;
    int port = bus.port;
    std::string token = bus.token;

    ask("协议 scheme [" + scheme + "]: ", scheme);
    scheme = normalize_scheme(scheme, bus.scheme);
    ask("主机 host [" + host + "]: ", host);
    host = normalize_host(host, bus.host);

    for (int attempt = 1;; ++attempt) {
        std::string input;
        ask("端口 port [" + std::to_string(port) + "]: ", input);
        input = trim_copy(input);
        if (input.empty()) {
            break;
        }
        int parsed = 0;
        if (parse_port(input, parsed)) {
            port = parsed;
            break;
        }
        std::fprintf(stderr,
                     "yi-niri: 端口必须是 1-65535 之间的整数\n");
        if (attempt >= 3) {
            std::fprintf(stderr, "yi-niri: 端口输入无效,放弃配置\n");
            return 1;
        }
    }

    const std::string token_hint =
        token.empty() ? "未设置，回车保留" : "已设置，回车保留";
    ask("令牌 token [" + token_hint + "]: ", token, true);

    const std::string probe_url =
        scheme_host_port_url(scheme, host, port) + "/healthz";
    const ProbeResult probe = probe_health(probe_url);
    if (probe.ok) {
        std::printf("ok\n");
    } else {
        std::printf("健康检查失败 (%s): %s\n", probe_url.c_str(),
                    probe.reason.c_str());
        if (interactive) {
            std::string answer;
            ask("仍然保存? [Y/n]: ", answer);
            answer = trim_copy(answer);
            if (!answer.empty() &&
                (answer.front() == 'n' || answer.front() == 'N')) {
                std::printf("已取消,未写入配置\n");
                return 1;
            }
        } else {
            std::printf("警告: 健康检查失败,继续保存\n");
        }
    }

    std::string error;
    if (!write_file_atomic(
            bus.path, bus_file_content(scheme, host, port, token), 0600,
            error)) {
        std::fprintf(stderr, "yi-niri: %s\n", error.c_str());
        return 1;
    }
    std::printf("已写入 %s\n", bus.path.c_str());

    std::string source = tool.source;
    ask("事件来源 source [" + source + "]: ", source);
    source = trim_copy(source);
    if (source.empty()) {
        source = tool.source;
    }

    int timeout_ms = tool.timeout_ms;
    for (int attempt = 1;; ++attempt) {
        std::string input;
        ask("请求超时 timeout_ms [" + std::to_string(timeout_ms) + "]: ",
            input);
        input = trim_copy(input);
        if (input.empty()) {
            break;
        }
        int parsed = 0;
        if (parse_positive_int(input, parsed)) {
            timeout_ms = parsed;
            break;
        }
        std::fprintf(stderr, "yi-niri: timeout_ms 必须是正整数\n");
        if (attempt >= 3) {
            std::fprintf(stderr,
                         "yi-niri: timeout_ms 输入无效,放弃配置\n");
            return 1;
        }
    }

    std::string content =
        "# yi-niri configuration (written by yi-niri init).\n"
        "# $XDG_CONFIG_HOME/yi/niri.conf; mode 0600.\n";
    content += "source = " + source + "\n";
    content += "timeout_ms = " + std::to_string(timeout_ms) + "\n";
    if (!write_file_atomic(tool.path, content, 0600, error)) {
        std::fprintf(stderr, "yi-niri: %s\n", error.c_str());
        return 1;
    }
    std::printf("已写入 %s\n", tool.path.c_str());

    std::string deploy_answer;
    ask("部署为 systemd user 服务? [y/N]: ", deploy_answer);
    deploy_answer = trim_copy(deploy_answer);
    const bool deploy =
        !deploy_answer.empty() &&
        (deploy_answer.front() == 'y' || deploy_answer.front() == 'Y');
    if (deploy) {
        if (!deploy_systemd_service(error)) {
            std::fprintf(stderr, "yi-niri: %s\n", error.c_str());
            std::fprintf(stderr,
                         "yi-niri: 查看日志: journalctl --user -u "
                         "yi-niri\n");
            return 1;
        }
        std::printf("服务已启用并启动\n");
        std::printf("\n查看日志:\n  journalctl --user -u yi-niri -f\n");
        return 0;
    }

    std::printf("\n下一步:\n  systemctl --user enable --now yi-niri\n");
    return 0;
}

void print_check(const Options &options) {
    std::printf("url: %s\n", options.url.c_str());
    std::printf("token: %s\n", options.token.empty() ? "(none)" : "***");
    std::printf("source: %s\n", options.source.c_str());
    std::printf("timeout_ms: %d\n", options.timeout_ms);
    std::printf("bus.conf: %s\n", options.bus_path.c_str());
    std::printf("niri.conf: %s\n", options.tool_path.c_str());
}

// --config must be known before niri.conf is loaded, hence a small pre-scan.
std::string prescan_config_path(int argc, char **argv) {
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 < argc) {
                path = argv[++i];
            }
        } else if (arg.rfind("--config=", 0) == 0) {
            path = arg.substr(9);
        }
    }
    return path;
}

} // namespace

static int run_main(int argc, char **argv) {
    const std::string config_override = prescan_config_path(argc, argv);
    const BusConfig bus = load_bus_config();
    const ToolConfig tool = load_tool_config(config_override);

    // Precedence, low to high: built-in defaults, tool config, bus.conf,
    // environment, command line.
    Options options;
    options.bus_path = bus.path;
    options.tool_path = tool.path;
    options.source = tool.source;
    options.timeout_ms = tool.timeout_ms;
    if (tool.has_url) {
        options.url = tool.url;
    }
    if (tool.has_token) {
        options.token = tool.token;
    }
    if (bus.has_url) {
        options.url = bus.url.empty()
                          ? scheme_host_port_url(bus.scheme, bus.host, bus.port)
                          : bus.url;
    }
    if (bus.has_token) {
        options.token = bus.token;
    }
    options.url = env_or("YI_URL", options.url);
    options.token = env_or("YI_TOKEN", options.token);

    std::string command = "run";
    bool have_command = false;
    bool check = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto take_value = [&](const char *flag) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "yi-niri: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            print_usage(stdout);
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            std::printf("yi-niri %s\n", kVersion);
            return 0;
        }
        if (arg == "--check") {
            check = true;
        } else if (arg == "-u" || arg == "--url") {
            options.url = take_value("--url");
        } else if (arg.rfind("--url=", 0) == 0) {
            options.url = arg.substr(6);
        } else if (arg == "-t" || arg == "--token") {
            options.token = take_value("--token");
        } else if (arg.rfind("--token=", 0) == 0) {
            options.token = arg.substr(8);
        } else if (arg == "-s" || arg == "--source") {
            options.source = take_value("--source");
        } else if (arg.rfind("--source=", 0) == 0) {
            options.source = arg.substr(9);
        } else if (arg == "--timeout-ms") {
            options.timeout_ms = std::atoi(take_value("--timeout-ms").c_str());
        } else if (arg.rfind("--timeout-ms=", 0) == 0) {
            options.timeout_ms = std::atoi(arg.substr(13).c_str());
        } else if (arg == "--config") {
            const std::string value = take_value("--config");
            if (value.empty()) {
                std::fprintf(stderr, "yi-niri: --config must not be empty\n");
                return 2;
            }
            options.tool_path = value;
        } else if (arg.rfind("--config=", 0) == 0) {
            const std::string value = arg.substr(9);
            if (value.empty()) {
                std::fprintf(stderr, "yi-niri: --config must not be empty\n");
                return 2;
            }
            options.tool_path = value;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "yi-niri: unknown option %s\n", arg.c_str());
            return 2;
        } else if (!have_command) {
            command = arg;
            have_command = true;
        } else {
            std::fprintf(stderr, "yi-niri: unexpected argument %s\n", arg.c_str());
            return 2;
        }
    }

    if (options.timeout_ms <= 0) {
        std::fprintf(stderr, "yi-niri: --timeout-ms must be positive\n");
        return 2;
    }
    if (options.source.empty()) {
        std::fprintf(stderr, "yi-niri: --source must not be empty\n");
        return 2;
    }

    if (check) {
        print_check(options);
        return 0;
    }
    if (command == "init") {
        return run_init(bus, tool);
    }

    try {
        Agent agent(options);

        if (command == "run") {
            return agent.run();
        }
        if (command == "boot") {
            return agent.notify("system.boot") ? 0 : 1;
        }
        if (command == "lock") {
            return agent.notify("system.lock") ? 0 : 1;
        }
        if (command == "unlock") {
            return agent.notify("system.unlock") ? 0 : 1;
        }
        if (command == "shutdown") {
            return agent.notify("system.shutdown") ? 0 : 1;
        }

        std::fprintf(stderr, "yi-niri: unknown command %s\n", command.c_str());
        print_usage(stderr);
        return 2;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "yi-niri: %s\n", error.what());
        return 1;
    }
}

int main(int argc, char **argv) {
    try {
        return run_main(argc, argv);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "yi-niri: %s\n", error.what());
        return 1;
    }
}
