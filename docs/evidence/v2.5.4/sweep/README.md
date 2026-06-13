> **⚠️ SUPERSEDED — DO NOT TREAT AS GREEN.**
>
> This sweep was assembled before issue #11 (2026-06-07).
> Two material gaps surfaced after it was filed:
>
> 1. **Tiger network commands**: the ✅ marks on rows 29 and 30
>    ("guest-network-get-interfaces" and "guest-network-get-route" on
>    Tiger) reflect the v2.5.4 short-circuit returning an empty array
>    without errors — they were implementation-tests, not outcome-tests.
>    A correct sweep would have asserted "response contains at least
>    one interface with a non-zero IPv4 address" and FAILED on Tiger.
>    See `tests/outcome-sweep.sh` for what those assertions should look
>    like and `src/cmd-network.c` `tiger_get_interfaces_ioctl()` /
>    `tiger_get_routes_sysctl()` for the real fix.
>
> 2. **Lifecycle coverage**: item 17 below ("Install/uninstall
>    idempotency") was run against BAM-Xserve ONLY. Tiger, Leopard, and
>    Snow Leopard never had `--install` / `--upgrade` / `--uninstall`
>    exercised. The upgrade verify rollback on Tiger was hit immediately
>    because the standard-mode verify gave the daemon `sleep(1)` —
>    Tiger's `launchctl list` shell-out itself takes 200-500 ms from
>    daemon context, leaving an effective budget under one second.
>    Caught by widening to a 10-iter poll loop. See
>    `tests/lifecycle-test.sh` for the regression harness.
>
> Outcome-based re-sweep is in progress. Treat everything below as
> historical until that re-sweep lands.

## v2.5.4 release-readiness sweep evidence (2026-06-05/06)

Comprehensive 4-VM validation against the v2.5.4 universal artifact
(`build/mac-guest-agent-universal`, SHA-256 of the binary at the time of the
sweep is recorded in each per-VM evidence file). This sweep was run between
~22:00 2026-06-05 and ~01:30 2026-06-06 against Proxmox VE 9.1.1 / pc-q35-6.1
/ Penryn / OpenCore + HfsPlusLegacy.efi guests.

### Coverage matrix

The four VMs covered:

| VMID | Name         | macOS         | Kernel arch    | Slice selected by `lipo -thin` | Notes |
|------|--------------|---------------|----------------|--------------------------------|-------|
| 107  | BAM-Xserve   | 10.11.6       | x86_64         | x86_64                         | Real-world stripped Xserve3,1 install with MacBookPro3,1 SMBIOS spoof. SSH bridged on REDACTED-NET via vmbr0. |
| 111  | Tiger        | 10.4.11       | i386 (RELEASE_I386) | **x86_64** (XNU grade_binary picks x86_64 on EM64T even on i386 kernel)  | Slirp NAT, hostfwd 22111. SMBIOS iMac19,1 (pinned by Tiger OC quirks). |
| 112  | Leopard      | 10.5.8        | i386 (RELEASE_I386) | **i386** (XNU grade_binary correctly picks i386 on non-Xserve SMBIOS) | Slirp NAT, hostfwd 22112. SMBIOS MacBookPro3,1. APM→GPT-repacked install media. |
| 113  | Snow Leopard | 10.6.8 (10K549) | x86_64 (RELEASE_X86_64, 64-bit kernel) | x86_64 | Slirp NAT, hostfwd 22113. SMBIOS MacPro3,1. APM→GPT-repacked install media. |

### Test categories validated (PASS on all 4 VMs unless noted)

1. **Self-test JSON** — selected_arch reported correctly per VM, 19-20 passes / 0-1 warnings / 0 errors. The single warning on 10.4 and 10.5 is the absent `tmutil` (Time Machine CLI is a 10.7+ tool).
2. **PVE-whitelisted QGA commands (16)** — ping, get-time, get-osinfo, get-host-name, get-vcpus, get-memory-block-info, get-memory-blocks, get-fsinfo, network-get-interfaces, get-users, get-timezone, info, fsfreeze-status, fsfreeze-freeze, fsfreeze-thaw, fstrim.
3. **Raw QGA commands (7)** — guest-get-load, guest-get-disks, guest-get-diskstats, guest-get-cpustats, guest-get-hostname, guest-network-get-route, guest-fsfreeze-freeze-list. Driven via socat against `/var/run/qemu-server/<vmid>.qga`.
4. **File CRUD (6)** — guest-file-open / write / flush / seek / read / close. Round-trip verified: written payload b64-decodes to expected content.
5. **SSH key CRUD (3)** — guest-ssh-add-authorized-keys + verify-presence + remove + verify-absence.
6. **Exec — basic + status polling** — `qm guest exec --synchronous 0 -- /bin/sleep 4`, status @1s shows running, status @6s shows exitcode 0.
7. **Exec — env var pass-through** — `env: ["FOO=bar-baz"]`, child stdout decodes to "bar-baz".
8. **Exec — capture-output=false** — exec returns pid, status has no `out-data`/`err-data` fields. Confirms agent honors the spec.
9. **Exec — ENOEXEC fallback** — created a no-shebang executable script, exec correctly retried via `/bin/sh path args` and produced the expected output. Validates the v2.5.4 `exec_child_image()` shell-retry path.
10. **Exec — signaled child** — `kill -TERM $$` inside the child correctly surfaces as `exitcode: -1, signal: 15` in the status response.
11. **`guest-fsfreeze-freeze-list`** — selective freeze of just `/` returns the right frozen-mount count, `fsfreeze-status` reports `frozen`.
12. **Frozen-state command rejection gate** — during freeze, `guest-ping` (allowlisted) succeeds; `guest-get-osinfo` (not allowlisted) returns `"Command not allowed while filesystem is frozen"`. Codex Test 6 PASS.
13. **`/etc/qemu/fsfreeze-hook.d/*` hooks** — installed a hook script, freeze+thaw, log file shows two timestamped entries.
14. **Stress — 30 sequential pings** — 30/30 PASS at ~1.23 s per ping (PVE-side per-ping overhead, not agent latency).
15. **Stress — 100 sequential pings (Tiger + BAM)** — 100/100 PASS each in ~123 s.
16. **Stress — 100 freeze/thaw cycles (all 4 VMs)** — 100/100 PASS each in ~269-290 s. Final state `thawed` confirms no state leak between cycles.
17. **Install/uninstall idempotency (BAM)** — install when already installed → idempotent; uninstall → clean removal; reinstall → working ping; uninstall when not installed → no-op success. 7 scenarios PASS.

### v2.5.4 commits (in order on `main`)

1. `4d2917d` — fix(load): x86_64 slice now loads on Tiger 10.4.7+ (issue #9)
2. `e8db739` — fix(channel): treat read==0 on isa-serial as EAGAIN, not EOF (issue #10)
3. `f0fae8f` — chore(v2.5.4): VERSION bump + CHANGELOG + Tiger setup guide + evidence
4. `7883573` — fix(exec): bypass i386 libc execvp wrapper for absolute paths
5. `6490a99` — log(watchdog): drop Tiger-specific wording, downgrade WARN → INFO
6. `0822407` — docs(COMPATIBILITY): runtime evidence drop for v2.5.4 across 10.4-10.11

### Open caveats

- **Tiger mixed-command chardev wedging**: 15-20 commands of MIXED types in rapid succession (e.g. info + fsfreeze-freeze + fsfreeze-thaw + get-disks + get-diskstats) can wedge PVE's host-side QGA chardev proxy state (the agent INSIDE the VM stays healthy and processing). 100 commands of the SAME type (100 pings, 100 freeze/thaw cycles) do not wedge. Recovery: reload the agent via `launchctl unload + load` of `/Library/LaunchDaemons/com.macos.guest-agent.plist`. **Hypothesis** (not yet root-caused): chardev unix-socket backend on PVE has a response-queue state machine that loses sync when response sizes vary widely in rapid succession. **Not a v2.5.4 release blocker** — every real workload pattern we tested (verify.sh, monitoring scrapes, backup freeze cycles) is uniform-command per phase.
- **`qm guest exec` 64KB output cap** — PVE's `qm guest exec` wrapper truncates output at 64KB regardless of the agent's 16MB `MAX_CAPTURE_SIZE`. The agent's truncation handling is exercised by the unit + proactive + fuzz tests in `make test`. Real-world workloads needing > 64KB output should use raw QGA via socat or use file-* commands.
- **`guest-set-vcpus` / `guest-set-memory-blocks`** — registered in the command table with `enabled=0` (unsupported on macOS — XNU has no hot-plug). Return `CommandNotFound` to callers, which is the correct QGA-spec behavior for disabled commands. Could be clearer in self-test output (currently says "45 registered" without noting 2 are disabled). Cosmetic only.
<!-- (hook arg caveat removed: the agent DOES pass action as argv[1] per src/cmd-fs.c:162, our test script had a heredoc-quoting bug that ate $1 in the writer shell) -->
