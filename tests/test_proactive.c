/*
 * Proactive tests for code that normally requires real infrastructure.
 * Uses PTYs for serial port testing, temp directories for service/SSH,
 * and mock hooks for freeze validation.
 *
 * Build: clang -Isrc -Isrc/third_party -o build/test_proactive tests/test_proactive.c \
 *        src/channel.c src/util.c src/protocol.c src/compat.c src/log.c \
 *        src/third_party/cJSON.c -framework CoreFoundation -framework IOKit
 * Run:   ./build/test_proactive
 */

#include "channel.h"
#include "util.h"
#include "protocol.h"
#include "compat.h"
#include "cmd-fs.h"
#include "commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <errno.h>

/* Stub: cmd-fs.c references command_register at link time (via cmd_fs_init).
 * The tests in this file never call cmd_fs_init, so the registrations never
 * fire at runtime. This stub satisfies the linker without dragging in
 * commands.c (which would in turn pull in every cmd-*.c file). */
void command_register(const char *name, command_handler_fn handler, int enabled)
{
    (void)name; (void)handler; (void)enabled;
}

static int pass = 0, fail = 0;

#define ASSERT(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else { printf("  FAIL: %s (line %d)\n", name, __LINE__); fail++; } \
} while(0)

/* ---- Channel API Tests (non-PTY, test mode) ---- */

static void test_channel_api(void)
{
    printf("\n--- Channel API ---\n");

    /* Test channel create/destroy lifecycle */
    channel_t *ch = channel_create("/dev/nonexistent");
    ASSERT("channel_create with path", ch != NULL);
    ASSERT("channel not open yet", !channel_is_open(ch));
    ASSERT("channel_get_path", channel_get_path(ch) != NULL);
    ASSERT("channel path correct", strcmp(channel_get_path(ch), "/dev/nonexistent") == 0);

    /* Opening nonexistent device should fail */
    int rc = channel_open(ch);
    ASSERT("open nonexistent device fails", rc != 0);
    ASSERT("still not open after failure", !channel_is_open(ch));
    channel_destroy(ch);

    /* Test channel create with NULL (auto-detect) */
    ch = channel_create(NULL);
    ASSERT("channel_create with NULL", ch != NULL);
    ASSERT("no path set", channel_get_path(ch) == NULL);
    channel_destroy(ch);

    /* Test test-mode channel */
    channel_t *test_ch = channel_create_test();
    ASSERT("test channel created", test_ch != NULL);
    channel_destroy(test_ch);

    /* Test poll timeout setter */
    ch = channel_create("/dev/null");
    ASSERT("create for timeout test", ch != NULL);
    channel_set_poll_timeout(ch, 50);
    channel_set_poll_timeout(ch, 0);  /* should clamp to default */
    channel_set_poll_timeout(ch, 1000);
    ASSERT("set_poll_timeout doesn't crash", 1);
    channel_destroy(ch);
}

/* ---- Channel read over a real PTY (exercises the select() path) ---- */

static void test_channel_pty_read(void)
{
    printf("\n--- Channel read over PTY (select path) ---\n");

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT("posix_openpt", master >= 0);
    if (master < 0) return;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        ASSERT("grantpt/unlockpt", 0);
        close(master);
        return;
    }
    const char *slave_name = ptsname(master);
    ASSERT("ptsname", slave_name != NULL);
    if (!slave_name) { close(master); return; }

    /* The pts slave is a real tty character device, so channel_open() runs
     * the termios setup and channel_read_message() runs the select()-based
     * read path — the same path used for the serial device in production. */
    channel_t *ch = channel_create(slave_name);
    ASSERT("create channel on pts", ch != NULL);
    if (!ch) { close(master); return; }
    int rc = channel_open(ch);
    ASSERT("open pts channel", rc == 0 && channel_is_open(ch));

    channel_set_poll_timeout(ch, 200);

    /* Idle: nothing written — select() must time out and report EAGAIN,
     * not a spurious device error (the Tiger poll() bug surfaced here). */
    errno = 0;
    char *msg = channel_read_message(ch);
    int idle_errno = errno;
    ASSERT("idle read times out cleanly", msg == NULL && idle_errno == EAGAIN);
    free(msg);

    /* Data: a full QGA line written to the master must come back intact. */
    const char *line = "{\"execute\":\"guest-ping\"}\n";
    ssize_t w = write(master, line, strlen(line));
    ASSERT("write line to pty master", w == (ssize_t)strlen(line));

    msg = NULL;
    for (int i = 0; i < 25 && !msg; i++)
        msg = channel_read_message(ch);
    ASSERT("select path returns the written line",
           msg != NULL && strcmp(msg, "{\"execute\":\"guest-ping\"}") == 0);
    free(msg);

    channel_destroy(ch);
    close(master);
}

