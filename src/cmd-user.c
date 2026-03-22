#include "commands.h"
#include "util.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/*
 * Set user password securely: pipe the password via stdin to avoid
 * exposing it on the command line (visible in ps aux).
 *
 * Uses: dscl . -passwd /Users/<username> <oldpassword> <newpassword>
 * Since we don't know the old password, we use a two-step approach:
 * 1. Use /usr/bin/dscl via OpenDirectory framework through a helper
 * 2. Fallback: write password via pipe to avoid command-line exposure
 */
static int set_password_secure(const char *username, const char *password)
{
    char user_path[256];
    snprintf(user_path, sizeof(user_path), "/Users/%s", username);

    /*
     * Method: fork a child that writes the password to dscl's stdin.
     * We invoke: /usr/bin/dscl . -passwd /Users/<username>
     * When called without the new password argument, dscl reads it
     * from stdin (prompts "New Password:").
     *
     * As root, dscl doesn't require the old password.
     */
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdin from pipe, exec dscl */
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);

        /* Redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        /* dscl . -passwd /Users/<username>
         * When run as root without the password args, reads new password from stdin */
        execl("/usr/bin/dscl", "dscl", ".", "-passwd", user_path, NULL);
        _exit(127);
    }

    /* Parent: write password to pipe, then close */
    close(pipefd[0]);

    /* Write password + newline (dscl expects newline-terminated input) */
    size_t passlen = strlen(password);
    write(pipefd[1], password, passlen);
    write(pipefd[1], "\n", 1);
    /* dscl may prompt for confirmation — send it twice */
    write(pipefd[1], password, passlen);
    write(pipefd[1], "\n", 1);
    close(pipefd[1]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;

    /* Fallback: try sysadminctl which is available on 10.10+ */
    /* sysadminctl also reads from stdin when interactive, but we use -newPassword - */
    /* Actually, sysadminctl -newPassword also shows in ps. Skip it. */

    return -1;
}

static cJSON *handle_set_user_password(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(args, "username");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(args, "password");
    cJSON *crypted_item = cJSON_GetObjectItemCaseSensitive(args, "crypted");

    if (!cJSON_IsString(user_item) || !user_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'username' argument";
        return NULL;
    }
    if (!cJSON_IsString(pass_item) || !pass_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'password' argument";
        return NULL;
    }

    const char *username = user_item->valuestring;
    const char *password = pass_item->valuestring;

    /* If password is base64 encoded (crypted=false means plain base64 in QGA protocol) */
    char *decoded_pass = NULL;
    if (!cJSON_IsTrue(crypted_item)) {
        size_t decoded_len;
        unsigned char *raw = base64_decode(password, &decoded_len);
        if (raw) {
            decoded_pass = (char *)raw;
            password = decoded_pass;
        }
    }

    /* Validate username — prevent injection */
    for (const char *p = username; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') &&
            !(*p >= '0' && *p <= '9') && *p != '_' && *p != '-' && *p != '.') {
            free(decoded_pass);
            *err_class = "InvalidParameter";
            *err_desc = "Invalid username characters";
            return NULL;
        }
    }

    int rc = set_password_secure(username, password);

    /* Zero out password in memory */
    if (decoded_pass) {
        memset(decoded_pass, 0, strlen(decoded_pass));
        free(decoded_pass);
    }

    if (rc != 0) {
        *err_class = "GenericError";
        *err_desc = "Failed to set user password";
        return NULL;
    }

    LOG_DEBUG("Password changed for user %s", username);
    return cJSON_CreateObject();
}

void cmd_user_init(void)
{
    command_register("guest-set-user-password", handle_set_user_password, 1);
}
