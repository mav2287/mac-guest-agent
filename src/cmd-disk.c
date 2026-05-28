#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mount.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>

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

/* Read a uint64 value out of a CFDictionary keyed by a C string.
 * Returns 0 if the key isn't present or doesn't hold a CFNumber. */
static uint64_t cfdict_u64(CFDictionaryRef d, const char *key)
{
    if (!d || !key) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    if (!k) return 0;
    CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(d, k);
    CFRelease(k);
    if (!n || CFGetTypeID(n) != CFNumberGetTypeID()) return 0;
    uint64_t v = 0;
    CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

/* Per-disk stats matched to the QGA GuestDiskStats schema. macOS-native
 * counters cover 6 of the 15 spec fields (the byte/operation/total-time
 * counters IOKit exposes via IOBlockStorageDriver's `Statistics` dict).
 * The remaining 9 are Linux-block-layer concepts (request merging,
 * discard accounting, in-flight count, weighted I/O ticks) that
 * IOKit doesn't expose; we emit them as 0 — same precedent as
 * `guest-get-cpustats` emitting `nice: 0` on macOS (where
 * host_processor_info doesn't split niced time from user) and as
 * `guest-network-get-route` emitting `metric: 0` / `irtt: 0`. Honest
 * zeros over silently dropping the field, so spec-strict consumers
 * (virsh / PVE plugins) get the canonical shape they can parse. */
static cJSON *handle_get_diskstats(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    CFMutableDictionaryRef match = IOServiceMatching(kIOBlockStorageDriverClass);
    if (!match) {
        *err_class = "GenericError";
        *err_desc  = "IOServiceMatching(IOBlockStorageDriver) failed";
        return NULL;
    }

    io_iterator_t iter = IO_OBJECT_NULL;
    /* IOServiceGetMatchingServices CONSUMES the matching dict — no
     * separate CFRelease needed below regardless of the result.
     *
     * First arg: `kIOMasterPortDefault` (deprecated in macOS 12) and
     * `kIOMainPortDefault` (the macOS 12+ rename) both #define to
     * `MACH_PORT_NULL` (which is `0`). Using `0` directly works on
     * every macOS version from 10.0 through 26.x without tripping
     * the -Wdeprecated-declarations warning that fails the coverage
     * build under -Werror. CI caught the kIOMasterPortDefault use
     * after Phase 2 commit 5f04253. */
    kern_return_t kr = IOServiceGetMatchingServices(0, match, &iter);
    if (kr != KERN_SUCCESS) {
        *err_class = "GenericError";
        *err_desc  = "IOServiceGetMatchingServices failed";
        return NULL;
    }

    cJSON *result = cJSON_CreateArray();
    io_object_t drv;
    int count = 0;
    while ((drv = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        /* IOBlockStorageDriver hangs its cumulative per-disk counters
         * off the "Statistics" CFDictionary property. */
        CFDictionaryRef stats = (CFDictionaryRef)IORegistryEntryCreateCFProperty(
            drv, CFSTR(kIOBlockStorageDriverStatisticsKey),
            kCFAllocatorDefault, 0);

        /* BSD device name ("disk0", "disk1s2" etc.) lives on the
         * IOBlockStorageDriver's CHILD IOMedia node, not on the driver
         * itself and not on its parent (which is typically the
         * controller — IONVMeBlockStorageDevice etc.). Use
         * IORegistryEntrySearchCFProperty with kIORegistryIterateRecursively
         * to walk the children looking for the BSD Name property. */
        char bsd_name[64] = {0};
        CFStringRef bsd = (CFStringRef)IORegistryEntrySearchCFProperty(
            drv, kIOServicePlane, CFSTR(kIOBSDNameKey),
            kCFAllocatorDefault, kIORegistryIterateRecursively);
        if (bsd) {
            CFStringGetCString(bsd, bsd_name, sizeof(bsd_name), kCFStringEncodingUTF8);
            CFRelease(bsd);
        }

        if (stats && bsd_name[0]) {
            uint64_t bytes_r = cfdict_u64(stats, kIOBlockStorageDriverStatisticsBytesReadKey);
            uint64_t bytes_w = cfdict_u64(stats, kIOBlockStorageDriverStatisticsBytesWrittenKey);
            uint64_t ops_r   = cfdict_u64(stats, kIOBlockStorageDriverStatisticsReadsKey);
            uint64_t ops_w   = cfdict_u64(stats, kIOBlockStorageDriverStatisticsWritesKey);
            uint64_t ns_r    = cfdict_u64(stats, kIOBlockStorageDriverStatisticsTotalReadTimeKey);
            uint64_t ns_w    = cfdict_u64(stats, kIOBlockStorageDriverStatisticsTotalWriteTimeKey);

            /* Spec sectors are 512 bytes by convention (matches Linux
             * /proc/diskstats — sectors there are always 512 regardless
             * of the device's actual physical sector size). */
            uint64_t sectors_r = bytes_r / 512;
            uint64_t sectors_w = bytes_w / 512;

            /* Spec ticks are milliseconds (Linux /proc/diskstats unit).
             * IOKit reports total time as nanoseconds — convert. */
            uint64_t ms_r = ns_r / 1000000;
            uint64_t ms_w = ns_w / 1000000;

            cJSON *info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "name", bsd_name);
            /* major/minor: macOS doesn't have stable Linux-style
             * (major, minor) device numbers. The closest is st_dev
             * from stat(2), but that's per-filesystem not per-block-
             * device. Emit 0 honestly. */
            cJSON_AddNumberToObject(info, "major", 0);
            cJSON_AddNumberToObject(info, "minor", 0);

            cJSON *st = cJSON_CreateObject();
            /* Mappable from IOKit. */
            cJSON_AddNumberToObject(st, "read-sectors",  (double)sectors_r);
            cJSON_AddNumberToObject(st, "read-ios",      (double)ops_r);
            cJSON_AddNumberToObject(st, "write-sectors", (double)sectors_w);
            cJSON_AddNumberToObject(st, "write-ios",     (double)ops_w);
            cJSON_AddNumberToObject(st, "read-ticks",    (double)ms_r);
            cJSON_AddNumberToObject(st, "write-ticks",   (double)ms_w);
            /* Linux-only concepts — honest 0s. */
            cJSON_AddNumberToObject(st, "read-merges",    0);
            cJSON_AddNumberToObject(st, "write-merges",   0);
            cJSON_AddNumberToObject(st, "discard-sectors", 0);
            cJSON_AddNumberToObject(st, "discard-ios",     0);
            cJSON_AddNumberToObject(st, "discard-merges",  0);
            cJSON_AddNumberToObject(st, "discard-ticks",   0);
            cJSON_AddNumberToObject(st, "in-flight",       0);
            cJSON_AddNumberToObject(st, "io-ticks",        0);
            cJSON_AddNumberToObject(st, "time-in-queue",   0);
            cJSON_AddItemToObject(info, "stats", st);
            cJSON_AddItemToArray(result, info);
            count++;
        }

        if (stats) CFRelease(stats);
        IOObjectRelease(drv);
    }
    IOObjectRelease(iter);

    LOG_DEBUG("Retrieved diskstats for %d block device(s)", count);
    return result;
}

void cmd_disk_init(void)
{
    command_register("guest-get-disks", handle_get_disks, 1);
    command_register("guest-get-fsinfo", handle_get_fsinfo, 1);
    command_register("guest-get-diskstats", handle_get_diskstats, 1);
}