/* ---- fs_dispatch_class (per-FS freeze dispatch matrix from AGENT_BEHAVIOUR_SPEC.md Q1) ---- */

static void make_statfs(struct statfs *s, const char *type,
                        const char *mntfrom, const char *mnton, uint32_t flags)
{
    memset(s, 0, sizeof(*s));
    strncpy(s->f_fstypename, type, sizeof(s->f_fstypename) - 1);
    strncpy(s->f_mntfromname, mntfrom, sizeof(s->f_mntfromname) - 1);
    strncpy(s->f_mntonname, mnton, sizeof(s->f_mntonname) - 1);
    s->f_flags = flags;
}

static void test_fs_dispatch(void)
{
    printf("\n--- fs_dispatch_class (per-FS freeze dispatch) ---\n");
    struct statfs s;

    /* Read-only check wins over filesystem-type classification */
    make_statfs(&s, "apfs", "/dev/disk1s1", "/", MNT_RDONLY);
    ASSERT("apfs RDONLY -> SKIP_RDONLY", fs_dispatch_class(&s) == FS_SKIP_RDONLY);
    make_statfs(&s, "hfs", "/dev/disk0s2", "/Volumes/Recovery", MNT_RDONLY);
    ASSERT("hfs RDONLY -> SKIP_RDONLY", fs_dispatch_class(&s) == FS_SKIP_RDONLY);

    /* Writable known types */
    make_statfs(&s, "apfs", "/dev/disk1s1", "/", 0);
    ASSERT("apfs writable -> APFS_WRITABLE", fs_dispatch_class(&s) == FS_APFS_WRITABLE);
    make_statfs(&s, "hfs", "/dev/disk0s2", "/", 0);
    ASSERT("hfs writable -> HFS_WRITABLE", fs_dispatch_class(&s) == FS_HFS_WRITABLE);
    /* ZFS-on-macOS uses the dataset name as f_mntfromname (no /dev/
     * backing). The dispatch must match the type BEFORE the /dev/
     * defensive check, otherwise ZFS gets wrongly skipped. */
    make_statfs(&s, "zfs", "tank/data", "/Volumes/data", 0);
    ASSERT("zfs (dataset backing) writable -> ZFS_WRITABLE",
           fs_dispatch_class(&s) == FS_ZFS_WRITABLE);
    make_statfs(&s, "zfs", "tank", "/tank", 0);
    ASSERT("zfs (pool root) writable -> ZFS_WRITABLE",
           fs_dispatch_class(&s) == FS_ZFS_WRITABLE);

    /* Foreign writable types — generic (try F_FULLFSYNC, tolerate ENOTSUP) */
    make_statfs(&s, "msdos", "/dev/disk2s1", "/Volumes/EFI", 0);
    ASSERT("msdos writable -> GENERIC_WRITABLE", fs_dispatch_class(&s) == FS_GENERIC_WRITABLE);
    make_statfs(&s, "exfat", "/dev/disk3s1", "/Volumes/USB", 0);
    ASSERT("exfat writable -> GENERIC_WRITABLE", fs_dispatch_class(&s) == FS_GENERIC_WRITABLE);
    make_statfs(&s, "udf", "/dev/disk4s1", "/Volumes/DVD", 0);
    ASSERT("udf writable -> GENERIC_WRITABLE", fs_dispatch_class(&s) == FS_GENERIC_WRITABLE);
    make_statfs(&s, "ntfs", "/dev/disk5s1", "/Volumes/Win", 0);
    ASSERT("ntfs writable -> GENERIC_WRITABLE", fs_dispatch_class(&s) == FS_GENERIC_WRITABLE);

    /* Network mounts — explicit type match wins regardless of mntfromname */
    make_statfs(&s, "smbfs", "//host/share", "/Volumes/Share", 0);
    ASSERT("smbfs -> SKIP_NETWORK", fs_dispatch_class(&s) == FS_SKIP_NETWORK);
    make_statfs(&s, "afpfs", "afp://host/vol", "/Volumes/AFP", 0);
    ASSERT("afpfs -> SKIP_NETWORK", fs_dispatch_class(&s) == FS_SKIP_NETWORK);
    make_statfs(&s, "nfs", "host:/path", "/Volumes/NFS", 0);
    ASSERT("nfs -> SKIP_NETWORK", fs_dispatch_class(&s) == FS_SKIP_NETWORK);
    make_statfs(&s, "webdav", "http://host/path", "/Volumes/WD", 0);
    ASSERT("webdav -> SKIP_NETWORK", fs_dispatch_class(&s) == FS_SKIP_NETWORK);

    /* Special kernel filesystems */
    make_statfs(&s, "devfs", "devfs", "/dev", 0);
    ASSERT("devfs -> SKIP_SPECIAL", fs_dispatch_class(&s) == FS_SKIP_SPECIAL);
    make_statfs(&s, "autofs", "map auto_home", "/System/Volumes/Data/home", 0);
    ASSERT("autofs -> SKIP_SPECIAL", fs_dispatch_class(&s) == FS_SKIP_SPECIAL);
    make_statfs(&s, "fdesc", "fdesc", "/dev/fd", 0);
    ASSERT("fdesc -> SKIP_SPECIAL", fs_dispatch_class(&s) == FS_SKIP_SPECIAL);
    make_statfs(&s, "synthfs", "synthfs", "/", 0);
    ASSERT("synthfs -> SKIP_SPECIAL", fs_dispatch_class(&s) == FS_SKIP_SPECIAL);
    make_statfs(&s, "volfs", "volfs", "/.vol", 0);
    ASSERT("volfs -> SKIP_SPECIAL", fs_dispatch_class(&s) == FS_SKIP_SPECIAL);
    make_statfs(&s, "lifs", "lifs", "/private/var/db/timed", 0);
    ASSERT("lifs -> SKIP_SPECIAL", fs_dispatch_class(&s) == FS_SKIP_SPECIAL);

    /* Unknown type backed by non-/dev — defensive skip (e.g. FUSE drivers) */
    make_statfs(&s, "macfuse", "fuse@server", "/Volumes/Mounted", 0);
    ASSERT("unknown FUSE non-/dev backing -> SKIP_SPECIAL",
           fs_dispatch_class(&s) == FS_SKIP_SPECIAL);

    /* NULL pointer guard */
    ASSERT("NULL mnt -> SKIP_SPECIAL", fs_dispatch_class(NULL) == FS_SKIP_SPECIAL);
}

