// yi-mail — watch several IMAP mailboxes and publish mail.received
// events to Yi.
//
// Design notes:
//   * One process, accounts polled sequentially every `interval` seconds.
//   * Newness is tracked with the per-folder IMAP UID cursor, not \Seen, so
//     mail read elsewhere still counts as new. UIDVALIDITY is stored next to
//     the cursor and a change resets the baseline instead of replaying mail.
//   * Event ids are derived from (source, folder, uidvalidity, uid), so a
//     crash between publish and state write republishes the same id and Yi
//     deduplicates it.
//   * First sight of a folder only records a baseline; set `backfill = true`
//     to publish the newest `max_fetch` messages instead.
//
// Dependencies: libc, OpenSSL, libcurl. iconv comes from glibc.

#define _GNU_SOURCE

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define IMAP_LINE_MAX (1u << 20)

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
    fputs("yi-mail: ", stderr);
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

static bool parse_bool(const char *value, bool fallback) {
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 ||
        strcmp(value, "1") == 0 || strcasecmp(value, "on") == 0) {
        return true;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 ||
        strcmp(value, "0") == 0 || strcasecmp(value, "off") == 0) {
        return false;
    }
    return fallback;
}

static bool parse_port_value(const char *text, int *out) {
    errno = 0;
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }
    *out = (int)value;
    return true;
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
        snprintf(path, size, "%s/yi/mail.conf", xdg);
        return path;
    }
    return expand_home("~/.config/yi/mail.conf");
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

static char *default_state_path(void) {
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg != NULL && *xdg != '\0') {
        size_t size = strlen(xdg) + 40;
        char *path = malloc(size);
        snprintf(path, size, "%s/yi-mail/state.tsv", xdg);
        return path;
    }
    return expand_home("~/.local/state/yi-mail/state.tsv");
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

// ------------------------------------------------------------------- config

typedef enum { TLS_NONE = 0, TLS_IMPLICIT = 1, TLS_STARTTLS = 2 } tls_mode_t;

typedef struct {
    char *name;
    char *host;
    int port;
    tls_mode_t tls;
    bool verify;
    char *user;
    char *password;
    char *source;
    char **folders;
    size_t folder_count;
} Account;

typedef struct {
    char *url;
    char *token;
    char *state_path;
    int interval;
    int max_fetch;
    bool backfill;
    bool verbose;
    bool legacy_url;
    bool legacy_token;
    Account *accounts;
    size_t count;
    size_t cap;
} Config;

typedef struct {
    char *scheme;
    char *host;
    int port;
    char *token;
    char *url;
} BusSettings;

static void account_free(Account *account) {
    free(account->name);
    free(account->host);
    free(account->user);
    free(account->password);
    free(account->source);
    for (size_t i = 0; i < account->folder_count; i++) {
        free(account->folders[i]);
    }
    free(account->folders);
    memset(account, 0, sizeof *account);
}

static void config_free(Config *config) {
    free(config->url);
    free(config->token);
    free(config->state_path);
    for (size_t i = 0; i < config->count; i++) {
        account_free(&config->accounts[i]);
    }
    free(config->accounts);
}

static void config_defaults(Config *config) {
    memset(config, 0, sizeof *config);
    config->url = xstrdup("http://127.0.0.1:9092");
    config->token = xstrdup("");
    config->state_path = default_state_path();
    config->interval = 60;
    config->max_fetch = 50;
    config->backfill = false;
}

static void set_string(char **slot, const char *value) {
    free(*slot);
    *slot = xstrdup(value);
}

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

// Fills scheme/host/port from the optional `url` override. Callers use this
// when they need the individual parts, as `--check` and publish() can keep the
// raw url.
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

static void config_remove_last_account(Config *config) {
    if (config->count == 0) {
        return;
    }
    config->count--;
    account_free(&config->accounts[config->count]);
}

static Account *config_add_account(Config *config, const char *name) {
    if (config->count == config->cap) {
        size_t cap = config->cap != 0 ? config->cap * 2 : 4;
        Account *grown = realloc(config->accounts, cap * sizeof *grown);
        if (grown == NULL) {
            log_line("out of memory");
            exit(1);
        }
        config->accounts = grown;
        config->cap = cap;
    }
    Account *account = &config->accounts[config->count++];
    memset(account, 0, sizeof *account);
    account->name = xstrdup(name);
    account->port = 0;
    account->tls = TLS_IMPLICIT;
    account->verify = true;
    account->source = xstrdup(name);
    account->folders = malloc(sizeof(char *));
    account->folders[0] = xstrdup("INBOX");
    account->folder_count = 1;
    return account;
}

static void account_set_folders(Account *account, const char *value) {
    for (size_t i = 0; i < account->folder_count; i++) {
        free(account->folders[i]);
    }
    free(account->folders);
    account->folders = NULL;
    account->folder_count = 0;

    char *copy = xstrdup(value);
    char *cursor = copy;
    while (cursor != NULL) {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        char *item = trim(cursor);
        if (*item != '\0') {
            char **grown = realloc(account->folders,
                                   (account->folder_count + 1) * sizeof(char *));
            if (grown == NULL) {
                log_line("out of memory");
                exit(1);
            }
            account->folders = grown;
            account->folders[account->folder_count++] = xstrdup(item);
        }
        cursor = comma != NULL ? comma + 1 : NULL;
    }
    free(copy);

    if (account->folder_count == 0) {
        account->folders = malloc(sizeof(char *));
        account->folders[0] = xstrdup("INBOX");
        account->folder_count = 1;
    }
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
    Account *current = NULL;
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
            if (colon == NULL || strncasecmp(name, "account", 7) != 0) {
                log_line("%s:%d: unknown section [%s]", path, line_number, name);
                problems++;
                current = NULL;
                continue;
            }
            *colon = '\0';
            char *label = trim(colon + 1);
            if (strcasecmp(trim(name), "account") != 0 || *label == '\0') {
                log_line("%s:%d: account section needs a name", path, line_number);
                problems++;
                current = NULL;
                continue;
            }
            current = config_add_account(config, label);
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
            if (strcasecmp(key, "url") == 0) {
                set_string(&config->url, value);
                config->legacy_url = true;
            } else if (strcasecmp(key, "token") == 0) {
                set_string(&config->token, value);
                config->legacy_token = true;
            } else if (strcasecmp(key, "state") == 0) {
                free(config->state_path);
                config->state_path = expand_home(value);
            } else if (strcasecmp(key, "interval") == 0) {
                config->interval = atoi(value);
            } else if (strcasecmp(key, "max_fetch") == 0) {
                config->max_fetch = atoi(value);
            } else if (strcasecmp(key, "backfill") == 0) {
                config->backfill = parse_bool(value, config->backfill);
            } else if (strcasecmp(key, "verbose") == 0) {
                config->verbose = parse_bool(value, config->verbose);
            } else {
                log_line("%s:%d: unknown global key '%s'", path, line_number, key);
                problems++;
            }
            continue;
        }

        if (strcasecmp(key, "host") == 0) {
            set_string(&current->host, value);
        } else if (strcasecmp(key, "port") == 0) {
            current->port = atoi(value);
        } else if (strcasecmp(key, "tls") == 0) {
            if (strcasecmp(value, "implicit") == 0 || strcasecmp(value, "ssl") == 0) {
                current->tls = TLS_IMPLICIT;
            } else if (strcasecmp(value, "starttls") == 0) {
                current->tls = TLS_STARTTLS;
            } else if (strcasecmp(value, "none") == 0 || strcasecmp(value, "plain") == 0) {
                current->tls = TLS_NONE;
            } else {
                log_line("%s:%d: tls must be implicit, starttls or none",
                         path, line_number);
                problems++;
            }
        } else if (strcasecmp(key, "tls_verify") == 0) {
            current->verify = parse_bool(value, current->verify);
        } else if (strcasecmp(key, "user") == 0) {
            set_string(&current->user, value);
        } else if (strcasecmp(key, "password") == 0) {
            set_string(&current->password, value);
        } else if (strcasecmp(key, "source") == 0) {
            set_string(&current->source, value);
        } else if (strcasecmp(key, "folders") == 0) {
            account_set_folders(current, value);
        } else {
            log_line("%s:%d: unknown account key '%s'", path, line_number, key);
            problems++;
        }
    }

    free(line);
    fclose(file);

    if (config->interval < 5) {
        log_line("interval below 5 seconds; using 5");
        config->interval = 5;
    }
    if (config->max_fetch < 1) {
        config->max_fetch = 1;
    }
    if (config->max_fetch > 500) {
        config->max_fetch = 500;
    }

    for (size_t i = 0; i < config->count; i++) {
        Account *account = &config->accounts[i];
        if (account->port == 0) {
            account->port = account->tls == TLS_IMPLICIT ? 993 : 143;
        }
        if (account->host == NULL || account->user == NULL ||
            account->password == NULL) {
            log_line("account '%s' needs host, user and password", account->name);
            problems++;
        }
    }

    return problems == 0 ? 0 : -1;
}

