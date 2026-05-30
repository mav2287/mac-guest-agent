#include "agent.h"
#include "cmd-fs.h"
#include "commands.h"
#include "compat.h"
#include "log.h"
#include "selftest.h"
#include "service.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

/*
 * CLI flags mirror the Linux qemu-ga where applicable:
 *   -d, --daemonize         Run as daemon
 *   -p, --path PATH         Device path
 *   -l, --logfile PATH      Log file (default: stderr, daemon: /var/log/mac-guest-agent.log)
 *   -f, --pidfile PATH      PID file
 *   -v, --verbose           Debug logging
 *   -V, --version           Show version
 *   -b, --block-rpcs LIST   Comma-separated RPCs to disable
 *   -a, --allow-rpcs LIST   Comma-separated RPCs to allow (allowlist mode)
 *   -c, --config PATH       Config file (default: /etc/qemu/qemu-ga.conf)
 *   -t, --test              Test mode (stdin/stdout, no device needed)
 *       --install            Install as LaunchDaemon
 *       --uninstall          Uninstall LaunchDaemon
 *       --dry-run            With --install/--uninstall/--update: print
 *                              actions without touching filesystem/launchctl
 *                              (v2.5.1+; root check also skipped).
 *   -D, --dump-conf         Print effective configuration
 *   -h, --help              Show help
 */

#define DEFAULT_CONFIG_PATH "/etc/qemu/qemu-ga.conf"
#define DEFAULT_PIDFILE     "/var/run/qemu-ga.pid"

typedef struct {
    int         daemonize;
    int         verbose;
    int         test_mode;
    int         do_install;
    int         do_uninstall;
    int         do_selftest;
    int         selftest_json;
    int         do_safetest;
    int         safetest_json;
    int         dump_conf;
    const char *update_path;
    int         do_upgrade;    /* v2.5.3+: --upgrade (uses self via _NSGetExecutablePath) */
    int         install_virtio;       /* v2.5.3+: --virtio modifier for --install */
    int         install_virtio_force; /* v2.5.3+: --virtio-force modifier for --install */
    int         dry_run;
    const char *path;
    const char *logfile;
    const char *pidfile;
    const char *config_path;
    const char *block_rpcs;
    const char *allow_rpcs;
} config_t;

static agent_t *g_agent = NULL;
static volatile sig_atomic_t g_stop_requested = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

