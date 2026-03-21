#include "util.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>

int run_command_capture(const char *cmd, char **output)
{
    if (output)
        *output = NULL;

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(fp);
        return -1;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                pclose(fp);
                return -1;
            }
            buf = tmp;
        }
    }
    buf[len] = '\0';

    int status = pclose(fp);
    if (output) {
        *output = buf;
    } else {
        free(buf);
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

int run_command(const char *cmd)
{
    return run_command_capture(cmd, NULL);
}

int run_command_v(const char *path, char *const argv[], char **out_buf, size_t *out_len)
{
    int pipefd[2];
    if (out_buf) {
        *out_buf = NULL;
        if (out_len) *out_len = 0;
        if (pipe(pipefd) < 0)
            return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (out_buf) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        /* Child */
        setsid();
        if (out_buf) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(path, argv);
        _exit(127);
    }

    /* Parent */
    if (out_buf) {
        close(pipefd[1]);

        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        ssize_t n;
        while (buf && (n = read(pipefd[0], buf + len, cap - len - 1)) > 0) {
            len += (size_t)n;
            if (len + 1 >= cap) {
                cap *= 2;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(buf); buf = NULL; break; }
                buf = tmp;
            }
        }
        close(pipefd[0]);

        if (buf) {
            buf[len] = '\0';
            *out_buf = buf;
            if (out_len) *out_len = len;
        }
    }

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* Base64 tables */
static const char b64_enc_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const unsigned char b64_dec_table[256] = {
    ['A']=0,  ['B']=1,  ['C']=2,  ['D']=3,  ['E']=4,  ['F']=5,  ['G']=6,
    ['H']=7,  ['I']=8,  ['J']=9,  ['K']=10, ['L']=11, ['M']=12, ['N']=13,
    ['O']=14, ['P']=15, ['Q']=16, ['R']=17, ['S']=18, ['T']=19, ['U']=20,
    ['V']=21, ['W']=22, ['X']=23, ['Y']=24, ['Z']=25,
    ['a']=26, ['b']=27, ['c']=28, ['d']=29, ['e']=30, ['f']=31, ['g']=32,
    ['h']=33, ['i']=34, ['j']=35, ['k']=36, ['l']=37, ['m']=38, ['n']=39,
    ['o']=40, ['p']=41, ['q']=42, ['r']=43, ['s']=44, ['t']=45, ['u']=46,
    ['v']=47, ['w']=48, ['x']=49, ['y']=50, ['z']=51,
    ['0']=52, ['1']=53, ['2']=54, ['3']=55, ['4']=56, ['5']=57, ['6']=58,
    ['7']=59, ['8']=60, ['9']=61, ['+']=62, ['/']=63
};

char *base64_encode(const unsigned char *data, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3, j += 4) {
        unsigned int v = (unsigned int)data[i] << 16;
        if (i + 1 < len) v |= (unsigned int)data[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned int)data[i + 2];

        out[j]     = b64_enc_table[(v >> 18) & 0x3F];
        out[j + 1] = b64_enc_table[(v >> 12) & 0x3F];
        out[j + 2] = (i + 1 < len) ? b64_enc_table[(v >> 6) & 0x3F] : '=';
        out[j + 3] = (i + 2 < len) ? b64_enc_table[v & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

unsigned char *base64_decode(const char *input, size_t *out_len)
{
    if (!input) return NULL;
    size_t in_len = strlen(input);
    if (in_len % 4 != 0) return NULL;

    size_t decoded_len = in_len / 4 * 3;
    if (in_len > 0 && input[in_len - 1] == '=') decoded_len--;
    if (in_len > 1 && input[in_len - 2] == '=') decoded_len--;

    unsigned char *out = malloc(decoded_len + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < in_len; i += 4) {
        unsigned int v = 0;
        v |= (unsigned int)b64_dec_table[(unsigned char)input[i]]     << 18;
        v |= (unsigned int)b64_dec_table[(unsigned char)input[i + 1]] << 12;
        v |= (unsigned int)b64_dec_table[(unsigned char)input[i + 2]] << 6;
        v |= (unsigned int)b64_dec_table[(unsigned char)input[i + 3]];

        if (j < decoded_len) out[j++] = (unsigned char)(v >> 16);
        if (j < decoded_len) out[j++] = (unsigned char)(v >> 8);
        if (j < decoded_len) out[j++] = (unsigned char)v;
    }
    out[decoded_len] = '\0';
    if (out_len) *out_len = decoded_len;
    return out;
}

char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) { fclose(fp); return NULL; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)size, fp);
    buf[n] = '\0';
    fclose(fp);

    if (out_len) *out_len = n;
    return buf;
}

int write_file(const char *path, const char *data, size_t len, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        written += (size_t)n;
    }
    close(fd);
    return 0;
}

char *safe_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

char *str_trim(char *s)
{
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}