static bool g_legacy_bus_warned = false;

// Precedence: YI_URL/YI_TOKEN, then bus.conf, then url/token left in mail.conf
// for compatibility, then the built-in defaults already in Config.
static void config_apply_bus(Config *config) {
    char *bus_path = default_bus_path();
    BusSettings bus;
    bus_defaults(&bus);
    const bool bus_present = bus_load_file(&bus, bus_path);

    if (bus_present) {
        char *url = bus_effective_url(&bus);
        set_string(&config->url, url);
        free(url);
        set_string(&config->token, bus.token);
    } else if (config->legacy_url || config->legacy_token) {
        if (!g_legacy_bus_warned) {
            g_legacy_bus_warned = true;
            log_line("deprecated: move url/token to ~/.config/yi/bus.conf (run init)");
        }
        // config->url and config->token already hold the old values.
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
    printf("state     = %s\n", config->state_path);
    printf("interval  = %d\n", config->interval);
    printf("max_fetch = %d\n", config->max_fetch);
    printf("backfill  = %s\n", config->backfill ? "true" : "false");
    printf("accounts  = %zu\n", config->count);
    for (size_t i = 0; i < config->count; i++) {
        const Account *account = &config->accounts[i];
        printf("  [%s] %s:%d tls=%s verify=%s user=%s source=%s folders=",
               account->name, account->host, account->port,
               account->tls == TLS_IMPLICIT ? "implicit"
                                            : (account->tls == TLS_STARTTLS ? "starttls" : "none"),
               account->verify ? "true" : "false", account->user, account->source);
        for (size_t f = 0; f < account->folder_count; f++) {
            printf("%s%s", f == 0 ? "" : ",", account->folders[f]);
        }
        printf("\n");
    }
}

// -------------------------------------------------------------------- state

typedef struct {
    char *account;
    char *folder;
    unsigned long long uidvalidity;
    unsigned long long last_uid;
} StateEntry;

typedef struct {
    StateEntry *items;
    size_t count;
    size_t cap;
    char *path;
} State;

static StateEntry *state_find(State *state, const char *account, const char *folder) {
    for (size_t i = 0; i < state->count; i++) {
        if (strcmp(state->items[i].account, account) == 0 &&
            strcmp(state->items[i].folder, folder) == 0) {
            return &state->items[i];
        }
    }
    return NULL;
}

static StateEntry *state_set(State *state, const char *account, const char *folder,
                             unsigned long long uidvalidity,
                             unsigned long long last_uid) {
    StateEntry *entry = state_find(state, account, folder);
    if (entry == NULL) {
        if (state->count == state->cap) {
            size_t cap = state->cap != 0 ? state->cap * 2 : 4;
            StateEntry *grown = realloc(state->items, cap * sizeof *grown);
            if (grown == NULL) {
                return NULL;
            }
            state->items = grown;
            state->cap = cap;
        }
        entry = &state->items[state->count++];
        memset(entry, 0, sizeof *entry);
        entry->account = xstrdup(account);
        entry->folder = xstrdup(folder);
    }
    entry->uidvalidity = uidvalidity;
    entry->last_uid = last_uid;
    return entry;
}

static void state_load(State *state, const char *path) {
    memset(state, 0, sizeof *state);
    state->path = xstrdup(path);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        char *fields[4];
        int found = 0;
        char *cursor = trim(line);
        char *start = cursor;
        for (char *p = cursor;; p++) {
            if (*p == '\t' || *p == '\0') {
                bool done = *p == '\0';
                *p = '\0';
                if (found < 4) {
                    fields[found++] = start;
                }
                start = p + 1;
                if (done) {
                    break;
                }
            }
        }
        if (found != 4 || *fields[0] == '\0' || *fields[1] == '\0') {
            continue;
        }
        state_set(state, fields[0], fields[1],
                  strtoull(fields[2], NULL, 10),
                  strtoull(fields[3], NULL, 10));
    }
    free(line);
    fclose(file);
}

