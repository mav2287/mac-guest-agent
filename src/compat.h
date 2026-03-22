#ifndef MGA_COMPAT_H
#define MGA_COMPAT_H

#include <sys/types.h>

typedef struct {
    int major;
    int minor;
    int patch;
} os_version_t;

void compat_init(void);
const os_version_t *compat_os_version(void);
int compat_has_compressed_memory(void);
int compat_has_apfs(void);
int compat_has_tmutil(void);
int compat_cloexec(int fd);
char *compat_strndup(const char *s, size_t n);

#endif
