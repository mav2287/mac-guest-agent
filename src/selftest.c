#include "selftest.h"
#include "agent.h"
#include "compat.h"
#include "commands.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define HOOK_DIR "/etc/qemu/fsfreeze-hook.d"
#define CONFIG_PATH "/etc/qemu/qemu-ga.conf"
#define MAX_RESULTS 128

typedef enum { ST_PASS, ST_WARN, ST_FAIL, ST_INFO } st_level_t;

typedef struct {
    st_level_t level;
    char section[32];
    char name[64];
    char detail[256];
} st_result_t;

static st_result_t results[MAX_RESULTS];
static int num_results = 0;
static int json_mode = 0;

static void add_result(st_level_t level, const char *section, const char *name, const char *detail)
{
    if (num_results >= MAX_RESULTS) return;
    st_result_t *r = &results[num_results++];
    r->level = level;
    snprintf(r->section, sizeof(r->section), "%s", section);
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->detail, sizeof(r->detail), "%s", detail ? detail : "");
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int file_writable(const char *path)
{
    return access(path, W_OK) == 0;
}

static int tool_available(const char *name)
{
    /* Check common macOS tool paths directly instead of using system() */
    const char *dirs[] = {"/usr/bin/", "/usr/sbin/", "/bin/", "/sbin/", NULL};
    char path[256];
    for (int i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        if (access(path, X_OK) == 0) return 1;
    }
    return 0;
}

static void check_version(void)
{
    compat_init();
    const os_version_t *ver = compat_os_version();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d", ver->major, ver->minor, ver->patch);

    if (ver->major == 0)
        add_result(ST_FAIL, "version", "macOS version", "could not detect (sw_vers failed)");
    else
        add_result(ST_PASS, "version", "macOS version", buf);

    struct utsname uts;
    if (uname(&uts) == 0) {
        char arch_buf[128];
        snprintf(arch_buf, sizeof(arch_buf), "%s (%s)", uts.machine, uts.sysname);
        add_result(ST_PASS, "version", "architecture", arch_buf);
    } else {
        add_result(ST_WARN, "version", "architecture", "uname failed");
    }
}

static void check_privileges(void)
{
    if (geteuid() == 0)
        add_result(ST_PASS, "privileges", "running as root", "yes");
    else
        add_result(ST_WARN, "privileges", "running as root", "no (some commands require root)");
}

static void check_serial_device(void)
{
    const char *isa_devices[] = {
        "/dev/cu.serial1", "/dev/cu.serial2", "/dev/cu.serial", NULL
    };
    const char *virtio_devices[] = {
        "/dev/cu.org.qemu.guest_agent.0", "/dev/tty.org.qemu.guest_agent.0",
        "/dev/cu.virtio-console.0", "/dev/cu.virtio-serial",
        "/dev/cu.virtio", "/dev/tty.virtio",  /* UTM */
        NULL
    };

    int found_isa = 0, found_virtio = 0;

    for (int i = 0; isa_devices[i]; i++) {
        if (file_exists(isa_devices[i])) {
            char detail[256];
            int r = access(isa_devices[i], R_OK) == 0;
            int w = access(isa_devices[i], W_OK) == 0;
            snprintf(detail, sizeof(detail), "%s (r=%s w=%s)",
                     isa_devices[i], r ? "yes" : "no", w ? "yes" : "no");
            add_result(ST_PASS, "serial", "ISA serial device", detail);
            if (!r || !w)
                add_result(ST_WARN, "serial", "ISA serial permissions", "device not fully accessible");
            found_isa = 1;
            break;
        }
    }

    for (int i = 0; virtio_devices[i]; i++) {
        if (file_exists(virtio_devices[i])) {
            add_result(ST_INFO, "serial", "VirtIO serial device", virtio_devices[i]);
            found_virtio = 1;
            break;
        }
    }

    if (!found_isa && !found_virtio)
        add_result(ST_WARN, "serial", "serial device", "no ISA or VirtIO serial device found (expected in VM with agent enabled)");
    else if (!found_isa && found_virtio)
        add_result(ST_INFO, "serial", "transport", "VirtIO only (type=isa recommended for broader compatibility)");
}