static int is_running_in_qemu(void)
{
    char *model = NULL;
    if (run_command_capture("sysctl -n hw.model", &model) == 0 && model) {
        for (char *p = model; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (strstr(model, "QEMU")) { free(model); return 1; }
        free(model);
    }

    const char *devices[] = {
        "/dev/cu.org.qemu.guest_agent.0",
        "/dev/tty.org.qemu.guest_agent.0",
        "/dev/virtio", "/dev/vda", "/dev/vdb", NULL
    };
    for (int i = 0; devices[i]; i++)
        if (access(devices[i], F_OK) == 0) return 1;

    char *profiler = NULL;
    if (run_command_capture("system_profiler SPHardwareDataType 2>/dev/null", &profiler) == 0 && profiler) {
        for (char *p = profiler; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p -= 32;
        int found = (strstr(profiler, "QEMU") || strstr(profiler, "VIRTUAL"));
        free(profiler);
        if (found) return 1;
    }

    return 0;
}

/* Parse INI-style config file (matches Linux /etc/qemu/qemu-ga.conf format) */
static void parse_config_file(const char *path, config_t *cfg)
{
    char *data = read_file(path, NULL);
    if (!data) return;

    char *save = NULL;
    char *line = strtok_r(data, "\n", &save);
    while (line) {
        /* Skip comments and section headers */
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == ';' || *line == '[' || *line == '\0') {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) { line = strtok_r(NULL, "\n", &save); continue; }

        *eq = '\0';
        char *key = str_trim(line);
        char *val = str_trim(eq + 1);

        if (strcmp(key, "daemonize") == 0)
            cfg->daemonize = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        else if (strcmp(key, "verbose") == 0)
            cfg->verbose = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
        else if (strcmp(key, "method") == 0) {
            /* v2.5.1: the `method` config key is no longer used. The field
             * existed in v2.4.x to express ISA-vs-VirtIO transport intent;
             * v2.5.0 removed VirtIO transport entirely, leaving the field
             * with no behavior to gate (auto / isa-serial were synonyms).
             * The field is removed from the agent in v2.5.1 — see CHANGELOG.
             *
             * To avoid breaking existing /etc/qemu/qemu-ga.conf files that
             * still carry `method = auto` or similar from earlier releases,
             * accept the key and ignore the value with a one-time deprecation
             * notice on stderr. Removing the line entirely is the cleanest
             * fix on the operator side; this branch is the migration ramp. */
            fprintf(stderr,
                "notice: the `method` config key was removed in v2.5.1 and "
                "is ignored. This agent uses ISA serial only (v2.5.0+); "
                "transport selection has no other knobs to set. Remove the "
                "`method = %s` line from %s to silence this notice.\n",
                val, cfg->config_path ? cfg->config_path : DEFAULT_CONFIG_PATH);
        }
        else if (strcmp(key, "path") == 0)
            cfg->path = safe_strdup(val);
        else if (strcmp(key, "logfile") == 0)
            cfg->logfile = safe_strdup(val);
        else if (strcmp(key, "pidfile") == 0)
            cfg->pidfile = safe_strdup(val);
        else if (strcmp(key, "block-rpcs") == 0)
            cfg->block_rpcs = safe_strdup(val);
        else if (strcmp(key, "allow-rpcs") == 0)
            cfg->allow_rpcs = safe_strdup(val);

        line = strtok_r(NULL, "\n", &save);
    }
    free(data);
}

static void dump_config(const config_t *cfg)
{
    printf("[general]\n");
    printf("daemonize = %d\n", cfg->daemonize);
    printf("verbose = %d\n", cfg->verbose);
    printf("path = %s\n", cfg->path ? cfg->path : "(auto-detect)");
    printf("logfile = %s\n", cfg->logfile ? cfg->logfile : LOG_PATH);
    printf("pidfile = %s\n", cfg->pidfile ? cfg->pidfile : DEFAULT_PIDFILE);
    if (cfg->block_rpcs)
        printf("block-rpcs = %s\n", cfg->block_rpcs);
    if (cfg->allow_rpcs)
        printf("allow-rpcs = %s\n", cfg->allow_rpcs);
}

static void write_pidfile(const char *path)
{
    if (!path) return;
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%d\n", getpid());
        fclose(fp);
    }
}

