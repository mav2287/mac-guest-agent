/*
 * relauncher.c — first-thing-in-main bridge for legacy macOS.
 *
 * Why this exists
 * ---------------
 * On macOS 10.4 Tiger and 10.5 Leopard, when the kernel reports the host as
 * 64-bit-capable (`ml_is64bit()` true), XNU's grade_binary picks the x86_64
 * slice from a fat binary in preference to i386 (xnu-792/bsd/dev/i386/
 * kern_machdep.c grades x86_64=2, i386=1). The x86_64 slice then loads, but:
 *
 *   - 10.4 Tiger does not ship x86_64 CoreFoundation/IOKit. Strong-linking
 *     them aborts the load. We work around that by `-weak_framework`-ing
 *     them in the x86_64 build; missing frameworks become NULL at load
 *     time and dyld no longer aborts.
 *   - 10.5 Leopard does have x86_64 frameworks but its dyld (~dyld-95) does
 *     not understand `LC_DYLD_INFO_ONLY`. We work around that by building
 *     the x86_64 slice with `-mmacosx-version-min=10.5` + `ld_classic` +
 *     `crt1.10.5.o`, which emits classic LC_SYMTAB/LC_DYSYMTAB binding
 *     instead of dyld-info.
 *
 * Those two changes mean the x86_64 slice *loads* on Tiger and Leopard.
 * But our agent uses CoreFoundation and IOKit, and on Tiger those are
 * NULL. We cannot execute the normal main() path. So the very first thing
 * main() does is call relaunch_as_i386_if_legacy_macos(): if we detect
 * Darwin kernel < 10 (i.e. < 10.6 Snow Leopard), we extract the i386 slice
 * via /usr/bin/lipo and `execv` it. The i386 slice has full Tiger-era
 * symbol coverage and runs normally.
 *
 * On 10.6 Snow Leopard onwards (Darwin >= 10), this function is a no-op
 * and execution continues normally as x86_64.
 *
 * On the i386 slice and the arm64 slice, this function is a no-op
 * (gated by `#if defined(__x86_64__)`). The i386 slice is only ever
 * selected by kernels that do the right thing already; the arm64 slice
 * runs on Apple Silicon where this whole consideration doesn't apply.
 */

#include "relauncher.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)

#include <mach-o/dyld.h>

/* Darwin kernel major version that corresponds to 10.6 Snow Leopard. */
#define DARWIN_SNOW_LEOPARD 10

/* Resolve the absolute path of the currently-executing binary.
 * Prefers _NSGetExecutablePath (canonical) and falls back to argv[0]. */