static int state_save(State *state) {
    if (mkdir_p_for(state->path) != 0) {
        log_line("cannot create state directory for %s", state->path);
        return -1;
    }
    size_t tmp_size = strlen(state->path) + 8;
    char *tmp = malloc(tmp_size);
    if (tmp == NULL) {
        return -1;
    }
    snprintf(tmp, tmp_size, "%s.tmp", state->path);

    FILE *file = fopen(tmp, "w");
    if (file == NULL) {
        log_line("cannot write %s: %s", tmp, strerror(errno));
        free(tmp);
        return -1;
    }
    for (size_t i = 0; i < state->count; i++) {
        fprintf(file, "%s\t%s\t%llu\t%llu\n", state->items[i].account,
                state->items[i].folder, state->items[i].uidvalidity,
                state->items[i].last_uid);
    }
    fflush(file);
    fclose(file);
    if (rename(tmp, state->path) != 0) {
        log_line("cannot replace %s: %s", state->path, strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static void state_free(State *state) {
    for (size_t i = 0; i < state->count; i++) {
        free(state->items[i].account);
        free(state->items[i].folder);
    }
    free(state->items);
    free(state->path);
    memset(state, 0, sizeof *state);
}

// --------------------------------------------------------------------- IMAP

typedef struct {
    str_t line;
    str_t literal;
    bool has_literal;
} ImapResponse;

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    int tag;
    char buffer[4096];
    size_t offset;
    size_t size;
    char last_error[256];
} Imap;

typedef void (*imap_line_cb)(const ImapResponse *response, void *userdata);

static int imap_fill(Imap *imap) {
    if (imap->offset < imap->size) {
        return 0;
    }
    ssize_t got;
    if (imap->ssl != NULL) {
        got = SSL_read(imap->ssl, imap->buffer, (int)sizeof imap->buffer);
        if (got <= 0) {
            int error = SSL_get_error(imap->ssl, (int)got);
            snprintf(imap->last_error, sizeof imap->last_error,
                     "SSL_read failed (%d)", error);
        }
    } else {
        got = recv(imap->fd, imap->buffer, sizeof imap->buffer, 0);
        if (got <= 0) {
            snprintf(imap->last_error, sizeof imap->last_error,
                     "recv failed: %s", got == 0 ? "closed" : strerror(errno));
        }
    }
    if (got <= 0) {
        return -1;
    }
    imap->offset = 0;
    imap->size = (size_t)got;
    return 0;
}

static int imap_getc(Imap *imap) {
    if (imap_fill(imap) < 0) {
        return -1;
    }
    return (unsigned char)imap->buffer[imap->offset++];
}

static int imap_write(Imap *imap, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t wrote;
        if (imap->ssl != NULL) {
            wrote = SSL_write(imap->ssl, data + sent, (int)(size - sent));
        } else {
            wrote = send(imap->fd, data + sent, size - sent, MSG_NOSIGNAL);
        }
        if (wrote <= 0) {
            return -1;
        }
        sent += (size_t)wrote;
    }
    return 0;
}

static int imap_read_line(Imap *imap, str_t *out) {
    while (true) {
        int c = imap_getc(imap);
        if (c < 0) {
            return -1;
        }
        if (c == '\n') {
            break;
        }
        if (str_putc(out, (char)c) < 0) {
            return -1;
        }
        if (out->len > IMAP_LINE_MAX) {
            snprintf(imap->last_error, sizeof imap->last_error, "IMAP line too long");
            return -1;
        }
    }
    if (out->len > 0 && out->s[out->len - 1] == '\r') {
        out->len--;
        out->s[out->len] = '\0';
    }
    return 0;
}

static int imap_read_exact(Imap *imap, size_t size, str_t *out) {
    while (size > 0) {
        if (imap_fill(imap) < 0) {
            return -1;
        }
        size_t available = imap->size - imap->offset;
        size_t take = size < available ? size : available;
        if (str_append(out, imap->buffer + imap->offset, take) < 0) {
            return -1;
        }
        imap->offset += take;
        size -= take;
    }
    return 0;
}

// Reads one logical response: a line, plus the body of a trailing {n}
// literal and the continuation line that follows it.
static int imap_next(Imap *imap, ImapResponse *response) {
    str_reset(&response->line);
    str_reset(&response->literal);
    response->has_literal = false;

    if (imap_read_line(imap, &response->line) < 0) {
        return -1;
    }

    size_t length = response->line.len;
    if (length < 3 || response->line.s[length - 1] != '}') {
        return 0;
    }

    const char *text = response->line.s;
    size_t close = length - 1;
    size_t open = close;
    while (open > 0 && text[open - 1] != '{') {
        open--;
    }
    if (open == 0 || text[open - 1] != '{') {
        return 0;
    }

    size_t end = close;
    if (end > open && text[end - 1] == '+') {
        end--;
    }
    if (end <= open) {
        return 0;
    }
    for (size_t i = open; i < end; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return 0;
        }
    }

    char number[32];
    size_t digits = end - open;
    if (digits >= sizeof number) {
        return 0;
    }
    memcpy(number, text + open, digits);
    number[digits] = '\0';
    const unsigned long long literal_size = strtoull(number, NULL, 10);
    if (literal_size > IMAP_LINE_MAX) {
        snprintf(imap->last_error, sizeof imap->last_error,
                 "IMAP literal too large (%llu bytes)", literal_size);
        return -1;
    }

    if (imap_read_exact(imap, (size_t)literal_size, &response->literal) < 0) {
        return -1;
    }
    response->has_literal = true;

    str_t continuation;
    str_init(&continuation);
    int result = imap_read_line(imap, &continuation);
    if (result == 0) {
        str_putc(&response->line, ' ');
        str_append(&response->line, continuation.s, continuation.len);
    }
    str_free(&continuation);
    return result;
}

static char *imap_command(Imap *imap, const char *format, ...) {
    str_t line;
    str_init(&line);
    imap->tag++;
    str_printf(&line, "A%04d ", imap->tag);

    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int size = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (size >= 0) {
        if (str_reserve(&line, (size_t)size) == 0) {
            vsnprintf(line.s + line.len, (size_t)size + 1, format, copy);
            line.len += (size_t)size;
        }
    }
    va_end(copy);
    str_puts(&line, "\r\n");

    char *tag = malloc(8);
    snprintf(tag, 8, "A%04d", imap->tag);
    if (imap_write(imap, line.s, line.len) != 0) {
        free(tag);
        str_free(&line);
        return NULL;
    }
    str_free(&line);
    return tag;
}

static int imap_wait_ex(Imap *imap, const char *tag, imap_line_cb callback,
                        void *userdata, bool log_errors) {
    ImapResponse response;
    str_init(&response.line);
    str_init(&response.literal);

    const size_t tag_len = strlen(tag);
    int result = -1;
    while (imap_next(imap, &response) == 0) {
        const char *line = response.line.s != NULL ? response.line.s : "";
        if (strncmp(line, tag, tag_len) == 0 && line[tag_len] == ' ') {
            result = strncasecmp(line + tag_len + 1, "OK", 2) == 0 ? 0 : -1;
            if (result != 0 && log_errors) {
                log_line("IMAP %s: %s", tag, line);
            }
            break;
        }
        if (callback != NULL) {
            callback(&response, userdata);
        }
    }
    str_free(&response.line);
    str_free(&response.literal);
    return result;
}

static int imap_wait(Imap *imap, const char *tag, imap_line_cb callback,
                     void *userdata) {
    return imap_wait_ex(imap, tag, callback, userdata, true);
}

static void imap_close(Imap *imap) {
    if (imap->ssl != NULL) {
        SSL_shutdown(imap->ssl);
        SSL_free(imap->ssl);
        imap->ssl = NULL;
    }
    if (imap->ctx != NULL) {
        SSL_CTX_free(imap->ctx);
        imap->ctx = NULL;
    }
    if (imap->fd >= 0) {
        close(imap->fd);
        imap->fd = -1;
    }
}

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char service[16];
    snprintf(service, sizeof service, "%d", port);

    struct addrinfo *results = NULL;
    if (getaddrinfo(host, service, &hints, &results) != 0) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *it = results; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);

    if (fd >= 0) {
        struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    }
    return fd;
}

static int tls_start(Imap *imap, const Account *account) {
    imap->ctx = SSL_CTX_new(TLS_client_method());
    if (imap->ctx == NULL) {
        return -1;
    }
    SSL_CTX_set_min_proto_version(imap->ctx, TLS1_2_VERSION);
    if (account->verify) {
        SSL_CTX_set_default_verify_paths(imap->ctx);
        SSL_CTX_set_verify(imap->ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(imap->ctx, SSL_VERIFY_NONE, NULL);
    }

    imap->ssl = SSL_new(imap->ctx);
    if (imap->ssl == NULL) {
        return -1;
    }
    SSL_set_fd(imap->ssl, imap->fd);
    if (account->verify) {
        X509_VERIFY_PARAM *param = SSL_get0_param(imap->ssl);
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        X509_VERIFY_PARAM_set1_host(param, account->host, 0);
    }
    SSL_set_tlsext_host_name(imap->ssl, account->host);
    if (SSL_connect(imap->ssl) != 1) {
        unsigned long error = ERR_get_error();
        char text[256];
        ERR_error_string_n(error, text, sizeof text);
        log_line("%s: TLS handshake failed: %s", account->name,
                 error != 0 ? text : "verify failed");
        return -1;
    }
    return 0;
}

static int imap_open(Imap *imap, const Account *account) {
    memset(imap, 0, sizeof *imap);
    imap->fd = -1;

    imap->fd = tcp_connect(account->host, account->port);
    if (imap->fd < 0) {
        log_line("%s: cannot connect to %s:%d", account->name, account->host,
                 account->port);
        return -1;
    }

    if (account->tls == TLS_IMPLICIT && tls_start(imap, account) != 0) {
        imap_close(imap);
        return -1;
    }

    ImapResponse greeting;
    str_init(&greeting.line);
    str_init(&greeting.literal);
    int result = imap_next(imap, &greeting);
    const char *text = result == 0 && greeting.line.s != NULL ? greeting.line.s : "";
    if (result != 0 || strncmp(text, "* OK", 4) != 0) {
        if (strncmp(text, "* PREAUTH", 9) != 0) {
            log_line("%s: bad greeting: %s", account->name, text);
            str_free(&greeting.line);
            str_free(&greeting.literal);
            imap_close(imap);
            return -1;
        }
    }
    str_free(&greeting.line);
    str_free(&greeting.literal);

    if (account->tls == TLS_STARTTLS) {
        char *tag = imap_command(imap, "STARTTLS");
        if (tag == NULL) {
            imap_close(imap);
            return -1;
        }
        const int started = imap_wait(imap, tag, NULL, NULL);
        free(tag);
        if (started != 0 || tls_start(imap, account) != 0) {
            log_line("%s: STARTTLS failed", account->name);
            imap_close(imap);
            return -1;
        }
        imap->offset = imap->size = 0;
    }
    return 0;
}

static void imap_quote(str_t *out, const char *text) {
    str_putc(out, '"');
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            str_putc(out, '\\');
        }
        str_putc(out, *p);
    }
    str_putc(out, '"');
}