static void print_usage(const char *prog)
{
    printf("macOS QEMU Guest Agent v%s\n\n", AGENT_VERSION);
    printf("Usage: %s [options]\n\n", prog);
    printf("Options (compatible with Linux qemu-ga):\n");
    printf("  -d, --daemonize        Daemonize (log to file; launchd handles backgrounding)\n");
    printf("  -p, --path PATH        Device/socket path [default: auto-detect ISA serial]\n");
    printf("  -l, --logfile PATH     Log file path [default: stderr]\n");
    printf("  -f, --pidfile PATH     PID file path\n");
    printf("  -v, --verbose          Enable debug logging\n");
    printf("  -V, --version          Show version\n");
    printf("  -b, --block-rpcs LIST  Comma-separated RPCs to disable\n");
    printf("  -a, --allow-rpcs LIST  Comma-separated RPCs to allow\n");
    printf("  -c, --config PATH      Config file [default: %s]\n", DEFAULT_CONFIG_PATH);
    printf("  -D, --dump-conf        Print effective configuration\n");
    printf("  -t, --test             Test mode (stdin/stdout)\n");
    printf("  -h, --help             Show this help\n");
    printf("\nmacOS-specific:\n");
    printf("      --install          Install as LaunchDaemon service. Combinable with\n");
    printf("                         --virtio or --virtio-force (see below).\n");
    printf("      --virtio           Modifier for --install: gated VirtIO override install\n");
    printf("                         (macOS 11+, SIP disabled, unloads AppleQEMUGuestAgent,\n");
    printf("                         drops marker for marker-aware --uninstall). Refuses\n");
    printf("                         if an existing install or operator config is present.\n");
    printf("                         Interactive yes/no via /dev/tty. Unsupported config —\n");
    printf("                         see docs/NO_ISA_OVERRIDE.md.\n");
    printf("      --virtio-force     Modifier for --install: VirtIO install with NO safety\n");
    printf("                         checks (no SIP probe, no Apple-agent unload, no prompt).\n");
    printf("                         For experts who have already configured the host\n");
    printf("                         manually. Unsupported.\n");
    printf("      --uninstall        Uninstall LaunchDaemon service. If a VirtIO marker is\n");
    printf("                         present, also removes the override config and reloads\n");
    printf("                         AppleQEMUGuestAgent (mode=full) or leaves it alone\n");
    printf("                         (mode=force). SIP is not re-enabled (operator action).\n");
    printf("      --upgrade          In-place upgrade using the running binary as source.\n");
    printf("                         Run the NEW binary with --upgrade; it self-installs over\n");
    printf("                         the existing one. Detects current install mode, backs up\n");
    printf("                         current binary, regenerates plist, restarts, verifies.\n");
    printf("                         On failure: restores backup. No PATH argument — the\n");
    printf("                         binary knows where it lives.\n");
    printf("      --update PATH      DEPRECATED in v2.5.3+; delegates to --upgrade. Kept for\n");
    printf("                         operators who still need to specify a source path\n");
    printf("                         explicitly. Will be removed in a future release.\n");
    printf("      --self-test        Check environment and report readiness\n");
    printf("      --self-test-json   Same as --self-test but output JSON\n");
    printf("      --safe-test        Validate read-only commands work correctly\n");
    printf("      --safe-test-json   Same as --safe-test but output JSON\n");
    printf("      --dry-run          Combined with --install / --uninstall / --update /\n");
    printf("                         --upgrade: print every action the operation would take\n");
    printf("                         without touching the filesystem or calling launchctl.\n");
    printf("                         Root is also skipped (no privileged ops execute).\n");
    printf("\nConfig file format (%s):\n", DEFAULT_CONFIG_PATH);
    printf("  [general]\n");
    printf("  path = /dev/cu.serial1\n");
    printf("  logfile = %s\n", LOG_PATH);
    printf("  verbose = 0\n");
}

/* Long-only sentinels (no short alias). Values >255 so they don't collide
 * with any single-char short option. */
#define OPT_VIRTIO        0x100
#define OPT_VIRTIO_FORCE  0x101
#define OPT_UPGRADE       0x102

