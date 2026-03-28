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
#include <signal.h>
#include <time.h>

#define MAX_PROCESSES 64
#define MAX_CAPTURE_SIZE (16 * 1024 * 1024)  /* 16MB, matches Linux qemu-ga */

typedef struct {
    int     in_use;
    int     pid;        /* internal PID (not OS PID) */
    pid_t   real_pid;
    int     exited;
    int     exit_code;
    int     wait_status; /* raw wait status for WIFSIGNALED etc. */
    char   *out_data;   /* base64 encoded */
    char   *err_data;   /* base64 encoded */
    time_t  start_time;
} exec_process_t;

static exec_process_t process_table[MAX_PROCESSES];
static int next_pid = 1;

static exec_process_t *alloc_process(void)
{
    /* Clean up old entries (>30 min) */
    time_t now = time(NULL);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].in_use && process_table[i].exited &&
            now - process_table[i].start_time > 1800) {
            free(process_table[i].out_data);
            free(process_table[i].err_data);
            memset(&process_table[i], 0, sizeof(exec_process_t));
        }
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].in_use)
            return &process_table[i];
    }
    return NULL;
}

static exec_process_t *find_process(int pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].in_use && process_table[i].pid == pid)
            return &process_table[i];
    }
    return NULL;
}

static cJSON *handle_exec(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'path' argument";
        return NULL;
    }

    cJSON *arg_arr = cJSON_GetObjectItemCaseSensitive(args, "arg");
    cJSON *capture_item = cJSON_GetObjectItemCaseSensitive(args, "capture-output");
    int capture = cJSON_IsTrue(capture_item);

    /* Build argv */
    int argc = 1;
    if (cJSON_IsArray(arg_arr))
        argc += cJSON_GetArraySize(arg_arr);

    char **argv = calloc((size_t)(argc + 1), sizeof(char *));
    if (!argv) {
        *err_class = "GenericError";
        *err_desc = "Memory allocation failed";
        return NULL;
    }

    argv[0] = path_item->valuestring;
    if (cJSON_IsArray(arg_arr)) {
        int idx = 1;
        cJSON *item;
        cJSON_ArrayForEach(item, arg_arr) {
            if (cJSON_IsString(item))
                argv[idx++] = item->valuestring;
        }
    }

    exec_process_t *proc = alloc_process();
    if (!proc) {
        free(argv);
        *err_class = "GenericError";
        *err_desc = "Too many running processes";
        return NULL;
    }

    /* Set up pipes for output capture */
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (capture) {
        if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
            free(argv);
            if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
            if (err_pipe[0] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
            *err_class = "GenericError";
            *err_desc = "Failed to create pipes for output capture";
            return NULL;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(argv);
        if (capture) {
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
        }
        *err_class = "GenericError";
        *err_desc = "fork() failed";
        return NULL;
    }

    if (pid == 0) {
        /* Child */
        setsid();
        if (capture) {
            close(out_pipe[0]);
            close(err_pipe[0]);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(out_pipe[1]);
            close(err_pipe[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }

        /* Set environment if provided (use setenv which copies the string) */
        cJSON *env_arr = cJSON_GetObjectItemCaseSensitive(args, "env");
        if (cJSON_IsArray(env_arr)) {
            cJSON *env_item;
            cJSON_ArrayForEach(env_item, env_arr) {
                if (cJSON_IsString(env_item) && env_item->valuestring) {
                    char *eq = strchr(env_item->valuestring, '=');
                    if (eq) {
                        *eq = '\0';
                        setenv(env_item->valuestring, eq + 1, 1);
                        *eq = '=';
                    }
                }
            }
        }

        execvp(path_item->valuestring, argv);
        _exit(127);
    }

    /* Parent */
    free(argv);

    proc->in_use = 1;
    proc->pid = next_pid++;
    proc->real_pid = pid;
    proc->exited = 0;
    proc->exit_code = 0;
    proc->out_data = NULL;
    proc->err_data = NULL;
    proc->start_time = time(NULL);

    if (capture) {
        close(out_pipe[1]);
        close(err_pipe[1]);

        /* Read stdout */
        char *stdout_buf = NULL;
        size_t stdout_len = 0;
        {
            size_t cap = 4096, len = 0;
            char *buf = malloc(cap);
            ssize_t n;
            while (buf && len < MAX_CAPTURE_SIZE &&
                   (n = read(out_pipe[0], buf + len, cap - len - 1)) > 0) {
                len += (size_t)n;
                if (len + 1 >= cap && cap < MAX_CAPTURE_SIZE) {
                    cap *= 2;
                    if (cap > MAX_CAPTURE_SIZE) cap = MAX_CAPTURE_SIZE;
                    char *tmp = realloc(buf, cap);
                    if (!tmp) { free(buf); buf = NULL; break; }
                    buf = tmp;
                }
            }
            close(out_pipe[0]);
            if (buf) { buf[len] = '\0'; stdout_buf = buf; stdout_len = len; }
        }

        /* Read stderr */
        char *stderr_buf = NULL;
        size_t stderr_len = 0;
        {
            size_t cap = 4096, len = 0;
            char *buf = malloc(cap);
            ssize_t n;
            while (buf && len < MAX_CAPTURE_SIZE &&
                   (n = read(err_pipe[0], buf + len, cap - len - 1)) > 0) {
                len += (size_t)n;
                if (len + 1 >= cap && cap < MAX_CAPTURE_SIZE) {
                    cap *= 2;
                    if (cap > MAX_CAPTURE_SIZE) cap = MAX_CAPTURE_SIZE;
                    char *tmp = realloc(buf, cap);
                    if (!tmp) { free(buf); buf = NULL; break; }
                    buf = tmp;
                }
            }
            close(err_pipe[0]);
            if (buf) { buf[len] = '\0'; stderr_buf = buf; stderr_len = len; }
        }

        /* Wait for child */
        int status;
        waitpid(pid, &status, 0);
        proc->exited = 1;
        proc->wait_status = status;
        proc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        if (stdout_buf && stdout_len > 0)
            proc->out_data = base64_encode((unsigned char *)stdout_buf, stdout_len);
        if (stderr_buf && stderr_len > 0)
            proc->err_data = base64_encode((unsigned char *)stderr_buf, stderr_len);

        free(stdout_buf);
        free(stderr_buf);
    }

    LOG_INFO("Executed process: %s (internal pid=%d, real pid=%d)",
             path_item->valuestring, proc->pid, (int)pid);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "pid", proc->pid);
    return result;
}

static cJSON *handle_exec_status(cJSON *args, const char **err_class, const char **err_desc)
{
    cJSON *pid_item = cJSON_GetObjectItemCaseSensitive(args, "pid");
    if (!cJSON_IsNumber(pid_item)) {
        *err_class = "InvalidParameter";
        *err_desc = "Missing 'pid' argument";
        return NULL;
    }

    int pid = (int)pid_item->valuedouble;
    exec_process_t *proc = find_process(pid);
    if (!proc) {
        *err_class = "InvalidParameter";
        *err_desc = "Invalid PID";
        return NULL;
    }

    /* Check if process has exited (if we haven't captured yet) */
    if (!proc->exited) {
        int status;
        pid_t w = waitpid(proc->real_pid, &status, WNOHANG);
        if (w > 0) {
            proc->exited = 1;
            proc->wait_status = status;
            proc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "exited", proc->exited);
    if (proc->exited) {
        cJSON_AddNumberToObject(result, "exitcode", proc->exit_code);
        if (proc->out_data)
            cJSON_AddStringToObject(result, "out-data", proc->out_data);
        if (proc->err_data)
            cJSON_AddStringToObject(result, "err-data", proc->err_data);

        /* Use raw wait_status for signal detection, not the extracted exit_code */
        if (WIFSIGNALED(proc->wait_status))
            cJSON_AddNumberToObject(result, "signal", WTERMSIG(proc->wait_status));
    }

    return result;
}

void cmd_exec_init(void)
{
    memset(process_table, 0, sizeof(process_table));
    command_register("guest-exec", handle_exec, 1);
    command_register("guest-exec-status", handle_exec_status, 1);
}
