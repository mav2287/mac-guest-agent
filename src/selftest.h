#ifndef MGA_SELFTEST_H
#define MGA_SELFTEST_H

/* Run self-test diagnostics. Returns 0 if all checks pass, 1 if any fail.
   If json_output is non-zero, emit machine-readable JSON instead of text. */
int selftest_run(int json_output);

#endif
