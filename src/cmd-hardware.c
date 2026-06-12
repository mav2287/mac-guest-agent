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

    /* QGA `online` means "is this memory block plugged in / online" for memory
     * hotplug. macOS has no memory hotplug and never offlines RAM, so EVERY
     * block is online and none can be offlined. The old code set online from
     * current memory *usage* (used/block_size) — that conflated "used" with
     * "online" and misled any hotplug consumer (a half-used VM looked like it
     * had half its RAM unplugged). Report the honest hotplug truth instead. */

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

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < num_blocks; i++) {
        cJSON *block = cJSON_CreateObject();
        cJSON_AddNumberToObject(block, "phys-index", i);
        cJSON_AddBoolToObject(block, "online", 1);      /* macOS RAM is always online */
        cJSON_AddBoolToObject(block, "can-offline", 0); /* and can never be offlined */
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

/* Per-CPU CPU-load tick counters, spec-shaped per QGA's GuestCpuStats
 * (an array of discriminated-union records).
 *
 * Discriminator note: the upstream QGA spec
 * (qga/qapi-schema.json) gates guest-get-cpustats on CONFIG_LINUX, and
 * its GuestCpuStatsType enum currently has no "darwin" value. Emitting
 * type:"darwin" would be rejected by strict QAPI parsers as an unknown
 * enum value. Omitting `type` is worse — it's part of the union's
 * required base, so strict parsers would reject "missing required
 * field". We emit type:"linux": the user/system/idle/nice tick
 * semantics map cleanly from macOS's PROCESSOR_CPU_LOAD_INFO to
 * Linux's GuestLinuxCpuStats struct, so the field shape is honest
 * even when the discriminator names the spec-defined variant.
 *
 * Source: docs/design/AGENT_BEHAVIOUR_SPEC.md Q4 (full reasoning,
 * including why we're not dropping the command and why "darwin"
 * upstream is deferred). */
static cJSON *handle_get_cpustats(cJSON *args, const char **err_class, const char **err_desc)
{
    (void)args;

    natural_t cpu_count = 0;
    processor_info_array_t info_array = NULL;
    mach_msg_type_number_t info_count = 0;
    kern_return_t kr = host_processor_info(mach_host_self(),
                                           PROCESSOR_CPU_LOAD_INFO,
                                           &cpu_count, &info_array, &info_count);
    if (kr != KERN_SUCCESS) {
        *err_class = "GenericError";
        *err_desc = "Failed to get per-CPU statistics";
        return NULL;
    }

    processor_cpu_load_info_t cpu_load = (processor_cpu_load_info_t)info_array;

    cJSON *result = cJSON_CreateArray();
    if (!result) {
        vm_deallocate(mach_task_self(), (vm_address_t)info_array,
                      info_count * sizeof(integer_t));
        *err_class = "GenericError";
        *err_desc = "Out of memory";
        return NULL;
    }

    for (natural_t i = 0; i < cpu_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) continue;  /* cJSON failure on this entry; skip, continue */
        cJSON_AddStringToObject(entry, "type", "linux");
        cJSON_AddNumberToObject(entry, "cpu",    (double)i);
        cJSON_AddNumberToObject(entry, "user",   (double)cpu_load[i].cpu_ticks[CPU_STATE_USER]);
        cJSON_AddNumberToObject(entry, "nice",   (double)cpu_load[i].cpu_ticks[CPU_STATE_NICE]);
        cJSON_AddNumberToObject(entry, "system", (double)cpu_load[i].cpu_ticks[CPU_STATE_SYSTEM]);
        cJSON_AddNumberToObject(entry, "idle",   (double)cpu_load[i].cpu_ticks[CPU_STATE_IDLE]);
        cJSON_AddItemToArray(result, entry);
    }

    vm_deallocate(mach_task_self(), (vm_address_t)info_array,
                  info_count * sizeof(integer_t));

    return result;
}

void cmd_hardware_init(void)
{
    command_register("guest-get-vcpus", handle_get_vcpus, 1);
    command_register("guest-set-vcpus", handle_set_vcpus, 0);  /* unsupported on macOS */
    command_register("guest-get-memory-blocks", handle_get_memory_blocks, 1);
    command_register("guest-get-memory-block-info", handle_get_memory_block_info, 1);
    command_register("guest-set-memory-blocks", handle_set_memory_blocks, 0);  /* unsupported on macOS */
    command_register("guest-get-cpustats", handle_get_cpustats, 1);
}
