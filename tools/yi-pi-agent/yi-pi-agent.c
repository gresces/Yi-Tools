// yi-pi-agent — consume Yi events, run pi, publish the result.
//
// One process is both a consumer and a producer:
//   * register a consumer for exact `types` plus Go RE2 `patterns`;
//   * fetch one event at a time, run the configured command synchronously,
//     publish pi-agent.result / pi-agent.error, then ack (or nack on publish
//     failure);
//   * result ids are derived from (source, published type, request id), so a
//     redelivered request produces the same id and Yi can deduplicate it.
//
// Dependencies: libc, OpenSSL, libcurl. JSON parsing uses the bundled jsmn.

#define _GNU_SOURCE

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define JSMN_STATIC
#include "jsmn.h"

#define VERSION "0.1.0"
#define PROMPT_MAX 65536
#define STDERR_MAX 8192
#define MAX_PATTERNS 64
#define MAX_JSON_TOKENS (1u << 20)

// ---------------------------------------------------------------- utilities

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} str_t;

static void str_init(str_t *s) {
    s->s = NULL;
    s->len = 0;
    s->cap = 0;
}

static void str_free(str_t *s) {
    free(s->s);
    str_init(s);
}

static void str_reset(str_t *s) {
    s->len = 0;
    if (s->s != NULL) {
        s->s[0] = '\0';
    }
}

static int str_reserve(str_t *s, size_t extra) {
    if (extra > SIZE_MAX - s->len - 1) {
        return -1;
    }
    size_t need = s->len + extra + 1;
    if (need <= s->cap) {
        return 0;
    }
    size_t cap = s->cap != 0 ? s->cap : 64;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return -1;
        }
        cap *= 2;
    }
    char *grown = realloc(s->s, cap);
    if (grown == NULL) {
        return -1;
    }
    s->s = grown;
    s->cap = cap;
    return 0;
}

static int str_append(str_t *s, const char *data, size_t size) {
    if (str_reserve(s, size) < 0) {
        return -1;
    }
    if (size != 0) {
        memcpy(s->s + s->len, data, size);
    }
    s->len += size;
    s->s[s->len] = '\0';
    return 0;
}

static int str_puts(str_t *s, const char *text) {
    return str_append(s, text, strlen(text));
}

static int str_putc(str_t *s, char c) {
    return str_append(s, &c, 1);
}

static int str_printf(str_t *s, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int size = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (size < 0 || str_reserve(s, (size_t)size) < 0) {
        va_end(copy);
        return -1;
    }
    vsnprintf(s->s + s->len, (size_t)size + 1, format, copy);
    va_end(copy);
    s->len += (size_t)size;
    return 0;
}

static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("yi-pi-agent: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static char *xstrdup(const char *text) {
    if (text == NULL) {
        return NULL;
    }
    char *copy = strdup(text);
    if (copy == NULL) {
        log_line("out of memory");
        exit(1);
    }
    return copy;
}

static char *trim(char *text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool parse_int_range(const char *text, long minimum, long maximum, int *out) {
    errno = 0;
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum ||
        value > maximum) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_bool_strict(const char *text, bool *out) {
    if (strcasecmp(text, "true") == 0 || strcasecmp(text, "yes") == 0 ||
        strcmp(text, "1") == 0 || strcasecmp(text, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(text, "false") == 0 || strcasecmp(text, "no") == 0 ||
        strcmp(text, "0") == 0 || strcasecmp(text, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool session_id_valid(const char *text) {
    if (text == NULL) {
        return false;
    }
    const size_t length = strlen(text);
    if (length < 1 || length > 128) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
              *p == '-')) {
            return false;
        }
    }
    return true;
}

static bool parse_port_value(const char *text, int *out) {
    return parse_int_range(text, 1, 65535, out);
}

static char *expand_home(const char *path) {
    if (path == NULL) {
        return NULL;
    }
    if (path[0] != '~' || (path[1] != '/' && path[1] != '\0')) {
        return xstrdup(path);
    }
    const char *home = getenv("HOME");
    if (home == NULL) {
        return xstrdup(path);
    }
    size_t size = strlen(home) + strlen(path);
    char *out = malloc(size + 1);
    if (out == NULL) {
        log_line("out of memory");
        exit(1);
    }
    snprintf(out, size + 1, "%s%s", home, path + 1);
    return out;
}

static char *default_config_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && *xdg != '\0') {
        size_t size = strlen(xdg) + 40;
        char *path = malloc(size);
        snprintf(path, size, "%s/yi/pi-agent.conf", xdg);
        return path;
    }
    return expand_home("~/.config/yi/pi-agent.conf");
}

static char *default_bus_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && *xdg != '\0') {
        size_t size = strlen(xdg) + 32;
        char *path = malloc(size);
        snprintf(path, size, "%s/yi/bus.conf", xdg);
        return path;
    }
    return expand_home("~/.config/yi/bus.conf");
}

static char *format_origin(const char *scheme, const char *host, int port) {
    str_t out;
    str_init(&out);
    const bool brackets = strchr(host, ':') != NULL && host[0] != '[';
    str_printf(&out, "%s://%s%s%s:%d", scheme, brackets ? "[" : "", host,
               brackets ? "]" : "", port);
    if (out.s == NULL) {
        return xstrdup("");
    }
    return out.s;
}

static void format_rfc3339(time_t value, char out[32]) {
    struct tm tm;
    out[0] = '\0';
    if (gmtime_r(&value, &tm) == NULL) {
        return;
    }
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (long long)time(NULL) * 1000;
    }
    return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
}

static void sleep_msec(long msec) {
    if (msec <= 0) {
        return;
    }
    struct timespec pause = {.tv_sec = msec / 1000,
                             .tv_nsec = (msec % 1000) * 1000000};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        // Finish the backoff even if an unrelated signal arrives.
    }
}

static int mkdir_p_for(const char *file_path) {
    char *path = xstrdup(file_path);
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        free(path);
        return 0;
    }
    *slash = '\0';
    for (char *p = path + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            free(path);
            return -1;
        }
        *p = '/';
    }
    int result = mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
    free(path);
    return result;
}

// Creates the directory itself (mkdir -p), unlike mkdir_p_for() which creates
// only the parent of a file path.
static int mkdir_p_all(const char *directory) {
    if (directory == NULL || *directory == '\0') {
        return -1;
    }
    char *path = xstrdup(directory);
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') {
        path[--length] = '\0';
    }
    for (char *p = path + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            free(path);
            return -1;
        }
        *p = '/';
    }
    int result = mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
    free(path);
    return result;
}

// Writes a file through a sibling temporary file and rename(2), so a crash
// never leaves a half-written file behind.
static int write_atomic_mode(const char *path, const char *data, size_t size,
                             mode_t mode) {
    if (mkdir_p_for(path) != 0) {
        log_line("cannot create directory for %s", path);
        return -1;
    }

    size_t tmp_size = strlen(path) + 32;
    char *tmp = malloc(tmp_size);
    if (tmp == NULL) {
        log_line("out of memory");
        return -1;
    }
    snprintf(tmp, tmp_size, "%s.tmp.%ld", path, (long)getpid());

    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        log_line("cannot write %s: %s", tmp, strerror(errno));
        free(tmp);
        return -1;
    }
    (void)fchmod(fd, mode);

    int result = 0;
    size_t written = 0;
    while (written < size) {
        ssize_t chunk = write(fd, data + written, size - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_line("cannot write %s: %s", tmp, strerror(errno));
            result = -1;
            break;
        }
        if (chunk == 0) {
            log_line("cannot write %s: short write", tmp);
            result = -1;
            break;
        }
        written += (size_t)chunk;
    }
    if (close(fd) != 0 && result == 0) {
        log_line("cannot write %s: %s", tmp, strerror(errno));
        result = -1;
    }
    if (result == 0 && rename(tmp, path) != 0) {
        log_line("cannot replace %s: %s", path, strerror(errno));
        result = -1;
    }
    if (result != 0) {
        unlink(tmp);
    }
    free(tmp);
    return result;
}

static int write_atomic_private(const char *path, const char *data, size_t size) {
    return write_atomic_mode(path, data, size, 0600);
}

static void set_string(char **slot, const char *value) {
    free(*slot);
    *slot = xstrdup(value);
}

static void free_list(char **items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static void append_owned_string(char ***items, size_t *count, char *value) {
    char **grown = realloc(*items, (*count + 1) * sizeof(char *));
    if (grown == NULL) {
        free(value);
        log_line("out of memory");
        exit(1);
    }
    *items = grown;
    (*items)[(*count)++] = value;
}

static char **split_list(const char *value, size_t *count_out) {
    *count_out = 0;
    if (value == NULL) {
        return NULL;
    }
    char *copy = xstrdup(value);
    char **items = NULL;
    size_t count = 0;
    char *cursor = copy;
    while (cursor != NULL) {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        char *item = trim(cursor);
        if (*item != '\0') {
            append_owned_string(&items, &count, xstrdup(item));
        }
        cursor = comma != NULL ? comma + 1 : NULL;
    }
    free(copy);
    *count_out = count;
    return items;
}

static void set_list(char ***slot, size_t *count, const char *value) {
    free_list(*slot, *count);
    *slot = split_list(value, count);
}

// Whitespace split with double-quote grouping; backslash escapes the next
// character both inside and outside quotes.
static char **split_args(const char *text, size_t *count_out) {
    *count_out = 0;
    if (text == NULL) {
        return NULL;
    }
    char **items = NULL;
    size_t count = 0;
    bool quoted = false;
    bool has_token = false;
    str_t current;
    str_init(&current);

    for (const char *p = text;; p++) {
        const char c = *p;
        if (c == '\\' && p[1] != '\0') {
            str_putc(&current, p[1]);
            has_token = true;
            p++;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            has_token = true;
            continue;
        }
        if (!quoted && (c == '\0' || isspace((unsigned char)c))) {
            if (has_token) {
                append_owned_string(&items, &count,
                                    xstrdup(current.s != NULL ? current.s : ""));
                has_token = false;
                str_reset(&current);
            }
            if (c == '\0') {
                break;
            }
            continue;
        }
        str_putc(&current, c);
        has_token = true;
    }
    str_free(&current);
    *count_out = count;
    return items;
}

static char *join_list(const char *const *items, size_t count,
                       const char *separator) {
    str_t out;
    str_init(&out);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            str_puts(&out, separator);
        }
        str_puts(&out, items[i]);
    }
    if (out.s == NULL) {
        return xstrdup("");
    }
    return out.s;
}