static int resolve_self_path(char *out, size_t outsz, const char *argv0) {
    uint32_t sz = (uint32_t)outsz;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        /* Canonical absolute path. */
        return 0;
    }
    /* Buffer was too small for _NSGetExecutablePath. Fall back to argv[0]. */
    if (argv0 && *argv0) {
        if (strlen(argv0) >= outsz) return -1;
        strncpy(out, argv0, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    return -1;
}

/* Return Darwin kernel major version (e.g. 8 for Tiger, 9 for Leopard,
 * 10 for Snow Leopard) or -1 on failure. */
static int darwin_kernel_major(void) {
    char osrelease[64];
    size_t sz = sizeof(osrelease);
    if (sysctlbyname("kern.osrelease", osrelease, &sz, NULL, 0) != 0) {
        return -1;
    }
    osrelease[sizeof(osrelease) - 1] = '\0';
    long v = strtol(osrelease, NULL, 10);
    if (v <= 0 || v > 1000) return -1;
    return (int)v;
}

void relaunch_as_i386_if_legacy_macos(int argc, char **argv) {
    (void)argc;

    /* If we already ran (via the constructor below), main()'s call is
     * a no-op — but on Snow Leopard+ the constructor returns early as a
     * no-op too, so main()'s call must still be authoritative for non-
     * Tiger hosts. */
    int darwin = darwin_kernel_major();
    if (darwin < 0) {
        /* Can't determine — proceed as x86_64. If we crash from null CF/IOKit
         * later, the operator will see that error; we don't want to silently
         * exit on a working modern host because of a transient sysctl issue. */
        return;
    }
    if (darwin >= DARWIN_SNOW_LEOPARD) {
        return; /* 10.6 Snow Leopard and onwards: keep running as x86_64. */
    }

    /* Tiger (Darwin 8) or Leopard (Darwin 9). We must re-exec the i386 slice
     * because CF/IOKit are NULL (weak-linked) and the agent calls them. */

    char self_path[PATH_MAX];
    if (resolve_self_path(self_path, sizeof(self_path),
                          (argc > 0) ? argv[0] : NULL) != 0) {
        fprintf(stderr,
                "mac-guest-agent: relauncher cannot determine self path\n");
        exit(1);
    }

    /* Pick a unique temp path for the i386-thin binary. Use mkstemp to
     * reserve a name, then unlink before lipo writes to avoid a race. */
    char thin_path[] = "/tmp/.mga-i386.XXXXXX";
    int fd = mkstemp(thin_path);
    if (fd < 0) {
        fprintf(stderr,
                "mac-guest-agent: relauncher mkstemp failed: %s\n",
                strerror(errno));
        exit(1);
    }
    close(fd);
    unlink(thin_path); /* lipo refuses to overwrite an existing target. */

    /* Run `/usr/bin/lipo -thin i386 <self_path> -output <thin_path>` via
     * fork+execv (NOT system()). _NSGetExecutablePath() returns whatever
     * Apple allows in HFS+/APFS paths, which includes characters that
     * /bin/sh would interpret as metacharacters (backticks, $(), even
     * inside double quotes — backslash-escapes can also break out of
     * "..." quoting). Passing the arguments as a vector to execv bypasses
     * shell parsing entirely, so the path is inert. */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr,
                "mac-guest-agent: relauncher fork failed: %s\n",
                strerror(errno));
        unlink(thin_path);
        exit(1);
    }
    if (pid == 0) {
        /* Child: redirect lipo's stderr to our stderr so failures are
         * still visible, then exec lipo with the arg vector. */
        char *const lipo_argv[] = {
            "lipo",
            "-thin", "i386",
            self_path,
            "-output", thin_path,
            NULL,
        };
        execv("/usr/bin/lipo", lipo_argv);
        /* execv only returns on failure; use _exit (not exit) in child
         * to avoid running atexit handlers from the parent's state. */
        fprintf(stderr,
                "mac-guest-agent: relauncher child execv lipo failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    /* Parent: wait for lipo to finish. */
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        fprintf(stderr,
                "mac-guest-agent: relauncher waitpid failed: %s\n",
                strerror(errno));
        unlink(thin_path);
        exit(1);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "mac-guest-agent: relauncher lipo exited %d (signal=%d) "
                "source=%s; the binary at %s may not include an i386 slice\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                self_path, self_path);
        unlink(thin_path);
        exit(1);
    }

    if (chmod(thin_path, 0700) != 0) {
        fprintf(stderr,
                "mac-guest-agent: relauncher chmod failed: %s\n",
                strerror(errno));
        unlink(thin_path);
        exit(1);
    }

    /* execv replaces the current process; on success it never returns. */
    execv(thin_path, argv);

    /* execv only returns on failure. */
    fprintf(stderr,
            "mac-guest-agent: relauncher execv(%s) failed: %s\n",
            thin_path, strerror(errno));
    unlink(thin_path);
    exit(1);
}

/* Constructor wrapper — runs after dyld has bound symbols but BEFORE main().
 *
 * On Tiger the x86_64 slice exhibits a process-init crash if anything in
 * main()'s prologue (config parsing, getopt_long initialization) reaches
 * into a libSystem code path that has a non-trivial ABI shim removed in
 * 10.4. By moving the relaunch trigger to a constructor we exec the i386
 * slice before *any* user code runs in main(), eliminating the exposure.
 *
 * We retrieve argc/argv via the Darwin-specific _NSGetArgc()/_NSGetArgv()
 * APIs (declared in <crt_externs.h>) — these are populated by libdyld during
 * process init, before C++ static initializers and before constructors.
 * If they return sane values, relauncher proceeds normally and execs the
 * i386 slice; on success the constructor never returns (process is
 * replaced). On modern macOS the relauncher function itself short-circuits
 * (Darwin >= 10) so the constructor is a cheap no-op.
 *
 * Priority 101 is the lowest user-visible constructor priority (0-100 are
 * reserved by the toolchain for runtime initialization). Setting it
 * explicitly ensures this runs BEFORE any other constructors the agent
 * might gain in the future (currently zero, but defense-in-depth). */
#include <crt_externs.h>

__attribute__((constructor(101)))
static void relauncher_ctor(void) {
    int *argcp = _NSGetArgc();
    char ***argvp = _NSGetArgv();
    if (!argcp || !argvp || !*argvp) {
        /* Dyld didn't populate the process args yet — fall through to the
         * from-main call. Shouldn't happen at constructor time, but if it
         * does we don't want to crash here. */
        return;
    }
    relaunch_as_i386_if_legacy_macos(*argcp, *argvp);
    /* If we returned, we're on Snow Leopard+ — let main() take over
     * normally. The from-main call will be a no-op on this path. */
}

#else /* !__x86_64__ */

void relaunch_as_i386_if_legacy_macos(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* i386 slice: already i386, no re-exec needed. Arm64: not relevant to
     * x86_64-vs-i386 grading. Both: no-op. */
}

#endif
