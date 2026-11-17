// yi-timer — publish calendar-timer events to Yi.
//
// Design notes:
//   * One process, single thread. The loop sleeps with
//     clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME) until the nearest due
//     timer, but never longer than 30 seconds, so config edits are picked up
//     without a restart.
//   * All schedules are local-time based. `every` is anchored at local
//     midnight, daily/weekly use localtime_r + mktime.
//   * Event ids are derived from (source, name, planned time), so a retry of
//     the same occurrence keeps the same id and Yi can deduplicate it.
//
// Dependencies: libc, OpenSSL, libcurl.

#define _GNU_SOURCE

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
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

#define VERSION "0.1.0"
#define MAX_SLEEP 30
#define MAX_SCHEDULE_SECONDS 86400

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
    fputs("yi-timer: ", stderr);
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
        size_t size = strlen(xdg) + 32;
        char *path = malloc(size);
        snprintf(path, size, "%s/yi/timers.conf", xdg);
        return path;
    }
    return expand_home("~/.config/yi/timers.conf");
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

static void format_local(time_t value, char out[32]) {
    struct tm tm;
    out[0] = '\0';
    if (localtime_r(&value, &tm) == NULL) {
        return;
    }
    strftime(out, 32, "%Y-%m-%d %H:%M:%S", &tm);
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

// Fills scheme/host/port from the optional `url` override so init can show
// the individual parts.
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
        // Unknown keys are ignored so the shared file can grow new settings.
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

// ----------------------------------------------------------------- schedule

typedef enum {
    SCHEDULE_EVERY = 0,
    SCHEDULE_DAILY = 1,
    SCHEDULE_WEEKLY = 2,
} schedule_kind_t;

typedef struct {
    schedule_kind_t kind;
    int interval;
    int hour;
    int minute;
    int second;
    int weekday;
} Schedule;

static bool parse_clock(const char *text, int *hour, int *minute, int *second) {
    char copy[32];
    if (strlen(text) >= sizeof copy) {
        return false;
    }
    strcpy(copy, text);

    char *parts[3];
    int count = 0;
    char *save = NULL;
    for (char *token = strtok_r(copy, ":", &save); token != NULL;
         token = strtok_r(NULL, ":", &save)) {
        if (count >= 3) {
            return false;
        }
        parts[count++] = token;
    }
    if (count < 2 || count > 3) {
        return false;
    }

    int h = 0;
    int m = 0;
    int s = 0;
    if (!parse_int_range(parts[0], 0, 23, &h) ||
        !parse_int_range(parts[1], 0, 59, &m)) {
        return false;
    }
    if (count == 3 && !parse_int_range(parts[2], 0, 59, &s)) {
        return false;
    }
    *hour = h;
    *minute = m;
    *second = s;
    return true;
}

static bool parse_weekday(const char *text, int *weekday) {
    static const char *names[] = {"sun", "mon", "tue", "wed",
                                  "thu", "fri", "sat"};
    for (int i = 0; i < 7; i++) {
        if (strcasecmp(text, names[i]) == 0) {
            *weekday = i;
            return true;
        }
    }
    return false;
}

static bool schedule_parse(const char *text, Schedule *out, char *error,
                           size_t error_size) {
    memset(out, 0, sizeof *out);

    char copy[128];
    if (strlen(text) >= sizeof copy) {
        snprintf(error, error_size, "schedule is too long");
        return false;
    }
    strcpy(copy, text);

    char *save = NULL;
    char *kind = strtok_r(copy, " \t", &save);
    if (kind == NULL) {
        snprintf(error, error_size, "schedule is empty");
        return false;
    }

    if (strcasecmp(kind, "every") == 0) {
        char *value = strtok_r(NULL, " \t", &save);
        if (value == NULL || strtok_r(NULL, " \t", &save) != NULL) {
            goto invalid;
        }

        errno = 0;
        char *end = NULL;
        const long number = strtol(value, &end, 10);
        if (errno != 0 || end == value || number <= 0 ||
            number > MAX_SCHEDULE_SECONDS) {
            goto invalid;
        }
        int multiplier = 0;
        const char unit = (char)tolower((unsigned char)*end);
        if (unit == 's') {
            multiplier = 1;
        } else if (unit == 'm') {
            multiplier = 60;
        } else if (unit == 'h') {
            multiplier = 3600;
        } else {
            goto invalid;
        }
        if (end[1] != '\0') {
            goto invalid;
        }
        const long long total = (long long)number * multiplier;
        if (total < 1 || total > MAX_SCHEDULE_SECONDS) {
            goto invalid;
        }

        out->kind = SCHEDULE_EVERY;
        out->interval = (int)total;
        return true;
    }

    if (strcasecmp(kind, "daily") == 0) {
        char *clock = strtok_r(NULL, " \t", &save);
        if (clock == NULL || strtok_r(NULL, " \t", &save) != NULL) {
            goto invalid;
        }
        if (!parse_clock(clock, &out->hour, &out->minute, &out->second)) {
            goto invalid;
        }
        out->kind = SCHEDULE_DAILY;
        return true;
    }

    if (strcasecmp(kind, "weekly") == 0) {
        char *day = strtok_r(NULL, " \t", &save);
        char *clock = strtok_r(NULL, " \t", &save);
        if (day == NULL || clock == NULL ||
            strtok_r(NULL, " \t", &save) != NULL) {
            goto invalid;
        }
        if (!parse_weekday(day, &out->weekday) ||
            !parse_clock(clock, &out->hour, &out->minute, &out->second)) {
            goto invalid;
        }
        out->kind = SCHEDULE_WEEKLY;
        return true;
    }

invalid:
    snprintf(error, error_size,
             "schedule must be 'every Ns|Nm|Nh', 'daily HH:MM[:SS]' or "
             "'weekly mon..sun HH:MM[:SS]'");
    return false;
}

// Returns the first trigger strictly after t, or (time_t)-1 on failure.
static time_t next_after(const Schedule *schedule, time_t t) {
    if (schedule == NULL) {
        return (time_t)-1;
    }

    struct tm tm;
    switch (schedule->kind) {
    case SCHEDULE_EVERY: {
        if (schedule->interval < 1 || localtime_r(&t, &tm) == NULL) {
            return (time_t)-1;
        }
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        const time_t base = mktime(&tm);
        if (base == (time_t)-1) {
            return (time_t)-1;
        }

        long long elapsed = (long long)t - (long long)base;
        if (elapsed < 0) {
            elapsed = 0;
        }
        const long long k = elapsed / schedule->interval + 1;
        time_t candidate = base + (time_t)(k * schedule->interval);
        while (candidate <= t) {
            candidate += schedule->interval;
        }
        return candidate;
    }
    case SCHEDULE_DAILY: {
        if (localtime_r(&t, &tm) == NULL) {
            return (time_t)-1;
        }
        tm.tm_hour = schedule->hour;
        tm.tm_min = schedule->minute;
        tm.tm_sec = schedule->second;
        tm.tm_isdst = -1;
        time_t candidate = mktime(&tm);
        if (candidate == (time_t)-1) {
            return (time_t)-1;
        }
        while (candidate <= t) {
            tm.tm_mday += 1;
            tm.tm_isdst = -1;
            candidate = mktime(&tm);
            if (candidate == (time_t)-1) {
                return (time_t)-1;
            }
        }
        return candidate;
    }
    case SCHEDULE_WEEKLY: {
        if (localtime_r(&t, &tm) == NULL) {
            return (time_t)-1;
        }
        const int delta = (schedule->weekday - tm.tm_wday + 7) % 7;
        tm.tm_mday += delta;
        tm.tm_hour = schedule->hour;
        tm.tm_min = schedule->minute;
        tm.tm_sec = schedule->second;
        tm.tm_isdst = -1;
        time_t candidate = mktime(&tm);
        if (candidate == (time_t)-1) {
            return (time_t)-1;
        }
        while (candidate <= t) {
            tm.tm_mday += 7;
            tm.tm_isdst = -1;
            candidate = mktime(&tm);
            if (candidate == (time_t)-1) {
                return (time_t)-1;
            }
        }
        return candidate;
    }
    }
    return (time_t)-1;
}

// ------------------------------------------------------------------- config

typedef struct {
    char *name;
    char *schedule;
    char *type;
    char *source;
    Schedule parsed;
    bool has_schedule;
    time_t due;
    unsigned long long seq;
} Timer;

typedef struct {
    char *url;
    char *token;
    int max_late;
    Timer *timers;
    size_t count;
    size_t cap;
} Config;

static void timer_free(Timer *timer) {
    free(timer->name);
    free(timer->schedule);
    free(timer->type);
    free(timer->source);
    memset(timer, 0, sizeof *timer);
}

static void config_free(Config *config) {
    free(config->url);
    free(config->token);
    for (size_t i = 0; i < config->count; i++) {
        timer_free(&config->timers[i]);
    }
    free(config->timers);
    memset(config, 0, sizeof *config);
}

static void config_defaults(Config *config) {
    memset(config, 0, sizeof *config);
    config->url = xstrdup("http://127.0.0.1:9092");
    config->token = xstrdup("");
    config->max_late = 3600;
}

static Timer *config_find_timer(Config *config, const char *name) {
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->timers[i].name, name) == 0) {
            return &config->timers[i];
        }
    }
    return NULL;
}