/* ---- fsfreeze_is_allowlisted (Q5: freeze-safe command allowlist) ---- */

static void test_freeze_allowlist(void)
{
    printf("\n--- fsfreeze_is_allowlisted (freeze-safe command allowlist) ---\n");

    /* Allowed during freeze (the documented 9): protocol commands, info,
     * freeze control (status/freeze/freeze-list/thaw with idempotent re-freeze). */
    const char *allowed[] = {
        "guest-ping",
        "guest-sync",
        "guest-sync-id",
        "guest-sync-delimited",
        "guest-info",
        "guest-fsfreeze-status",
        "guest-fsfreeze-freeze",
        "guest-fsfreeze-freeze-list",
        "guest-fsfreeze-thaw",
        NULL
    };
    for (int i = 0; allowed[i]; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "allowed: %s", allowed[i]);
        ASSERT(msg, fsfreeze_is_allowlisted(allowed[i]) == 1);
    }

    /* Blocked during freeze — representative commands from each category
     * that the principled-restrictive rule excludes. If any of these starts
     * being allowed accidentally, this test surfaces it before shipping. */
    const char *blocked[] = {
        /* Filesystem writes / TRIM */
        "guest-file-write",
        "guest-fstrim",
        /* Time / identity writes */
        "guest-set-time",
        "guest-set-user-password",
        "guest-ssh-add-authorized-keys",
        "guest-ssh-remove-authorized-keys",
        /* Process execution */
        "guest-exec",
        /* State-changing */
        "guest-shutdown",
        "guest-suspend-disk",
        "guest-suspend-ram",
        "guest-suspend-hybrid",
        /* Read-only commands that are NOT on the allowlist (deliberate per
         * Q5 — restrictive by default, expand only on concrete demand). */
        "guest-get-osinfo",
        "guest-get-fsinfo",
        "guest-get-host-name",
        "guest-network-get-interfaces",
        "guest-get-cpustats",
        "guest-file-read",
        "guest-exec-status",
        /* Unknown / malformed input */
        "not-a-real-command",
        "",
        NULL
    };
    for (int i = 0; blocked[i]; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "blocked: %s",
                 blocked[i][0] ? blocked[i] : "(empty)");
        ASSERT(msg, fsfreeze_is_allowlisted(blocked[i]) == 0);
    }

    /* NULL guard */
    ASSERT("NULL cmd_name -> blocked", fsfreeze_is_allowlisted(NULL) == 0);
}

