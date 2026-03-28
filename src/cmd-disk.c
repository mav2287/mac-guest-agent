#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

/* ---- guest-get-disks ---- */

static int is_safe_disk_name(const char *name)
{
    /* Only allow /dev/diskN format — prevent command injection */
    if (!name || strncmp(name, "/dev/disk", 9) != 0) return 0;
    for (const char *p = name + 9; *p; p++) {
        if (!(*p >= '0' && *p <= '9') && *p != 's') return 0;
    }
    return 1;
}

static char *get_disk_size(const char *disk_name)
{
    if (!is_safe_disk_name(disk_name)) return NULL;

    char *const argv[] = { "diskutil", "info", (char *)disk_name, NULL };
    char *out = NULL;
    if (run_command_v("diskutil", argv, &out, NULL) != 0 || !out) {
        free(out);
        return NULL;
    }

    /* Look for "Disk Size" or "Total Size" line with (NNN Bytes) */
    char *save_ptr = NULL;
    char *line = strtok_r(out, "\n", &save_ptr);
    while (line) {
        if (strstr(line, "Disk Size") || strstr(line, "Total Size")) {
            char *paren = strchr(line, '(');
            if (paren) {
                paren++;
                char *space = strchr(paren, ' ');
                if (space) {
                    *space = '\0';
                    char *result = safe_strdup(paren);
                    free(out);
                    return result;
                }
            }
        }
        line = strtok_r(NULL, "\n", &save_ptr);
    }
    free(out);
    return NULL;
}

static cJSON *handle_get_disks(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    char *out = NULL;
    if (run_command_capture("diskutil list", &out) != 0 || !out) {
        free(out);
        *err_class = "GenericError";
        *err_desc = "Failed to run diskutil";
        return NULL;
    }

    cJSON *disks = cJSON_CreateArray();
    cJSON *current_disk = NULL;
    char current_name[64] = "";

    char *save_ptr = NULL;
    char *line = strtok_r(out, "\n", &save_ptr);
    while (line) {
        while (*line == ' ' || *line == '\t') line++;

        /* Detect disk header like "/dev/disk0 (internal):" */
        if (strncmp(line, "/dev/disk", 9) == 0) {
            /* Extract disk name: /dev/diskN (without sN partition suffix) */
            char dname[64] = "";
            if (sscanf(line, "%63s", dname) == 1) {
                /* Only process whole disks, not partitions */
                char *paren = strchr(dname, ' ');
                if (paren) *paren = '\0';

                /* Check it's a whole disk (diskN not diskNsM) */
                const char *p = dname + 9; /* skip "/dev/disk" */
                int is_whole = 1;
                while (*p) {
                    if (*p == 's' && p > dname + 9) { is_whole = 0; break; }
                    if (!(*p >= '0' && *p <= '9')) { is_whole = 0; break; }
                    p++;
                }

                if (is_whole) {
                    /* Save previous disk */
                    if (current_disk)
                        cJSON_AddItemToArray(disks, current_disk);

                    current_disk = cJSON_CreateObject();
                    strncpy(current_name, dname, sizeof(current_name) - 1);
                    cJSON_AddStringToObject(current_disk, "name", dname);
                    cJSON_AddBoolToObject(current_disk, "partition", 0);
                    cJSON_AddBoolToObject(current_disk, "has-media", 1);

                    /* Address */
                    cJSON *addr = cJSON_CreateObject();
                    cJSON_AddStringToObject(addr, "bus-type", "unknown");
                    cJSON_AddNumberToObject(addr, "bus", -1);
                    cJSON_AddNumberToObject(addr, "target", -1);
                    cJSON_AddNumberToObject(addr, "unit", -1);
                    cJSON *pci = cJSON_CreateObject();
                    cJSON_AddNumberToObject(pci, "domain", -1);
                    cJSON_AddNumberToObject(pci, "bus", -1);
                    cJSON_AddNumberToObject(pci, "slot", -1);
                    cJSON_AddNumberToObject(pci, "function", -1);
                    cJSON_AddItemToObject(addr, "pci-controller", pci);
                    cJSON_AddStringToObject(addr, "dev", dname);
                    cJSON_AddItemToObject(current_disk, "address", addr);

                    /* Get disk size */
                    char *size_str = get_disk_size(dname);
                    if (size_str) {
                        long long sz = strtoll(size_str, NULL, 10);
                        if (sz > 0)
                            cJSON_AddNumberToObject(current_disk, "size", (double)sz);
                        free(size_str);
                    }
                }
            }
        }
        line = strtok_r(NULL, "\n", &save_ptr);
    }

    if (current_disk)
        cJSON_AddItemToArray(disks, current_disk);

    free(out);
    LOG_DEBUG("Retrieved %d disks", cJSON_GetArraySize(disks));
    return disks;
}

