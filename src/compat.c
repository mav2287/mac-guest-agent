#include "compat.h"
#include "util.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static os_version_t cached_version = {0, 0, 0};
static int version_detected = 0;

void compat_init(void)
{
    if (version_detected)
        return;

    char *output = NULL;
    if (run_command_capture("sw_vers -productVersion", &output) == 0 && output) {
        char *p = output;
        cached_version.major = (int)strtol(p, &p, 10);
        if (*p == '.') p++;
        cached_version.minor = (int)strtol(p, &p, 10);
        if (*p == '.') p++;
        cached_version.patch = (int)strtol(p, &p, 10);
        free(output);
    }
    version_detected = 1;
}

const os_version_t *compat_os_version(void)
{
    if (!version_detected)
        compat_init();
    return &cached_version;
}

int compat_has_compressed_memory(void)
{
    const os_version_t *v = compat_os_version();
    /* Memory compression was introduced in OS X 10.9 Mavericks */
    return (v->major > 10) || (v->major == 10 && v->minor >= 9);
}

int compat_has_apfs(void)
{
    const os_version_t *v = compat_os_version();
    /* APFS introduced in macOS 10.13 High Sierra */
    return (v->major > 10) || (v->major == 10 && v->minor >= 13);
}

int compat_has_tmutil(void)
{
    /* tmutil available since 10.7 Lion, localsnapshot since 10.13 */
    if (!compat_has_apfs()) return 0;
    struct stat st;
    return (stat("/usr/bin/tmutil", &st) == 0 && (st.st_mode & S_IXUSR));
}

int compat_cloexec(int fd)
{
    if (fd < 0)
        return -1;
    /* O_CLOEXEC available since 10.7; use fcntl for all versions */
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

char *compat_strndup(const char *s, size_t n)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    if (len > n)
        len = n;
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}
