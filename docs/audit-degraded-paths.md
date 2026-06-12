# Audit: degraded code paths across the agent

**Status:** in-progress audit triggered by vit9696 issue #11 (Tiger 10.4
`network-get-interfaces` returning `[]` and `--upgrade` rolling back
even on success). Posted here so the next pass — and the next bug
report — has a single place to start.

## What an "implementation-test-as-outcome-test" looks like

The bug class that escaped v2.5.4 review: a test that asserts the
agent "did not return an error" passes even when the agent returns
data that is structurally valid but semantically wrong — empty arrays
where the caller expected content, default values where measurements
were promised, zeros where counters should have advanced.

A correct test asks the question the operator would ask:
**"would a human reading this response say 'yes, that answers my
question'?"** If the answer is "no, but the JSON is well-formed,"
the test is testing the wrong thing.

`tests/outcome-sweep.sh` is the harness that asks the right question.

## Audited subsystems

### `src/cmd-network.c` — Tiger short-circuits

Originally three "return empty thing" branches keyed on Darwin 8.x.
All three have been replaced with real implementations in
v2.5.5-dev. See `tiger_get_interfaces_ioctl()` (SIOCGIFCONF +
per-name SIOCGIFFLAGS/SIOCGIFNETMASK + AF_LINK MAC extraction) and
`tiger_get_routes_sysctl()` (sysctl(NET_RT_DUMP, AF_INET)) and
`tiger_add_stats_from_iflist()` (sysctl(NET_RT_IFLIST) for
per-interface counters). All three bypass libc's hanging
`getifaddrs()` / slow daemon-context `popen()` paths by going
straight to the kernel via the BSD primitives `ifconfig(8)` /
`netstat(1)` themselves use.

**Boundary:** the Tiger paths handle IPv4 only. Tiger 10.4 ships
IPv6 disabled by default; the IPv6 ioctl story is messy on Tiger
and adding an untested path would re-introduce the "looks correct
but isn't" failure mode. If an operator running Tiger with IPv6
manually enabled needs IPv6 here, they should open an issue with a
repro and the v2.5.6 work can pull in the AF_INET6 ioctl pass.

### `src/cmd-hardware.c` — memory-stats fallback

`get_vm_stat()` prefers the Mach `host_statistics64` API (10.6+) and
falls back to parsing `vm_stat` text output on Tiger/Leopard. Tiger's
`vm_stat` reports:
- `Pages free` / `active` / `inactive` — populated (real values).
- `Pages wired down` — matched by the `"Pages wired"` prefix in the
  parser; populated.
- `Pages speculative` / `Pages purgeable` / `Pages stored in compressor`
  — not present in Tiger's vm_stat (those fields were added in 10.5,
  10.6, 10.9 respectively). Parser leaves them at 0.

**Verdict:** the zeros are honest — those features did not exist on
Tiger. Not a lie. No action.

### `src/cmd-power.c` — multi-fork `pmset` parsing

`pmset -g | grep hibernatemode | awk '{print $2}'` runs three forks.
On Tiger from daemon context that's 600-1500 ms. Slow but bounded;
the response is the actual `hibernatemode` value. Not deceptive.

**Action:** acceptable. If we later observe PVE timeouts on this
command on Tiger, rewrite as direct IOPMrootDomain query.

### `src/cmd-disk.c` — `diskutil list` shell-out

`run_command_capture("diskutil list", &out)`. `diskutil` exists on
Tiger. Real disk list is returned. Slow first invocation (fork +
exec from daemon context) but bounded. Not deceptive.

**Action:** acceptable.

### `src/cmd-fs.c` — `tmutil localsnapshot` shell-out

`run_command_capture("tmutil localsnapshot / 2>&1", &output)`.
`tmutil` was introduced in macOS 10.7. On Tiger/Leopard this
command path is not reached (the caller checks the OS version
elsewhere) — and if it is, run_command_capture returns non-zero
and the handler surfaces a proper error. Not deceptive.

**Action:** none.

### `src/service.c` — Tiger libc `_realpath` weak-link

Tiger 10.4 libSystem exports only the unversioned `_realpath`,
which our code paths around at file resolution. No silent fallback;
errors surface as errors.

**Action:** none.

### `src/relauncher.c` — i386 vs x86_64 re-exec on Tiger/Leopard

Re-execs the correct slice. Code path is "load right architecture
or fail loudly" — not "silently degrade to wrong arch." Not
deceptive.

**Action:** none.

## Process gap to close

The v2.5.4 sweep matrix gave ✅ marks based on `rc=0`. That accepted
the empty-array Tiger responses as passing. The new
`tests/outcome-sweep.sh` requires:

- `network-get-interfaces` to contain ≥1 interface with both a
  matching MAC and at least one non-zero IPv4 address.
- `network-get-route` to contain ≥1 default route (destination
  `0.0.0.0`) with a non-empty gateway.
- `get-cpustats[0]` to have `user >= 0` and `idle >= 0`.
- `get-memory-block-info` to have a meaningful `size`.
- etc.

The harness must be run against every supported OS as part of
release-readiness — not just BAM, as v2.5.4's lifecycle item 17
limited itself to. `tests/lifecycle-test.sh` is the per-VM driver
that bundles upgrade-verify, outcome-sweep, uninstall, and
reinstall into one go/no-go pass.

## Next-pass items

- Audit `cmd-exec.c` / `cmd-file.c` for similar "succeeds with empty
  result when it shouldn't" paths. Quick read suggests they're
  error-explicit already, but worth a careful read.
- Audit `cmd-ssh.c` — the `guest-ssh-get-authorized-keys` command
  returns an empty object on missing-file. That may be correct (no
  keys is a valid state), or may be the kind of "looks fine but
  isn't" pattern we just fixed. Decide via a test that creates a
  known key, reads it back, and asserts the read matches what was
  written.
