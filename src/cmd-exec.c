#include "commands.h"
#include "cmd-exec.h"
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

/* Async guest-exec implementation matching the QGA spec contract:
 *
 *   guest-exec        - forks the child, returns {pid: N} immediately.
 *   guest-exec-status - drains any new output, reaps the child via
 *                       waitpid(WNOHANG), returns the current state.
 *
 * The agent main loop (src/agent.c) also calls cmd_exec_drain_all() on
 * every wake-up tick (~1s) so an in-flight child whose caller hasn't
 * polled guest-exec-status recently doesn't have its stdout/stderr pipe
 * fill up (typical macOS pipe buffer is 64 KB — without periodic drain a
 * verbose child blocks on write).
 *
 * This is the same async model as upstream Linux qemu-ga (GLib I/O
 * callbacks off the main loop) and Windows qemu-ga (one reader thread
 * per pipe), without the GLib/threading dependencies — built on POSIX
 * primitives that have been on macOS since 10.0 (fork / pipe / fcntl
 * F_SETFL O_NONBLOCK / waitpid WNOHANG). The CHANGELOG v2.4.3 entry
 * for `guest-exec` documents the prior sync implementation's deadlock +
 * agent-blocking flaws that drove this rewrite.
 */

#define MAX_PROCESSES 64
#define MAX_CAPTURE_SIZE (16 * 1024 * 1024)  /* 16MB, matches Linux qemu-ga */
#define DRAIN_CHUNK 4096                     /* per-read buffer size */

typedef struct {
    int     in_use;
    int     pid;                /* internal PID returned to the caller */
    pid_t   real_pid;           /* OS-level PID */
    int     exited;
    int     exit_code;
    int     wait_status;        /* raw wait status for WIFSIGNALED etc. */

    /* Pipe read ends — set non-blocking once captured. -1 when closed
     * (EOF on read, error, or capture-output was false from the start). */
    int     out_fd;
    int     err_fd;

    /* Accumulated raw output. Grown on demand up to MAX_CAPTURE_SIZE. */
    char   *out_buf;
    size_t  out_len;
    size_t  out_cap;
    char   *err_buf;
    size_t  err_len;
    size_t  err_cap;

    /* Set to 1 once we've hit MAX_CAPTURE_SIZE and started discarding
     * additional bytes. Surfaces in guest-exec-status. */
    int     out_truncated;
    int     err_truncated;

    time_t  start_time;
} exec_process_t;

static exec_process_t process_table[MAX_PROCESSES];
static int next_pid = 1;

static void exec_child_image(const char *path, char *const argv[])
{
    if (strchr(path, '/')) {
        execv(path, argv);
        if (errno == ENOEXEC) {
            int argc = 0;
            while (argv[argc]) argc++;

            char **sh_argv = calloc((size_t)argc + 2, sizeof(char *));
            if (sh_argv) {
                sh_argv[0] = "/bin/sh";
                sh_argv[1] = (char *)path;
                for (int i = 1; i < argc; i++)
                    sh_argv[i + 1] = argv[i];
                execv("/bin/sh", sh_argv);
            }
        }
    } else {
        execvp(path, argv);
    }

    /* Preserve the real failure reason in captured stderr. */
    int e = errno;
    char msg[512];
    int n = snprintf(msg, sizeof(msg),
                     "mac-guest-agent: exec failed for %s: %s (errno=%d)\n",
                     path, strerror(e), e);
    if (n > 0) {
        size_t len = (n < (int)sizeof(msg)) ? (size_t)n : sizeof(msg) - 1;
        (void)write(STDERR_FILENO, msg, len);
    }
    _exit(127);
}

/* Release any resources owned by a slot and clear it back to defaults. */
static void release_process(exec_process_t *p)
{
    if (p->out_fd >= 0) close(p->out_fd);
    if (p->err_fd >= 0) close(p->err_fd);
    free(p->out_buf);
    free(p->err_buf);
    memset(p, 0, sizeof(*p));
    p->out_fd = -1;
    p->err_fd = -1;
}