static struct option long_options[] = {
    {"daemonize",  no_argument,       NULL, 'd'},
    {"path",       required_argument, NULL, 'p'},
    {"logfile",    required_argument, NULL, 'l'},
    {"pidfile",    required_argument, NULL, 'f'},
    {"verbose",    no_argument,       NULL, 'v'},
    {"version",    no_argument,       NULL, 'V'},
    {"block-rpcs", required_argument, NULL, 'b'},
    {"allow-rpcs", required_argument, NULL, 'a'},
    {"config",     required_argument, NULL, 'c'},
    {"dump-conf",  no_argument,       NULL, 'D'},
    {"test",       no_argument,       NULL, 't'},
    {"help",       no_argument,       NULL, 'h'},
    {"install",    no_argument,       NULL, 'I'},
    {"uninstall",  no_argument,       NULL, 'U'},
    {"update",     required_argument, NULL, 'u'},
    {"self-test",  no_argument,       NULL, 'S'},
    {"self-test-json", no_argument,  NULL, 'J'},
    {"safe-test",  no_argument,       NULL, 'T'},
    {"safe-test-json", no_argument,  NULL, 'K'},
    /* Long-form only; no short alias. Gates side-effects in
     * service_install / _uninstall / _update (v2.5.1+). */
    {"dry-run",    no_argument,       NULL, 'n'},
    /* v2.5.3+: VirtIO override modifiers for --install + proper --upgrade.
     * The canonical form is lowercase (`--virtio`, matching Linux qemu-ga
     * convention and QEMU docs), but "VirtIO" / "virtIO" capitalization is
     * common in technical writing (the "I/O" in the middle gets emphasized),
     * so accept both case variants as aliases rather than greet the typo
     * with an unrecognized-option error. */
    {"virtio",         no_argument,       NULL, OPT_VIRTIO},
    {"virtIO",         no_argument,       NULL, OPT_VIRTIO},
    {"VirtIO",         no_argument,       NULL, OPT_VIRTIO},
    {"virtio-force",   no_argument,       NULL, OPT_VIRTIO_FORCE},
    {"virtIO-force",   no_argument,       NULL, OPT_VIRTIO_FORCE},
    {"VirtIO-force",   no_argument,       NULL, OPT_VIRTIO_FORCE},
    {"upgrade",        no_argument,       NULL, OPT_UPGRADE},
    /* Legacy long-form aliases */
    {"daemon",     no_argument,       NULL, 'd'},
    {"device",     required_argument, NULL, 'p'},
    {NULL, 0, NULL, 0}
};