static void check_config(void)
{
    if (file_exists(CONFIG_PATH))
        add_result(ST_PASS, "config", "config file", CONFIG_PATH);
    else
        add_result(ST_INFO, "config", "config file", "not present (using defaults)");
}

static void check_log(void)
{
    const char *log_path = "/var/log/mac-guest-agent.log";

    if (file_exists(log_path)) {
        if (file_writable(log_path))
            add_result(ST_PASS, "logging", "log file", log_path);
        else
            add_result(ST_WARN, "logging", "log file", "exists but not writable");
    } else {
        if (file_writable("/var/log"))
            add_result(ST_PASS, "logging", "log directory", "/var/log is writable (log file will be created)");
        else
            add_result(ST_WARN, "logging", "log directory", "/var/log not writable (need root)");
    }
}

static void check_filesystem(void)
{
    if (compat_has_apfs()) {
        add_result(ST_PASS, "filesystem", "APFS support", "yes (10.13+)");
        if (compat_has_tmutil())
            add_result(ST_PASS, "filesystem", "tmutil snapshots", "available (freeze will create APFS snapshot)");
        else
            add_result(ST_WARN, "filesystem", "tmutil snapshots", "tmutil not executable (freeze will use sync-only)");
    } else {
        add_result(ST_INFO, "filesystem", "APFS support", "no (pre-10.13, freeze will use sync + F_FULLFSYNC only)");
    }
}

static void check_hooks(void)
{
    struct stat st;
    if (stat(HOOK_DIR, &st) != 0) {
        add_result(ST_INFO, "hooks", "hook directory", "not present (no hooks configured)");
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        add_result(ST_FAIL, "hooks", "hook directory", HOOK_DIR " exists but is not a directory");
        return;
    }

    add_result(ST_PASS, "hooks", "hook directory", HOOK_DIR);

    char *output = NULL;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -1 '%s' 2>/dev/null", HOOK_DIR);
    if (run_command_capture(cmd, &output) == 0 && output && output[0]) {
        int count = 0, valid = 0, invalid = 0;
        char *save = NULL;
        char *line = strtok_r(output, "\n", &save);
        while (line) {
            count++;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", HOOK_DIR, line);
            struct stat fst;
            if (stat(path, &fst) == 0) {
                int ok = (fst.st_uid == 0) && !(fst.st_mode & S_IWOTH) && (fst.st_mode & S_IXUSR);
                if (ok) {
                    valid++;
                } else {
                    char detail[512];
                    snprintf(detail, sizeof(detail), "%s (uid=%d, mode=%04o)", line, fst.st_uid, fst.st_mode & 0777);
                    add_result(ST_WARN, "hooks", "hook script", detail);
                    invalid++;
                }
            }
            line = strtok_r(NULL, "\n", &save);
        }
        char summary[128];
        snprintf(summary, sizeof(summary), "%d script(s), %d valid, %d invalid", count, valid, invalid);
        add_result(invalid > 0 ? ST_WARN : ST_PASS, "hooks", "hook scripts", summary);
        free(output);
    } else {
        add_result(ST_INFO, "hooks", "hook scripts", "none");
        free(output);
    }
}

static void check_commands(void)
{
    commands_init();
    int count = commands_count();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d registered", count);
    add_result(ST_PASS, "commands", "commands", buf);
}

