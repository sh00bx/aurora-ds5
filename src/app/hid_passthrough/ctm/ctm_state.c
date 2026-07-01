/* Headless definition unit for the CTM bridge core: shared global state plus the
 * log sink and generic string/file helpers. Split out of ui_common.c so the core
 * (enumeration, agent/session/plug, controllers, monitor) links into a host app
 * without LVGL/SDL. The UI half stays in ui_common.c. No LVGL/SDL here. */

#include "ctm_state.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- headless global state (was defined in ui_common.c) ----------------- */
composite_enum_t g_composite_enums[COMPOSITE_ENUM_MAX_CACHE];
int g_composite_enum_count;
puck_enum_t g_puck_enum;
scan_result_t g_scan;
logical_result_t g_devices;
int g_selected_index = -1;
char g_selected_key[96];
char g_plugged_keys[MAX_DEVICES][96];
int g_plugged_key_count;
char g_expanded_keys[MAX_DEVICES][96];
int g_expanded_key_count;
bridge_session_t g_sessions[MAX_SESSIONS];
int g_session_count;
ui_device_settings_t g_settings[MAX_DEVICES];
int g_settings_count;
char g_agent_host[64];
int g_agent_port = CTM_AGENT_PORT;
bool g_agent_online;
bool g_running = true;
pthread_t g_stop_sniff_thread;
bool g_stop_sniff_thread_started;
pthread_mutex_t g_bt_mac_mutex = PTHREAD_MUTEX_INITIALIZER;
char g_bt_macs[MAX_DEVICES][64];
int g_bt_mac_count;
autoplug_entry_t g_autoplug[MAX_DEVICES];
int g_autoplug_count;

char g_log[LOG_LEN];
size_t g_log_used;
pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_log_dirty;
static char g_last_plug_error[160];
static pthread_mutex_t g_plug_error_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Append a timestamped line to the in-memory log buffer + stderr. Thread-safe
 * (buffer guarded by g_log_mutex, touches NO LVGL) so controller threads can
 * log through the sink. The on-screen console is refreshed separately by
 * ctm_ui_log_flush() on the UI thread. When: app + controller events. */
void log_append(const char *fmt, ...)
{
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char line[640];
    int written = snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s\n",
                           tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, body);
    if (written <= 0) {
        return;
    }
    if ((size_t)written >= sizeof(line)) {
        written = (int)sizeof(line) - 1;
        line[written] = '\0';
    }

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_used + (size_t)written + 1 >= sizeof(g_log)) {
        size_t keep = sizeof(g_log) / 2;
        if (g_log_used < keep) {
            keep = g_log_used;
        }
        memmove(g_log, g_log + g_log_used - keep, keep);
        g_log_used = keep;
        g_log[g_log_used] = '\0';
    }
    memcpy(g_log + g_log_used, line, (size_t)written);
    g_log_used += (size_t)written;
    g_log[g_log_used] = '\0';
    g_log_dirty = true;
    pthread_mutex_unlock(&g_log_mutex);

    fputs(line, stderr);
    fflush(stderr);
}

void ctm_set_plug_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_plug_error_mutex);
    vsnprintf(g_last_plug_error, sizeof(g_last_plug_error), fmt, ap);
    pthread_mutex_unlock(&g_plug_error_mutex);
    va_end(ap);
    log_append("%s", g_last_plug_error);
}

void ctm_clear_plug_error(void)
{
    pthread_mutex_lock(&g_plug_error_mutex);
    g_last_plug_error[0] = '\0';
    pthread_mutex_unlock(&g_plug_error_mutex);
}

const char *ctm_last_plug_error(void)
{
    pthread_mutex_lock(&g_plug_error_mutex);
    const char *msg = g_last_plug_error[0] ? g_last_plug_error : NULL;
    pthread_mutex_unlock(&g_plug_error_mutex);
    return msg;
}

int count_dir_entries(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

int read_text_file(const char *path, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    ssize_t n = read(fd, out, out_len - 1);
    close(fd);
    if (n < 0) {
        out[0] = '\0';
        return -1;
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return 0;
}

void join_path(char *out, size_t out_len, const char *a, const char *b)
{
    snprintf(out, out_len, "%s/%s", a, b);
}

bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; ++p) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               ascii_lower(p[i]) == ascii_lower(needle[i])) {
            ++i;
        }
        if (i == needle_len) return true;
    }
    return false;
}

bool valid_bt_address(const char *s)
{
    if (!s || strlen(s) != 17) {
        return false;
    }
    for (int i = 0; i < 17; ++i) {
        if ((i + 1) % 3 == 0) {
            if (s[i] != ':') return false;
        } else {
            if (!((s[i] >= '0' && s[i] <= '9') ||
                  (s[i] >= 'a' && s[i] <= 'f') ||
                  (s[i] >= 'A' && s[i] <= 'F'))) {
                return false;
            }
        }
    }
    return true;
}

void append_unique(char *dst, size_t dst_len, const char *value)
{
    if (!value || !value[0]) {
        return;
    }
    if (strstr(dst, value)) {
        return;
    }
    size_t used = strlen(dst);
    snprintf(dst + used, dst_len - used, "%s%s", used ? ", " : "", value);
}