static bool command_available(const char *command) {
    if (command == NULL || *command == '\0') {
        return false;
    }
    if (strchr(command, '/') != NULL) {
        return access(command, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (path == NULL || *path == '\0') {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char *copy = xstrdup(path);
    bool found = false;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir != NULL;
         dir = strtok_r(NULL, ":", &save)) {
        str_t candidate;
        str_init(&candidate);
        str_printf(&candidate, "%s/%s", *dir != '\0' ? dir : ".", command);
        if (candidate.s != NULL && access(candidate.s, X_OK) == 0) {
            found = true;
            str_free(&candidate);
            break;
        }
        str_free(&candidate);
    }
    free(copy);
    return found;
}

static char *default_pi_command(void) {
    const char *home = getenv("HOME");
    if (home != NULL && *home != '\0') {
        size_t size = strlen(home) + 32;
        char *path = malloc(size);
        if (path != NULL) {
            snprintf(path, size, "%s/.bun/bin/pi", home);
            if (access(path, X_OK) == 0) {
                return path;
            }
            free(path);
        }
    }
    return xstrdup("pi");
}

// ---------------------------------------------------------------------- bus

typedef struct {
    char *scheme;
    char *host;
    int port;
    char *token;
    char *url;
} BusSettings;

static void bus_defaults(BusSettings *bus) {
    bus->scheme = xstrdup("http");
    bus->host = xstrdup("127.0.0.1");
    bus->port = 9092;
    bus->token = xstrdup("");
    bus->url = NULL;
}

static void bus_free(BusSettings *bus) {
    free(bus->scheme);
    free(bus->host);
    free(bus->token);
    free(bus->url);
    memset(bus, 0, sizeof *bus);
}

static void bus_parse_url(BusSettings *bus) {
    if (bus->url == NULL || bus->url[0] == '\0') {
        return;
    }
    CURLU *handle = curl_url();
    if (handle == NULL) {
        return;
    }
    if (curl_url_set(handle, CURLUPART_URL, bus->url, 0) == CURLUE_OK) {
        char *value = NULL;
        if (curl_url_get(handle, CURLUPART_SCHEME, &value, 0) == CURLUE_OK) {
            set_string(&bus->scheme, value);
            curl_free(value);
            value = NULL;
        }
        if (curl_url_get(handle, CURLUPART_HOST, &value, 0) == CURLUE_OK) {
            set_string(&bus->host, value);
            curl_free(value);
            value = NULL;
        }
        int port = -1;
        if (curl_url_get(handle, CURLUPART_PORT, &value, 0) == CURLUE_OK) {
            int parsed = 0;
            if (parse_port_value(value, &parsed)) {
                port = parsed;
            }
            curl_free(value);
        }
        if (port < 0) {
            port = strcasecmp(bus->scheme, "https") == 0 ? 443 : 80;
        }
        bus->port = port;
    }
    curl_url_cleanup(handle);
}

static bool bus_load_file(BusSettings *bus, const char *path) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        char *text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';' || *text == '[') {
            continue;
        }
        char *equals = strchr(text, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        size_t value_len = strlen(value);
        if (value_len >= 2 && ((value[0] == '"' && value[value_len - 1] == '"') ||
                               (value[0] == '\'' && value[value_len - 1] == '\''))) {
            value[value_len - 1] = '\0';
            value++;
        }

        if (strcasecmp(key, "scheme") == 0) {
            set_string(&bus->scheme, value);
        } else if (strcasecmp(key, "host") == 0) {
            set_string(&bus->host, value);
        } else if (strcasecmp(key, "port") == 0) {
            int port = 0;
            if (parse_port_value(value, &port)) {
                bus->port = port;
            }
        } else if (strcasecmp(key, "token") == 0) {
            set_string(&bus->token, value);
        } else if (strcasecmp(key, "url") == 0) {
            free(bus->url);
            bus->url = *value != '\0' ? xstrdup(value) : NULL;
        }
    }
    free(line);
    fclose(file);
    return true;
}

static char *bus_effective_url(const BusSettings *bus) {
    if (bus->url != NULL && bus->url[0] != '\0') {
        return xstrdup(bus->url);
    }
    return format_origin(bus->scheme, bus->host, bus->port);
}

// ------------------------------------------------------------------- config

typedef struct {
    char *url;
    char *token;
    char *consumer;
    char **types;
    size_t type_count;
    char **patterns;
    size_t pattern_count;
    char *command;
    char **args;
    size_t arg_count;
    char *session;
    char *workdir;
    int timeout;
    int max_output;
    int poll_timeout;
    bool skip_history;
    char *result_type;
    char *error_type;
    char *source;
} Config;

static void config_free(Config *config) {
    free(config->url);
    free(config->token);
    free(config->consumer);
    free_list(config->types, config->type_count);
    free_list(config->patterns, config->pattern_count);
    free(config->command);
    free_list(config->args, config->arg_count);
    free(config->session);
    free(config->workdir);
    free(config->result_type);
    free(config->error_type);
    free(config->source);
    memset(config, 0, sizeof *config);
}

static void config_defaults(Config *config) {
    memset(config, 0, sizeof *config);
    config->url = xstrdup("http://127.0.0.1:9092");
    config->token = xstrdup("");
    config->consumer = xstrdup("pi-agent");
    config->pattern_count = 1;
    config->patterns = malloc(sizeof(char *));
    config->patterns[0] = xstrdup("^pi-agent\\.");
    config->command = default_pi_command();
    config->args = split_args("--print --mode text", &config->arg_count);
    config->session = xstrdup("");
    config->workdir = expand_home("~/.local/state/yi-pi-agent/work");
    config->timeout = 300;
    config->max_output = 20000;
    config->poll_timeout = 25;
    config->skip_history = true;
    config->result_type = xstrdup("pi-agent.result");
    config->error_type = xstrdup("pi-agent.error");
    config->source = xstrdup("pi-agent");
}

static int config_load(Config *config, const char *path) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        log_line("cannot read config %s: %s", path, strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    int line_number = 0;
    int problems = 0;

    while ((size = getline(&line, &capacity, file)) >= 0) {
        line_number++;
        char *text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }
        if (*text == '[') {
            log_line("%s:%d: unexpected section", path, line_number);
            problems++;
            continue;
        }

        char *equals = strchr(text, '=');
        if (equals == NULL) {
            log_line("%s:%d: expected key = value", path, line_number);
            problems++;
            continue;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        size_t value_len = strlen(value);
        if (value_len >= 2 && ((value[0] == '"' && value[value_len - 1] == '"') ||
                               (value[0] == '\'' && value[value_len - 1] == '\''))) {
            value[value_len - 1] = '\0';
            value++;
        }

        if (strcasecmp(key, "consumer") == 0) {
            set_string(&config->consumer, value);
        } else if (strcasecmp(key, "types") == 0) {
            set_list(&config->types, &config->type_count, value);
        } else if (strcasecmp(key, "patterns") == 0) {
            set_list(&config->patterns, &config->pattern_count, value);
        } else if (strcasecmp(key, "command") == 0) {
            set_string(&config->command, value);
        } else if (strcasecmp(key, "args") == 0) {
            free_list(config->args, config->arg_count);
            config->args = split_args(value, &config->arg_count);
        } else if (strcasecmp(key, "session") == 0) {
            set_string(&config->session, value);
        } else if (strcasecmp(key, "workdir") == 0) {
            free(config->workdir);
            config->workdir = expand_home(value);
        } else if (strcasecmp(key, "timeout") == 0) {
            int parsed = 0;
            if (parse_int_range(value, 5, 1800, &parsed)) {
                config->timeout = parsed;
            } else {
                log_line("%s:%d: timeout must be 5..1800", path, line_number);
                problems++;
            }
        } else if (strcasecmp(key, "max_output") == 0) {
            int parsed = 0;
            if (parse_int_range(value, 1024, 1048576, &parsed)) {
                config->max_output = parsed;
            } else {
                log_line("%s:%d: max_output must be 1024..1048576", path,
                         line_number);
                problems++;
            }
        } else if (strcasecmp(key, "poll_timeout") == 0) {
            int parsed = 0;
            if (parse_int_range(value, 1, 60, &parsed)) {
                config->poll_timeout = parsed;
            } else {
                log_line("%s:%d: poll_timeout must be 1..60", path, line_number);
                problems++;
            }
        } else if (strcasecmp(key, "skip_history") == 0) {
            bool parsed = false;
            if (parse_bool_strict(value, &parsed)) {
                config->skip_history = parsed;
            } else {
                log_line("%s:%d: skip_history must be true or false", path,
                         line_number);
                problems++;
            }
        } else if (strcasecmp(key, "result_type") == 0) {
            set_string(&config->result_type, value);
        } else if (strcasecmp(key, "error_type") == 0) {
            set_string(&config->error_type, value);
        } else if (strcasecmp(key, "source") == 0) {
            set_string(&config->source, value);
        } else if (strcasecmp(key, "url") == 0 ||
                   strcasecmp(key, "token") == 0) {
            log_line("%s:%d: %s in the tool config is ignored; use bus.conf",
                     path, line_number, key);
        } else {
            log_line("%s:%d: unknown key '%s'", path, line_number, key);
            problems++;
        }
    }

    free(line);
    fclose(file);

    if (config->consumer == NULL || config->consumer[0] == '\0') {
        log_line("%s: consumer must not be empty", path);
        problems++;
    }
    if (config->type_count == 0 && config->pattern_count == 0) {
        log_line("%s: types or patterns is required", path);
        problems++;
    }
    if (config->pattern_count > MAX_PATTERNS) {
        log_line("%s: at most %d patterns are allowed", path, MAX_PATTERNS);
        problems++;
    }
    if (config->command == NULL || config->command[0] == '\0') {
        log_line("%s: command must not be empty", path);
        problems++;
    }
    if (config->workdir == NULL || config->workdir[0] == '\0') {
        log_line("%s: workdir must not be empty", path);
        problems++;
    }
    if (config->session != NULL && config->session[0] != '\0' &&
        !session_id_valid(config->session)) {
        log_line("%s: session must match [A-Za-z0-9._-]{1,128}", path);
        problems++;
    }
    if (config->result_type == NULL || config->result_type[0] == '\0' ||
        config->error_type == NULL || config->error_type[0] == '\0' ||
        config->source == NULL || config->source[0] == '\0') {
        log_line("%s: result_type, error_type and source must not be empty", path);
        problems++;
    }

    return problems == 0 ? 0 : -1;
}