// --------------------------------------------------------------- RFC 2047

static void append_base64_decoded(str_t *out, const char *text, size_t size) {
    str_t padded;
    str_init(&padded);
    str_append(&padded, text, size);
    while (padded.len % 4 != 0) {
        str_putc(&padded, '=');
    }
    size_t capacity = padded.len / 4 * 3 + 4;
    unsigned char *decoded = malloc(capacity);
    if (decoded == NULL) {
        str_free(&padded);
        return;
    }
    const int written = EVP_DecodeBlock(decoded, (const unsigned char *)padded.s,
                                        (int)padded.len);
    if (written > 0) {
        size_t real = (size_t)written;
        for (size_t i = padded.len; i > 0; i--) {
            if (padded.s[i - 1] == '=') {
                real--;
            } else {
                break;
            }
        }
        str_append(out, (const char *)decoded, real);
    }
    free(decoded);
    str_free(&padded);
}

static void append_q_decoded(str_t *out, const char *text, size_t size) {
    for (size_t i = 0; i < size; i++) {
        char c = text[i];
        if (c == '_') {
            str_putc(out, ' ');
        } else if (c == '=' && i + 2 < size && isxdigit((unsigned char)text[i + 1]) &&
                   isxdigit((unsigned char)text[i + 2])) {
            char hex[3] = {text[i + 1], text[i + 2], '\0'};
            str_putc(out, (char)strtol(hex, NULL, 16));
            i += 2;
        } else {
            str_putc(out, c);
        }
    }
}

static void append_as_utf8(str_t *out, const char *charset, const char *data,
                           size_t size) {
    if (strcasecmp(charset, "utf-8") == 0 || strcasecmp(charset, "us-ascii") == 0 ||
        strcasecmp(charset, "ascii") == 0) {
        str_append(out, data, size);
        return;
    }

    iconv_t converter = iconv_open("UTF-8", charset);
    if (converter == (iconv_t)-1) {
        str_append(out, data, size);
        return;
    }

    size_t capacity = size * 4 + 16;
    char *converted = malloc(capacity);
    if (converted == NULL) {
        iconv_close(converter);
        return;
    }
    char *input = (char *)data;
    size_t input_left = size;
    char *output = converted;
    size_t output_left = capacity - 1;
    if (iconv(converter, &input, &input_left, &output, &output_left) != (size_t)-1) {
        str_append(out, converted, capacity - 1 - output_left);
    } else {
        // Fall back to the raw bytes for undecodable sequences.
        str_append(out, data, size);
    }
    free(converted);
    iconv_close(converter);
}

static char *decode_header(const char *value) {
    str_t out;
    str_init(&out);

    const char *p = value;
    while (*p != '\0') {
        if (p[0] == '=' && p[1] == '?') {
            const char *charset_end = strchr(p + 2, '?');
            const char *encoding_end =
                charset_end != NULL ? strchr(charset_end + 1, '?') : NULL;
            const char *text_end =
                encoding_end != NULL ? strstr(encoding_end + 1, "?=") : NULL;
            if (charset_end == NULL || encoding_end == NULL || text_end == NULL ||
                encoding_end != charset_end + 2) {
                str_putc(&out, *p++);
                continue;
            }

            char charset[64];
            size_t charset_len = (size_t)(charset_end - (p + 2));
            if (charset_len >= sizeof charset) {
                charset_len = sizeof charset - 1;
            }
            memcpy(charset, p + 2, charset_len);
            charset[charset_len] = '\0';

            const char encoding = (char)toupper((unsigned char)charset_end[1]);
            const char *text = encoding_end + 1;
            const size_t text_len = (size_t)(text_end - text);

            str_t decoded;
            str_init(&decoded);
            if (encoding == 'B') {
                append_base64_decoded(&decoded, text, text_len);
            } else if (encoding == 'Q') {
                append_q_decoded(&decoded, text, text_len);
            } else {
                str_append(&decoded, text, text_len);
            }
            append_as_utf8(&out, charset, decoded.s != NULL ? decoded.s : "",
                           decoded.len);
            str_free(&decoded);

            p = text_end + 2;
            // Adjacent encoded words: the whitespace between them is not text.
            const char *next = p;
            while (*next == ' ' || *next == '\t') {
                next++;
            }
            if (next[0] == '=' && next[1] == '?') {
                p = next;
            }
            continue;
        }
        str_putc(&out, *p++);
    }

    if (out.s == NULL) {
        return xstrdup("");
    }
    return out.s;
}

// ------------------------------------------------------------------ headers

typedef struct {
    char *from;
    char *subject;
    char *date;
    char *message_id;
} Headers;

static void headers_free(Headers *headers) {
    free(headers->from);
    free(headers->subject);
    free(headers->date);
    free(headers->message_id);
    memset(headers, 0, sizeof *headers);
}

static void header_set(char **slot, const char *value) {
    free(*slot);
    *slot = decode_header(value);
}

static Headers parse_headers(const char *data, size_t size) {
    Headers headers;
    memset(&headers, 0, sizeof headers);

    char *copy = malloc(size + 1);
    if (copy == NULL) {
        return headers;
    }
    memcpy(copy, data, size);
    copy[size] = '\0';

    char *current_name = NULL;
    str_t current_value;
    str_init(&current_value);

    char *line = copy;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next = '\0';
            next++;
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\r') {
                line[len - 1] = '\0';
            }
        }

        if ((line[0] == ' ' || line[0] == '\t') && current_name != NULL) {
            str_putc(&current_value, ' ');
            str_puts(&current_value, trim(line));
        } else {
            if (current_name != NULL) {
                if (strcasecmp(current_name, "From") == 0) {
                    header_set(&headers.from, current_value.s);
                } else if (strcasecmp(current_name, "Subject") == 0) {
                    header_set(&headers.subject, current_value.s);
                } else if (strcasecmp(current_name, "Date") == 0) {
                    header_set(&headers.date, current_value.s);
                } else if (strcasecmp(current_name, "Message-ID") == 0) {
                    header_set(&headers.message_id, current_value.s);
                }
            }
            str_reset(&current_value);
            current_name = NULL;
            if (*line == '\0') {
                break;
            }
            char *colon = strchr(line, ':');
            if (colon != NULL) {
                *colon = '\0';
                current_name = trim(line);
                str_puts(&current_value, trim(colon + 1));
            }
        }
        line = next;
    }

    if (current_name != NULL) {
        if (strcasecmp(current_name, "From") == 0) {
            header_set(&headers.from, current_value.s);
        } else if (strcasecmp(current_name, "Subject") == 0) {
            header_set(&headers.subject, current_value.s);
        } else if (strcasecmp(current_name, "Date") == 0) {
            header_set(&headers.date, current_value.s);
        } else if (strcasecmp(current_name, "Message-ID") == 0) {
            header_set(&headers.message_id, current_value.s);
        }
    }
    str_free(&current_value);
    free(copy);

    if (headers.from == NULL) {
        headers.from = xstrdup("");
    }
    if (headers.subject == NULL) {
        headers.subject = xstrdup("");
    }
    if (headers.date == NULL) {
        headers.date = xstrdup("");
    }
    if (headers.message_id == NULL) {
        headers.message_id = xstrdup("");
    }
    return headers;
}