static exec_process_t *alloc_process(void)
{
    /* Reap exited slots older than 30 minutes — caller never came back
     * for the status. The cap-then-overwrite is more important than the
     * 30-minute number; it just keeps zombies out of the table. */
    time_t now = time(NULL);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        exec_process_t *p = &process_table[i];
        if (p->in_use && p->exited && now - p->start_time > 1800) {
            release_process(p);
        }
    }

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].in_use) {
            release_process(&process_table[i]);   /* set fds to -1 */
            return &process_table[i];
        }
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

/* Drain one nonblocking pipe fd into the accumulating buffer.
 *
 * Reads in DRAIN_CHUNK-sized chunks until EAGAIN (no more data right
 * now), EOF (closes the fd), or an unrecoverable error (also closes).
 * When the buffer hits MAX_CAPTURE_SIZE, subsequent bytes are read from
 * the pipe (to prevent the child from blocking on write) but discarded;
 * `*truncated` is set so guest-exec-status can surface the flag.
 *
 * Returns silently — caller doesn't need to distinguish "drained some"
 * from "no data right now". After return, *fd is either -1 (closed) or
 * still valid (more data may arrive later).
 */
static void drain_one_fd(int *fd, char **buf, size_t *len, size_t *cap,
                         int *truncated)
{
    if (*fd < 0) return;
    char chunk[DRAIN_CHUNK];
    for (;;) {
        ssize_t n = read(*fd, chunk, sizeof(chunk));
        if (n == 0) {
            close(*fd);
            *fd = -1;
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            /* Real error (EBADF, EIO, etc.) — give up on this fd. */
            close(*fd);
            *fd = -1;
            return;
        }
        /* n > 0 — accept up to MAX_CAPTURE_SIZE, discard the rest. */
        size_t accept = (size_t)n;
        if (*len + accept > MAX_CAPTURE_SIZE) {
            accept = MAX_CAPTURE_SIZE - *len;
            *truncated = 1;
        }
        if (accept > 0) {
            /* Grow buffer geometrically; cap at MAX_CAPTURE_SIZE + 1
             * (the +1 is for the trailing NUL the encoder writes). */
            if (*len + accept + 1 > *cap) {
                size_t new_cap = (*cap > 0) ? *cap : DRAIN_CHUNK;
                while (new_cap < *len + accept + 1) new_cap *= 2;
                if (new_cap > MAX_CAPTURE_SIZE + 1) new_cap = MAX_CAPTURE_SIZE + 1;
                char *t = realloc(*buf, new_cap);
                if (!t) {
                    /* Can't grow — drop what we just read AND mark
                     * truncated so the caller doesn't get a quiet hole. */
                    *truncated = 1;
                    continue;
                }
                *buf = t;
                *cap = new_cap;
            }
            memcpy(*buf + *len, chunk, accept);
            *len += accept;
        }
        /* If we discarded any bytes this iteration, keep draining so the
         * child doesn't block on the next write — but cap is hit, no
         * more memcpy. The next loop iteration will read the next chunk
         * straight to the discard path. */
    }
}

/* Drain a single process's pipes AND reap the child if it has exited.
 * Call from both guest-exec-status (so a poll picks up final data) and
 * cmd_exec_drain_all (so a quiet caller doesn't let pipes back up). */
static void drain_one_process(exec_process_t *p)
{
    drain_one_fd(&p->out_fd, &p->out_buf, &p->out_len, &p->out_cap,
                 &p->out_truncated);
    drain_one_fd(&p->err_fd, &p->err_buf, &p->err_len, &p->err_cap,
                 &p->err_truncated);
    if (!p->exited) {
        int status;
        pid_t w = waitpid(p->real_pid, &status, WNOHANG);
        if (w > 0) {
            p->exited = 1;
            p->wait_status = status;
            p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            /* Final drain after the child is gone — picks up any bytes
             * that landed in the pipe between our last read and exit. */
            drain_one_fd(&p->out_fd, &p->out_buf, &p->out_len, &p->out_cap,
                         &p->out_truncated);
            drain_one_fd(&p->err_fd, &p->err_buf, &p->err_len, &p->err_cap,
                         &p->err_truncated);
        }
    }
}