int main(int argc, char *argv[])
{
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.config_path = DEFAULT_CONFIG_PATH;

    /* Parse CLI args (first pass: find config path) */
    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "dp:l:f:vVb:a:c:Dth", long_options, NULL)) != -1) {
        if (opt == 'c') cfg.config_path = optarg;
    }

    /* Load config file */
    parse_config_file(cfg.config_path, &cfg);

    /* Parse CLI args again (CLI overrides config file) */
    optind = 1;
    while ((opt = getopt_long(argc, argv, "dp:l:f:vVb:a:c:Dth", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd': cfg.daemonize = 1; break;
        case 'p': cfg.path = optarg; break;
        case 'l': cfg.logfile = optarg; break;
        case 'f': cfg.pidfile = optarg; break;
        case 'v': cfg.verbose = 1; break;
        case 'V': printf("mac-guest-agent %s\n", AGENT_VERSION); return 0;
        case 'b': cfg.block_rpcs = optarg; break;
        case 'a': cfg.allow_rpcs = optarg; break;
        case 'c': break; /* already handled */
        case 'D': cfg.dump_conf = 1; break;
        case 't': cfg.test_mode = 1; break;
        case 'h': print_usage(argv[0]); return 0;
        case 'I': cfg.do_install = 1; break;
        case 'U': cfg.do_uninstall = 1; break;
        case 'u': cfg.update_path = optarg; break;
        case 'S': cfg.do_selftest = 1; break;
        case 'J': cfg.do_selftest = 1; cfg.selftest_json = 1; break;
        case 'T': cfg.do_safetest = 1; break;
        case 'K': cfg.do_safetest = 1; cfg.safetest_json = 1; break;
        case 'n': cfg.dry_run = 1; break;  /* --dry-run, gates service.c side-effects */
        case OPT_VIRTIO:       cfg.install_virtio = 1; break;
        case OPT_VIRTIO_FORCE: cfg.install_virtio_force = 1; break;
        case OPT_UPGRADE:      cfg.do_upgrade = 1; break;
        case '?':
        default:
            /* Unknown option. getopt has already printed its own error
             * ("invalid option -- 'i'" or "unrecognized option `--xyz'").
             * Don't dump full usage — it scrolls the actionable error off
             * the top of the screen. Print a short hint instead. */
            fprintf(stderr, "Try '%s --help' for the full option list.\n", argv[0]);
            fprintf(stderr, "Common typo: --virtio takes TWO dashes. Single-dash "
                            "'-virtio' is parsed as short-option chaining (-v -i -r -t -i -o).\n");
            return 1;
        }
    }

    /* v2.5.3+: --virtio and --virtio-force are mutually exclusive. */
    if (cfg.install_virtio && cfg.install_virtio_force) {
        fprintf(stderr, "Error: --virtio and --virtio-force cannot combine\n");
        return 1;
    }
    /* --virtio[-force] is a modifier for --install only. */
    if ((cfg.install_virtio || cfg.install_virtio_force) && !cfg.do_install) {
        fprintf(stderr, "Error: --virtio / --virtio-force is a modifier for --install. "
                        "Run: sudo %s --install --virtio\n", argv[0]);
        return 1;
    }
    /* --upgrade is its own operation; can't combine with --install or
     * --uninstall or --update. */
    if (cfg.do_upgrade && (cfg.do_install || cfg.do_uninstall || cfg.update_path)) {
        fprintf(stderr, "Error: --upgrade cannot combine with --install / --uninstall / --update\n");
        return 1;
    }

    if (cfg.dump_conf) { dump_config(&cfg); return 0; }
    if (cfg.do_selftest) return selftest_run(cfg.selftest_json);
    if (cfg.do_safetest) return safetest_run(cfg.safetest_json);
    if (cfg.do_upgrade) return service_upgrade(NULL, cfg.dry_run);
    if (cfg.update_path) return service_update(cfg.update_path, cfg.dry_run);
    if (cfg.do_install) {
        install_mode_t mode = INSTALL_MODE_STANDARD;
        if (cfg.install_virtio)       mode = INSTALL_MODE_VIRTIO;
        else if (cfg.install_virtio_force) mode = INSTALL_MODE_VIRTIO_FORCE;
        return service_install(cfg.dry_run, mode);
    }
    if (cfg.do_uninstall) return service_uninstall(cfg.dry_run);

    /* Initialize logging */
    const char *logfile = cfg.logfile;
    if (!logfile && cfg.daemonize)
        logfile = LOG_PATH;
    log_init(logfile, cfg.verbose ? LOG_DEBUG : LOG_INFO);

    LOG_INFO("macOS Guest Agent v%s starting...", AGENT_VERSION);

    /* Initialize compatibility layer */
    compat_init();
    const os_version_t *ver = compat_os_version();
    LOG_INFO("Detected macOS %d.%d.%d", ver->major, ver->minor, ver->patch);

    /* Log QEMU environment detection (informational only).
     * The Linux qemu-ga does NOT gate on environment detection —
     * it just tries to open its configured transport device and fails
     * if it's not there. We do the same. macOS QEMU VMs typically use
     * real Mac hardware models (MacPro6,1 etc.) so hw.model won't
     * say "QEMU" and system_profiler won't say "VIRTUAL". */
    if (!cfg.test_mode) {
        if (is_running_in_qemu()) {
            LOG_INFO("QEMU environment detected");
        } else {
            LOG_INFO("QEMU environment not detected via hw.model/system_profiler (this is normal for macOS VMs with custom hardware models)");
        }
    }

    /* Check root (skip in test mode) */
    if (!cfg.test_mode && geteuid() != 0) {
        LOG_ERROR("Root privileges required. Use sudo.");
        log_close();
        return 1;
    }

    /* Initialize commands */
    commands_init();

    /* Apply RPC filters */
    commands_apply_filters(cfg.block_rpcs, cfg.allow_rpcs);

    /* In test mode, freeze operations are dry-run (don't touch real filesystems) */
    if (cfg.test_mode) {
        fsfreeze_set_test_mode(1);
    }

    /* Write PID file */
    write_pidfile(cfg.pidfile);

    /* Create and run agent */
    g_agent = agent_create(cfg.path, cfg.test_mode);
    if (!g_agent) {
        LOG_FATAL("Failed to create agent");
        log_close();
        return 1;
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    int rc = agent_run(g_agent, &g_stop_requested);

    agent_destroy(g_agent);
    g_agent = NULL;

    /* Clean up PID file */
    if (cfg.pidfile)
        unlink(cfg.pidfile);

    log_close();
    return rc;
}