// Finds the first +HHMM / -HHMM / +HH:MM token. Looking at the last token is
// not enough: "Sun, 15 Nov 2026 16:30:06 +0800 (CST)" ends with a comment.
static long timezone_offset(const char *date) {
    for (const char *p = date; (p = strchr(p, ' ')) != NULL; p++) {
        if (*p != ' ') {
            continue;
        }
        const char *token = p + 1;
        if ((*token != '+' && *token != '-') ||
            !isdigit((unsigned char)token[1]) ||
            !isdigit((unsigned char)token[2])) {
            continue;
        }
        const int hours = (token[1] - '0') * 10 + (token[2] - '0');
        const char *rest = token + 3;
        if (*rest == ':') {
            rest++;
        }
        if (!isdigit((unsigned char)rest[0]) || !isdigit((unsigned char)rest[1])) {
            continue;
        }
        const int minutes = (rest[0] - '0') * 10 + (rest[1] - '0');
        const long offset = hours * 3600L + minutes * 60L;
        return *token == '-' ? -offset : offset;
    }
    return 0;
}

static time_t parse_rfc822(const char *date) {
    static const char *formats[] = {
        "%a, %d %b %Y %H:%M:%S",
        "%d %b %Y %H:%M:%S",
        "%a, %d %b %Y %H:%M",
        "%d %b %Y %H:%M",
        NULL,
    };
    for (size_t i = 0; formats[i] != NULL; i++) {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        if (strptime(date, formats[i], &tm) != NULL) {
            time_t value = timegm(&tm);
            if (value != (time_t)-1) {
                return value - timezone_offset(date);
            }
        }
    }
    return 0;
}