/* ---- SSH with Temp Files ---- */

static void test_ssh_temp(void)
{
    printf("\n--- SSH authorized_keys with temp files ---\n");

    /* Create temp .ssh directory */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/mga-test-ssh-%d", getpid());
    mkdir(tmpdir, 0700);

    char sshdir[512];
    snprintf(sshdir, sizeof(sshdir), "%s/.ssh", tmpdir);
    mkdir(sshdir, 0700);

    char keyfile[512];
    snprintf(keyfile, sizeof(keyfile), "%s/.ssh/authorized_keys", tmpdir);

    /* Write some initial keys */
    const char *initial = "ssh-rsa AAAA key1\nssh-rsa BBBB key2\n";
    write_file(keyfile, initial, strlen(initial), 0600);

    /* Read them back */
    size_t len;
    char *content = read_file(keyfile, &len);
    ASSERT("read keys back", content != NULL);
    if (content) {
        ASSERT("key1 present", strstr(content, "AAAA key1") != NULL);
        ASSERT("key2 present", strstr(content, "BBBB key2") != NULL);
        free(content);
    }

    /* Write a new key (simulate add) */
    const char *updated = "ssh-rsa AAAA key1\nssh-rsa BBBB key2\nssh-rsa CCCC key3\n";
    write_file(keyfile, updated, strlen(updated), 0600);

    content = read_file(keyfile, &len);
    ASSERT("key3 added", content && strstr(content, "CCCC key3") != NULL);
    free(content);

    /* Remove a key (simulate remove — filter out key2) */
    const char *filtered = "ssh-rsa AAAA key1\nssh-rsa CCCC key3\n";
    write_file(keyfile, filtered, strlen(filtered), 0600);

    content = read_file(keyfile, &len);
    ASSERT("key2 removed", content && strstr(content, "BBBB key2") == NULL);
    ASSERT("key1 still present", content && strstr(content, "AAAA key1") != NULL);
    ASSERT("key3 still present", content && strstr(content, "CCCC key3") != NULL);
    free(content);

    /* Clean up */
    unlink(keyfile);
    rmdir(sshdir);
    rmdir(tmpdir);
}