/* Public: called from src/agent.c main loop on every tick. */
void cmd_exec_drain_all(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].in_use) {
            drain_one_process(&process_table[i]);
        }
    }
}

/* Public: 1 if any captured child still has an open stdout/stderr pipe, i.e.
 * output may still be arriving. The main loop uses this to poll fast while a
 * child is producing — otherwise a verbose child's 64 KB pipe fills and it
 * blocks between the ~1 s idle ticks, throttling capture to ~64 KB/s. */
int cmd_exec_has_pending_output(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].in_use &&
            (process_table[i].out_fd >= 0 || process_table[i].err_fd >= 0))
            return 1;
    }
    return 0;
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

    /* Optional stdin for the child (QGA "input-data": base64). Decode up front
     * so a malformed value fails the call cleanly before we fork. */
    cJSON *input_item = cJSON_GetObjectItemCaseSensitive(args, "input-data");
    unsigned char *input_data = NULL;
    size_t input_len = 0;
    if (input_item) {
        if (!cJSON_IsString(input_item) || !input_item->valuestring) {
            *err_class = "InvalidParameter";
            *err_desc = "'input-data' must be a base64 string";
            return NULL;
        }
        input_data = base64_decode(input_item->valuestring, &input_len);
        if (!input_data) {
            *err_class = "InvalidParameter";
            *err_desc = "'input-data' is not valid base64";
            return NULL;
        }
    }

    /* Build argv */
    int argc = 1;
    if (cJSON_IsArray(arg_arr))
        argc += cJSON_GetArraySize(arg_arr);

    char **argv = calloc((size_t)(argc + 1), sizeof(char *));
    if (!argv) {
        free(input_data);
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
        free(input_data);
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
            free(input_data);
            if (out_pipe[0] >= 0) { close(out_pipe[0]); close(out_pipe[1]); }
            if (err_pipe[0] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
            *err_class = "GenericError";
            *err_desc = "Failed to create pipes for output capture";
            return NULL;
        }
    }

    /* Set up the stdin pipe only when input-data was supplied. */
    int in_pipe[2] = {-1, -1};
    if (input_data) {
        if (pipe(in_pipe) < 0) {
            free(argv);
            free(input_data);
            if (capture) {
                close(out_pipe[0]); close(out_pipe[1]);
                close(err_pipe[0]); close(err_pipe[1]);
            }
            *err_class = "GenericError";
            *err_desc = "Failed to create stdin pipe";
            return NULL;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(argv);
        free(input_data);
        if (capture) {
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
        }
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        *err_class = "GenericError";
        *err_desc = "fork() failed";
        return NULL;
    }

    if (pid == 0) {
        /* Child */
        setsid();

        /* stdin: feed from the input pipe when input-data was supplied,
         * otherwise /dev/null. Without this the child would inherit the
         * agent's stdin — which is the QGA serial device — so a child that
         * reads stdin could steal protocol bytes off the channel. */
        if (input_data) {
            close(in_pipe[1]);
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
        } else {
            int devnull_in = open("/dev/null", O_RDONLY);
            if (devnull_in >= 0) {
                dup2(devnull_in, STDIN_FILENO);
                close(devnull_in);
            }
        }

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

        exec_child_image(path_item->valuestring, argv);
    }

    /* Parent */
    free(argv);

    /* Feed stdin to the child, then close the write end so it sees EOF.
     * SIGPIPE is globally ignored (main.c), so a child that exits without
     * reading turns into a short write we simply stop on rather than a
     * signal. input-data is meant for modest payloads; a blocking write is
     * fine and matches upstream qemu-ga behavior. */
    if (input_data) {
        close(in_pipe[0]);
        size_t off = 0;
        while (off < input_len) {
            ssize_t w = write(in_pipe[1], input_data + off, input_len - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;  /* EPIPE (child gone) or other error — stop feeding */
            }
            off += (size_t)w;
        }
        close(in_pipe[1]);
        free(input_data);
        input_data = NULL;
    }

    proc->in_use = 1;
    proc->pid = next_pid++;
    proc->real_pid = pid;
    proc->exited = 0;
    proc->exit_code = 0;
    proc->start_time = time(NULL);

    if (capture) {
        close(out_pipe[1]);
        close(err_pipe[1]);
        proc->out_fd = out_pipe[0];
        proc->err_fd = err_pipe[0];

        /* Nonblocking — drains will return EAGAIN when no data is ready
         * instead of blocking the agent main loop. */
        int fl;
        if ((fl = fcntl(proc->out_fd, F_GETFL, 0)) >= 0)
            fcntl(proc->out_fd, F_SETFL, fl | O_NONBLOCK);
        if ((fl = fcntl(proc->err_fd, F_GETFL, 0)) >= 0)
            fcntl(proc->err_fd, F_SETFL, fl | O_NONBLOCK);
    }

    LOG_INFO("Spawned process: %s (internal pid=%d, real pid=%d, capture=%d)",
             path_item->valuestring, proc->pid, (int)pid, capture);

    /* Spec contract: return {pid: N} immediately. Output capture and
     * exit-code reaping happen asynchronously via guest-exec-status +
     * the main-loop drain. */
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

    /* Catch up: drain any newly-available output and reap if exited.
     * cmd_exec_drain_all in the main loop also does this once per tick;
     * doing it again here means callers that poll faster than the tick
     * rate get fresh data, and callers that poll slower still see
     * everything before exited:true flips. */
    drain_one_process(proc);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "exited", proc->exited);
    if (proc->exited) {
        cJSON_AddNumberToObject(result, "exitcode", proc->exit_code);

        /* Encode at status-return time rather than at capture time —
         * accumulated buffers are raw bytes; one encode per status call
         * is cheap and lets the truncation flags reflect everything up
         * to and including the final drain. */
        if (proc->out_buf && proc->out_len > 0) {
            char *b64 = base64_encode((unsigned char *)proc->out_buf, proc->out_len);
            if (b64) {
                cJSON_AddStringToObject(result, "out-data", b64);
                free(b64);
            }
        }
        if (proc->err_buf && proc->err_len > 0) {
            char *b64 = base64_encode((unsigned char *)proc->err_buf, proc->err_len);
            if (b64) {
                cJSON_AddStringToObject(result, "err-data", b64);
                free(b64);
            }
        }
        if (proc->out_truncated) cJSON_AddBoolToObject(result, "out-truncated", 1);
        if (proc->err_truncated) cJSON_AddBoolToObject(result, "err-truncated", 1);

        /* Raw wait_status for signal detection — exit_code is -1 for
         * signaled exits but we want the signal number surfaced. */
        if (WIFSIGNALED(proc->wait_status))
            cJSON_AddNumberToObject(result, "signal", WTERMSIG(proc->wait_status));

        /* v2.5.2: release the slot now that the caller has received the
         * terminal status with all captured output. Without this, the 64-
         * entry process_table fills up after 64 short execs even when the
         * caller polls correctly until exited:true — the 30-minute
         * cleanup in alloc_process() only reaps slots after a half-hour
         * idle, so a backup tool / monitoring loop hits "Too many running
         * processes" until that wall passes. Releasing here matches the
         * common "poll until exited, then move on" pattern documented in
         * the QGA usage; subsequent calls to guest-exec-status with the
         * same PID will now return InvalidParameter (the QGA spec does
         * not promise idempotent terminal polling, and we never did
         * either — but the slot was leaking).
         *
         * The 30-minute cleanup in alloc_process() stays as the safety
         * net for callers who launched and never polled at all. */
        release_process(proc);
    }

    return result;
}

void cmd_exec_init(void)
{
    memset(process_table, 0, sizeof(process_table));
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].out_fd = -1;
        process_table[i].err_fd = -1;
    }
    command_register("guest-exec", handle_exec, 1);
    command_register("guest-exec-status", handle_exec_status, 1);
}
