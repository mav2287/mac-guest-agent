#include "commands.h"
#include "compat.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <utmpx.h>
#include <pwd.h>

/* ---- guest-get-osinfo ---- */

static char *get_sw_vers_field(const char *field)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "sw_vers -%s 2>/dev/null", field);
    char *out = NULL;
    if (run_command_capture(cmd, &out) == 0 && out) {
        str_trim(out);
        return out;
    }
    free(out);
    return NULL;
}

static cJSON *handle_get_osinfo(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    cJSON *info = cJSON_CreateObject();

    char *product_name = get_sw_vers_field("productName");
    char *product_version = get_sw_vers_field("productVersion");
    char *build_version = get_sw_vers_field("buildVersion");

    cJSON_AddStringToObject(info, "id", "macos");
    cJSON_AddStringToObject(info, "name", product_name ? product_name : "macOS");
    cJSON_AddStringToObject(info, "variant", "desktop");
    cJSON_AddStringToObject(info, "variant-id", "desktop");

    char pretty[256] = "";
    snprintf(pretty, sizeof(pretty), "%s %s",
             product_name ? product_name : "macOS",
             product_version ? product_version : "");
    cJSON_AddStringToObject(info, "pretty-name", pretty);

    if (product_version)
        cJSON_AddStringToObject(info, "version", product_version);
    if (build_version)
        cJSON_AddStringToObject(info, "version-id", build_version);

    struct utsname uts;
    if (uname(&uts) == 0) {
        cJSON_AddStringToObject(info, "kernel-release", uts.release);
        cJSON_AddStringToObject(info, "kernel-version", uts.version);
        cJSON_AddStringToObject(info, "machine", uts.machine);
    }

    free(product_name);
    free(product_version);
    free(build_version);

    LOG_DEBUG("Retrieved OS information");
    return info;
}

/* ---- guest-get-host-name ---- */

static cJSON *handle_get_hostname(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get hostname";
        return NULL;
    }
    /* POSIX does not guarantee NUL-termination if the name fills the buffer. */
    hostname[sizeof(hostname) - 1] = '\0';

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "host-name", hostname);
    return result;
}

/* ---- guest-get-timezone ---- */

static cJSON *handle_get_timezone(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    time_t now = time(NULL);
    struct tm tm_buf;
    if (!localtime_r(&now, &tm_buf)) {
        *err_class = "GenericError";
        *err_desc = "Failed to get local time";
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    if (tm_buf.tm_zone)
        cJSON_AddStringToObject(result, "zone", tm_buf.tm_zone);
    cJSON_AddNumberToObject(result, "offset", (double)tm_buf.tm_gmtoff);
    return result;
}

/* ---- guest-get-time ---- */

static cJSON *handle_get_time(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get time of day";
        return NULL;
    }
    /* Return nanoseconds since epoch */
    long long ns = (long long)tv.tv_sec * 1000000000LL + (long long)tv.tv_usec * 1000LL;
    return cJSON_CreateNumber((double)ns);
}

/* ---- guest-set-time ---- */