// Precedence: YI_URL/YI_TOKEN, then bus.conf, then built-in defaults.
static void config_apply_bus(Config *config) {
    char *bus_path = default_bus_path();
    BusSettings bus;
    bus_defaults(&bus);

    if (bus_load_file(&bus, bus_path)) {
        char *url = bus_effective_url(&bus);
        set_string(&config->url, url);
        free(url);
        set_string(&config->token, bus.token);
    }

    const char *env_url = getenv("YI_URL");
    if (env_url != NULL && env_url[0] != '\0') {
        set_string(&config->url, env_url);
    }
    const char *env_token = getenv("YI_TOKEN");
    if (env_token != NULL) {
        set_string(&config->token, env_token);
    }

    bus_free(&bus);
    free(bus_path);
}

static void config_print(const Config *config) {
    char *types = join_list((const char *const *)config->types,
                            config->type_count, ",");
    char *patterns = join_list((const char *const *)config->patterns,
                               config->pattern_count, ",");
    char *args = join_list((const char *const *)config->args,
                           config->arg_count, " ");
    printf("url       = %s\n", config->url);
    printf("token     = %s\n", config->token[0] != '\0' ? "***" : "(none)");
    printf("consumer  = %s\n", config->consumer);
    printf("types     = %s\n", config->type_count != 0 ? types : "(none)");
    printf("patterns  = %s\n", config->pattern_count != 0 ? patterns : "(none)");
    printf("command   = %s (%s)\n", config->command,
           command_available(config->command) ? "ok" : "missing");
    printf("args      = %s\n", args);
    printf("session   = %s\n",
           config->session != NULL && config->session[0] != '\0'
               ? config->session
               : "(none)");
    printf("workdir   = %s\n", config->workdir);
    printf("timeout   = %d\n", config->timeout);
    printf("max_output= %d\n", config->max_output);
    printf("result    = %s\n", config->result_type);
    printf("error     = %s\n", config->error_type);
    printf("poll_timeout = %d\n", config->poll_timeout);
    printf("skip_history = %s\n", config->skip_history ? "true" : "false");
    printf("source    = %s\n", config->source);
    free(types);
    free(patterns);
    free(args);
}

// --------------------------------------------------------------------- HTTP

typedef struct {
    long status;
    str_t body;
} HttpResponse;

static void http_response_init(HttpResponse *response) {
    response->status = 0;
    str_init(&response->body);
}

static void http_response_free(HttpResponse *response) {
    str_free(&response->body);
}

static size_t http_write(char *data, size_t size, size_t count, void *userdata) {
    HttpResponse *response = userdata;
    str_append(&response->body, data, size * count);
    return size * count;
}

static volatile sig_atomic_t g_stop = 0;

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return g_stop ? 1 : 0;
}

static char *api_url(const Config *config, const char *path) {
    str_t url;
    str_init(&url);
    str_puts(&url, config->url);
    while (url.len > 0 && url.s[url.len - 1] == '/') {
        url.s[--url.len] = '\0';
    }
    str_puts(&url, path);
    if (url.s == NULL) {
        return xstrdup("");
    }
    return url.s;
}

static int http_request(const Config *config, const char *method, const char *path,
                        const char *body, long timeout_seconds,
                        HttpResponse *response, bool abort_on_stop) {
    http_response_init(response);

    char *url = api_url(config, path);
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        log_line("%s %s failed: cannot initialise libcurl", method, path);
        free(url);
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");
    if (config->token[0] != '\0') {
        str_t auth;
        str_init(&auth);
        str_printf(&auth, "Authorization: Bearer %s", config->token);
        headers = curl_slist_append(headers, auth.s);
        str_free(&auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (abort_on_stop) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    }

    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body != NULL ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         (long)strlen(body != NULL ? body : ""));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body != NULL ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         (long)strlen(body != NULL ? body : ""));
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response->status = status;

    int result = 0;
    if (code != CURLE_OK) {
        if (!(g_stop && code == CURLE_ABORTED_BY_CALLBACK)) {
            log_line("%s %s failed: %s", method, path,
                     curl_easy_strerror(code));
        }
        result = -1;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(url);
    return result;
}

static char *url_encode(const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    str_t out;
    str_init(&out);
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            str_putc(&out, (char)*p);
        } else {
            str_putc(&out, '%');
            str_putc(&out, hex[*p >> 4]);
            str_putc(&out, hex[*p & 0x0f]);
        }
    }
    if (out.s == NULL) {
        return xstrdup("");
    }
    return out.s;
}

static char *consumer_path(const Config *config, const char *suffix) {
    char *name = url_encode(config->consumer);
    str_t path;
    str_init(&path);
    str_printf(&path, "/consumers/%s%s", name, suffix);
    free(name);
    if (path.s == NULL) {
        return xstrdup("");
    }
    return path.s;
}

static void json_escape(str_t *out, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        switch (*p) {
        case '"':
            str_puts(out, "\\\"");
            break;
        case '\\':
            str_puts(out, "\\\\");
            break;
        case '\n':
            str_puts(out, "\\n");
            break;
        case '\r':
            str_puts(out, "\\r");
            break;
        case '\t':
            str_puts(out, "\\t");
            break;
        default:
            if (*p < 0x20) {
                str_printf(out, "\\u%04x", *p);
            } else {
                str_putc(out, (char)*p);
            }
            break;
        }
    }
}

static char *build_subscription(const Config *config) {
    str_t body;
    str_init(&body);
    str_puts(&body, "{\"types\":[");
    for (size_t i = 0; i < config->type_count; i++) {
        if (i > 0) {
            str_putc(&body, ',');
        }
        str_putc(&body, '"');
        json_escape(&body, config->types[i]);
        str_putc(&body, '"');
    }
    str_puts(&body, "],\"patterns\":[");
    for (size_t i = 0; i < config->pattern_count; i++) {
        if (i > 0) {
            str_putc(&body, ',');
        }
        str_putc(&body, '"');
        json_escape(&body, config->patterns[i]);
        str_putc(&body, '"');
    }
    str_puts(&body, "]}");
    return body.s;
}

static int register_consumer(const Config *config, bool *created) {
    if (created != NULL) {
        *created = false;
    }
    char *body = build_subscription(config);
    char *path = consumer_path(config, "");
    HttpResponse response;
    int status_code = http_request(config, "PUT", path, body, 15, &response, false);
    bool ok = status_code == 0 &&
              (response.status == 201 || response.status == 200);

    if (!ok && status_code == 0 && response.status == 409) {
        log_line("consumer '%s' exists with a different subscription; recreating",
                 config->consumer);
        http_response_free(&response);
        HttpResponse deleted;
        const int delete_code =
            http_request(config, "DELETE", path, NULL, 10, &deleted, false);
        if (delete_code != 0 ||
            (deleted.status != 204 && deleted.status != 200 &&
             deleted.status != 404)) {
            log_line("delete consumer failed with HTTP %ld", deleted.status);
        }
        http_response_free(&deleted);

        http_response_init(&response);
        status_code = http_request(config, "PUT", path, body, 15, &response,
                                   false);
        ok = status_code == 0 &&
             (response.status == 201 || response.status == 200);
    }

    if (!ok) {
        if (status_code == 0) {
            log_line("register consumer failed with HTTP %ld", response.status);
        } else {
            log_line("register consumer failed");
        }
    } else {
        log_line("registered consumer '%s' (%zu types, %zu patterns)",
                 config->consumer, config->type_count, config->pattern_count);
        if (created != NULL) {
            *created = response.status == 201;
        }
    }

    http_response_free(&response);
    free(body);
    free(path);
    return ok ? 0 : -1;
}

// 1: event available, 0: long-poll timeout, -1: request error.
static int consumer_next_request(const Config *config, const char *timeout_text,
                                 long http_timeout, HttpResponse *response) {
    char suffix[64];
    snprintf(suffix, sizeof suffix, "/next?timeout=%s", timeout_text);
    char *path = consumer_path(config, suffix);
    const int result =
        http_request(config, "GET", path, NULL, http_timeout, response, true);
    free(path);
    if (result != 0) {
        return -1;
    }
    if (response->status == 204) {
        return 0;
    }
    if (response->status == 200) {
        return 1;
    }
    log_line("next failed with HTTP %ld", response->status);
    return -1;
}

// 1: event available, 0: long-poll timeout, -1: request error.
static int consumer_next(const Config *config, int timeout_seconds,
                         HttpResponse *response) {
    char timeout_text[16];
    snprintf(timeout_text, sizeof timeout_text, "%d", timeout_seconds);
    return consumer_next_request(config, timeout_text,
                                 (long)timeout_seconds + 10, response);
}

