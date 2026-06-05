/*
 * relauncher.h — first-thing-in-main bridge for legacy macOS.
 *
 * See src/relauncher.c for the full rationale. In short: when the x86_64
 * slice is loaded on Tiger or Leopard (Darwin kernel < 10), we re-exec
 * the i386 slice via lipo + execv, because CF/IOKit are weak-linked and
 * absent on Tiger and dyld can't bind dyld_info on Leopard. On Snow
 * Leopard and onwards, this is a no-op. On the i386 and arm64 slices,
 * it is a compile-time no-op.
 */

#ifndef MGA_RELAUNCHER_H
#define MGA_RELAUNCHER_H

/* Call as the very first thing in main(). If the current architecture is
 * x86_64 and the running kernel is Darwin < 10 (i.e. pre-10.6 Snow
 * Leopard), this function extracts the i386 slice via `/usr/bin/lipo`
 * and `execv`s it, replacing the current process. On success it never
 * returns. On failure it prints an error and calls exit(1). On modern
 * macOS (Darwin >= 10) or non-x86_64 architectures, it returns
 * immediately as a no-op.
 *
 * NOTE: On the x86_64 slice this is ALSO installed as a constructor with
 * priority 101 (the earliest user-defined priority that ld accepts; 0-100
 * are reserved). The constructor runs after dyld has loaded libSystem +
 * bound all symbols, but BEFORE main(). On Tiger this is critical because
 * some code paths in main()'s prologue (config parsing, getopt) reference
 * libSystem helpers that pass through ABI shims absent on 10.4 — calling
 * the relauncher from a constructor escapes them entirely. The from-main
 * call remains as a defense-in-depth no-op (relauncher has already exec'd
 * by the time main runs on Tiger, so the from-main call is a fall-through
 * on every supported platform). */
void relaunch_as_i386_if_legacy_macos(int argc, char **argv);

#endif /* MGA_RELAUNCHER_H */