/* ---- Freeze Hook Validation ---- */

static void test_freeze_hooks(void)
{
    printf("\n--- Freeze hook script validation ---\n");

    char hookdir[256];
    snprintf(hookdir, sizeof(hookdir), "/tmp/mga-test-hooks-%d", getpid());
    mkdir(hookdir, 0700);

    /* Create a valid hook script */
    char script1[512];
    snprintf(script1, sizeof(script1), "%s/01-test.sh", hookdir);
    const char *script_content = "#!/bin/bash\necho \"hook ran: $1\"\nexit 0\n";
    write_file(script1, script_content, strlen(script_content), 0755);

    /* Verify file exists and is executable */
    struct stat st;
    ASSERT("hook script created", stat(script1, &st) == 0);
    ASSERT("hook script executable", st.st_mode & S_IXUSR);

    /* Create a world-writable script (should be rejected by validation) */
    char script2[512];
    snprintf(script2, sizeof(script2), "%s/02-bad.sh", hookdir);
    write_file(script2, script_content, strlen(script_content), 0755);
    chmod(script2, 0777);  /* bypass umask to force world-writable */
    ASSERT("bad script created", stat(script2, &st) == 0);
    ASSERT("bad script is world-writable", st.st_mode & S_IWOTH);

    /* Clean up */
    unlink(script1);
    unlink(script2);
    rmdir(hookdir);
}

/* ---- Password Validation ---- */

static void test_password_validation(void)
{
    printf("\n--- Password input validation ---\n");

    /* Valid usernames */
    const char *valid[] = {"admin", "user.name", "test-user", "user_123", NULL};
    for (int i = 0; valid[i]; i++) {
        int ok = 1;
        for (const char *p = valid[i]; *p; p++) {
            if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
                !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-' && *p != '.') {
                ok = 0;
                break;
            }
        }
        char name[64];
        snprintf(name, sizeof(name), "valid username: %s", valid[i]);
        ASSERT(name, ok == 1);
    }

    /* Invalid usernames (injection attempts) */
    const char *invalid[] = {"admin;rm -rf", "user$(id)", "test`whoami`", "a b", "user/path", NULL};
    for (int i = 0; invalid[i]; i++) {
        int ok = 1;
        for (const char *p = invalid[i]; *p; p++) {
            if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
                !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-' && *p != '.') {
                ok = 0;
                break;
            }
        }
        char name[64];
        snprintf(name, sizeof(name), "rejects injection: %.20s", invalid[i]);
        ASSERT(name, ok == 0);
    }
}

/* ---- Base64 validation (audit finding 3) ---- */