static int settle_event(const Config *config, bool ack, const char *id,
                        const char *error) {
    char id_prefix[9];
    size_t prefix_len = strlen(id != NULL ? id : "");
    if (prefix_len > 8) {
        prefix_len = 8;
    }
    memcpy(id_prefix, id != NULL ? id : "", prefix_len);
    id_prefix[prefix_len] = '\0';

    str_t body;
    str_init(&body);
    str_putc(&body, '{');
    bool wrote = false;
    if (id != NULL && *id != '\0') {
        str_puts(&body, "\"id\":\"");
        json_escape(&body, id);
        str_putc(&body, '"');
        wrote = true;
    }
    if (!ack) {
        if (wrote) {
            str_putc(&body, ',');
        }
        str_puts(&body, "\"error\":\"");
        json_escape(&body, error != NULL ? error : "nacked");
        str_putc(&body, '"');
    }
    str_putc(&body, '}');

    char *path = consumer_path(config, ack ? "/ack" : "/nack");
    HttpResponse response;
    const int request = http_request(config, "POST", path, body.s, 10, &response,
                                     false);
    const bool ok = request == 0 &&
                    (response.status == 204 || response.status == 200);
    if (ok) {
        if (ack) {
            log_line("ack %s", id_prefix[0] != '\0' ? id_prefix : "(none)");
        } else {
            log_line("nack %s: %s", id_prefix[0] != '\0' ? id_prefix : "(none)",
                     error != NULL ? error : "nacked");
        }
    } else if (request == 0) {
        log_line("%s %s failed with HTTP %ld", ack ? "ack" : "nack",
                 id_prefix[0] != '\0' ? id_prefix : "(none)", response.status);
    }

    http_response_free(&response);
    free(path);
    str_free(&body);
    return ok ? 0 : -1;
}

// --------------------------------------------------------------- jsmn / JSON

static int json_parse_document(const char *json, jsmntok_t **out_tokens,
                               int *out_count) {
    size_t capacity = 256;
    const size_t length = strlen(json);
    while (capacity <= MAX_JSON_TOKENS) {
        jsmntok_t *tokens = malloc(capacity * sizeof *tokens);
        if (tokens == NULL) {
            return -1;
        }
        jsmn_parser parser;
        jsmn_init(&parser);
        const int result = jsmn_parse(&parser, json, length, tokens,
                                      (unsigned int)capacity);
        if (result >= 0) {
            if (result == 0) {
                free(tokens);
                return -1;
            }
            *out_tokens = tokens;
            *out_count = result;
            return 0;
        }
        free(tokens);
        if (result != JSMN_ERROR_NOMEM) {
            return -1;
        }
        capacity *= 2;
    }
    return -1;
}

static bool token_equals(const char *json, const jsmntok_t *token,
                         const char *text) {
    const size_t length = strlen(text);
    return token->type == JSMN_STRING &&
           (size_t)(token->end - token->start) == length &&
           memcmp(json + token->start, text, length) == 0;
}

static int json_skip(const jsmntok_t *tokens, int count, int index) {
    if (index < 0 || index >= count) {
        return count;
    }
    int next = index + 1;
    if (tokens[index].type == JSMN_OBJECT) {
        for (int i = 0; i < tokens[index].size; i++) {
            if (next >= count) {
                return count;
            }
            next = json_skip(tokens, count, next);
            if (next >= count) {
                return count;
            }
            next = json_skip(tokens, count, next);
        }
    } else if (tokens[index].type == JSMN_ARRAY) {
        for (int i = 0; i < tokens[index].size; i++) {
            if (next >= count) {
                return count;
            }
            next = json_skip(tokens, count, next);
        }
    }
    return next;
}

static int json_object_get(const char *json, const jsmntok_t *tokens,
                           int count, int object, const char *key) {
    if (object < 0 || object >= count ||
        tokens[object].type != JSMN_OBJECT) {
        return -1;
    }
    int index = object + 1;
    for (int i = 0; i < tokens[object].size; i++) {
        if (index + 1 >= count) {
            return -1;
        }
        if (token_equals(json, &tokens[index], key)) {
            return index + 1;
        }
        index = json_skip(tokens, count, index + 1);
    }
    return -1;
}

static int json_array_get(const jsmntok_t *tokens, int count, int array,
                          int item) {
    if (array < 0 || array >= count || tokens[array].type != JSMN_ARRAY ||
        item < 0 || item >= tokens[array].size) {
        return -1;
    }
    int index = array + 1;
    for (int i = 0; i < item; i++) {
        index = json_skip(tokens, count, index);
        if (index >= count) {
            return -1;
        }
    }
    return index;
}

static int append_utf8(str_t *out, unsigned int codepoint) {
    if (codepoint <= 0x7f) {
        return str_putc(out, (char)codepoint);
    }
    if (codepoint <= 0x7ff) {
        if (str_putc(out, (char)(0xc0 | (codepoint >> 6))) < 0) {
            return -1;
        }
        return str_putc(out, (char)(0x80 | (codepoint & 0x3f)));
    }
    if (codepoint <= 0xffff) {
        if (str_putc(out, (char)(0xe0 | (codepoint >> 12))) < 0 ||
            str_putc(out, (char)(0x80 | ((codepoint >> 6) & 0x3f))) < 0) {
            return -1;
        }
        return str_putc(out, (char)(0x80 | (codepoint & 0x3f)));
    }
    if (codepoint <= 0x10ffff) {
        if (str_putc(out, (char)(0xf0 | (codepoint >> 18))) < 0 ||
            str_putc(out, (char)(0x80 | ((codepoint >> 12) & 0x3f))) < 0 ||
            str_putc(out, (char)(0x80 | ((codepoint >> 6) & 0x3f))) < 0) {
            return -1;
        }
        return str_putc(out, (char)(0x80 | (codepoint & 0x3f)));
    }
    return -1;
}

static bool parse_hex4(const char *text, unsigned int *out) {
    unsigned int value = 0;
    for (int i = 0; i < 4; i++) {
        const unsigned char c = (unsigned char)text[i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (unsigned int)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= (unsigned int)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = value;
    return true;
}

// Decodes the inside of a JSON string (jsmn excludes the quotes).
static char *json_unescape(const char *start, size_t length) {
    str_t out;
    str_init(&out);
    const char *p = start;
    const char *end = start + length;

    while (p < end) {
        const unsigned char c = (unsigned char)*p;
        if (c != '\\') {
            if (str_putc(&out, (char)c) < 0) {
                goto failed;
            }
            p++;
            continue;
        }
        p++;
        if (p >= end) {
            goto failed;
        }
        switch (*p) {
        case '"':
            str_putc(&out, '"');
            p++;
            break;
        case '\\':
            str_putc(&out, '\\');
            p++;
            break;
        case '/':
            str_putc(&out, '/');
            p++;
            break;
        case 'b':
            str_putc(&out, '\b');
            p++;
            break;
        case 'f':
            str_putc(&out, '\f');
            p++;
            break;
        case 'n':
            str_putc(&out, '\n');
            p++;
            break;
        case 'r':
            str_putc(&out, '\r');
            p++;
            break;
        case 't':
            str_putc(&out, '\t');
            p++;
            break;
        case 'u': {
            if (p + 4 >= end) {
                goto failed;
            }
            unsigned int codepoint = 0;
            if (!parse_hex4(p + 1, &codepoint)) {
                goto failed;
            }
            p += 5;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (p + 5 < end && p[0] == '\\' && p[1] == 'u') {
                    unsigned int low = 0;
                    if (parse_hex4(p + 2, &low) &&
                        low >= 0xdc00 && low <= 0xdfff) {
                        codepoint = 0x10000 +
                                    ((codepoint - 0xd800) << 10) +
                                    (low - 0xdc00);
                        p += 6;
                    } else {
                        codepoint = 0xfffd;
                    }
                } else {
                    codepoint = 0xfffd;
                }
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                codepoint = 0xfffd;
            }
            if (codepoint == 0) {
                codepoint = 0xfffd;
            }
            if (append_utf8(&out, codepoint) < 0) {
                goto failed;
            }
            break;
        }
        default:
            goto failed;
        }
    }

    if (out.s == NULL) {
        return xstrdup("");
    }
    return out.s;

failed:
    str_free(&out);
    return NULL;
}

static char *token_string(const char *json, const jsmntok_t *token) {
    if (token->type != JSMN_STRING) {
        return NULL;
    }
    return json_unescape(json + token->start,
                         (size_t)(token->end - token->start));
}

typedef struct {
    char *id;
    char *source;
    char **types;
    size_t type_count;
    char *text;
    char *session;
} Event;

static void event_free(Event *event) {
    free(event->id);
    free(event->source);
    free_list(event->types, event->type_count);
    free(event->text);
    free(event->session);
    memset(event, 0, sizeof *event);
}

static int event_parse(const char *json, Event *event, bool id_only) {
    memset(event, 0, sizeof *event);
    jsmntok_t *tokens = NULL;
    int count = 0;
    if (json_parse_document(json, &tokens, &count) != 0) {
        return -1;
    }
    if (tokens[0].type != JSMN_OBJECT) {
        free(tokens);
        return -1;
    }

    int object = 0;
    int wrapped = json_object_get(json, tokens, count, 0, "event");
    if (wrapped >= 0) {
        if (tokens[wrapped].type != JSMN_OBJECT) {
            free(tokens);
            return -1;
        }
        object = wrapped;
    }

    int id = json_object_get(json, tokens, count, object, "id");
    event->id = id >= 0 ? token_string(json, &tokens[id]) : NULL;
    if (event->id == NULL) {
        event->id = xstrdup("");
    }
    if (id_only) {
        free(tokens);
        return 0;
    }

    int source = json_object_get(json, tokens, count, object, "source");
    event->source = source >= 0 ? token_string(json, &tokens[source]) : NULL;
    if (event->source == NULL) {
        event->source = xstrdup("");
    }

    int types = json_object_get(json, tokens, count, object, "types");
    if (types >= 0 && tokens[types].type == JSMN_ARRAY) {
        for (int i = 0; i < tokens[types].size; i++) {
            int item = json_array_get(tokens, count, types, i);
            if (item < 0) {
                break;
            }
            char *value = token_string(json, &tokens[item]);
            if (value != NULL) {
                append_owned_string(&event->types, &event->type_count, value);
            }
        }
    }

    int data = json_object_get(json, tokens, count, object, "data");
    if (data >= 0 && tokens[data].type == JSMN_OBJECT) {
        static const char *keys[] = {"text", "prompt", "instruction"};
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            int value = json_object_get(json, tokens, count, data, keys[i]);
            if (value < 0 || tokens[value].type != JSMN_STRING) {
                continue;
            }
            char *text = token_string(json, &tokens[value]);
            if (text == NULL) {
                continue;
            }
            if (*text != '\0') {
                event->text = text;
                break;
            }
            free(text);
        }
        int session = json_object_get(json, tokens, count, data, "session");
        if (session >= 0 && tokens[session].type == JSMN_STRING) {
            char *value = token_string(json, &tokens[session]);
            if (value != NULL && *value != '\0') {
                event->session = value;
            } else {
                free(value);
            }
        }
    }

    free(tokens);
    return 0;
}