/* ---- guest-get-fsinfo ---- */

static cJSON *handle_get_fsinfo(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    struct statfs *mntbuf;
    int count = getmntinfo(&mntbuf, MNT_NOWAIT);
    if (count <= 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get filesystem info";
        return NULL;
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        /* Skip non-device filesystems */
        if (mntbuf[i].f_mntfromname[0] != '/')
            continue;

        cJSON *fs = cJSON_CreateObject();
        cJSON_AddStringToObject(fs, "name", mntbuf[i].f_mntfromname);
        cJSON_AddStringToObject(fs, "mountpoint", mntbuf[i].f_mntonname);
        cJSON_AddStringToObject(fs, "type", mntbuf[i].f_fstypename);

        long long total = (long long)mntbuf[i].f_blocks * mntbuf[i].f_bsize;
        long long used = (long long)(mntbuf[i].f_blocks - mntbuf[i].f_bfree) * mntbuf[i].f_bsize;
        cJSON_AddNumberToObject(fs, "total-bytes", (double)total);
        cJSON_AddNumberToObject(fs, "used-bytes", (double)used);

        /* Empty disk array per the protocol */
        cJSON_AddItemToObject(fs, "disk", cJSON_CreateArray());
        cJSON_AddItemToArray(arr, fs);
    }

    LOG_DEBUG("Retrieved %d filesystems", cJSON_GetArraySize(arr));
    return arr;
}

/* ---- guest-get-diskstats ---- */

static cJSON *handle_get_diskstats(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    /* Use iostat for disk statistics - available on all macOS */
    char *out = NULL;
    if (run_command_capture("iostat -d -c 1 2>/dev/null", &out) != 0 || !out) {
        free(out);
        *err_class = "GenericError";
        *err_desc = "Failed to get disk statistics";
        return NULL;
    }

    /* Parse iostat -d output into per-disk stats.
     * Format: line 1 = disk names, line 2 = headers, line 3 = values.
     * Values are grouped 3 per disk: KB/t, tps, MB/s. */

    /* Make a copy since we'll parse with strtok */
    char *copy = safe_strdup(out);
    if (!copy) { free(out); *err_class = "GenericError"; *err_desc = "Out of memory"; return NULL; }

    /* Extract disk names from line 1 */
    char disk_names[16][32];
    int disk_count = 0;
    char *line_end = strchr(copy, '\n');
    if (line_end) {
        *line_end = '\0';
        char *save2 = NULL;
        char *tok = strtok_r(copy, " \t", &save2);
        while (tok && disk_count < 16) {
            strncpy(disk_names[disk_count], tok, 31);
            disk_names[disk_count][31] = '\0';
            disk_count++;
            tok = strtok_r(NULL, " \t", &save2);
        }
    }

    /* Find data line (3rd line in original output) */
    char *p = out;
    int newlines = 0;
    while (*p && newlines < 2) { if (*p == '\n') newlines++; p++; }

    cJSON *result = cJSON_CreateArray();
    if (*p) {
        double vals[48];
        int nvals = 0;
        char *save2 = NULL;
        char *data_copy = safe_strdup(p);
        if (data_copy) {
            char *nl = strchr(data_copy, '\n');
            if (nl) *nl = '\0';
            char *tok = strtok_r(data_copy, " \t", &save2);
            while (tok && nvals < 48) {
                vals[nvals++] = strtod(tok, NULL);
                tok = strtok_r(NULL, " \t", &save2);
            }
            free(data_copy);

            for (int i = 0; i < disk_count && i * 3 + 2 < nvals; i++) {
                cJSON *disk = cJSON_CreateObject();
                cJSON_AddStringToObject(disk, "name", disk_names[i]);
                cJSON_AddNumberToObject(disk, "kb-per-transfer", vals[i * 3]);
                cJSON_AddNumberToObject(disk, "transfers-per-second", vals[i * 3 + 1]);
                cJSON_AddNumberToObject(disk, "mb-per-second", vals[i * 3 + 2]);
                cJSON_AddItemToArray(result, disk);
            }
        }
    }

    free(copy);
    free(out);
    return result;
}

void cmd_disk_init(void)
{
    command_register("guest-get-disks", handle_get_disks, 1);
    command_register("guest-get-fsinfo", handle_get_fsinfo, 1);
    command_register("guest-get-diskstats", handle_get_diskstats, 1);
}