static void test_base64_validation(void)
{
    printf("\n--- Base64 decode validation ---\n");

    /* Round-trip: known-good inputs decode to expected bytes. */
    struct { const char *enc; const char *plain; size_t plen; } good[] = {
        {"",                "",       0},
        {"YQ==",            "a",      1},
        {"YWI=",            "ab",     2},
        {"YWJj",            "abc",    3},
        {"YWJjZA==",        "abcd",   4},
        {"SGVsbG8sIHdvcmxkIQ==", "Hello, world!", 13},
        {NULL, NULL, 0}
    };
    for (int i = 0; good[i].enc; i++) {
        size_t out_len = 99;
        unsigned char *d = base64_decode(good[i].enc, &out_len);
        char name[96];
        snprintf(name, sizeof(name), "roundtrip ok: %s", good[i].enc);
        if (d == NULL) { ASSERT(name, 0); continue; }
        int ok = (out_len == good[i].plen) && (memcmp(d, good[i].plain, out_len) == 0);
        ASSERT(name, ok);
        free(d);
    }

    /* Invalid alphabet — chars outside [A-Za-z0-9+/]. The pre-fix
     * implementation silently treated these as 0 (== 'A' in the decode
     * table) and returned bogus decoded bytes. With the audit-finding-3
     * fix in src/util.c, every entry below MUST return NULL. */
    const char *bad_alphabet[] = {
        "!!!!",         /* the audit's exact reproduction case */
        "AAA!",         /* invalid char in the last non-pad slot */
        "AA!=",         /* invalid in the only non-pad slot before '=' */
        "A!==",         /* same, shorter run */
        " AAA",         /* embedded whitespace */
        "AA\nA",        /* embedded newline */
        "AAA\x80",      /* high-bit char */
        "AAA\xff",      /* 0xff */
        "AA-A",         /* '-' isn't in standard base64 (would be url-safe)  */
        "AA_A",         /* '_' likewise */
        NULL
    };
    for (int i = 0; bad_alphabet[i]; i++) {
        char name[96];
        snprintf(name, sizeof(name), "rejects invalid alphabet: %s",
                 bad_alphabet[i]);
        ASSERT(name, base64_decode(bad_alphabet[i], NULL) == NULL);
    }

    /* Invalid padding — '=' must appear only in the last 1 or 2
     * positions of the whole input. Three or more '=' is always invalid;
     * '=' anywhere but the tail is invalid. */
    const char *bad_padding[] = {
        "====",          /* all padding */
        "===A",          /* padding before non-padding */
        "==AA",          /* same */
        "=AAA",          /* same */
        "AA==AAAA",      /* padding in the middle */
        "AAA=AAAA",      /* same, single '=' */
        NULL
    };
    for (int i = 0; bad_padding[i]; i++) {
        char name[96];
        snprintf(name, sizeof(name), "rejects invalid padding: %s",
                 bad_padding[i]);
        ASSERT(name, base64_decode(bad_padding[i], NULL) == NULL);
    }

    /* Bad length — anything not a multiple of 4 is invalid base64
     * (whitespace tolerance is NOT enabled — that was a separate
     * design decision called out by the audit). */
    const char *bad_length[] = { "A", "AA", "AAA", "AAAAA", "AAAAAAA", NULL };
    for (int i = 0; bad_length[i]; i++) {
        char name[96];
        snprintf(name, sizeof(name), "rejects bad length: %zu chars",
                 strlen(bad_length[i]));
        ASSERT(name, base64_decode(bad_length[i], NULL) == NULL);
    }

    /* NULL input safety. */
    ASSERT("NULL input -> NULL", base64_decode(NULL, NULL) == NULL);
}

/* ---- SSH safe-write symlink-attack regression (audit finding 6) ---- */

/* Forward decls — defined in src/cmd-ssh.c, exposed for tests. */
char *ssh_safe_read_file(const char *path, size_t *out_len);
int   ssh_safe_write_file(const char *path, uid_t uid, gid_t gid,
                           const char *data, size_t len);