static void check_tools(void)
{
    struct { const char *name; const char *purpose; int required; } tools[] = {
        {"sw_vers",    "OS version detection",    1},
        {"sysctl",     "hardware info",           1},
        {"diskutil",   "disk information",        1},
        {"netstat",    "network info (fallback)", 0},
        {"osascript",  "graceful shutdown",       0},
        {"shutdown",   "shutdown fallback",       1},
        {"pmset",      "suspend/hibernate",       0},
        {"dscl",       "password changes",        0},
        {"launchctl",  "service management",      1},
        {"tmutil",     "APFS snapshots",          0},
        {NULL, NULL, 0}
    };

    for (int i = 0; tools[i].name; i++) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%s (%s)", tools[i].name, tools[i].purpose);
        if (tool_available(tools[i].name))
            add_result(ST_PASS, "tools", "tool", detail);
        else
            add_result(tools[i].required ? ST_FAIL : ST_WARN, "tools", "tool missing", detail);
    }
}

static void check_service(void)
{
    const char *plist = "/Library/LaunchDaemons/com.macos.guest-agent.plist";

    if (file_exists(plist)) {
        add_result(ST_PASS, "service", "LaunchDaemon plist", "installed");

        char *output = NULL;
        if (run_command_capture("launchctl list com.macos.guest-agent 2>/dev/null", &output) == 0 && output) {
            if (strstr(output, "com.macos.guest-agent"))
                add_result(ST_PASS, "service", "service status", "loaded");
            else
                add_result(ST_INFO, "service", "service status", "not loaded");
            free(output);
        } else {
            add_result(ST_INFO, "service", "service status", "not loaded (or not running as root)");
            free(output);
        }
    } else {
        add_result(ST_INFO, "service", "LaunchDaemon plist", "not installed (run --install)");
    }
}

