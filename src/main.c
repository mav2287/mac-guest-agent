#include "agent.h"
#include "cmd-fs.h"
#include "commands.h"
#include "compat.h"
#include "log.h"
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
 *   -m, --method METHOD     Transport: virtio-serial (default)
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
 *   -D, --dump-conf         Print effective configuration
 *   -h, --help              Show help
 */

#define DEFAULT_CONFIG_PATH "/etc/qemu/qemu-ga.conf"
#define DEFAULT_METHOD      "virtio-serial"
#define DEFAULT_PIDFILE     "/var/run/qemu-ga.pid"

typedef struct {
    int         daemonize;
    int         verbose;
    int         test_mode;
    int         do_install;
    int         do_uninstall;
    int         dump_conf;
    const char *update_path;
    const char *method;
    const char *path;
    const char *logfile;
    const char *pidfile;
    const char *config_path;
    const char *block_rpcs;
    const char *allow_rpcs;
} config_t;

static agent_t *g_agent = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    if (g_agent)
        agent_stop(g_agent);
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
        else if (strcmp(key, "method") == 0)
            cfg->method = safe_strdup(val);
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
    printf("method = %s\n", cfg->method ? cfg->method : DEFAULT_METHOD);
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
    printf("  -d, --daemonize        Run as daemon\n");
    printf("  -m, --method METHOD    Transport method [default: virtio-serial]\n");
    printf("  -p, --path PATH        Device/socket path [default: auto-detect]\n");
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
    printf("      --install          Install as LaunchDaemon service\n");
    printf("      --uninstall        Uninstall LaunchDaemon service\n");
    printf("      --update PATH      Update binary from local file\n");
    printf("\nConfig file format (%s):\n", DEFAULT_CONFIG_PATH);
    printf("  [general]\n");
    printf("  path = /dev/cu.serial1\n");
    printf("  logfile = %s\n", LOG_PATH);
    printf("  verbose = 0\n");
}

static struct option long_options[] = {
    {"daemonize",  no_argument,       NULL, 'd'},
    {"method",     required_argument, NULL, 'm'},
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
    while ((opt = getopt_long(argc, argv, "dm:p:l:f:vVb:a:c:Dth", long_options, NULL)) != -1) {
        if (opt == 'c') cfg.config_path = optarg;
    }

    /* Load config file */
    parse_config_file(cfg.config_path, &cfg);

    /* Parse CLI args again (CLI overrides config file) */
    optind = 1;
    while ((opt = getopt_long(argc, argv, "dm:p:l:f:vVb:a:c:Dth", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd': cfg.daemonize = 1; break;
        case 'm': cfg.method = optarg; break;
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
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (cfg.dump_conf) { dump_config(&cfg); return 0; }
    if (cfg.update_path) return service_update(cfg.update_path);
    if (cfg.do_install) return service_install();
    if (cfg.do_uninstall) return service_uninstall();

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
     * it just tries to open the virtio-serial device and fails if
     * it's not there. We do the same. macOS QEMU VMs typically use
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

    int rc = agent_run(g_agent);

    agent_destroy(g_agent);
    g_agent = NULL;

    /* Clean up PID file */
    if (cfg.pidfile)
        unlink(cfg.pidfile);

    log_close();
    return rc;
}
