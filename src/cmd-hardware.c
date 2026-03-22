#include "commands.h"
#include "compat.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>

/* ---- helpers ---- */

/* Use the sysctl C API directly instead of shelling out.
   This is stable across all macOS versions and immune to
   command-line tool path or output format changes. */

static long long get_total_memory(void)
{
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0)
        return (long long)memsize;

    /* Fallback to command if sysctlbyname fails */
    char *out = NULL;
    if (run_command_capture("sysctl -n hw.memsize", &out) == 0 && out) {
        long long val = strtoll(str_trim(out), NULL, 10);
        free(out);
        return val;
    }
    free(out);
    return 0;
}

static int get_logical_cpus(void)
{
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0)
        return ncpu;

    /* Fallback */
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0)
        return ncpu;

    char *out = NULL;
    if (run_command_capture("sysctl -n hw.logicalcpu", &out) == 0 && out) {
        int val = atoi(str_trim(out));
        free(out);
        return val > 0 ? val : 1;
    }
    free(out);
    return 1;
}

/* Get memory statistics using the Mach host_statistics64 API.
   This is a stable kernel-level API that doesn't depend on text parsing
   and works across all macOS versions from 10.4 through current. */
static int get_vm_stat_mach(long long *free_pages, long long *active_pages,
                            long long *inactive_pages, long long *wired_pages,
                            long long *compressed_pages, long long *speculative_pages,
                            long long *purgeable_pages, long long *page_size)
{
    vm_size_t ps;
    host_page_size(mach_host_self(), &ps);
    *page_size = (long long)ps;

    vm_statistics64_data_t vm_info;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                         (host_info64_t)&vm_info, &count);
    if (kr != KERN_SUCCESS) {
        return -1;
    }

    *free_pages = (long long)vm_info.free_count;
    *active_pages = (long long)vm_info.active_count;
    *inactive_pages = (long long)vm_info.inactive_count;
    *wired_pages = (long long)vm_info.wire_count;
    *speculative_pages = (long long)vm_info.speculative_count;
    *purgeable_pages = (long long)vm_info.purgeable_count;
    *compressed_pages = (long long)vm_info.compressor_page_count;

    return 0;
}

/* Fallback: parse vm_stat text output for older systems where
   host_statistics64 may not support VM_INFO64 */
static int get_vm_stat_text(long long *free_pages, long long *active_pages,
                            long long *inactive_pages, long long *wired_pages,
                            long long *compressed_pages, long long *speculative_pages,
                            long long *purgeable_pages, long long *page_size)
{
    char *out = NULL;
    if (run_command_capture("vm_stat", &out) != 0 || !out) {
        free(out);
        return -1;
    }

    char *line = strtok(out, "\n");
    while (line) {
        char *ps = strstr(line, "page size of ");
        if (ps) {
            *page_size = strtoll(ps + 13, NULL, 10);
            line = strtok(NULL, "\n");
            continue;
        }

        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            char *val_str = colon + 1;
            while (*val_str == ' ' || *val_str == '\t') val_str++;
            size_t vlen = strlen(val_str);
            if (vlen > 0 && val_str[vlen - 1] == '.')
                val_str[vlen - 1] = '\0';

            long long val = strtoll(val_str, NULL, 10);

            if (strstr(line, "Pages free"))
                *free_pages = val;
            else if (strstr(line, "Pages active"))
                *active_pages = val;
            else if (strstr(line, "Pages inactive"))
                *inactive_pages = val;
            else if (strstr(line, "Pages speculative"))
                *speculative_pages = val;
            else if (strstr(line, "Pages wired"))
                *wired_pages = val;
            else if (strstr(line, "Pages purgeable"))
                *purgeable_pages = val;
            else if (strstr(line, "Pages stored in compressor"))
                *compressed_pages = val;
        }
        line = strtok(NULL, "\n");
    }
    free(out);
    return 0;
}

/* Get memory stats: prefer Mach API, fall back to text parsing */
static int get_vm_stat(long long *free_pages, long long *active_pages,
                       long long *inactive_pages, long long *wired_pages,
                       long long *compressed_pages, long long *speculative_pages,
                       long long *purgeable_pages, long long *page_size)
{
    *free_pages = 0;
    *active_pages = 0;
    *inactive_pages = 0;
    *wired_pages = 0;
    *compressed_pages = 0;
    *speculative_pages = 0;
    *purgeable_pages = 0;
    *page_size = 4096;

    if (get_vm_stat_mach(free_pages, active_pages, inactive_pages, wired_pages,
                         compressed_pages, speculative_pages, purgeable_pages,
                         page_size) == 0) {
        return 0;
    }

    LOG_DEBUG("Mach VM stats unavailable, falling back to vm_stat command");
    return get_vm_stat_text(free_pages, active_pages, inactive_pages, wired_pages,
                            compressed_pages, speculative_pages, purgeable_pages,
                            page_size);
}

/* ---- guest-get-vcpus ---- */