static cJSON *handle_set_time(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *time_item = cJSON_GetObjectItemCaseSensitive(args, "time");

    /* Argless form (QGA contract): "set the system clock from the hardware RTC"
     * (Linux does `hwclock --hctosys`). macOS exposes NO userspace way to read
     * the RTC and re-apply it — the kernel owns that at boot and there is no
     * `hwclock` equivalent. Rather than return a success that did nothing (a
     * dead no-op that lies to the caller), fail honestly: the caller must pass
     * an explicit `time` (which orchestrators do after snapshot/restore/
     * migration — the form that actually matters). */
    if (!time_item) {
        *err_class = "GenericError";
        *err_desc  = "argless guest-set-time (resync from RTC) is not supported on "
                     "macOS: there is no userspace hwclock equivalent. Pass an "
                     "explicit 'time' (nanoseconds since epoch).";
        return NULL;
    }

    if (!cJSON_IsNumber(time_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid 'time' argument (must be a number: nanoseconds since epoch)";
        return NULL;
    }

    long long ns = (long long)time_item->valuedouble;
    struct timeval tv;
    tv.tv_sec = (time_t)(ns / 1000000000LL);
    tv.tv_usec = (suseconds_t)((ns % 1000000000LL) / 1000LL);

    if (settimeofday(&tv, NULL) != 0) {
        /* Fallback to date command */
        time_t t = tv.tv_sec;
        struct tm tm_buf;
        localtime_r(&t, &tm_buf);
        char cmd[64];
        /* date format: MMddHHmmYY */
        strftime(cmd, sizeof(cmd), "date %m%d%H%M%y", &tm_buf);
        if (run_command(cmd) != 0) {
            *err_class = "GenericError";
            *err_desc = "Failed to set system time";
            return NULL;
        }
    }

    LOG_INFO("System time set");
    return cJSON_CreateObject();
}

/* ---- guest-get-users ---- */

/* Returns 1 if username is already present in the seen[] set, else records it
 * and returns 0. Caps at 32 distinct users (matches the array bound). */
static int user_seen(char seen[][64], int *seen_count, const char *name)
{
    for (int i = 0; i < *seen_count; i++) {
        if (strcmp(seen[i], name) == 0) return 1;
    }
    if (*seen_count < 32) {
        strncpy(seen[*seen_count], name, 63);
        seen[*seen_count][63] = '\0';
        (*seen_count)++;
    }
    return 0;
}

/* Tiger/legacy fallback: on Mac OS X 10.4 the loginwindow and sshd populate the
 * BSD legacy utmp database, NOT utmpx — so getutxent() returns nothing even with
 * a console user logged in (confirmed on 10.4.11). Parse `who`, which reads the
 * legacy database on every macOS version. `who` line format (BSD):
 *     name  line         Mon DD HH:MM
 * `who` omits the year, so login-time is reconstructed against the current year.
 * Appends any not-already-seen users to the array. */
static void append_users_from_who(cJSON *users, char seen[][64], int *seen_count)
{
    char *out = NULL;
    if (run_command_capture("who 2>/dev/null", &out) != 0 || !out) {
        free(out);
        return;
    }

    time_t now = time(NULL);
    struct tm now_tm;
    localtime_r(&now, &now_tm);

    char *save_line = NULL;
    for (char *line = strtok_r(out, "\n", &save_line); line;
         line = strtok_r(NULL, "\n", &save_line)) {
        char name[64] = "", tty[64] = "", mon[16] = "", day[8] = "", hhmm[16] = "";
        /* name  tty  Mon  DD  HH:MM   (extra trailing fields like host ignored) */
        if (sscanf(line, "%63s %63s %15s %7s %15s", name, tty, mon, day, hhmm) < 5)
            continue;
        if (name[0] == '\0') continue;
        if (user_seen(seen, seen_count, name)) continue;

        cJSON *user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "user", name);

        /* Reconstruct login-time from "Mon DD HH:MM" + current year. If parsing
         * fails, omit login-time rather than emit a bogus value. */
        char when[64];
        snprintf(when, sizeof(when), "%s %s %s %d", mon, day, hhmm,
                 now_tm.tm_year + 1900);
        struct tm lt;
        memset(&lt, 0, sizeof(lt));
        lt.tm_isdst = -1;
        if (strptime(when, "%b %d %H:%M %Y", &lt) != NULL) {
            time_t lts = mktime(&lt);
            /* If the reconstructed time is in the future (login was last year,
             * e.g. Dec login read in Jan), roll back a year. */
            if (lts > now + 86400) {
                lt.tm_year -= 1;
                lts = mktime(&lt);
            }
            if (lts != (time_t)-1)
                cJSON_AddNumberToObject(user, "login-time", (double)lts);
        }
        cJSON_AddItemToArray(users, user);
    }
    free(out);
}

static cJSON *handle_get_users(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    cJSON *users = cJSON_CreateArray();
    /* Track seen usernames to avoid duplicates */
    char seen[32][64];
    int seen_count = 0;

    setutxent();
    struct utmpx *entry;
    while ((entry = getutxent()) != NULL) {
        if (entry->ut_type != USER_PROCESS)
            continue;
        if (user_seen(seen, &seen_count, entry->ut_user)) continue;

        cJSON *user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "user", entry->ut_user);
        double login_time = (double)entry->ut_tv.tv_sec;
        cJSON_AddNumberToObject(user, "login-time", login_time);
        cJSON_AddItemToArray(users, user);
    }
    endutxent();

    /* utmpx is empty on Tiger 10.4 (uses the BSD legacy utmp DB) — fall back to
     * `who` so a logged-in console/SSH user is still reported. The fallback runs
     * whenever utmpx yielded nothing; on modern macOS utmpx is populated and the
     * `who` pass simply finds the same (already-seen) users and adds nothing. */
    if (cJSON_GetArraySize(users) == 0) {
        append_users_from_who(users, seen, &seen_count);
    }

    LOG_DEBUG("Retrieved %d logged-in users", cJSON_GetArraySize(users));
    return users;
}

/* ---- guest-get-load ---- */

static cJSON *handle_get_load(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    double loadavg[3];
    if (getloadavg(loadavg, 3) < 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get load averages";
        return NULL;
    }

    /* Field names match the QGA GuestLoadStats schema: load1m / load5m /
     * load15m (the "m" suffix marks "minutes"). Prior to this change we
     * emitted load1/load5/load15 — which strict QGA parsers reject and
     * which lost the spec's "this is in minutes" hint. */
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "load1m",  loadavg[0]);
    cJSON_AddNumberToObject(result, "load5m",  loadavg[1]);
    cJSON_AddNumberToObject(result, "load15m", loadavg[2]);
    return result;
}

void cmd_system_init(void)
{
    command_register("guest-get-osinfo", handle_get_osinfo, 1);
    command_register("guest-get-host-name", handle_get_hostname, 1);
    command_register("guest-get-hostname", handle_get_hostname, 1);
    command_register("guest-get-timezone", handle_get_timezone, 1);
    command_register("guest-get-time", handle_get_time, 1);
    command_register("guest-set-time", handle_set_time, 1);
    command_register("guest-get-users", handle_get_users, 1);
    command_register("guest-get-load", handle_get_load, 1);
}