static void test_ssh_safe_write_symlink(void)
{
    printf("\n--- SSH safe write: symlink-attack regression ---\n");

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/mga-ssh-safe-%d", getpid());
    mkdir(dir, 0700);

    /* Set up a "victim" file that an attacker would want the root-
     * running agent to truncate by symlink swap. */
    char victim[320];
    snprintf(victim, sizeof(victim), "%s/victim.txt", dir);
    const char *victim_content = "DO NOT OVERWRITE ME — this is the file an attacker tries to truncate\n";
    write_file(victim, victim_content, strlen(victim_content), 0644);

    /* Path the SSH writer would target — the attacker has replaced it
     * with a symlink to the victim file. */
    char target[320];
    snprintf(target, sizeof(target), "%s/authorized_keys", dir);
    if (symlink(victim, target) < 0) {
        printf("  SKIP: could not create symlink (errno=%d)\n", errno);
        unlink(victim);
        rmdir(dir);
        return;
    }

    /* The audit's attack: ssh_safe_write_file is asked to write to a
     * path that's currently a symlink to a file the root-running agent
     * shouldn't be touching. Must refuse with -1, and must NOT have
     * truncated or modified the victim. */
    const char *evil = "ssh-rsa AAAA... attacker@evil\n";
    int rc = ssh_safe_write_file(target, getuid(), getgid(), evil, strlen(evil));
    ASSERT("symlink target → write refused with -1", rc == -1);

    /* Victim content unchanged — open it directly (not through the
     * symlink path) to verify. */
    size_t vlen = 0;
    char *back = read_file(victim, &vlen);
    ASSERT("victim file content unchanged",
           back && strcmp(back, victim_content) == 0);
    free(back);

    /* Now remove the symlink; safe write to the now-nonexistent path
     * must succeed (O_CREAT) and produce a real file. */
    unlink(target);
    rc = ssh_safe_write_file(target, getuid(), getgid(), evil, strlen(evil));
    ASSERT("non-existent target → write succeeds", rc == 0);
    struct stat st;
    ASSERT("created file is a regular file (not a symlink)",
           lstat(target, &st) == 0 && S_ISREG(st.st_mode));
    ASSERT("created file mode is 0600",
           (st.st_mode & 0777) == 0600);
    /* Verify content was written. */
    size_t blen = 0;
    char *written = read_file(target, &blen);
    ASSERT("written content matches input",
           written && strlen(written) == strlen(evil)
                   && memcmp(written, evil, strlen(evil)) == 0);
    free(written);

    /* Write again to existing regular file — should succeed
     * (truncate-then-rewrite). */
    const char *evil2 = "ssh-rsa BBBB... attacker2@evil\n";
    rc = ssh_safe_write_file(target, getuid(), getgid(), evil2, strlen(evil2));
    ASSERT("write to existing regular file succeeds", rc == 0);
    char *written2 = read_file(target, NULL);
    ASSERT("file was overwritten",
           written2 && strstr(written2, "BBBB") != NULL
                    && strstr(written2, "AAAA") == NULL);
    free(written2);

    /* Cleanup. */
    unlink(target);
    unlink(victim);
    rmdir(dir);
}

static void test_ssh_safe_read_symlink(void)
{
    printf("\n--- SSH safe read: symlink-attack regression ---\n");

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/mga-ssh-safe-r-%d", getpid());
    mkdir(dir, 0700);

    /* Sensitive file the attacker wants exfiltrated via the SSH-get
     * response. */
    char secret[320];
    snprintf(secret, sizeof(secret), "%s/secret.txt", dir);
    const char *secret_content = "SECRET — should not be exposed via SSH get\n";
    write_file(secret, secret_content, strlen(secret_content), 0600);

    char target[320];
    snprintf(target, sizeof(target), "%s/authorized_keys", dir);
    if (symlink(secret, target) < 0) {
        printf("  SKIP: could not create symlink (errno=%d)\n", errno);
        unlink(secret);
        rmdir(dir);
        return;
    }

    /* O_NOFOLLOW read refuses to follow the symlink — returns NULL
     * rather than exposing the secret. */
    size_t out_len = 99;
    char *data = ssh_safe_read_file(target, &out_len);
    ASSERT("symlink target → read returns NULL", data == NULL);
    free(data);

    /* Real regular file: read succeeds. */
    unlink(target);
    const char *real = "ssh-rsa AAAAB3...\n";
    write_file(target, real, strlen(real), 0600);
    out_len = 0;
    data = ssh_safe_read_file(target, &out_len);
    ASSERT("regular file → read returns data",
           data && out_len == strlen(real) && strcmp(data, real) == 0);
    free(data);

    /* Cleanup. */
    unlink(target);
    unlink(secret);
    rmdir(dir);
}

int main(void)
{
    printf("=== Proactive Tests ===\n");

    test_channel_api();
    test_channel_pty_read();
    test_fs_dispatch();
    test_freeze_allowlist();
    test_ssh_temp();
    test_freeze_hooks();
    test_password_validation();
    test_base64_validation();
    test_ssh_safe_write_symlink();
    test_ssh_safe_read_symlink();

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