static Timer *config_add_timer(Config *config, const char *name) {
    if (config->count == config->cap) {
        size_t cap = config->cap != 0 ? config->cap * 2 : 4;
        Timer *grown = realloc(config->timers, cap * sizeof *grown);
        if (grown == NULL) {
            log_line("out of memory");
            exit(1);
        }
        config->timers = grown;
        config->cap = cap;
    }
    Timer *timer = &config->timers[config->count++];
    memset(timer, 0, sizeof *timer);
    timer->name = xstrdup(name);
    timer->type = xstrdup("timer.fired");
    timer->source = xstrdup("timer");
    return timer;
}

static void config_remove_last_timer(Config *config) {
    if (config->count == 0) {
        return;
    }
    config->count--;
    timer_free(&config->timers[config->count]);
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
    Timer *current = NULL;
    int line_number = 0;
    int problems = 0;

    while ((size = getline(&line, &capacity, file)) >= 0) {
        line_number++;
        char *text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';') {
            continue;
        }

        if (*text == '[') {
            char *close = strchr(text, ']');
            if (close == NULL) {
                log_line("%s:%d: malformed section", path, line_number);
                problems++;
                current = NULL;
                continue;
            }
            *close = '\0';
            char *name = trim(text + 1);
            char *colon = strchr(name, ':');
            if (colon == NULL || strncasecmp(name, "timer", 5) != 0) {
                log_line("%s:%d: unknown section [%s]", path, line_number, name);
                problems++;
                current = NULL;
                continue;
            }
            *colon = '\0';
            char *label = trim(colon + 1);
            if (strcasecmp(trim(name), "timer") != 0 || *label == '\0') {
                log_line("%s:%d: timer section needs a name", path, line_number);
                problems++;
                current = NULL;
                continue;
            }
            if (config_find_timer(config, label) != NULL) {
                log_line("%s:%d: duplicate timer '%s'", path, line_number, label);
                problems++;
                current = NULL;
                continue;
            }
            current = config_add_timer(config, label);
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

        if (current == NULL) {
            if (strcasecmp(key, "max_late") == 0) {
                int max_late = 0;
                if (parse_int_range(value, 0, INT_MAX, &max_late)) {
                    config->max_late = max_late;
                } else {
                    log_line("%s:%d: max_late must be a non-negative integer",
                             path, line_number);
                    problems++;
                }
            } else {
                log_line("%s:%d: unknown global key '%s'", path, line_number, key);
                problems++;
            }
            continue;
        }

        if (strcasecmp(key, "schedule") == 0) {
            set_string(&current->schedule, value);
            char error[160];
            if (schedule_parse(value, &current->parsed, error, sizeof error)) {
                current->has_schedule = true;
            } else {
                log_line("%s:%d: %s", path, line_number, error);
                current->has_schedule = false;
                problems++;
            }
        } else if (strcasecmp(key, "type") == 0) {
            set_string(&current->type, value);
        } else if (strcasecmp(key, "source") == 0) {
            set_string(&current->source, value);
        } else {
            log_line("%s:%d: unknown timer key '%s'", path, line_number, key);
            problems++;
        }
    }

    free(line);
    fclose(file);

    for (size_t i = 0; i < config->count; i++) {
        if (!config->timers[i].has_schedule) {
            log_line("%s: timer '%s' needs a valid schedule", path,
                     config->timers[i].name);
            problems++;
        }
    }

    return problems == 0 ? 0 : -1;
}