static bool event_is_self_loop(const Config *config, const Event *event) {
    for (size_t i = 0; i < event->type_count; i++) {
        if (strcmp(event->types[i], config->result_type) == 0 ||
            strcmp(event->types[i], config->error_type) == 0) {
            return true;
        }
    }
    return false;
}

// A brand-new consumer starts at the beginning of the active log. Drain those
// historical events without running pi, so a first start does not replay old
// requests (and burn tokens). Uses the same next/event parsing path as the
// main loop, but only keeps the id for the ack.
static void drain_history(const Config *config) {
    size_t skipped = 0;
    while (!g_stop) {
        HttpResponse response;
        const int next = consumer_next_request(config, "0.2", 10, &response);
        if (next < 0) {
            log_line("history drain stopped: next request failed");
            http_response_free(&response);
            break;
        }
        if (next == 0) {
            http_response_free(&response);
            break;
        }

        Event event;
        if (event_parse(response.body.s != NULL ? response.body.s : "", &event,
                        true) != 0) {
            log_line("history drain stopped: cannot parse event");
            (void)settle_event(config, true, "", NULL);
            http_response_free(&response);
            break;
        }

        const int acked = settle_event(config, true, event.id, NULL);
        event_free(&event);
        http_response_free(&response);
        if (acked != 0) {
            log_line("history drain stopped: ack failed");
            break;
        }
        skipped++;
    }
    log_line("skipped %zu existing event(s) on first registration", skipped);
}

static void event_id_prefix(const char *id, char out[9]) {
    size_t length = id != NULL ? strlen(id) : 0;
    if (length > 8) {
        length = 8;
    }
    memcpy(out, id != NULL ? id : "", length);
    out[length] = '\0';
}

// -------------------------------------------------------------------- events

static void event_id(const char *source, const char *type, const char *request_id,
                     char out[33]) {
    str_t material;
    str_init(&material);
    str_printf(&material, "%s\x1f%s\x1f%s", source, type, request_id);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(material.s != NULL ? material.s : "", material.len, digest, &size,
               EVP_sha256(), NULL);
    for (unsigned int i = 0; i < 16 && i < size; i++) {
        sprintf(out + i * 2, "%02x", digest[i]);
    }
    out[32] = '\0';
    str_free(&material);
}

typedef struct {
    bool success;
    int exit_code;
    bool timed_out;
    bool truncated;
    long long duration_ms;
    char *output;
    char *stderr_text;
    char *error;
} RunResult;

static void run_result_free(RunResult *result) {
    free(result->output);
    free(result->stderr_text);
    free(result->error);
    memset(result, 0, sizeof *result);
}

static char *build_result_json(const Config *config, const Event *event,
                               const char *published_type, const RunResult *run,
                               const char *session, const char *session_source) {
    char time_text[32];
    char id[33];
    format_rfc3339(time(NULL), time_text);
    event_id(config->source, published_type, event->id, id);

    str_t json;
    str_init(&json);
    str_puts(&json, "{\"id\":\"");
    json_escape(&json, id);
    str_puts(&json, "\",\"source\":\"");
    json_escape(&json, config->source);
    str_puts(&json, "\",\"types\":[\"");
    json_escape(&json, published_type);
    str_puts(&json, "\"],\"time\":\"");
    json_escape(&json, time_text);
    str_puts(&json, "\",\"data\":{\"request_id\":\"");
    json_escape(&json, event->id);
    str_puts(&json, "\",\"request_source\":\"");
    json_escape(&json, event->source);
    str_puts(&json, "\",\"request_types\":[");
    for (size_t i = 0; i < event->type_count; i++) {
        if (i > 0) {
            str_putc(&json, ',');
        }
        str_putc(&json, '"');
        json_escape(&json, event->types[i]);
        str_putc(&json, '"');
    }
    str_puts(&json, "],\"prompt\":\"");
    json_escape(&json, event->text != NULL ? event->text : "");

    if (run->success) {
        str_puts(&json, "\",\"output\":\"");
        json_escape(&json, run->output != NULL ? run->output : "");
        str_printf(&json,
                   "\",\"exit_code\":%d,\"duration_ms\":%lld,\"truncated\":%s",
                   run->exit_code, run->duration_ms,
                   run->truncated ? "true" : "false");
    } else {
        str_puts(&json, "\",\"error\":\"");
        json_escape(&json, run->error != NULL ? run->error : "pi failed");
        str_puts(&json, "\",\"stderr\":\"");
        json_escape(&json, run->stderr_text != NULL ? run->stderr_text : "");
        str_printf(&json,
                   "\",\"exit_code\":%d,\"duration_ms\":%lld,\"timed_out\":%s",
                   run->exit_code, run->duration_ms,
                   run->timed_out ? "true" : "false");
    }
    str_puts(&json, ",\"session\":");
    if (session != NULL && *session != '\0') {
        str_putc(&json, '"');
        json_escape(&json, session);
        str_putc(&json, '"');
    } else {
        str_puts(&json, "null");
    }
    str_puts(&json, ",\"session_source\":\"");
    json_escape(&json, session_source != NULL ? session_source : "none");
    str_puts(&json, "\"}}");
    return json.s;
}

static int publish_event(const Config *config, const char *json,
                         const char *published_type, const char *request_id) {
    char request_prefix[9];
    event_id_prefix(request_id, request_prefix);

    for (int attempt = 0; attempt < 3; attempt++) {
        HttpResponse response;
        const int request = http_request(config, "POST", "/events", json, 10,
                                         &response, false);
        const bool ok = request == 0 &&
                        (response.status == 200 || response.status == 201);
        if (ok) {
            log_line("published %s for request %s", published_type,
                     request_prefix[0] != '\0' ? request_prefix : "(none)");
            http_response_free(&response);
            return 0;
        }
        if (request == 0) {
            log_line("publish %s failed with HTTP %ld", published_type,
                     response.status);
        }
        http_response_free(&response);
        if (attempt < 2) {
            sleep_msec(500);
        }
    }
    return -1;
}

// -------------------------------------------------------------------- pi run

static bool args_have_session_flag(const Config *config) {
    for (size_t i = 0; i < config->arg_count; i++) {
        const char *arg = config->args[i];
        if (strcmp(arg, "--no-session") == 0 ||
            strcmp(arg, "--session-id") == 0 ||
            strcmp(arg, "--session") == 0 ||
            strcmp(arg, "--continue") == 0 || strcmp(arg, "-c") == 0 ||
            strcmp(arg, "--resume") == 0 || strcmp(arg, "-r") == 0) {
            return true;
        }
    }
    return false;
}

static char **build_argv(const Config *config, const char *prompt,
                         const char *session) {
    const bool have_session = session != NULL && *session != '\0';
    char **argv = malloc((config->arg_count + 5) * sizeof(char *));
    if (argv == NULL) {
        return NULL;
    }

    size_t index = 0;
    argv[index++] = config->command;
    for (size_t i = 0; i < config->arg_count; i++) {
        const char *arg = config->args[i];
        if (have_session) {
            if (strcmp(arg, "--no-session") == 0) {
                continue;
            }
            if (strcmp(arg, "--session-id") == 0 ||
                strcmp(arg, "--session") == 0) {
                if (i + 1 < config->arg_count) {
                    i++; // drop the option value too
                }
                continue;
            }
        }
        argv[index++] = (char *)arg;
    }

    if (have_session) {
        argv[index++] = "--session-id";
        argv[index++] = (char *)session;
    } else if (!args_have_session_flag(config)) {
        argv[index++] = "--no-session";
    }
    argv[index++] = (char *)prompt;
    argv[index] = NULL;
    return argv;
}

// pi is often a JS wrapper with "#! /usr/bin/env node", and node may live next
// to pi (for example ~/.bun/bin/node) rather than in the systemd PATH. Put the
// command's own directory first in the child's PATH so env can find it.
static void prepend_command_dir_to_path(const char *command) {
    const char *slash = strrchr(command, '/');
    if (slash == NULL) {
        return;
    }
    size_t dir_length = (size_t)(slash - command);
    if (dir_length == 0) {
        dir_length = 1; // command is directly below "/"
    }
    char *dir = malloc(dir_length + 1);
    if (dir == NULL) {
        return;
    }
    memcpy(dir, command, dir_length);
    dir[dir_length] = '\0';

    const char *old_path = getenv("PATH");
    str_t path;
    str_init(&path);
    str_puts(&path, dir);
    if (old_path != NULL && *old_path != '\0') {
        str_putc(&path, ':');
        str_puts(&path, old_path);
    }
    if (path.s != NULL) {
        (void)setenv("PATH", path.s, 1);
    }
    str_free(&path);
    free(dir);
}

