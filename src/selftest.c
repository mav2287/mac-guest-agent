#include "selftest.h"
#include "agent.h"
#include "compat.h"
#include "commands.h"
#include "cmd-fs.h"
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
    /* VirtIO list retained ONLY to detect the v2.4.x → v2.5.0 misconfiguration
     * mode: VM has VirtIO instead of ISA. The agent no longer uses VirtIO as
     * a transport (see CHANGELOG v2.5.0 BREAKING), but a self-test on a
     * VirtIO-configured VM should report what's wrong, not silently say
     * "no serial device." */
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
            found_virtio = 1;
            break;
        }
    }

    if (!found_isa && !found_virtio) {
        add_result(ST_WARN, "serial", "serial device", "no ISA serial device found (v2.5.0+ requires ISA; configure type=isa on PVE, isa-serial on libvirt, QemuGuestAgent interface on UTM, -device isa-serial on raw QEMU)");
    } else if (!found_isa && found_virtio) {
        /* The v2.4.x → v2.5.0 footgun. Loud about it. */
        add_result(ST_FAIL, "serial", "VirtIO-only configuration", "v2.5.0 removed VirtIO transport — reconfigure VM for ISA serial (see CHANGELOG v2.5.0 BREAKING)");
    }
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

    /* Which slice of the universal binary dyld actually picked at load
     * time. Compile-time constant — set per slice during the tri-fat build.
     * Both "arch" (uname(2) uts.machine) and "selected_arch" report the
     * running process slice, not the host hardware: under Rosetta, uname()
     * inside an x86_64 process on arm64 hardware returns "x86_64", so the
     * two fields agree. selected_arch is kept as the compile-time-derived
     * source of truth (arch can drift if a host's uname is virtualized or
     * unavailable), and gives evidence-drop readers an unambiguous "which
     * slice ran" without having to parse `arch` semantics per platform. */
    const char *selected_arch =
#if defined(__i386__)
        "i386"
#elif defined(__x86_64__)
        "x86_64"
#elif defined(__arm64__)
        "arm64"
#else
        "unknown"
#endif
        ;
    printf("\"selected_arch\":\"%s\",", selected_arch);

    /* Command count */
    printf("\"command_count\":%d", commands_count());

    printf("},");
}

/* Emit the freeze_dispatch JSON object — sibling of system_info. Surfaces
 * the per-FS dispatch policy, the agent's log file path, the ZFS CLI
 * availability, and the documented divergences from upstream QGA, so that
 * verify.sh (and contributors) can introspect what the agent will do
 * during freeze without having to run a real freeze. See Q3 of
 * docs/design/AGENT_BEHAVIOUR_SPEC.md for the rationale. */
static void emit_freeze_dispatch(void)
{
    printf("\"freeze_dispatch\":{");

    /* Log file — default path used by the launchd plist; CLI may override
     * via -l/--logfile. Documented here so verifier tooling knows where to
     * grep for the per-event "Filesystem frozen:" INFO line. */
    printf("\"log_path_default\":\"/var/log/mac-guest-agent.log\",");
    printf("\"log_line_prefix\":\"Filesystem frozen:\",");

    /* Per-fstypename dispatch table. Keys are f_fstypename strings as
     * returned by getfsstat(2); values are the freeze treatment applied
     * by sync_all_volumes() / fs_dispatch_class(). Mirrors src/cmd-fs.c
     * exactly — keep these two in sync. */
    printf("\"per_fstypename\":{");
    printf("\"apfs\":\"tmutil_snapshot+f_fullfsync\",");
    printf("\"hfs\":\"f_fullfsync\",");
    printf("\"msdos\":\"f_fullfsync_with_enotsup_tolerated\",");
    printf("\"vfat\":\"f_fullfsync_with_enotsup_tolerated\",");
    printf("\"exfat\":\"f_fullfsync_with_enotsup_tolerated\",");
    printf("\"udf\":\"f_fullfsync_with_enotsup_tolerated\",");
    printf("\"ntfs\":\"f_fullfsync_with_enotsup_tolerated\",");
    printf("\"zfs\":\"zfs_snapshot_if_cli_else_f_fullfsync\",");
    printf("\"smbfs\":\"skip_network\",");
    printf("\"afpfs\":\"skip_network\",");
    printf("\"nfs\":\"skip_network\",");
    printf("\"webdav\":\"skip_network\",");
    printf("\"ftp\":\"skip_network\",");
    printf("\"devfs\":\"skip_special\",");
    printf("\"autofs\":\"skip_special\",");
    printf("\"fdesc\":\"skip_special\",");
    printf("\"volfs\":\"skip_special\",");
    printf("\"synthfs\":\"skip_special\",");
    printf("\"lifs\":\"skip_special\",");
    printf("\"_default_writable_dev_backed\":\"f_fullfsync_with_enotsup_tolerated\",");
    printf("\"_default_unknown_non_dev\":\"skip_special\"");
    printf("},");

    /* ZFS CLI runtime check — operators can confirm at install time
     * whether ZFS dispatch will actually use `zfs snapshot`. */
    printf("\"zfs_cli_available\":%s,", fsfreeze_has_zfs_cli() ? "true" : "false");

    /* Divergences from upstream QEMU Guest Agent (spec-conformance notes
     * — see Phase 1 Target 1/2 in docs/research/UPSTREAM_NOTES.md). */
    printf("\"divergences_from_upstream_qga\":{");
    printf("\"idempotent_re_freeze\":true,");          /* re-freeze does not error */
    printf("\"persistent_frozen_state_marker\":false,"); /* in-process flag only */
    printf("\"logging_disabled_during_freeze\":false"); /* we log to the configured logfile */
    printf("},");

    /* CPU stats discriminator note — Q4 of AGENT_BEHAVIOUR_SPEC.md.
     * The QGA schema's GuestCpuStats union has no "darwin" variant, so
     * macOS emits type="linux" with per-CPU jiffy-equivalents derived
     * from host_processor_info(PROCESSOR_CPU_LOAD_INFO). */
    printf("\"cpustats_discriminator\":\"linux\",");
    printf("\"cpustats_note\":\"type=linux emitted on macOS (no darwin enum value exists upstream); see docs/design/AGENT_BEHAVIOUR_SPEC.md Q4\"");

    printf("},");
}