static cJSON *handle_get_vcpus(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    int count = get_logical_cpus();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *cpu = cJSON_CreateObject();
        cJSON_AddNumberToObject(cpu, "logical-id", i);
        cJSON_AddBoolToObject(cpu, "online", 1);
        cJSON_AddBoolToObject(cpu, "can-offline", 0);
        cJSON_AddItemToArray(arr, cpu);
    }
    LOG_DEBUG("Retrieved %d vCPUs", count);
    return arr;
}

static cJSON *handle_set_vcpus(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    *err_class = "GenericError";
    *err_desc = "CPU hotplug is not supported on macOS";
    return NULL;
}

/* ---- guest-get-memory-blocks ---- */

static cJSON *handle_get_memory_blocks(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    long long total = get_total_memory();
    if (total <= 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get memory size";
        return NULL;
    }

    /* Get memory usage from vm_stat */
    long long free_p, active_p, inactive_p, wired_p, compressed_p, spec_p, purg_p, pgsz;
    long long used = total / 2; /* fallback */
    if (get_vm_stat(&free_p, &active_p, &inactive_p, &wired_p, &compressed_p,
                    &spec_p, &purg_p, &pgsz) == 0) {
        used = (wired_p + active_p + compressed_p) * pgsz;
    }

    /* Calculate block size */
    long long block_size;
    long long GB = 1024LL * 1024 * 1024;
    long long MB = 1024LL * 1024;

    if (total < 4 * GB)
        block_size = 256 * MB;
    else if (total < 16 * GB)
        block_size = 512 * MB;
    else
        block_size = 1 * GB;

    int num_blocks = (int)(total / block_size);
    if (total % block_size > 0) num_blocks++;
    if (num_blocks < 8) {
        block_size = total / 8;
        if (block_size < 128 * MB) block_size = 128 * MB;
        num_blocks = (int)(total / block_size);
        if (total % block_size > 0) num_blocks++;
    } else if (num_blocks > 32) {
        block_size = total / 32;
        num_blocks = (int)(total / block_size);
        if (total % block_size > 0) num_blocks++;
    }
    if (num_blocks < 1) num_blocks = 1;

    int online_blocks = (int)(used / block_size);
    if (used % block_size > 0) online_blocks++;
    if (online_blocks < 1) online_blocks = 1;
    if (online_blocks > num_blocks) online_blocks = num_blocks;

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num_blocks; i++) {
        cJSON *block = cJSON_CreateObject();
        cJSON_AddNumberToObject(block, "phys-index", i);
        cJSON_AddBoolToObject(block, "online", i < online_blocks);
        cJSON_AddBoolToObject(block, "can-offline", 0);
        cJSON_AddItemToArray(arr, block);
    }
    return arr;
}

static cJSON *handle_get_memory_block_info(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args; (void)err_class; (void)err_desc;

    long long total = get_total_memory();
    if (total <= 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to get memory size";
        return NULL;
    }

    long long block_size;
    long long GB = 1024LL * 1024 * 1024;
    long long MB = 1024LL * 1024;
    if (total < 4 * GB) block_size = 256 * MB;
    else if (total < 16 * GB) block_size = 512 * MB;
    else block_size = 1 * GB;

    int num = (int)(total / block_size);
    if (total % block_size > 0) num++;
    if (num < 8) { block_size = total / 8; if (block_size < 128 * MB) block_size = 128 * MB; }
    else if (num > 32) { block_size = total / 32; }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "size", (double)block_size);
    return result;
}

static cJSON *handle_set_memory_blocks(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;
    *err_class = "GenericError";
    *err_desc = "Memory hotplug is not supported on macOS";
    return NULL;
}

/* ---- guest-get-cpustats ---- */

static cJSON *handle_get_cpustats(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    host_cpu_load_info_data_t cpu_load;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    kern_return_t kr = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                                       (host_info_t)&cpu_load, &count);
    if (kr != KERN_SUCCESS) {
        *err_class = "GenericError";
        *err_desc = "Failed to get CPU statistics";
        return NULL;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "user", (double)cpu_load.cpu_ticks[CPU_STATE_USER]);
    cJSON_AddNumberToObject(result, "system", (double)cpu_load.cpu_ticks[CPU_STATE_SYSTEM]);
    cJSON_AddNumberToObject(result, "idle", (double)cpu_load.cpu_ticks[CPU_STATE_IDLE]);
    cJSON_AddNumberToObject(result, "nice", (double)cpu_load.cpu_ticks[CPU_STATE_NICE]);
    return result;
}

void cmd_hardware_init(void)
{
    command_register("guest-get-vcpus", handle_get_vcpus, 1);
    command_register("guest-set-vcpus", handle_set_vcpus, 1);
    command_register("guest-get-memory-blocks", handle_get_memory_blocks, 1);
    command_register("guest-get-memory-block-info", handle_get_memory_block_info, 1);
    command_register("guest-set-memory-blocks", handle_set_memory_blocks, 1);
    command_register("guest-get-cpustats", handle_get_cpustats, 1);
}