static void append_capped(str_t *destination, const char *data, size_t size,
                          size_t cap, bool *truncated) {
    size_t room = destination->len < cap ? cap - destination->len : 0;
    size_t take = size < room ? size : room;
    if (take > 0) {
        str_append(destination, data, take);
    }
    if (size > take && truncated != NULL) {
        *truncated = true;
    }
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void execute_command(const Config *config, const char *prompt,
                            const char *session, RunResult *run) {
    memset(run, 0, sizeof *run);
    run->exit_code = -1;

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        run->error = xstrdup("cannot create stdout pipe");
        return;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        run->error = xstrdup("cannot create stderr pipe");
        return;
    }

    char **argv = build_argv(config, prompt, session);
    if (argv == NULL) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        run->error = xstrdup("out of memory");
        return;
    }

    const long long started = monotonic_ms();
    const pid_t pid = fork();
    if (pid < 0) {
        run->error = xstrdup("cannot fork");
        free(argv);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return;
    }

    if (pid == 0) {
        (void)setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        close(out_pipe[0]);
        close(err_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(err_pipe[1], STDERR_FILENO) < 0) {
            dprintf(STDERR_FILENO, "exec failed: dup2: %s\n", strerror(errno));
            _exit(127);
        }
        close(out_pipe[1]);
        close(err_pipe[1]);

        if (chdir(config->workdir) != 0) {
            dprintf(STDERR_FILENO, "exec failed: chdir %s: %s\n",
                    config->workdir, strerror(errno));
            _exit(127);
        }
        prepend_command_dir_to_path(config->command);
        execvp(config->command, argv);
        dprintf(STDERR_FILENO, "exec failed: %s: %s\n", config->command,
                strerror(errno));
        _exit(127);
    }

    (void)setpgid(pid, pid);
    close(out_pipe[1]);
    close(err_pipe[1]);
    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);

    str_t stdout_text;
    str_t stderr_text;
    str_init(&stdout_text);
    str_init(&stderr_text);

    bool out_eof = false;
    bool err_eof = false;
    bool child_done = false;
    bool sent_term = false;
    bool sent_kill = false;
    bool drain_set = false;
    bool stderr_truncated = false;
    int status = 0;
    long long deadline = started + (long long)config->timeout * 1000;
    long long term_at = 0;
    long long drain_at = 0;

    while (true) {
        const long long now = monotonic_ms();
        if (!sent_term && now >= deadline) {
            (void)kill(-pid, SIGTERM);
            sent_term = true;
            run->timed_out = true;
            term_at = now;
        }
        if (sent_term && !sent_kill && now - term_at >= 5000) {
            (void)kill(-pid, SIGKILL);
            sent_kill = true;
        }
        if (!child_done) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                child_done = true;
            }
        }
        if (child_done && !drain_set) {
            drain_set = true;
            drain_at = now + 500;
        }
        if (child_done &&
            ((out_eof && err_eof) || (drain_set && now >= drain_at))) {
            break;
        }

        struct pollfd fds[2];
        int count = 0;
        if (!out_eof) {
            fds[count].fd = out_pipe[0];
            fds[count].events = POLLIN | POLLHUP;
            fds[count].revents = 0;
            count++;
        }
        if (!err_eof) {
            fds[count].fd = err_pipe[0];
            fds[count].events = POLLIN | POLLHUP;
            fds[count].revents = 0;
            count++;
        }

        const int polled = poll(fds, (nfds_t)count, 100);
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (polled == 0) {
            continue;
        }

        for (int i = 0; i < count; i++) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const bool is_stdout = !out_eof && fds[i].fd == out_pipe[0];
            const bool is_stderr = !err_eof && fds[i].fd == err_pipe[0];
            if (!is_stdout && !is_stderr) {
                continue;
            }
            char buffer[4096];
            for (;;) {
                const ssize_t got = read(fds[i].fd, buffer, sizeof buffer);
                if (got > 0) {
                    if (is_stdout) {
                        append_capped(&stdout_text, buffer, (size_t)got,
                                      (size_t)config->max_output,
                                      &run->truncated);
                    } else {
                        append_capped(&stderr_text, buffer, (size_t)got,
                                      STDERR_MAX, &stderr_truncated);
                    }
                    continue;
                }
                if (got == 0) {
                    if (is_stdout) {
                        out_eof = true;
                    } else {
                        err_eof = true;
                    }
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (is_stdout) {
                    out_eof = true;
                } else {
                    err_eof = true;
                }
                break;
            }
        }
    }

    if (!child_done) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    free(argv);

    run->duration_ms = monotonic_ms() - started;
    run->output = stdout_text.s != NULL ? stdout_text.s : xstrdup("");
    if (stdout_text.s != NULL) {
        str_init(&stdout_text);
    }
    run->stderr_text = stderr_text.s != NULL ? stderr_text.s : xstrdup("");
    if (stderr_text.s != NULL) {
        str_init(&stderr_text);
    }
    str_free(&stdout_text);
    str_free(&stderr_text);

    if (WIFEXITED(status)) {
        run->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        run->exit_code = 128 + WTERMSIG(status);
    } else {
        run->exit_code = -1;
    }

    if (run->timed_out) {
        run->error = xstrdup("timed out");
        run->success = false;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        run->success = true;
    } else if (WIFSIGNALED(status)) {
        char message[64];
        snprintf(message, sizeof message, "pi terminated by signal %d",
                 WTERMSIG(status));
        run->error = xstrdup(message);
    } else {
        char message[64];
        snprintf(message, sizeof message, "pi exited with code %d",
                 run->exit_code);
        run->error = xstrdup(message);
    }
}

// ------------------------------------------------------------------ consumer

static void process_event(const Config *config, const Event *event) {
    char prefix[9];
    event_id_prefix(event->id, prefix);
    char *types = join_list((const char *const *)event->types,
                            event->type_count, ",");
    log_line("event %s source=%s types=%s",
             prefix[0] != '\0' ? prefix : "(none)",
             event->source[0] != '\0' ? event->source : "(none)",
             event->type_count != 0 ? types : "(none)");
    free(types);

    if (event_is_self_loop(config, event)) {
        log_line("event %s is a result/error type; ack and skip",
                 prefix[0] != '\0' ? prefix : "(none)");
        (void)settle_event(config, true, event->id, NULL);
        return;
    }

    RunResult run;
    memset(&run, 0, sizeof run);
    run.exit_code = -1;

    const char *session = NULL;
    const char *session_source = "none";
    bool session_ok = true;
    if (event->session != NULL && event->session[0] != '\0') {
        if (!session_id_valid(event->session)) {
            log_line("event %s: invalid data.session; skipping",
                     prefix[0] != '\0' ? prefix : "(none)");
            run.error = xstrdup("invalid data.session");
            session_ok = false;
        } else {
            session = event->session;
            session_source = "request";
        }
    } else if (config->session != NULL && config->session[0] != '\0') {
        session = config->session;
        session_source = "config";
    }

    if (session_ok) {
        if (event->text == NULL || event->text[0] == '\0') {
            run.error = xstrdup("missing data.text");
        } else if (strlen(event->text) > PROMPT_MAX) {
            run.error = xstrdup("prompt exceeds 65536 bytes");
        } else {
            execute_command(config, event->text, session, &run);
            log_line("pi finished in %lldms (exit %d%s)", run.duration_ms,
                     run.exit_code, run.timed_out ? ", timed out" : "");
        }
    }

    const char *published_type =
        run.success ? config->result_type : config->error_type;
    char *json = build_result_json(config, event, published_type, &run, session,
                                   session_source);
    if (json == NULL) {
        log_line("cannot build result for event %s",
                 prefix[0] != '\0' ? prefix : "(none)");
        (void)settle_event(config, false, event->id, "cannot build result");
        run_result_free(&run);
        return;
    }

    const int published = publish_event(config, json, published_type, event->id);
    free(json);

    if (published == 0) {
        (void)settle_event(config, true, event->id, NULL);
    } else {
        (void)settle_event(config, false, event->id, "publish failed");
    }
    run_result_free(&run);
}

// --------------------------------------------------------------------- init

static char *build_bus_conf(const BusSettings *bus) {
    str_t text;
    str_init(&text);
    str_puts(&text, "# ~/.config/yi/bus.conf -- Yi bus connection (shared)\n");
    str_puts(&text, "# Written by yi-pi-agent init; keep it private (0600).\n");
    str_puts(&text, "\n");
    str_printf(&text, "scheme = %s\n", bus->scheme);
    str_printf(&text, "host = %s\n", bus->host);
    str_printf(&text, "port = %d\n", bus->port);
    str_printf(&text, "token = %s\n", bus->token);
    return text.s;
}

static void append_quoted_arg(str_t *text, const char *arg) {
    bool quote = *arg == '\0' || strpbrk(arg, " \t\"\\") != NULL;
    if (!quote) {
        str_puts(text, arg);
        return;
    }
    str_putc(text, '"');
    for (const char *p = arg; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            str_putc(text, '\\');
        }
        str_putc(text, *p);
    }
    str_putc(text, '"');
}

static char *build_tool_conf(const Config *config) {
    char *types = join_list((const char *const *)config->types,
                            config->type_count, ",");
    char *patterns = join_list((const char *const *)config->patterns,
                               config->pattern_count, ",");
    str_t args;
    str_init(&args);
    for (size_t i = 0; i < config->arg_count; i++) {
        if (i > 0) {
            str_putc(&args, ' ');
        }
        append_quoted_arg(&args, config->args[i]);
    }

    str_t text;
    str_init(&text);
    str_puts(&text, "# ~/.config/yi/pi-agent.conf -- yi-pi-agent\n");
    str_puts(&text, "# Bus address and token live in ~/.config/yi/bus.conf.\n\n");
    str_printf(&text, "consumer = %s\n", config->consumer);
    str_printf(&text, "patterns = %s\n", patterns);
    str_printf(&text, "types = %s\n", types);
    str_printf(&text, "command = %s\n", config->command);
    str_printf(&text, "args = %s\n", args.s != NULL ? args.s : "");
    str_printf(&text, "session = %s\n", config->session);
    str_printf(&text, "workdir = %s\n", config->workdir);
    str_printf(&text, "timeout = %d\n", config->timeout);
    str_printf(&text, "max_output = %d\n", config->max_output);
    str_printf(&text, "poll_timeout = %d\n", config->poll_timeout);
    str_printf(&text, "skip_history = %s\n",
               config->skip_history ? "true" : "false");
    str_printf(&text, "result_type = %s\n", config->result_type);
    str_printf(&text, "error_type = %s\n", config->error_type);
    str_printf(&text, "source = %s\n", config->source);

    free(types);
    free(patterns);
    str_free(&args);
    return text.s;
}