// Precedence: YI_URL/YI_TOKEN, then bus.conf, then the built-in defaults.
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
    printf("url       = %s\n", config->url);
    printf("token     = %s\n", config->token[0] != '\0' ? "***" : "(none)");
    printf("max_late  = %d\n", config->max_late);
    printf("timers    = %zu\n", config->count);

    const time_t now = time(NULL);
    for (size_t i = 0; i < config->count; i++) {
        const Timer *timer = &config->timers[i];
        const time_t next = next_after(&timer->parsed, now);
        char local[32];
        char utc[32];
        if (next == (time_t)-1) {
            snprintf(local, sizeof local, "(invalid)");
            utc[0] = '\0';
        } else {
            format_local(next, local);
            format_rfc3339(next, utc);
        }
        printf("  %-8s  %-12s  type=%-12s  source=%-8s  next=%s",
               timer->name, timer->schedule != NULL ? timer->schedule : "",
               timer->type, timer->source, local);
        if (next != (time_t)-1) {
            printf(" (%s)", utc);
        }
        printf("\n");
    }
}

static void config_schedule_initial(Config *config, time_t now) {
    for (size_t i = 0; i < config->count; i++) {
        Timer *timer = &config->timers[i];
        timer->seq = 0;
        timer->due = next_after(&timer->parsed, now);
        if (timer->due == (time_t)-1) {
            log_line("%s: cannot compute the next trigger; retrying in %ds",
                     timer->name, MAX_SLEEP);
            timer->due = now + MAX_SLEEP;
        }
    }
}