static void check_environment(void)
{
    char *model = NULL;
    if (run_command_capture("sysctl -n hw.model", &model) == 0 && model) {
        char *nl = strchr(model, '\n');
        if (nl) *nl = '\0';
        add_result(ST_INFO, "environment", "hardware model", model);
        free(model);
    } else {
        add_result(ST_WARN, "environment", "hardware model", "could not detect");
        free(model);
    }

    char *profiler = NULL;
    int qemu_detected = 0;

    if (run_command_capture("sysctl -n hw.model", &profiler) == 0 && profiler) {
        for (char *p = profiler; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (strstr(profiler, "QEMU"))
            qemu_detected = 1;
        free(profiler);
    }

    if (!qemu_detected) {
        const char *devices[] = {
            "/dev/cu.org.qemu.guest_agent.0", "/dev/tty.org.qemu.guest_agent.0",
            "/dev/virtio", "/dev/vda", "/dev/vdb", NULL
        };
        for (int i = 0; devices[i]; i++) {
            if (file_exists(devices[i])) { qemu_detected = 1; break; }
        }
    }

    if (qemu_detected)
        add_result(ST_PASS, "environment", "QEMU environment", "detected");
    else
        add_result(ST_INFO, "environment", "QEMU environment", "not detected (normal for macOS VMs with custom hardware models)");
}

static void check_backup_readiness(void)
{
    int ready = 1;
    char detail[256];
    const char *freeze_method;

    /* Determine freeze method */
    if (compat_has_apfs() && compat_has_tmutil()) {
        freeze_method = "APFS snapshot + sync + F_FULLFSYNC (best)";
    } else if (compat_has_apfs()) {
        freeze_method = "sync + F_FULLFSYNC (APFS, no tmutil)";
        ready = 0;
    } else {
        freeze_method = "sync + F_FULLFSYNC only (HFS+, no snapshots)";
    }
    add_result(ST_INFO, "backup", "freeze method", freeze_method);

    /* Check if running as root (required for freeze) */
    if (geteuid() != 0) {
        add_result(ST_WARN, "backup", "freeze capability", "not running as root (freeze requires root)");
        ready = 0;
    }

    /* Check hooks */
    struct stat st;
    if (stat(HOOK_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        char *output = NULL;
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "ls -1 '%s' 2>/dev/null", HOOK_DIR);
        if (run_command_capture(cmd, &output) == 0 && output && output[0]) {
            int count = 0;
            char *save = NULL;
            char *line = strtok_r(output, "\n", &save);
            while (line) { count++; line = strtok_r(NULL, "\n", &save); }
            snprintf(detail, sizeof(detail), "%d hook(s) installed", count);
            add_result(ST_PASS, "backup", "freeze hooks", detail);
            free(output);
        } else {
            add_result(ST_INFO, "backup", "freeze hooks", "none (OK if no databases to flush)");
            free(output);
        }
    } else {
        add_result(ST_INFO, "backup", "freeze hooks", "no hook directory (OK if no databases to flush)");
    }

    /* Overall verdict */
    if (ready) {
        add_result(ST_PASS, "backup", "backup readiness", "ready for PVE backup with freeze");
    } else {
        add_result(ST_WARN, "backup", "backup readiness", "freeze available but with limitations (see above)");
    }
}

static const char *level_str(st_level_t l)
{
    switch (l) {
    case ST_PASS: return "pass";
    case ST_WARN: return "warn";
    case ST_FAIL: return "fail";
    case ST_INFO: return "info";
    }
    return "unknown";
}

static void json_escape(const char *s, char *out, size_t out_sz)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j < out_sz - 6; i++) {
        if (s[i] == '"' || s[i] == '\\') {
            out[j++] = '\\'; out[j++] = s[i];
        } else if (s[i] == '\n') {
            out[j++] = '\\'; out[j++] = 'n';
        } else if (s[i] == '\r') {
            out[j++] = '\\'; out[j++] = 'r';
        } else if (s[i] == '\t') {
            out[j++] = '\\'; out[j++] = 't';
        } else if ((unsigned char)s[i] < 0x20) {
            /* Skip other control characters */
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
}

static void output_text(int errs, int warns)
{
    const char *cur_section = "";

    printf("mac-guest-agent %s self-test\n", AGENT_VERSION);
    printf("================================\n");

    for (int i = 0; i < num_results; i++) {
        st_result_t *r = &results[i];
        if (strcmp(r->section, cur_section) != 0) {
            cur_section = r->section;
            printf("\n[%c%s]\n", (cur_section[0] >= 'a' && cur_section[0] <= 'z')
                   ? cur_section[0] - 32 : cur_section[0], cur_section + 1);
        }
        const char *tag = "INFO";
        if (r->level == ST_PASS) tag = "PASS";
        else if (r->level == ST_WARN) tag = "WARN";
        else if (r->level == ST_FAIL) tag = "FAIL";

        printf("  %-4s  %s", tag, r->name);
        if (r->detail[0])
            printf(": %s", r->detail);
        printf("\n");
    }

    printf("\n================================\n");
    printf("Result: %d error(s), %d warning(s)\n", errs, warns);

    if (errs > 0)
        printf("Status: PROBLEMS DETECTED\n");
    else if (warns > 0)
        printf("Status: OK (with warnings)\n");
    else
        printf("Status: ALL CHECKS PASSED\n");
}

static void emit_system_info(void)
{
    char esc[512];

    printf("\"system_info\":{");

    /* OS version */
    const os_version_t *ver = compat_os_version();
    printf("\"os_version\":\"%d.%d.%d\",", ver->major, ver->minor, ver->patch);

    /* Architecture */
    struct utsname uts;
    if (uname(&uts) == 0) {
        printf("\"arch\":\"%s\",\"kernel\":\"%s\",", uts.machine, uts.release);
    }

    /* Hardware model */
    char *model = NULL;
    if (run_command_capture("sysctl -n hw.model", &model) == 0 && model) {
        char *nl = strchr(model, '\n');
        if (nl) *nl = '\0';
        json_escape(model, esc, sizeof(esc));
        printf("\"hw_model\":\"%s\",", esc);
        free(model);
    }

    /* CPU and memory */
    char *val = NULL;
    if (run_command_capture("sysctl -n hw.logicalcpu", &val) == 0 && val) {
        printf("\"cpu_count\":%d,", atoi(val));
        free(val); val = NULL;
    }
    if (run_command_capture("sysctl -n hw.memsize", &val) == 0 && val) {
        printf("\"memory_bytes\":%lld,", atoll(val));
        free(val); val = NULL;
    }

    /* Serial kext */
    if (file_exists("/System/Library/Extensions/Apple16X50Serial.kext")) {
        char *kver = NULL;
        if (run_command_capture("/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' /System/Library/Extensions/Apple16X50Serial.kext/Contents/Info.plist 2>/dev/null", &kver) == 0 && kver) {
            char *nl = strchr(kver, '\n');
            if (nl) *nl = '\0';
            printf("\"serial_kext_version\":\"%s\",", kver);
            free(kver);
        }
    }

    /* APFS/VirtIO capabilities */
    printf("\"has_apfs\":%s,", compat_has_apfs() ? "true" : "false");
    printf("\"has_tmutil\":%s,", compat_has_tmutil() ? "true" : "false");
    printf("\"has_virtio\":%s,", file_exists("/System/Library/Extensions/AppleVirtIO.kext") ? "true" : "false");

    /* Root filesystem type */
    char *fstype = NULL;
    if (run_command_capture("mount | head -1 | awk '{print $NF}' | tr -d '()'", &fstype) == 0 && fstype) {
        char *nl = strchr(fstype, '\n');
        if (nl) *nl = '\0';
        json_escape(fstype, esc, sizeof(esc));
        printf("\"root_fs_type\":\"%s\",", esc);
        free(fstype);
    }

    /* Backup readiness */
    if (compat_has_apfs() && compat_has_tmutil())
        printf("\"freeze_method\":\"apfs_snapshot\",");
    else if (compat_has_apfs())
        printf("\"freeze_method\":\"sync_fullfsync\",");
    else
        printf("\"freeze_method\":\"sync_only\",");

    /* Command count */
    printf("\"command_count\":%d", commands_count());

    printf("},");
}

static void output_json(int errs, int warns, int passes)
{
    char esc_name[128], esc_detail[512];

    printf("{\"agent_version\":\"%s\",", AGENT_VERSION);
    printf("\"errors\":%d,\"warnings\":%d,\"passes\":%d,", errs, warns, passes);
    printf("\"status\":\"%s\",", errs > 0 ? "fail" : "pass");

    emit_system_info();

    printf("\"checks\":[");

    for (int i = 0; i < num_results; i++) {
        st_result_t *r = &results[i];
        json_escape(r->name, esc_name, sizeof(esc_name));
        json_escape(r->detail, esc_detail, sizeof(esc_detail));

        if (i > 0) printf(",");
        printf("{\"section\":\"%s\",\"level\":\"%s\",\"name\":\"%s\",\"detail\":\"%s\"}",
               r->section, level_str(r->level), esc_name, esc_detail);
    }

    printf("]}\n");
}

int selftest_run(int json_output)
{
    json_mode = json_output;
    num_results = 0;

    check_version();
    check_privileges();
    check_environment();
    check_serial_device();
    check_config();
    check_log();
    check_filesystem();
    check_hooks();
    check_commands();
    check_tools();
    check_service();
    check_backup_readiness();

    int errs = 0, warns = 0, passes = 0;
    for (int i = 0; i < num_results; i++) {
        if (results[i].level == ST_FAIL) errs++;
        else if (results[i].level == ST_WARN) warns++;
        else if (results[i].level == ST_PASS) passes++;
    }

    if (json_mode)
        output_json(errs, warns, passes);
    else
        output_text(errs, warns);

    return errs > 0 ? 1 : 0;
}

/* ---- Safe Test: validate read-only commands return correct responses ---- */

static int run_safe_cmd(const char *cmd_name, const char *args_json, int expect_array)
{
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    char *resp = commands_dispatch(cmd_name, args, NULL);
    if (args) cJSON_Delete(args);

    if (!resp) return 0;  /* no response = fail */

    cJSON *parsed = cJSON_Parse(resp);
    free(resp);
    if (!parsed) return 0;

    cJSON *ret = cJSON_GetObjectItem(parsed, "return");
    int ok = (ret != NULL);

    /* Optionally verify it's an array or object */
    if (ok && expect_array && !cJSON_IsArray(ret)) ok = 0;
    if (ok && !expect_array && !cJSON_IsObject(ret) && !cJSON_IsArray(ret)
        && !cJSON_IsNumber(ret) && !cJSON_IsString(ret)) ok = 0;

    cJSON_Delete(parsed);
    return ok;
}

int safetest_run(int json_output)
{
    /* Initialize commands if not already done */
    commands_init();

    struct {
        const char *name;
        const char *args;
        int expect_array;
        const char *desc;
    } tests[] = {
        {"guest-ping",                     NULL, 0, "Protocol ping"},
        {"guest-sync",                     "{\"id\":99999}", 0, "Protocol sync"},
        {"guest-info",                     NULL, 0, "Agent info (version + commands)"},
        {"guest-get-osinfo",               NULL, 0, "OS identification"},
        {"guest-get-host-name",            NULL, 0, "Hostname"},
        {"guest-get-timezone",             NULL, 0, "Timezone"},
        {"guest-get-time",                 NULL, 0, "System time"},
        {"guest-get-users",                NULL, 1, "Logged-in users"},
        {"guest-get-load",                 NULL, 0, "System load averages"},
        {"guest-get-vcpus",                NULL, 1, "vCPU list"},
        {"guest-get-cpustats",             NULL, 0, "CPU statistics"},
        {"guest-get-memory-block-info",    NULL, 0, "Memory block size"},
        {"guest-get-memory-blocks",        NULL, 1, "Memory blocks"},
        {"guest-get-disks",                NULL, 1, "Disk list"},
        {"guest-get-fsinfo",               NULL, 1, "Filesystem info"},
        {"guest-get-diskstats",            NULL, 1, "Disk statistics"},
        {"guest-fsfreeze-status",          NULL, 0, "Freeze status"},
        {"guest-network-get-interfaces",   NULL, 1, "Network interfaces"},
        {"guest-network-get-route",        NULL, 1, "Routing table"},
        {"guest-fstrim",                   NULL, 0, "TRIM (no-op)"},
        {NULL, NULL, 0, NULL}
    };

    int pass = 0, fail = 0;

    if (!json_output) {
        printf("mac-guest-agent %s safe-test\n", AGENT_VERSION);
        printf("================================\n");
        printf("Read-only command validation (no modifications)\n\n");
    }

    for (int i = 0; tests[i].name; i++) {
        int ok = run_safe_cmd(tests[i].name, tests[i].args, tests[i].expect_array);
        if (ok) {
            pass++;
            if (!json_output)
                printf("  PASS  %s\n", tests[i].desc);
        } else {
            fail++;
            if (!json_output)
                printf("  FAIL  %s (%s)\n", tests[i].desc, tests[i].name);
        }
    }

    /* Error handling tests */
    char *err_resp = commands_dispatch("nonexistent-command", NULL, NULL);
    if (err_resp) {
        cJSON *parsed = cJSON_Parse(err_resp);
        free(err_resp);
        int ok = parsed && cJSON_GetObjectItem(parsed, "error");
        if (parsed) cJSON_Delete(parsed);
        if (ok) {
            pass++;
            if (!json_output) printf("  PASS  Unknown command returns error\n");
        } else {
            fail++;
            if (!json_output) printf("  FAIL  Unknown command error handling\n");
        }
    } else {
        fail++;
        if (!json_output) printf("  FAIL  Unknown command (no response)\n");
    }

    if (json_output) {
        printf("{\"test\":\"safe-test\",\"agent_version\":\"%s\",\"passes\":%d,\"failures\":%d,\"status\":\"%s\"}\n",
               AGENT_VERSION, pass, fail, fail > 0 ? "fail" : "pass");
    } else {
        printf("\n================================\n");
        printf("Results: %d passed, %d failed\n", pass, fail);
        printf("(Power, exec, file-write, SSH, password\n");
        printf(" commands intentionally not tested)\n");
    }

    return fail > 0 ? 1 : 0;
}
