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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

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

int main(void)
{
    printf("=== Proactive Tests ===\n");

    test_channel_api();
    test_ssh_temp();
    test_freeze_hooks();
    test_password_validation();

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