static void output_json(int errs, int warns, int passes)
{
    char esc_name[128], esc_detail[512];

    printf("{\"agent_version\":\"%s\",", AGENT_VERSION);
    printf("\"errors\":%d,\"warnings\":%d,\"passes\":%d,", errs, warns, passes);
    printf("\"status\":\"%s\",", errs > 0 ? "fail" : "pass");

    emit_system_info();
    emit_freeze_dispatch();

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
        {"guest-get-cpustats",             NULL, 1, "CPU statistics"},
        {"guest-get-memory-block-info",    NULL, 0, "Memory block size"},
        {"guest-get-memory-blocks",        NULL, 1, "Memory blocks"},
        {"guest-get-disks",                NULL, 1, "Disk list"},
        {"guest-get-fsinfo",               NULL, 1, "Filesystem info"},
        {"guest-get-diskstats",            NULL, 1, "Disk statistics"},
        {"guest-fsfreeze-status",          NULL, 0, "Freeze status"},
        {"guest-network-get-interfaces",   NULL, 1, "Network interfaces"},
        {"guest-network-get-route",        NULL, 1, "Routing table"},
        /* guest-fstrim is NOT here: it is intentionally not registered on
         * macOS (no FITRIM equivalent; matches upstream CONFIG_FSTRIM gating).
         * Its absence is asserted separately below. */
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

    /* Capability assertions: these commands must NOT be callable on macOS.
     * Dispatching each must return specifically CommandNotFound. Two reasons:
     *   - not registered at all (no macOS equivalent; matches upstream CONFIG_*
     *     gating): fstrim, set-vcpus, set-memory-blocks.
     *   - registered but enabled=0 (config-dependent + unsafe by default — no
     *     working wake path on QEMU/OpenCore guests): the suspend-* trio.
     * A disabled command also dispatches to CommandNotFound, so one check covers
     * both. This guards against silently re-introducing a command we cannot
     * honor OR re-enabling suspend (which wedges the VM — see #179 evidence). */
    static const char *must_be_uncallable[] = {
        "guest-fstrim",            /* not registered: no FITRIM equivalent */
        "guest-set-vcpus",         /* not registered: no CPU hotplug */
        "guest-set-memory-blocks", /* not registered: no memory hotplug */
        "guest-suspend-disk",      /* enabled=0: no QEMU wake path (wedges VM) */
        "guest-suspend-ram",       /* enabled=0: no QEMU wake path */
        "guest-suspend-hybrid",    /* enabled=0: no QEMU wake path */
        NULL
    };
    for (int u = 0; must_be_uncallable[u]; u++) {
        const char *name = must_be_uncallable[u];
        char *r = commands_dispatch(name, NULL, NULL);
        cJSON *parsed = r ? cJSON_Parse(r) : NULL;
        free(r);
        cJSON *err = parsed ? cJSON_GetObjectItem(parsed, "error") : NULL;
        cJSON *cls = err ? cJSON_GetObjectItem(err, "class") : NULL;
        int uncallable = cls && cJSON_IsString(cls) &&
                         strcmp(cls->valuestring, "CommandNotFound") == 0;
        if (parsed) cJSON_Delete(parsed);
        if (uncallable) {
            pass++;
            if (!json_output) printf("  PASS  %s not callable (CommandNotFound)\n", name);
        } else {
            fail++;
            if (!json_output) printf("  FAIL  %s must NOT be callable on macOS\n", name);
        }
    }

    /* Data-truth invariants: guard against "plausible but wrong" data — a
     * command returning the right SHAPE with WRONG values. Each of these
     * caught a real bug that all prior shape-only tests passed. */

    /* (a) Loopback interface counters must be internally consistent. A
     * loopback never accrues interface errors, and any byte/packet counted
     * implies a non-zero packet count. The old positional `netstat -ibn`
     * parse shifted columns for address-less interfaces (lo0 has a blank
     * Address column), reporting Ibytes as rx-errs and 0 as rx-packets —
     * e.g. rx-errs==tx-errs==432658 with rx-packets==0. */
    {
        char *r = commands_dispatch("guest-network-get-interfaces", NULL, NULL);
        cJSON *parsed = r ? cJSON_Parse(r) : NULL;
        free(r);
        cJSON *ret = parsed ? cJSON_GetObjectItem(parsed, "return") : NULL;
        int ok = 0, saw_lo = 0;
        if (cJSON_IsArray(ret)) {
            cJSON *iface;
            cJSON_ArrayForEach(iface, ret) {
                cJSON *nm = cJSON_GetObjectItem(iface, "name");
                if (!nm || !cJSON_IsString(nm) || strcmp(nm->valuestring, "lo0") != 0)
                    continue;
                saw_lo = 1;
                cJSON *st = cJSON_GetObjectItem(iface, "statistics");
                cJSON *rb = st ? cJSON_GetObjectItem(st, "rx-bytes")   : NULL;
                cJSON *tb = st ? cJSON_GetObjectItem(st, "tx-bytes")   : NULL;
                cJSON *rp = st ? cJSON_GetObjectItem(st, "rx-packets") : NULL;
                cJSON *tp = st ? cJSON_GetObjectItem(st, "tx-packets") : NULL;
                cJSON *re = st ? cJSON_GetObjectItem(st, "rx-errs")    : NULL;
                cJSON *te = st ? cJSON_GetObjectItem(st, "tx-errs")    : NULL;
                if (rb && tb && rp && tp && re && te &&
                    re->valuedouble == 0 && te->valuedouble == 0 &&
                    (rb->valuedouble == 0 || rp->valuedouble > 0) &&
                    (tb->valuedouble == 0 || tp->valuedouble > 0))
                    ok = 1;
                break;
            }
        }
        if (parsed) cJSON_Delete(parsed);
        if (!saw_lo) ok = 0;  /* lo0 must always be present */
        if (ok) { pass++; if (!json_output) printf("  PASS  lo0 statistics consistent (no column shift)\n"); }
        else    { fail++; if (!json_output) printf("  FAIL  lo0 statistics inconsistent (netstat -ibn parse)\n"); }
    }

    /* (b) The loopback network route 127.0.0.0/8 must be reported with prefix
     * 8, not /32. macOS `netstat -rn` abbreviates it as "127"; the old parser
     * treated every slashless destination as a /32 host route. */
    {
        char *r = commands_dispatch("guest-network-get-route", NULL, NULL);
        cJSON *parsed = r ? cJSON_Parse(r) : NULL;
        free(r);
        cJSON *ret = parsed ? cJSON_GetObjectItem(parsed, "return") : NULL;
        int ok = 0;
        if (cJSON_IsArray(ret)) {
            cJSON *rt;
            cJSON_ArrayForEach(rt, ret) {
                cJSON *d = cJSON_GetObjectItem(rt, "destination");
                cJSON *p = cJSON_GetObjectItem(rt, "desprefixlen");
                if (d && cJSON_IsString(d) && strcmp(d->valuestring, "127.0.0.0") == 0) {
                    ok = (p && cJSON_IsString(p) && strcmp(p->valuestring, "8") == 0);
                    break;
                }
            }
        }
        if (parsed) cJSON_Delete(parsed);
        if (ok) { pass++; if (!json_output) printf("  PASS  loopback route 127.0.0.0 has prefix /8\n"); }
        else    { fail++; if (!json_output) printf("  FAIL  loopback route mis-prefixed (expected 127.0.0.0/8)\n"); }
    }

    /* (c) guest-file-open with an unknown mode must error, not silently open
     * read-only. (d) guest-set-user-password with crypted=true must error
     * rather than set the account password to the raw base64 text. Both are
     * rejected before any side effect, so they are safe to exercise here. */
    {
        struct { const char *name; const char *args; const char *desc; } neg[] = {
            {"guest-file-open",
             "{\"path\":\"/tmp\",\"mode\":\"zzz\"}",
             "file-open rejects unknown mode"},
            {"guest-set-user-password",
             "{\"username\":\"root\",\"password\":\"eA==\",\"crypted\":true}",
             "set-user-password rejects crypted=true"},
            {NULL, NULL, NULL}
        };
        for (int i = 0; neg[i].name; i++) {
            cJSON *a = cJSON_Parse(neg[i].args);
            char *r = commands_dispatch(neg[i].name, a, NULL);
            if (a) cJSON_Delete(a);
            cJSON *parsed = r ? cJSON_Parse(r) : NULL;
            free(r);
            cJSON *err = parsed ? cJSON_GetObjectItem(parsed, "error") : NULL;
            cJSON *cls = err ? cJSON_GetObjectItem(err, "class") : NULL;
            int ok = cls && cJSON_IsString(cls) &&
                     strcmp(cls->valuestring, "InvalidParameter") == 0;
            if (parsed) cJSON_Delete(parsed);
            if (ok) { pass++; if (!json_output) printf("  PASS  %s\n", neg[i].desc); }
            else    { fail++; if (!json_output) printf("  FAIL  %s\n", neg[i].desc); }
        }
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
