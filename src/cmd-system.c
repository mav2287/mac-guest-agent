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
    gettimeofday(&tv, NULL);
    /* Return nanoseconds since epoch */
    long long ns = (long long)tv.tv_sec * 1000000000LL + (long long)tv.tv_usec * 1000LL;
    return cJSON_CreateNumber((double)ns);
}

/* ---- guest-set-time ---- */

static cJSON *handle_set_time(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *time_item = cJSON_GetObjectItemCaseSensitive(args, "time");
    if (!time_item || !cJSON_IsNumber(time_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing or invalid 'time' argument";
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

        /* Check if we've already added this user */
        int duplicate = 0;
        for (int i = 0; i < seen_count; i++) {
            if (strcmp(seen[i], entry->ut_user) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;

        if (seen_count < 32) {
            strncpy(seen[seen_count], entry->ut_user, 63);
            seen[seen_count][63] = '\0';
            seen_count++;
        }

        cJSON *user = cJSON_CreateObject();
        cJSON_AddStringToObject(user, "user", entry->ut_user);
        double login_time = (double)entry->ut_tv.tv_sec;
        cJSON_AddNumberToObject(user, "login-time", login_time);
        cJSON_AddItemToArray(users, user);
    }
    endutxent();

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

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "load1", loadavg[0]);
    cJSON_AddNumberToObject(result, "load5", loadavg[1]);
    cJSON_AddNumberToObject(result, "load15", loadavg[2]);
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
