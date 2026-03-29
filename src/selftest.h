#ifndef MGA_SELFTEST_H
#define MGA_SELFTEST_H

/* Run self-test diagnostics (environment checks).
   Returns 0 if all checks pass, 1 if any fail.
   If json_output is non-zero, emit machine-readable JSON instead of text. */
int selftest_run(int json_output);

/* Run safe-test (read-only command validation).
   Calls each safe command handler and validates the response.
   Returns 0 if all pass, 1 if any fail.
   If json_output is non-zero, emit JSON. */
int safetest_run(int json_output);

#endif