static char *read_answer(void) {
    char *line = NULL;
    size_t capacity = 0;
    const ssize_t size = getline(&line, &capacity, stdin);
    if (size < 0) {
        free(line);
        return NULL;
    }
    size_t length = (size_t)size;
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
    return line;
}

static char *ask_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fflush(stdout);
    return read_answer();
}

static char *ask_secret(const char *format, ...) {
    char prompt[512];
    va_list args;
    va_start(args, format);
    vsnprintf(prompt, sizeof prompt, format, args);
    va_end(args);

    struct termios saved;
    bool hidden = false;
    if (isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &saved) == 0) {
        struct termios quiet = saved;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &quiet) == 0) {
            hidden = true;
        }
    }
    fputs(prompt, stdout);
    fflush(stdout);
    char *line = read_answer();
    if (hidden) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return line;
}

static bool ask_yes_no(const char *prompt, bool fallback) {
    char *answer = ask_line("%s", prompt);
    if (answer == NULL) {
        return fallback;
    }
    char *text = trim(answer);
    bool result = fallback;
    if (*text != '\0') {
        const char first = (char)tolower((unsigned char)*text);
        if (first == 'y') {
            result = true;
        } else if (first == 'n') {
            result = false;
        }
    }
    free(answer);
    return result;
}

static size_t discard_response(char *data, size_t size, size_t count,
                               void *userdata) {
    (void)data;
    (void)userdata;
    return size * count;
}

static int health_probe(const char *scheme, const char *host, int port,
                        const char *token, char *reason, size_t reason_size) {
    char *origin = format_origin(scheme, host, port);
    char *endpoint = malloc(strlen(origin) + 9);
    if (endpoint == NULL) {
        free(origin);
        snprintf(reason, reason_size, "out of memory");
        return -1;
    }
    sprintf(endpoint, "%s/healthz", origin);
    free(origin);

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        free(endpoint);
        snprintf(reason, reason_size, "cannot initialise libcurl");
        return -1;
    }

    struct curl_slist *headers = NULL;
    if (token != NULL && token[0] != '\0') {
        str_t auth;
        str_init(&auth);
        str_printf(&auth, "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, auth.s);
        str_free(&auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
    if (headers != NULL) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    int result = 0;
    if (code != CURLE_OK) {
        snprintf(reason, reason_size, "%s", curl_easy_strerror(code));
        result = -1;
    } else if (status < 200 || status >= 300) {
        snprintf(reason, reason_size, "HTTP %ld", status);
        result = -1;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(endpoint);
    return result;
}

static char *executable_path(void) {
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (length <= 0) {
        return NULL;
    }
    buffer[length] = '\0';
    return xstrdup(buffer);
}

static char *installed_binary_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL || *home == '\0') {
        return NULL;
    }
    size_t size = strlen(home) + 40;
    char *path = malloc(size);
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, size, "%s/.local/bin/yi-pi-agent", home);
    return path;
}

static char *systemd_unit_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && *xdg != '\0') {
        size_t size = strlen(xdg) + 56;
        char *path = malloc(size);
        snprintf(path, size, "%s/systemd/user/yi-pi-agent.service", xdg);
        return path;
    }
    return expand_home("~/.config/systemd/user/yi-pi-agent.service");
}

static int copy_binary(const char *source, const char *target) {
    if (mkdir_p_for(target) != 0) {
        log_line("cannot create directory for %s", target);
        return -1;
    }

    size_t tmp_size = strlen(target) + 32;
    char *tmp = malloc(tmp_size);
    if (tmp == NULL) {
        log_line("out of memory");
        return -1;
    }
    snprintf(tmp, tmp_size, "%s.tmp.%ld", target, (long)getpid());

    const int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        log_line("cannot read %s: %s", source, strerror(errno));
        free(tmp);
        return -1;
    }
    const int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) {
        log_line("cannot write %s: %s", tmp, strerror(errno));
        close(in);
        free(tmp);
        return -1;
    }
    (void)fchmod(out, 0755);

    char buffer[65536];
    int result = 0;
    ssize_t got = 0;
    while ((got = read(in, buffer, sizeof buffer)) > 0) {
        size_t offset = 0;
        while (offset < (size_t)got) {
            ssize_t wrote = write(out, buffer + offset, (size_t)got - offset);
            if (wrote < 0) {
                if (errno == EINTR) {
                    continue;
                }
                log_line("cannot write %s: %s", tmp, strerror(errno));
                result = -1;
                break;
            }
            if (wrote == 0) {
                log_line("cannot write %s: short write", tmp);
                result = -1;
                break;
            }
            offset += (size_t)wrote;
        }
        if (result != 0) {
            break;
        }
    }
    if (got < 0 && result == 0) {
        log_line("cannot read %s: %s", source, strerror(errno));
        result = -1;
    }
    if (close(out) != 0 && result == 0) {
        log_line("cannot write %s: %s", tmp, strerror(errno));
        result = -1;
    }
    close(in);
    if (result == 0 && rename(tmp, target) != 0) {
        log_line("cannot replace %s: %s", target, strerror(errno));
        result = -1;
    }
    if (result != 0) {
        unlink(tmp);
    }
    free(tmp);
    return result;
}

static char *install_binary(void) {
    char *source = executable_path();
    char *target = installed_binary_path();
    if (source == NULL || target == NULL) {
        log_line("cannot locate the current executable or HOME");
        free(source);
        free(target);
        return NULL;
    }

    struct stat source_info;
    struct stat target_info;
    const bool same_path = strcmp(source, target) == 0;
    const bool same_file = stat(source, &source_info) == 0 &&
                           stat(target, &target_info) == 0 &&
                           source_info.st_dev == target_info.st_dev &&
                           source_info.st_ino == target_info.st_ino;
    if (!same_path && !same_file) {
        if (copy_binary(source, target) != 0) {
            free(source);
            free(target);
            return NULL;
        }
        printf("已安装二进制到 %s\n", target);
        fflush(stdout);
    }
    free(source);
    return target;
}

static char *build_unit(const char *binary) {
    str_t text;
    str_init(&text);
    str_puts(&text, "[Unit]\n");
    str_puts(&text, "Description=Yi pi agent (consumer + producer)\n");
    str_puts(&text, "Documentation=https://github.com/gresces/Yi-Tools/blob/main/tools/yi-pi-agent/README.md\n");
    str_puts(&text, "After=network-online.target\n");
    str_puts(&text, "Wants=network-online.target\n");
    str_puts(&text, "\n[Service]\n");
    str_puts(&text, "Type=simple\n");
    str_printf(&text, "ExecStart=%s\n", binary);
    str_puts(&text, "Restart=on-failure\n");
    str_puts(&text, "RestartSec=15s\n");
    str_puts(&text, "\n[Install]\n");
    str_puts(&text, "WantedBy=default.target\n");
    return text.s;
}

static char *join_command(char *const argv[]) {
    str_t text;
    str_init(&text);
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            str_putc(&text, ' ');
        }
        str_puts(&text, argv[i]);
    }
    return text.s;
}

static int run_command(char *const argv[]) {
    const pid_t pid = fork();
    if (pid < 0) {
        log_line("fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "yi-pi-agent: cannot run %s: %s\n", argv[0],
                strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            log_line("waitpid failed: %s", strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

static int deploy_service(void) {
    char *binary = install_binary();
    if (binary == NULL) {
        return -1;
    }

    char *unit_path = systemd_unit_path();
    if (unit_path == NULL) {
        log_line("cannot determine the systemd unit path");
        free(binary);
        return -1;
    }

    char *unit = build_unit(binary);
    if (unit == NULL ||
        write_atomic_mode(unit_path, unit, strlen(unit), 0644) != 0) {
        log_line("cannot write %s", unit_path);
        free(unit);
        free(binary);
        free(unit_path);
        return -1;
    }
    free(unit);
    printf("已写入 %s\n", unit_path);
    fflush(stdout);

    char *const reload_argv[] = {"systemctl", "--user", "daemon-reload", NULL};
    char *const enable_argv[] = {"systemctl", "--user", "enable",
                                 "yi-pi-agent.service", NULL};
    char *const restart_argv[] = {"systemctl", "--user", "restart",
                                  "yi-pi-agent.service", NULL};
    char *const *steps[] = {reload_argv, enable_argv, restart_argv};

    int result = 0;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        char *display = join_command(steps[i]);
        const int status = run_command(steps[i]);
        if (status != 0) {
            log_line("command failed (exit %d): %s", status,
                     display != NULL ? display : steps[i][0]);
            log_line("check the service with: journalctl --user -u yi-pi-agent");
            free(display);
            result = -1;
            break;
        }
        free(display);
    }

    if (result == 0) {
        printf("服务已启用并启动\n");
        fflush(stdout);
    }

    free(binary);
    free(unit_path);
    return result;
}

static void init_tool_config(Config *config) {
    char *types = join_list((const char *const *)config->types,
                            config->type_count, ",");
    char *patterns = join_list((const char *const *)config->patterns,
                               config->pattern_count, ",");
    char *args = join_list((const char *const *)config->args,
                           config->arg_count, " ");

    char *answer = ask_line("消费者名称 consumer [%s]: ", config->consumer);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            set_string(&config->consumer, value);
        }
        free(answer);
    }

    answer = ask_line("订阅模式 patterns [%s]: ", patterns);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            char **new_patterns = NULL;
            size_t new_count = 0;
            new_patterns = split_list(value, &new_count);
            if (new_count > MAX_PATTERNS) {
                log_line("最多允许 %d 个 patterns;保留原值", MAX_PATTERNS);
                free_list(new_patterns, new_count);
            } else if (new_count == 0 && config->type_count == 0) {
                log_line("types 为空时 patterns 不能为空;保留原值");
                free_list(new_patterns, new_count);
            } else {
                free_list(config->patterns, config->pattern_count);
                config->patterns = new_patterns;
                config->pattern_count = new_count;
            }
        }
        free(answer);
    }

    answer = ask_line("pi 命令 command [%s]: ", config->command);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            set_string(&config->command, value);
        }
        free(answer);
    }

    answer = ask_line("命令参数 args [%s]: ", args);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            size_t new_count = 0;
            char **new_args = split_args(value, &new_count);
            free_list(config->args, config->arg_count);
            config->args = new_args;
            config->arg_count = new_count;
        }
        free(answer);
    }

    answer = ask_line("默认 session(可空,回车表示不指定) [%s]: ",
                      config->session != NULL && config->session[0] != '\0'
                          ? config->session
                          : "(none)");
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            if (session_id_valid(value)) {
                set_string(&config->session, value);
            } else {
                fprintf(stderr,
                        "session 无效: 必须匹配 [A-Za-z0-9._-]{1,128}\n");
            }
        }
        free(answer);
    }

    answer = ask_line("工作目录 workdir [%s]: ", config->workdir);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            free(config->workdir);
            config->workdir = expand_home(value);
        }
        free(answer);
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        answer = ask_line("单次超时秒数 timeout [%d]: ", config->timeout);
        if (answer == NULL) {
            break;
        }
        char *value = trim(answer);
        if (*value == '\0') {
            free(answer);
            break;
        }
        int timeout = 0;
        if (parse_int_range(value, 5, 1800, &timeout)) {
            config->timeout = timeout;
            free(answer);
            break;
        }
        free(answer);
        fprintf(stderr, "timeout 必须是 5-1800 之间的整数\n");
    }

    // Defensive defaults: an existing broken file must not survive init.
    if (config->consumer == NULL || config->consumer[0] == '\0') {
        set_string(&config->consumer, "pi-agent");
    }
    if (config->command == NULL || config->command[0] == '\0') {
        set_string(&config->command, "pi");
    }
    if (config->workdir == NULL || config->workdir[0] == '\0') {
        free(config->workdir);
        config->workdir = expand_home("~/.local/state/yi-pi-agent/work");
    }
    if (config->session == NULL) {
        config->session = xstrdup("");
    } else if (config->session[0] != '\0' &&
               !session_id_valid(config->session)) {
        set_string(&config->session, "");
    }
    if (config->type_count == 0 && config->pattern_count == 0) {
        set_list(&config->patterns, &config->pattern_count, "^pi-agent\\.");
    }
    if (config->result_type == NULL || config->result_type[0] == '\0') {
        set_string(&config->result_type, "pi-agent.result");
    }
    if (config->error_type == NULL || config->error_type[0] == '\0') {
        set_string(&config->error_type, "pi-agent.error");
    }
    if (config->source == NULL || config->source[0] == '\0') {
        set_string(&config->source, "pi-agent");
    }

    free(types);
    free(patterns);
    free(args);
}