// Keeps due/seq for names that still exist. A changed schedule string resets
// due to the next future occurrence; seq keeps counting for the life of the
// process.
static void config_merge(Config *config, Config *fresh, time_t now) {
    for (size_t i = 0; i < fresh->count; i++) {
        Timer *next = &fresh->timers[i];
        const Timer *old = config_find_timer(config, next->name);
        if (old != NULL) {
            next->seq = old->seq;
            if (old->schedule != NULL && next->schedule != NULL &&
                strcmp(old->schedule, next->schedule) == 0) {
                next->due = old->due;
            } else {
                next->due = next_after(&next->parsed, now);
            }
        } else {
            next->due = next_after(&next->parsed, now);
        }
        if (next->due == (time_t)-1) {
            log_line("%s: cannot compute the next trigger; retrying in %ds",
                     next->name, MAX_SLEEP);
            next->due = now + MAX_SLEEP;
        }
    }

    config_free(config);
    *config = *fresh;
}

// ------------------------------------------------------------------- events

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

static void event_id(const char *source, const char *name, const char *planned,
                     char out[33]) {
    str_t material;
    str_init(&material);
    str_printf(&material, "%s\x1f%s\x1f%s", source, name, planned);

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

static char *build_event_json(const Timer *timer, time_t planned, time_t fired,
                              long long late, const char *host) {
    char planned_text[32];
    char fired_text[32];
    char id[33];
    format_rfc3339(planned, planned_text);
    format_rfc3339(fired, fired_text);
    event_id(timer->source, timer->name, planned_text, id);

    char local_text[32];
    struct tm local_tm;
    if (localtime_r(&fired, &local_tm) != NULL) {
        strftime(local_text, sizeof local_text, "%Y-%m-%d %H:%M:%S", &local_tm);
    } else {
        snprintf(local_text, sizeof local_text, "%s", fired_text);
    }

    str_t json;
    str_init(&json);
    str_puts(&json, "{\"id\":\"");
    json_escape(&json, id);
    str_puts(&json, "\",\"source\":\"");
    json_escape(&json, timer->source);
    str_puts(&json, "\",\"types\":[\"");
    json_escape(&json, timer->type);
    str_puts(&json, "\"],\"time\":\"");
    json_escape(&json, planned_text);
    str_puts(&json, "\",\"data\":{\"text\":\"");
    json_escape(&json, local_text);
    str_puts(&json, "\",\"name\":\"");
    json_escape(&json, timer->name);
    str_puts(&json, "\",\"schedule\":\"");
    json_escape(&json, timer->schedule != NULL ? timer->schedule : "");
    str_puts(&json, "\",\"planned\":\"");
    json_escape(&json, planned_text);
    str_puts(&json, "\",\"fired\":\"");
    json_escape(&json, fired_text);
    str_printf(&json, "\",\"late_sec\":%lld,\"seq\":%llu,\"host\":\"", late,
               timer->seq);
    json_escape(&json, host);
    str_puts(&json, "\"}}");
    return json.s;
}

static size_t discard_response(char *data, size_t size, size_t count, void *userdata) {
    (void)data;
    (void)userdata;
    return size * count;
}

static int publish(const Config *config, const char *json) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        log_line("publish failed: cannot initialise libcurl");
        return -1;
    }

    str_t url;
    str_init(&url);
    str_puts(&url, config->url);
    while (url.len > 0 && url.s[url.len - 1] == '/') {
        url.s[--url.len] = '\0';
    }
    str_puts(&url, "/events");

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

    curl_easy_setopt(curl, CURLOPT_URL, url.s);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    int result = -1;
    if (code != CURLE_OK) {
        log_line("publish failed: %s", curl_easy_strerror(code));
    } else if (status == 200 || status == 201) {
        result = 0;
    } else {
        log_line("publish rejected with HTTP %ld", status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    str_free(&url);
    return result;
}

// --------------------------------------------------------------------- init

static char *build_bus_conf(const BusSettings *bus) {
    str_t text;
    str_init(&text);
    str_puts(&text, "# ~/.config/yi/bus.conf -- Yi bus connection (shared)\n");
    str_puts(&text, "# Written by yi-timer init; keep it private (0600).\n");
    str_puts(&text, "\n");
    str_printf(&text, "scheme = %s\n", bus->scheme);
    str_printf(&text, "host = %s\n", bus->host);
    str_printf(&text, "port = %d\n", bus->port);
    str_printf(&text, "token = %s\n", bus->token);
    return text.s;
}

static char *build_timers_conf(const Config *config) {
    str_t text;
    str_init(&text);
    str_puts(&text, "# ~/.config/yi/timers.conf -- timers for yi-timer\n");
    str_puts(&text, "# Bus address and token live in ~/.config/yi/bus.conf.\n\n");
    str_printf(&text, "max_late = %d\n", config->max_late);
    for (size_t i = 0; i < config->count; i++) {
        const Timer *timer = &config->timers[i];
        str_printf(&text, "\n[timer:%s]\n", timer->name);
        str_printf(&text, "schedule = %s\n",
                   timer->schedule != NULL ? timer->schedule : "");
        str_printf(&text, "type = %s\n",
                   timer->type != NULL ? timer->type : "timer.fired");
        str_printf(&text, "source = %s\n",
                   timer->source != NULL ? timer->source : "timer");
    }
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

static int health_probe(const char *scheme, const char *host, int port,
                        char *reason, size_t reason_size) {
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
    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);

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
    curl_easy_cleanup(curl);
    free(endpoint);
    return result;
}

static void init_timers(Config *config, size_t *added) {
    if (!ask_yes_no("现在添加定时器? [Y/n]: ", true)) {
        return;
    }

    while (true) {
        char *name_answer = ask_line("名称 name: ");
        char *name = name_answer != NULL ? trim(name_answer) : NULL;
        if (name == NULL || *name == '\0') {
            free(name_answer);
            if (!ask_yes_no("继续添加? [y/N]: ", false)) {
                break;
            }
            continue;
        }
        if (config_find_timer(config, name) != NULL) {
            log_line("timer '%s' already exists; skipped", name);
            free(name_answer);
            if (!ask_yes_no("继续添加? [y/N]: ", false)) {
                break;
            }
            continue;
        }

        Timer *timer = config_add_timer(config, name);
        free(name_answer);

        bool valid = false;
        for (int attempt = 0; attempt < 3; attempt++) {
            char *schedule_answer = ask_line(
                "计划 schedule (every 30m | daily 09:00 | weekly fri 18:00): ");
            if (schedule_answer == NULL) {
                break;
            }
            char *value = trim(schedule_answer);
            if (*value != '\0') {
                char error[160];
                if (schedule_parse(value, &timer->parsed, error, sizeof error)) {
                    set_string(&timer->schedule, value);
                    timer->has_schedule = true;
                    valid = true;
                    free(schedule_answer);
                    break;
                }
                fprintf(stderr, "schedule 无效: %s\n", error);
            } else {
                fprintf(stderr, "schedule 不能为空\n");
            }
            free(schedule_answer);
        }

        if (!valid) {
            log_line("timer '%s' skipped: no valid schedule", timer->name);
            config_remove_last_timer(config);
            if (!ask_yes_no("继续添加? [y/N]: ", false)) {
                break;
            }
            continue;
        }

        char *type_answer = ask_line("类型 type [%s]: ", timer->type);
        if (type_answer != NULL) {
            char *value = trim(type_answer);
            if (*value != '\0') {
                set_string(&timer->type, value);
            }
            free(type_answer);
        }

        char *source_answer = ask_line("来源 source [%s]: ", timer->source);
        if (source_answer != NULL) {
            char *value = trim(source_answer);
            if (*value != '\0') {
                set_string(&timer->source, value);
            }
            free(source_answer);
        }

        (*added)++;
        if (!ask_yes_no("继续添加? [y/N]: ", false)) {
            break;
        }
    }
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
    size_t size = strlen(home) + 32;
    char *path = malloc(size);
    if (path == NULL) {
        return NULL;
    }
    snprintf(path, size, "%s/.local/bin/yi-timer", home);
    return path;
}

static char *systemd_unit_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && *xdg != '\0') {
        size_t size = strlen(xdg) + 48;
        char *path = malloc(size);
        snprintf(path, size, "%s/systemd/user/yi-timer.service", xdg);
        return path;
    }
    return expand_home("~/.config/systemd/user/yi-timer.service");
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
    str_puts(&text, "Description=Yi timers\n");
    str_puts(&text, "Documentation=https://github.com/gresces/Yi-Tools/blob/main/tools/yi-timer/README.md\n");
    str_puts(&text, "After=network-online.target\n");
    str_puts(&text, "Wants=network-online.target\n");
    str_puts(&text, "\n[Service]\n");
    str_puts(&text, "Type=simple\n");
    str_printf(&text, "ExecStart=%s\n", binary);
    str_puts(&text, "Restart=on-failure\n");
    str_puts(&text, "RestartSec=10s\n");
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
        fprintf(stderr, "yi-timer: cannot run %s: %s\n", argv[0],
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
                                 "yi-timer.service", NULL};
    char *const restart_argv[] = {"systemctl", "--user", "restart",
                                  "yi-timer.service", NULL};
    char *const *steps[] = {reload_argv, enable_argv, restart_argv};

    int result = 0;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        char *display = join_command(steps[i]);
        const int status = run_command(steps[i]);
        if (status != 0) {
            log_line("command failed (exit %d): %s", status,
                     display != NULL ? display : steps[i][0]);
            log_line("check the service with: journalctl --user -u yi-timer");
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

static int run_init(const char *timers_path) {
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
    const bool timers_present = stat(timers_path, &info) == 0;
    if (timers_present && config_load(&config, timers_path) != 0) {
        log_line("%s has errors; keeping the timers that parsed", timers_path);
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
    if (health_probe(bus.scheme, bus.host, bus.port, reason, sizeof reason) == 0) {
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

    size_t added = 0;
    init_timers(&config, &added);

    if (added > 0 || config.count > 0) {
        char *timers_text = build_timers_conf(&config);
        if (timers_text == NULL ||
            write_atomic_private(timers_path, timers_text,
                                 strlen(timers_text)) != 0) {
            free(timers_text);
            result = 1;
            goto done;
        }
        free(timers_text);
        printf("已写入 %s\n", timers_path);
        fflush(stdout);
    }
    if (config.count == 0) {
        printf("没有配置定时器;稍后编辑 %s 添加 [timer:名字] 段\n", timers_path);
        fflush(stdout);
    }

    bool deployed = false;
    if (ask_yes_no("部署为 systemd user 服务? [y/N]: ", false)) {
        if (deploy_service() != 0) {
            result = 1;
            goto done;
        }
        deployed = true;
    }

    if (deployed) {
        printf("查看日志: journalctl --user -u yi-timer -f\n");
    } else {
        printf("下一步: systemctl --user enable --now yi-timer\n");
    }
    fflush(stdout);

done:
    config_free(&config);
    bus_free(&bus);
    free(bus_path);
    curl_global_cleanup();
    return result;
}

// -------------------------------------------------------------------- runtime

static volatile sig_atomic_t g_stop = 0;

static void handle_stop(int signal_number) {
    (void)signal_number;
    g_stop = 1;
}

static void sleep_until(time_t deadline) {
    const time_t now = time(NULL);
    if (now >= deadline || g_stop) {
        return;
    }

    struct timespec target = {.tv_sec = deadline, .tv_nsec = 0};
    const int result =
        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &target, NULL);
    if (result == EINTR) {
        return;
    }
    if (result != 0) {
        log_line("clock_nanosleep failed: %s", strerror(result));
        struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&pause, NULL);
        return;
    }

    // Some environments return immediately from an absolute realtime sleep.
    // Waiting out the remainder relatively keeps the process from busy-looping.
    const time_t remaining = deadline - time(NULL);
    if (remaining > 0) {
        struct timespec pause = {.tv_sec = remaining, .tv_nsec = 0};
        (void)nanosleep(&pause, NULL);
    }
}

static void run_due_timers(Config *config, time_t now, const char *host) {
    for (size_t i = 0; i < config->count; i++) {
        Timer *timer = &config->timers[i];
        while (timer->due <= now) {
            const long long late = (long long)now - (long long)timer->due;
            if (config->max_late == 0 || late <= config->max_late) {
                timer->seq++;
                char *json = build_event_json(timer, timer->due, now, late, host);
                if (json != NULL) {
                    (void)publish(config, json);
                    free(json);
                } else {
                    log_line("%s: cannot build event", timer->name);
                }
            } else {
                log_line("%s: skipped occurrence (late %llds > max_late %ds)",
                         timer->name, late, config->max_late);
            }

            if (g_stop) {
                return;
            }
            const time_t next = next_after(&timer->parsed, timer->due);
            if (next == (time_t)-1) {
                log_line("%s: cannot compute the next trigger; retrying in %ds",
                         timer->name, MAX_SLEEP);
                timer->due = now + MAX_SLEEP;
                break;
            }
            timer->due = next;
        }
    }
}

static void usage(FILE *out) {
    fprintf(out,
            "yi-timer %s - publish calendar-timer events to Yi\n"
            "\n"
            "Usage: yi-timer [options]\n"
            "       yi-timer init\n"
            "\n"
            "Commands:\n"
            "  init                write bus.conf and timers.conf, optionally install a systemd user service\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH   config file (default ~/.config/yi/timers.conf)\n"
            "      --check         print the effective config and next triggers, then exit\n"
            "  -V, --version       print version and exit\n"
            "  -h, --help          print this help and exit\n",
            VERSION);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    bool check = false;
    bool initialise = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("yi-timer %s\n", VERSION);
            return 0;
        }
        if (strcmp(arg, "init") == 0) {
            initialise = true;
        } else if (strcmp(arg, "--check") == 0) {
            check = true;
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "yi-timer: %s needs a value\n", arg);
                return 2;
            }
            config_path = argv[++i];
        } else {
            fprintf(stderr, "yi-timer: unknown option %s\n", arg);
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
    // A missing config means "no timers yet": keep running and pick the file
    // up on the next reload. --check also works on machines without a config.
    if (!config_missing) {
        if (config_load(&config, config_path) != 0) {
            log_line("config %s has errors", config_path);
            free(owned_path);
            config_free(&config);
            return 1;
        }
    } else if (!check) {
        log_line("no timers in %s yet (waiting)", config_path);
    }
    config_apply_bus(&config);

    if (check) {
        config_print(&config);
        free(owned_path);
        config_free(&config);
        return 0;
    }

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_line("libcurl initialisation failed");
        free(owned_path);
        config_free(&config);
        return 1;
    }

    char host[256];
    memset(host, 0, sizeof host);
    if (gethostname(host, sizeof host - 1) != 0) {
        host[0] = '\0';
    }

    config_schedule_initial(&config, time(NULL));
    log_line("%s, %zu timer(s), max_late %ds -> %s", VERSION, config.count,
             config.max_late, config.url);

    while (!g_stop) {
        const time_t now = time(NULL);
        run_due_timers(&config, now, host);
        if (g_stop) {
            break;
        }

        time_t wake = time(NULL) + MAX_SLEEP;
        for (size_t i = 0; i < config.count; i++) {
            if (config.timers[i].due < wake) {
                wake = config.timers[i].due;
            }
        }
        sleep_until(wake);
        if (g_stop) {
            break;
        }

        // Re-read every wake so adding, changing or deleting a timer does not
        // require a restart. A broken edit keeps the previous config alive.
        Config fresh;
        config_defaults(&fresh);
        if (config_load(&fresh, config_path) != 0) {
            log_line("config reload failed; keeping previous settings");
            config_free(&fresh);
            continue;
        }
        config_apply_bus(&fresh);
        config_merge(&config, &fresh, time(NULL));
    }

    curl_global_cleanup();
    free(owned_path);
    config_free(&config);
    log_line("stopped");
    return 0;
}