static void format_rfc3339(time_t value, char out[32]) {
    struct tm tm;
    gmtime_r(&value, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
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

static void event_id(const char *source, const char *folder,
                     unsigned long long uidvalidity, unsigned long long uid,
                     char out[33]) {
    char material[512];
    snprintf(material, sizeof material, "%s\x1f%s\x1f%llu\x1f%llu", source,
             folder, uidvalidity, uid);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(material, strlen(material), digest, &size, EVP_sha256(), NULL);
    for (unsigned int i = 0; i < 16 && i < size; i++) {
        sprintf(out + i * 2, "%02x", digest[i]);
    }
    out[32] = '\0';
}

static char *build_event_json(const Account *account, const char *folder,
                              unsigned long long uidvalidity,
                              unsigned long long uid, unsigned long long size,
                              const Headers *headers, time_t when) {
    char id[33];
    event_id(account->source, folder, uidvalidity, uid, id);

    char time_text[32];
    format_rfc3339(when, time_text);

    str_t json;
    str_init(&json);
    str_puts(&json, "{\"id\":\"");
    json_escape(&json, id);
    str_puts(&json, "\",\"source\":\"");
    json_escape(&json, account->source);
    str_puts(&json, "\",\"types\":[\"mail.received\"],\"time\":\"");
    json_escape(&json, time_text);
    str_puts(&json, "\",\"data\":{");

    str_puts(&json, "\"account\":\"");
    json_escape(&json, account->name);
    str_printf(&json, "\",\"folder\":\"");
    json_escape(&json, folder);
    str_printf(&json, "\",\"uid\":%llu,\"uidvalidity\":%llu,\"size\":%llu",
               uid, uidvalidity, size);

    str_puts(&json, ",\"from\":\"");
    json_escape(&json, headers->from);
    str_puts(&json, "\",\"subject\":\"");
    json_escape(&json, headers->subject);
    str_puts(&json, "\",\"date\":\"");
    json_escape(&json, headers->date);
    str_puts(&json, "\",\"message_id\":\"");
    json_escape(&json, headers->message_id);
    str_puts(&json, "\"}}");
    return json.s;
}

typedef struct {
    str_t body;
} HttpBody;

static size_t http_write(char *data, size_t size, size_t count, void *userdata) {
    HttpBody *body = userdata;
    str_append(&body->body, data, size * count);
    return size * count;
}

static int publish(const Config *config, const char *json) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return -1;
    }

    HttpBody body;
    str_init(&body.body);

    str_t url;
    str_init(&url);
    str_puts(&url, config->url);
    size_t len = url.len;
    while (len > 0 && url.s[len - 1] == '/') {
        url.s[--len] = '\0';
        url.len = len;
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
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
        log_line("publish rejected with HTTP %ld: %s", status,
                 body.body.s != NULL ? body.body.s : "");
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    str_free(&body.body);
    str_free(&url);
    return result;
}

// ------------------------------------------------------------------ polling

typedef struct {
    unsigned long long uid;
    unsigned long long size;
    Headers headers;
} Message;

typedef struct {
    Message *items;
    size_t count;
    size_t cap;
    bool has_uidvalidity;
    bool has_uidnext;
    unsigned long long uidvalidity;
    unsigned long long uidnext;
    int messages;
} FolderState;

static void folder_free(FolderState *folder) {
    for (size_t i = 0; i < folder->count; i++) {
        headers_free(&folder->items[i].headers);
    }
    free(folder->items);
    memset(folder, 0, sizeof *folder);
}

static void select_cb(const ImapResponse *response, void *userdata) {
    FolderState *state = userdata;
    const char *line = response->line.s != NULL ? response->line.s : "";

    const char *marker = strstr(line, "UIDVALIDITY");
    if (marker != NULL) {
        state->uidvalidity = strtoull(marker + strlen("UIDVALIDITY"), NULL, 10);
        state->has_uidvalidity = true;
    }
    marker = strstr(line, "UIDNEXT");
    if (marker != NULL) {
        state->uidnext = strtoull(marker + strlen("UIDNEXT"), NULL, 10);
        state->has_uidnext = true;
    }
    if (strstr(line, "EXISTS") != NULL) {
        state->messages = atoi(line + 2);
    }
}

static void fetch_cb(const ImapResponse *response, void *userdata) {
    FolderState *state = userdata;
    const char *line = response->line.s != NULL ? response->line.s : "";
    if (strstr(line, " FETCH ") == NULL) {
        return;
    }

    const char *uid_text = strstr(line, "UID ");
    if (uid_text == NULL) {
        return;
    }

    Message message;
    memset(&message, 0, sizeof message);
    message.uid = strtoull(uid_text + 4, NULL, 10);

    const char *size_text = strstr(line, "RFC822.SIZE ");
    if (size_text != NULL) {
        message.size = strtoull(size_text + strlen("RFC822.SIZE "), NULL, 10);
    }

    if (response->has_literal) {
        message.headers = parse_headers(response->literal.s, response->literal.len);
    } else {
        memset(&message.headers, 0, sizeof message.headers);
        message.headers.from = xstrdup("");
        message.headers.subject = xstrdup("");
        message.headers.date = xstrdup("");
        message.headers.message_id = xstrdup("");
    }

    if (state->count == state->cap) {
        size_t cap = state->cap != 0 ? state->cap * 2 : 8;
        Message *grown = realloc(state->items, cap * sizeof *grown);
        if (grown == NULL) {
            headers_free(&message.headers);
            return;
        }
        state->items = grown;
        state->cap = cap;
    }
    state->items[state->count++] = message;
}

static int imap_login(Imap *imap, const Account *account) {
    str_t user;
    str_t password;
    str_init(&user);
    str_init(&password);
    imap_quote(&user, account->user);
    imap_quote(&password, account->password);

    char *tag = imap_command(imap, "LOGIN %s %s", user.s, password.s);
    str_free(&user);
    str_free(&password);
    if (tag == NULL) {
        return -1;
    }
    const int result = imap_wait(imap, tag, NULL, NULL);
    free(tag);
    return result;
}

typedef struct {
    str_t capabilities;
} CapabilityCtx;

static void capability_cb(const ImapResponse *response, void *userdata) {
    static const char prefix[] = "* CAPABILITY";
    const size_t prefix_len = sizeof prefix - 1;
    CapabilityCtx *ctx = userdata;
    const char *line = response->line.s != NULL ? response->line.s : "";
    const size_t length = strlen(line);

    if (length >= prefix_len && strncasecmp(line, prefix, prefix_len) == 0) {
        str_append(&ctx->capabilities, line + prefix_len, length - prefix_len);
        str_putc(&ctx->capabilities, ' ');
    }
}

static bool imap_supports(Imap *imap, const char *capability) {
    CapabilityCtx ctx;
    str_init(&ctx.capabilities);

    char *tag = imap_command(imap, "CAPABILITY");
    if (tag == NULL) {
        str_free(&ctx.capabilities);
        return false;
    }
    imap_wait_ex(imap, tag, capability_cb, &ctx, false);
    free(tag);

    bool found = false;
    char *cursor = ctx.capabilities.s;
    const size_t wanted = strlen(capability);
    while (cursor != NULL && *cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        char *end = cursor;
        while (*end != '\0' && *end != ' ') {
            end++;
        }
        if ((size_t)(end - cursor) == wanted &&
            strncasecmp(cursor, capability, wanted) == 0) {
            found = true;
            break;
        }
        cursor = *end != '\0' ? end + 1 : NULL;
    }
    str_free(&ctx.capabilities);
    return found;
}

// RFC 2971. Netease (163/126) answers SELECT with "Unsafe Login" until the
// client identifies itself; the extension is only used when advertised.
static void imap_identify(Imap *imap) {
    if (!imap_supports(imap, "ID")) {
        return;
    }
    char *tag = imap_command(
        imap,
        "ID (\"name\" \"yi-mail\" \"version\" \"%s\" \"vendor\" \"yi\")",
        VERSION);
    if (tag != NULL) {
        imap_wait_ex(imap, tag, NULL, NULL, false);
        free(tag);
    }
}

static int imap_select(Imap *imap, const char *folder, FolderState *state) {
    str_t quoted;
    str_init(&quoted);
    imap_quote(&quoted, folder);

    char *tag = imap_command(imap, "SELECT %s", quoted.s);
    str_free(&quoted);
    if (tag == NULL) {
        return -1;
    }
    const int result = imap_wait(imap, tag, select_cb, state);
    free(tag);
    return result;
}

static int imap_fetch(Imap *imap, unsigned long long from, FolderState *state) {
    char *tag = imap_command(
        imap,
        "UID FETCH %llu:* (UID RFC822.SIZE BODY.PEEK[HEADER.FIELDS "
        "(FROM SUBJECT DATE MESSAGE-ID)])",
        from);
    if (tag == NULL) {
        return -1;
    }
    const int result = imap_wait(imap, tag, fetch_cb, state);
    free(tag);
    return result;
}

static void poll_folder(const Config *config, State *state, Imap *imap,
                        const Account *account, const char *folder) {
    FolderState mail;
    memset(&mail, 0, sizeof mail);

    if (imap_select(imap, folder, &mail) != 0) {
        log_line("%s/%s: SELECT failed", account->name, folder);
        folder_free(&mail);
        return;
    }
    if (!mail.has_uidvalidity || !mail.has_uidnext) {
        log_line("%s/%s: server did not report UIDVALIDITY/UIDNEXT", account->name,
                 folder);
        folder_free(&mail);
        return;
    }

    StateEntry *entry = state_find(state, account->name, folder);
    if (entry == NULL) {
        const unsigned long long newest = mail.uidnext > 0 ? mail.uidnext - 1 : 0;
        unsigned long long baseline = newest;
        if (config->backfill && mail.uidnext > 0) {
            const unsigned long long window = (unsigned long long)config->max_fetch;
            baseline = newest > window ? newest - window : 0;
        }
        entry = state_set(state, account->name, folder, mail.uidvalidity, baseline);
        if (entry == NULL) {
            folder_free(&mail);
            return;
        }
        state_save(state);
        if (config->verbose) {
            log_line("%s/%s: new folder, baseline uid %llu", account->name, folder,
                     baseline);
        }
        if (!config->backfill) {
            folder_free(&mail);
            return;
        }
        // backfill: keep going and fetch the window above the baseline
    }

    if (entry->uidvalidity != mail.uidvalidity) {
        log_line("%s/%s: UIDVALIDITY changed (%llu -> %llu), resetting cursor",
                 account->name, folder, entry->uidvalidity, mail.uidvalidity);
        entry->uidvalidity = mail.uidvalidity;
        entry->last_uid = mail.uidnext > 0 ? mail.uidnext - 1 : 0;
        state_save(state);
        folder_free(&mail);
        return;
    }

    unsigned long long from = entry->last_uid + 1;
    if (mail.uidnext > 0 && from + (unsigned long long)config->max_fetch < mail.uidnext) {
        const unsigned long long skipped =
            mail.uidnext - (unsigned long long)config->max_fetch - from;
        log_line("%s/%s: %llu messages behind, keeping the newest %d",
                 account->name, folder, skipped, config->max_fetch);
        from = mail.uidnext - (unsigned long long)config->max_fetch;
    }

    if (imap_fetch(imap, from, &mail) != 0) {
        log_line("%s/%s: FETCH failed", account->name, folder);
        folder_free(&mail);
        return;
    }

    unsigned long long highest = entry->last_uid;
    size_t published = 0;
    for (size_t i = 0; i < mail.count; i++) {
        const Message *message = &mail.items[i];
        if (message->uid <= entry->last_uid) {
            continue; // 5:* returns the highest message even when 5 is past the end
        }
        time_t when = parse_rfc822(message->headers.date);
        if (when == 0) {
            when = time(NULL);
        }
        char *json = build_event_json(account, folder, mail.uidvalidity,
                                      message->uid, message->size,
                                      &message->headers, when);
        if (json == NULL) {
            continue;
        }
        if (publish(config, json) == 0) {
            published++;
        } else {
            log_line("%s/%s: uid %llu not acknowledged", account->name, folder,
                     message->uid);
        }
        free(json);
        if (message->uid > highest) {
            highest = message->uid;
        }
    }

    if (highest != entry->last_uid) {
        entry->last_uid = highest;
        if (state_save(state) != 0) {
            log_line("%s/%s: cannot persist cursor", account->name, folder);
        }
    }
    if (published > 0) {
        log_line("%s/%s: published %zu new message(s)", account->name, folder,
                 published);
    } else if (config->verbose) {
        log_line("%s/%s: nothing new (last uid %llu)", account->name, folder,
                 entry->last_uid);
    }
    folder_free(&mail);
}

static void poll_account(const Config *config, State *state,
                         const Account *account) {
    Imap imap;
    if (imap_open(&imap, account) != 0) {
        return;
    }

    if (imap_login(&imap, account) != 0) {
        log_line("%s: login failed", account->name);
        imap_close(&imap);
        return;
    }
    imap_identify(&imap);

    for (size_t i = 0; i < account->folder_count; i++) {
        poll_folder(config, state, &imap, account, account->folders[i]);
    }

    char *tag = imap_command(&imap, "LOGOUT");
    if (tag != NULL) {
        imap_wait(&imap, tag, NULL, NULL);
        free(tag);
    }
    imap_close(&imap);
}

// --------------------------------------------------------------------- init

static const char *tls_mode_name(tls_mode_t tls) {
    if (tls == TLS_IMPLICIT) {
        return "implicit";
    }
    if (tls == TLS_STARTTLS) {
        return "starttls";
    }
    return "none";
}

static char *build_bus_conf(const BusSettings *bus) {
    str_t text;
    str_init(&text);
    str_puts(&text, "# ~/.config/yi/bus.conf -- Yi bus connection (shared)\n");
    str_puts(&text, "# Written by yi-mail init; keep it private (0600).\n");
    str_puts(&text, "\n");
    str_printf(&text, "scheme = %s\n", bus->scheme);
    str_printf(&text, "host = %s\n", bus->host);
    str_printf(&text, "port = %d\n", bus->port);
    str_printf(&text, "token = %s\n", bus->token);
    return text.s;
}

static char *build_mail_conf(const Config *config) {
    str_t text;
    str_init(&text);
    str_puts(&text, "# ~/.config/yi/mail.conf -- IMAP accounts for yi-mail\n");
    str_puts(&text, "# Bus address and token live in ~/.config/yi/bus.conf.\n\n");
    str_printf(&text, "interval = %d\n", config->interval);
    str_printf(&text, "max_fetch = %d\n", config->max_fetch);
    str_printf(&text, "backfill = %s\n", config->backfill ? "true" : "false");
    if (config->verbose) {
        str_puts(&text, "verbose = true\n");
    }
    str_printf(&text, "state = %s\n", config->state_path);
    for (size_t i = 0; i < config->count; i++) {
        const Account *account = &config->accounts[i];
        str_printf(&text, "\n[account:%s]\n",
                   account->name != NULL ? account->name : "");
        str_printf(&text, "host = %s\n", account->host != NULL ? account->host : "");
        str_printf(&text, "port = %d\n", account->port);
        str_printf(&text, "tls = %s\n", tls_mode_name(account->tls));
        if (!account->verify) {
            str_puts(&text, "tls_verify = false\n");
        }
        str_printf(&text, "user = %s\n", account->user != NULL ? account->user : "");
        str_printf(&text, "password = %s\n",
                   account->password != NULL ? account->password : "");
        str_printf(&text, "source = %s\n",
                   account->source != NULL ? account->source : "");
        str_puts(&text, "folders = ");
        if (account->folder_count == 0) {
            str_puts(&text, "INBOX");
        }
        for (size_t f = 0; f < account->folder_count; f++) {
            if (f > 0) {
                str_putc(&text, ',');
            }
            str_puts(&text, account->folders[f]);
        }
        str_putc(&text, '\n');
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

static size_t discard_response(char *data, size_t size, size_t count, void *userdata) {
    (void)data;
    (void)userdata;
    return size * count;
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

static void init_accounts(Config *config, size_t *added) {
    if (!ask_yes_no("现在添加 IMAP 账户? [Y/n]: ", true)) {
        return;
    }
    while (true) {
        char *name_answer = ask_line("账户名称 name: ");
        char *name = name_answer != NULL ? trim(name_answer) : NULL;
        if (name == NULL || *name == '\0') {
            free(name_answer);
            if (!ask_yes_no("继续添加账户? [y/N]: ", false)) {
                break;
            }
            continue;
        }

        Account *account = config_add_account(config, name);
        free(name_answer);

        char *host_answer = ask_line("IMAP 主机 host: ");
        if (host_answer != NULL) {
            char *value = trim(host_answer);
            if (*value != '\0') {
                set_string(&account->host, value);
            }
            free(host_answer);
        }

        for (int attempt = 0; attempt < 3; attempt++) {
            char *port_answer = ask_line("端口 port [993]: ");
            if (port_answer == NULL) {
                break;
            }
            char *value = trim(port_answer);
            if (*value == '\0') {
                free(port_answer);
                break;
            }
            int port = 0;
            if (parse_port_value(value, &port)) {
                account->port = port;
                free(port_answer);
                break;
            }
            free(port_answer);
            fprintf(stderr, "端口必须是 1-65535 之间的整数\n");
        }
        if (account->port == 0) {
            account->port = 993;
        }

        char *tls_answer = ask_line("加密 tls (implicit/starttls/none) [implicit]: ");
        if (tls_answer != NULL) {
            char *value = trim(tls_answer);
            if (*value != '\0') {
                if (strcasecmp(value, "implicit") == 0 ||
                    strcasecmp(value, "ssl") == 0) {
                    account->tls = TLS_IMPLICIT;
                } else if (strcasecmp(value, "starttls") == 0) {
                    account->tls = TLS_STARTTLS;
                } else if (strcasecmp(value, "none") == 0 ||
                           strcasecmp(value, "plain") == 0) {
                    account->tls = TLS_NONE;
                } else {
                    log_line("unknown tls '%s'; using implicit", value);
                }
            }
            free(tls_answer);
        }

        char *user_answer = ask_line("用户名 user: ");
        if (user_answer != NULL) {
            char *value = trim(user_answer);
            if (*value != '\0') {
                set_string(&account->user, value);
            }
            free(user_answer);
        }

        char *password_answer = ask_secret("密码/授权码 password: ");
        if (password_answer != NULL && *password_answer != '\0') {
            set_string(&account->password, password_answer);
        }
        free(password_answer);

        char *source_answer = ask_line("来源 source [%s]: ", account->name);
        if (source_answer != NULL) {
            char *value = trim(source_answer);
            if (*value != '\0') {
                set_string(&account->source, value);
            }
            free(source_answer);
        }

        char *folders_answer = ask_line("文件夹 folders [INBOX]: ");
        if (folders_answer != NULL) {
            char *value = trim(folders_answer);
            if (*value != '\0') {
                account_set_folders(account, value);
            }
            free(folders_answer);
        }

        if (account->host == NULL || account->user == NULL ||
            account->password == NULL) {
            log_line("account '%s' needs host, user and password; skipped",
                     account->name);
            config_remove_last_account(config);
        } else {
            (*added)++;
        }

        if (!ask_yes_no("继续添加账户? [y/N]: ", false)) {
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
    snprintf(path, size, "%s/.local/bin/yi-mail", home);
    return path;
}

static char *systemd_unit_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && *xdg != '\0') {
        size_t size = strlen(xdg) + 48;
        char *path = malloc(size);
        snprintf(path, size, "%s/systemd/user/yi-mail.service", xdg);
        return path;
    }
    return expand_home("~/.config/systemd/user/yi-mail.service");
}

static char *absolute_path(const char *path) {
    char *resolved = realpath(path, NULL);
    if (resolved != NULL) {
        return resolved;
    }
    if (path[0] == '/') {
        return xstrdup(path);
    }
    char *cwd = getcwd(NULL, 0);
    if (cwd == NULL) {
        return xstrdup(path);
    }
    size_t size = strlen(cwd) + strlen(path) + 2;
    char *out = malloc(size);
    if (out == NULL) {
        free(cwd);
        return xstrdup(path);
    }
    snprintf(out, size, "%s/%s", cwd, path);
    free(cwd);
    return out;
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

static char *build_unit(const char *binary, const char *mail_path) {
    str_t text;
    str_init(&text);
    str_puts(&text, "[Unit]\n");
    str_puts(&text, "Description=Yi mail watcher (IMAP -> mail.received)\n");
    str_puts(&text, "Documentation=https://github.com/gresces/Yi-Tools/blob/main/tools/yi-mail/README.md\n");
    str_puts(&text, "After=network-online.target\n");
    str_puts(&text, "Wants=network-online.target\n");
    str_puts(&text, "\n[Service]\n");
    str_puts(&text, "Type=simple\n");
    str_printf(&text, "ExecStart=%s --config %s\n", binary, mail_path);
    str_puts(&text, "Restart=on-failure\n");
    str_puts(&text, "RestartSec=30s\n");
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
        fprintf(stderr, "yi-mail: cannot run %s: %s\n", argv[0],
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

static int deploy_service(const char *mail_path, size_t accounts) {
    if (accounts == 0) {
        printf("未配置 IMAP 账户:服务会按周期等待 %s 中出现账户\n", mail_path);
        fflush(stdout);
    }

    char *binary = install_binary();
    if (binary == NULL) {
        return -1;
    }

    char *unit_path = systemd_unit_path();
    char *mail_absolute = absolute_path(mail_path);
    if (unit_path == NULL || mail_absolute == NULL) {
        log_line("cannot determine the systemd unit path");
        free(binary);
        free(unit_path);
        free(mail_absolute);
        return -1;
    }

    char *unit = build_unit(binary, mail_absolute);
    if (unit == NULL ||
        write_atomic_mode(unit_path, unit, strlen(unit), 0644) != 0) {
        log_line("cannot write %s", unit_path);
        free(unit);
        free(binary);
        free(unit_path);
        free(mail_absolute);
        return -1;
    }
    free(unit);
    printf("已写入 %s\n", unit_path);
    fflush(stdout);

    char *const reload_argv[] = {"systemctl", "--user", "daemon-reload", NULL};
    char *const enable_argv[] = {"systemctl", "--user", "enable",
                                 "yi-mail.service", NULL};
    char *const restart_argv[] = {"systemctl", "--user", "restart",
                                  "yi-mail.service", NULL};
    char *const *steps[] = {reload_argv, enable_argv, restart_argv};

    int result = 0;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        char *display = join_command(steps[i]);
        const int status = run_command(steps[i]);
        if (status != 0) {
            log_line("command failed (exit %d): %s", status,
                     display != NULL ? display : steps[i][0]);
            log_line("check the service with: journalctl --user -u yi-mail");
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
    free(mail_absolute);
    return result;
}

static int run_init(const char *mail_path) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_line("libcurl initialisation failed");
        return 1;
    }

    char *bus_path = default_bus_path();
    BusSettings bus;
    bus_defaults(&bus);
    const bool bus_present = bus_load_file(&bus, bus_path);
    if (bus_present) {
        bus_parse_url(&bus);
    }

    Config config;
    config_defaults(&config);
    struct stat info;
    const bool mail_present = stat(mail_path, &info) == 0;
    if (mail_present && config_load(&config, mail_path) != 0) {
        log_line("%s has errors; keeping the accounts that parsed", mail_path);
    }

    // Without a bus.conf, start from the deprecated keys so init migrates them.
    if (!bus_present) {
        if (config.legacy_url) {
            free(bus.url);
            bus.url = xstrdup(config.url);
            bus_parse_url(&bus);
        }
        if (config.legacy_token) {
            set_string(&bus.token, config.token);
        }
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
    init_accounts(&config, &added);

    const bool migrated = config.legacy_url || config.legacy_token;
    if (added > 0 || migrated) {
        char *mail_text = build_mail_conf(&config);
        if (mail_text == NULL ||
            write_atomic_private(mail_path, mail_text, strlen(mail_text)) != 0) {
            free(mail_text);
            result = 1;
            goto done;
        }
        free(mail_text);
        printf("已写入 %s\n", mail_path);
        if (migrated) {
            printf("已把 url/token 迁移到 ~/.config/yi/bus.conf\n");
        }
        fflush(stdout);
    }
    if (config.count == 0) {
        printf("没有配置 IMAP 账户;稍后编辑 %s 添加 [account:名字] 段\n", mail_path);
        fflush(stdout);
    }

    bool deployed = false;
    if (ask_yes_no("部署为 systemd user 服务? [y/N]: ", false)) {
        if (deploy_service(mail_path, config.count) != 0) {
            result = 1;
            goto done;
        }
        deployed = true;
    }

    if (deployed) {
        printf("查看日志: journalctl --user -u yi-mail -f\n");
    } else {
        printf("下一步: systemctl --user enable --now yi-mail\n");
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

static volatile sig_atomic_t g_stop = 0;

static void handle_stop(int signal_number) {
    (void)signal_number;
    g_stop = 1;
}

static void sleep_interval(int seconds) {
    for (int waited = 0; waited < seconds * 1000 && !g_stop; waited += 200) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
}

static void usage(FILE *out) {
    fprintf(out,
            "yi-mail %s - IMAP watcher that publishes mail.received to Yi\n"
            "\n"
            "Usage: yi-mail [options]\n"
            "       yi-mail init\n"
            "\n"
            "Commands:\n"
            "  init                write bus.conf and mail.conf, optionally install a systemd user service\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH   config file (default ~/.config/yi/mail.conf)\n"
            "      --once          poll every account once and exit\n"
            "      --check         parse the config, print it and exit\n"
            "      --verbose       log every poll, not only new mail\n"
            "  -V, --version       print version and exit\n"
            "  -h, --help          print this help and exit\n",
            VERSION);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    bool once = false;
    bool check = false;
    bool initialise = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("yi-mail %s\n", VERSION);
            return 0;
        }
        if (strcmp(arg, "init") == 0) {
            initialise = true;
        } else if (strcmp(arg, "--once") == 0) {
            once = true;
        } else if (strcmp(arg, "--check") == 0) {
            check = true;
        } else if (strcmp(arg, "--verbose") == 0) {
            // applied after load, see below
            setenv("YI_MAIL_VERBOSE", "1", 1);
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "yi-mail: %s needs a value\n", arg);
                return 2;
            }
            config_path = argv[++i];
        } else {
            fprintf(stderr, "yi-mail: unknown option %s\n", arg);
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
    const bool mail_missing = stat(config_path, &info) != 0 && errno == ENOENT;
    // --check is also used by `make test` on machines without a config.
    if (!(check && mail_missing)) {
        if (config_load(&config, config_path) != 0) {
            log_line("config %s has errors", config_path);
            free(owned_path);
            config_free(&config);
            return 1;
        }
    }
    config_apply_bus(&config);
    if (getenv("YI_MAIL_VERBOSE") != NULL) {
        config.verbose = true;
    }

    if (check) {
        config_print(&config);
        free(owned_path);
        config_free(&config);
        return 0;
    }

    bool warned_empty = false;
    if (config.count == 0) {
        log_line("no accounts configured in %s (will retry every %ds)", config_path,
                 config.interval);
        warned_empty = true;
    }

    signal(SIGTERM, handle_stop);
    signal(SIGINT, handle_stop);
    signal(SIGPIPE, SIG_IGN);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        log_line("libcurl initialisation failed");
        free(owned_path);
        config_free(&config);
        return 1;
    }
    OpenSSL_add_all_algorithms();

    State state;
    state_load(&state, config.state_path);

    log_line("%s, %zu account(s), every %ds -> %s", VERSION, config.count,
             config.interval, config.url);

    while (!g_stop) {
        for (size_t i = 0; i < config.count && !g_stop; i++) {
            poll_account(&config, &state, &config.accounts[i]);
        }
        if (once) {
            break;
        }
        sleep_interval(config.interval);
        if (g_stop) {
            break;
        }

        // Re-read the config every cycle: adding an account or fixing a
        // password should not require restarting the service. A broken edit
        // keeps the previous config alive.
        Config fresh;
        config_defaults(&fresh);
        if (config_load(&fresh, config_path) != 0) {
            log_line("config reload failed; keeping previous settings");
            config_free(&fresh);
            continue;
        }
        config_apply_bus(&fresh);
        if (getenv("YI_MAIL_VERBOSE") != NULL) {
            fresh.verbose = true;
        }
        if (strcmp(fresh.state_path, config.state_path) != 0) {
            state_save(&state);
            state_free(&state);
            state_load(&state, fresh.state_path);
        }
        const bool had_accounts = config.count > 0;
        config_free(&config);
        config = fresh;
        if (!had_accounts && config.count > 0) {
            log_line("config loaded: %zu account(s)", config.count);
            warned_empty = false;
        } else if (config.count == 0 && !warned_empty) {
            log_line("no accounts configured in %s (will retry every %ds)",
                     config_path, config.interval);
            warned_empty = true;
        }
    }

    state_save(&state);
    state_free(&state);
    curl_global_cleanup();
    free(owned_path);
    config_free(&config);
    log_line("stopped");
    return 0;
}