static int run_init(const char *tool_path) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_line("libcurl initialisation failed");
        return 1;
    }

    char *bus_path = default_bus_path();
    BusSettings bus;
    bus_defaults(&bus);
    if (bus_load_file(&bus, bus_path)) {
        bus_parse_url(&bus);
    }

    Config config;
    config_defaults(&config);
    struct stat info;
    const bool tool_present = stat(tool_path, &info) == 0;
    if (tool_present && config_load(&config, tool_path) != 0) {
        log_line("%s has errors; keeping the keys that parsed", tool_path);
    }

    int result = 0;

    char *answer = ask_line("协议 scheme [%s]: ", bus.scheme);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            set_string(&bus.scheme, value);
        }
        free(answer);
    }

    answer = ask_line("主机 host [%s]: ", bus.host);
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            set_string(&bus.host, value);
        }
        free(answer);
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        answer = ask_line("端口 port [%d]: ", bus.port);
        if (answer == NULL) {
            break;
        }
        char *value = trim(answer);
        if (*value == '\0') {
            free(answer);
            break;
        }
        int port = 0;
        if (parse_port_value(value, &port)) {
            bus.port = port;
            free(answer);
            break;
        }
        free(answer);
        fprintf(stderr, "端口必须是 1-65535 之间的整数\n");
    }

    answer = ask_secret("令牌 token [%s，回车保留]: ",
                        bus.token[0] != '\0' ? "已设置" : "未设置");
    if (answer != NULL) {
        char *value = trim(answer);
        if (*value != '\0') {
            set_string(&bus.token, value);
        }
        free(answer);
    }

    char reason[256];
    if (health_probe(bus.scheme, bus.host, bus.port, bus.token, reason,
                     sizeof reason) == 0) {
        printf("ok\n");
        fflush(stdout);
    } else {
        printf("健康检查失败: %s\n", reason);
        fflush(stdout);
        if (isatty(STDIN_FILENO) != 0 &&
            !ask_yes_no("仍然保存? [Y/n]: ", true)) {
            log_line("已放弃保存");
            result = 1;
            goto done;
        }
    }

    char *bus_text = build_bus_conf(&bus);
    if (bus_text == NULL ||
        write_atomic_private(bus_path, bus_text, strlen(bus_text)) != 0) {
        free(bus_text);
        result = 1;
        goto done;
    }
    free(bus_text);
    printf("已写入 %s\n", bus_path);
    fflush(stdout);

    init_tool_config(&config);

    char *tool_text = build_tool_conf(&config);
    if (tool_text == NULL ||
        write_atomic_private(tool_path, tool_text, strlen(tool_text)) != 0) {
        free(tool_text);
        result = 1;
        goto done;
    }
    free(tool_text);
    printf("已写入 %s\n", tool_path);
    fflush(stdout);

    bool deployed = false;
    if (ask_yes_no("部署为 systemd user 服务? [y/N]: ", false)) {
        if (deploy_service() != 0) {
            result = 1;
            goto done;
        }
        deployed = true;
    }

    if (deployed) {
        printf("查看日志: journalctl --user -u yi-pi-agent -f\n");
    } else {
        printf("下一步: systemctl --user enable --now yi-pi-agent\n");
    }
    fflush(stdout);

done:
    config_free(&config);
    bus_free(&bus);
    free(bus_path);
    curl_global_cleanup();
    return result;
}

// --------------------------------------------------------------------- main

static void handle_stop(int signal_number) {
    (void)signal_number;
    g_stop = 1;
}

static void install_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void usage(FILE *out) {
    fprintf(out,
            "yi-pi-agent %s - run pi for Yi events and publish the result\n"
            "\n"
            "Usage: yi-pi-agent [options]\n"
            "       yi-pi-agent init\n"
            "\n"
            "Commands:\n"
            "  init                write bus.conf and pi-agent.conf, optionally install a systemd user service\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH   config file (default ~/.config/yi/pi-agent.conf)\n"
            "      --once          process at most one event (1s poll), then exit\n"
            "      --check         print the effective config and exit\n"
            "  -V, --version       print version and exit\n"
            "  -h, --help          print this help and exit\n",
            VERSION);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    bool check = false;
    bool once = false;
    bool initialise = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("yi-pi-agent %s\n", VERSION);
            return 0;
        }
        if (strcmp(arg, "init") == 0) {
            initialise = true;
        } else if (strcmp(arg, "--once") == 0) {
            once = true;
        } else if (strcmp(arg, "--check") == 0) {
            check = true;
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "yi-pi-agent: %s needs a value\n", arg);
                return 2;
            }
            config_path = argv[++i];
        } else {
            fprintf(stderr, "yi-pi-agent: unknown option %s\n", arg);
            return 2;
        }
    }

    char *owned_path = NULL;
    if (config_path == NULL) {
        owned_path = default_config_path();
        config_path = owned_path;
    }

    if (initialise) {
        const int status = run_init(config_path);
        free(owned_path);
        return status;
    }

    Config config;
    config_defaults(&config);
    struct stat info;
    const bool config_missing =
        stat(config_path, &info) != 0 && errno == ENOENT;

    if (check) {
        if (!config_missing && config_load(&config, config_path) != 0) {
            log_line("config %s has errors", config_path);
            free(owned_path);
            config_free(&config);
            return 1;
        }
        config_apply_bus(&config);
        config_print(&config);
        free(owned_path);
        config_free(&config);
        return 0;
    }

    if (config_missing) {
        log_line("no config at %s; run 'yi-pi-agent init'", config_path);
        free(owned_path);
        config_free(&config);
        return 1;
    }
    if (config_load(&config, config_path) != 0) {
        log_line("config %s has errors", config_path);
        free(owned_path);
        config_free(&config);
        return 1;
    }
    config_apply_bus(&config);

    install_signals();
    if (mkdir_p_all(config.workdir) != 0) {
        log_line("cannot create workdir %s", config.workdir);
        free(owned_path);
        config_free(&config);
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_line("libcurl initialisation failed");
        free(owned_path);
        config_free(&config);
        return 1;
    }

    bool consumer_created = false;
    if (register_consumer(&config, &consumer_created) != 0) {
        curl_global_cleanup();
        free(owned_path);
        config_free(&config);
        return 1;
    }
    if (consumer_created && config.skip_history) {
        drain_history(&config);
    }

    log_line("%s, consumer %s -> %s", VERSION, config.consumer, config.url);

    int result = 0;
    bool running = true;
    while (running && !g_stop) {
        HttpResponse response;
        const int next = consumer_next(&config, once ? 1 : config.poll_timeout,
                                       &response);
        if (next < 0) {
            if (once) {
                result = 1;
            }
            http_response_free(&response);
            if (!once && !g_stop) {
                sleep_msec(1000);
            }
            if (once) {
                running = false;
            }
            continue;
        }
        if (next == 0) {
            http_response_free(&response);
            if (once) {
                running = false;
            }
            continue;
        }

        Event event;
        if (event_parse(response.body.s != NULL ? response.body.s : "", &event,
                        false) != 0) {
            log_line("cannot parse event JSON");
            (void)settle_event(&config, true, "", NULL);
            http_response_free(&response);
            if (once) {
                running = false;
            }
            continue;
        }

        process_event(&config, &event);
        event_free(&event);
        http_response_free(&response);
        if (once) {
            running = false;
        }
    }

    log_line("stopped");
    curl_global_cleanup();
    free(owned_path);
    config_free(&config);
    return result;
}
