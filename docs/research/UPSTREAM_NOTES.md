# Upstream research: QGA spec, Linux reference impl, PVE wrapper behaviour

**Status:** historical reference (Phase 1 research that fed the v2.4.3 design decisions in `../design/AGENT_BEHAVIOUR_SPEC.md`). Preserved for evidence-chain context, not as a description of work in progress.

This document captures evidence — quoted source, schema excerpts, line numbers, URLs — for questions our implementation needs to answer before we touch code. Each section ends with a **Verdict** that states what the finding means for `mac-guest-agent`.

Nothing in this file is a fix proposal. Decisions flow from here into `../design/FREEZE_AND_GATING.md` in Phase 2.

## Method

For each target:
1. Fetch from a canonical source (upstream git, official docs).
2. Quote the relevant code or schema fragment with file path and line range.
3. Record the URL of the source revision consulted.
4. State what question(s) the finding answers and what is still open.
5. Write a one-paragraph **Verdict** stating the implication for our agent.

Where the answer is "not in this source" or "ambiguous," say so. Don't fill in by guessing.

---

## Target 1 — QGA schema (`qga/qapi-schema.json`)

**Source consulted:** `https://raw.githubusercontent.com/qemu/qemu/master/qga/qapi-schema.json` (QEMU `master`, fetched 2026-05-23).

### Freeze commands

```qapi
{ 'enum': 'GuestFsfreezeStatus',
  'data': [ 'thawed', 'frozen' ],
  'if': { 'any': ['CONFIG_WIN32', 'CONFIG_FSFREEZE'] } }

{ 'command': 'guest-fsfreeze-status',
  'returns': 'GuestFsfreezeStatus',
  'if': { 'any': ['CONFIG_WIN32', 'CONFIG_FSFREEZE'] } }

{ 'command': 'guest-fsfreeze-freeze',
  'returns': 'int',
  'if': { 'any': ['CONFIG_WIN32', 'CONFIG_FSFREEZE'] } }
  /* "Sync and freeze all freezable, local guest filesystems."
     Returns: number of file systems currently frozen. */

{ 'command': 'guest-fsfreeze-freeze-list',
  'data':    { '*mountpoints': ['str'] },
  'returns': 'int',
  'if': { 'any': ['CONFIG_WIN32', 'CONFIG_FSFREEZE'] } }
  /* Optional `mountpoints` arg: freeze only the listed mountpoints.
     Returns: number of file systems currently frozen. */

{ 'command': 'guest-fsfreeze-thaw',
  'returns': 'int',
  'if': { 'any': ['CONFIG_WIN32', 'CONFIG_FSFREEZE'] } }
  /* Returns: number of file systems thawed. */
```

**Our shape vs spec:**
- Status enum (`thawed` | `frozen`): we return the same enum strings (`src/cmd-fs.c:367`). ✓
- Freeze / thaw return: `int`, same. ✓
- `freeze-list` mountpoints arg: spec defines an optional `mountpoints: [str]` parameter. **We register `guest-fsfreeze-freeze-list` pointing at `handle_fsfreeze_freeze` (`src/cmd-fs.c:436`), the same handler as `guest-fsfreeze-freeze`, which `(void)args`. We silently ignore the mountpoints arg.** Anyone passing it would expect partial-freeze of named mountpoints; we'd freeze everything.

### `guest-get-cpustats`

```qapi
{ 'struct': 'GuestLinuxCpuStats',
  'data': {'cpu':       'int',
           'user':      'uint64',
           'nice':      'uint64',
           'system':    'uint64',
           'idle':      'uint64',
           '*iowait':   'uint64',
           '*irq':      'uint64',
           '*softirq':  'uint64',
           '*steal':    'uint64',
           '*guest':    'uint64',
           '*guestnice':'uint64' },
  'if': 'CONFIG_LINUX' }

{ 'union': 'GuestCpuStats',
  'base': { 'type': 'GuestCpuStatsType' },
  'discriminator': 'type',
  'data': { 'linux': 'GuestLinuxCpuStats' },
  'if': 'CONFIG_LINUX' }

{ 'command': 'guest-get-cpustats',
  'returns': ['GuestCpuStats'],
  'if': 'CONFIG_LINUX' }
```

**Our shape vs spec:** Spec returns `['GuestCpuStats']` — a **list of discriminated-union structs, one per CPU**, with a `type` discriminator and a `cpu` index. We return a single flat object (`src/cmd-hardware.c:343-348`):

```c
cJSON_AddNumberToObject(result, "user",   (double)cpu_load.cpu_ticks[CPU_STATE_USER]);
cJSON_AddNumberToObject(result, "system", (double)cpu_load.cpu_ticks[CPU_STATE_SYSTEM]);
cJSON_AddNumberToObject(result, "idle",   (double)cpu_load.cpu_ticks[CPU_STATE_IDLE]);
cJSON_AddNumberToObject(result, "nice",   (double)cpu_load.cpu_ticks[CPU_STATE_NICE]);
```

— aggregate across all vCPUs, no `type` discriminator, no `cpu` index, no array wrapper. **Not spec-conformant.**

Also note `'if': 'CONFIG_LINUX'`: in upstream QEMU, this command is **Linux-only** — there is no defined macOS variant of `GuestCpuStats`. Adding a `darwin` variant (analogous to `linux`) to a fork of the schema would be the spec-conformant way to expose macOS CPU stats. Returning an aggregate object effectively invents an out-of-spec shape on a Linux-only command, which is at minimum confusing to any consumer that follows the schema.

### Memory commands

```qapi
{ 'struct': 'GuestMemoryBlock',
  'data': {'phys-index':   'uint64',
           'online':       'bool',
           '*can-offline': 'bool'},
  'if': 'CONFIG_LINUX' }

{ 'command': 'guest-get-memory-blocks',
  'returns': ['GuestMemoryBlock'],
  'if': 'CONFIG_LINUX' }

{ 'struct': 'GuestMemoryBlockInfo',
  'data': {'size': 'uint64'},
  'if': 'CONFIG_LINUX' }

{ 'command': 'guest-get-memory-block-info',
  'returns': 'GuestMemoryBlockInfo',
  'if': 'CONFIG_LINUX' }
```

**Our shape vs spec:**
- `get-memory-blocks`: spec returns `[{phys-index, online, *can-offline}]`. We return the same (`src/cmd-hardware.c:273-281`). ✓
- `get-memory-block-info`: spec returns `{size}`. We return the same (`src/cmd-hardware.c:307-309`). ✓
- Both are `CONFIG_LINUX`-only in the spec. We register them on macOS — same caveat as `get-cpustats` (out-of-spec command, in-spec shape).

### Other CPU-adjacent commands the spec defines

- `guest-get-vcpus` → `['GuestLogicalProcessor']`, gated on `CONFIG_LINUX || CONFIG_WIN32`. We register it.
- `guest-set-vcpus` → `CONFIG_LINUX`-only. We register it as unsupported (returns error).
- `guest-get-load` → `GuestLoadAverage`, gated on `CONFIG_WIN32 || CONFIG_GETLOADAVG`. We register it.

### Verdict

Three concrete issues identified by the schema alone:

1. **`guest-get-cpustats` shape is not spec-conformant.** Spec wants per-CPU array of discriminated unions; we return a flat aggregate object. This is the most likely reason any consumer (PVE UI, monitoring tools, scripted health checks) that follows the QGA spec would silently drop our response and fall back to QMP host-side metrics. To match the spec we'd either (a) add a `'darwin'` variant to the union and produce per-CPU rows via `HOST_PROCESSOR_INFO` / `PROCESSOR_CPU_LOAD_INFO` instead of the aggregate `HOST_CPU_LOAD_INFO`, or (b) decline to register the command on macOS and document why.

2. **`guest-fsfreeze-freeze-list` ignores its `mountpoints` argument.** We point the same handler at both `freeze` and `freeze-list`; the spec defines `freeze-list` with an optional `*mountpoints: [str]` arg. A client that passes mountpoints expecting partial freeze gets a global freeze instead. Either implement the subset behaviour or stop registering `freeze-list`.

3. **Several commands we register are `CONFIG_LINUX`-only in upstream QEMU.** That's not strictly a bug — we ship an agent the spec doesn't acknowledge for macOS — but we should be deliberate about which commands we expose and whether our shape on each one is in-spec or extended.

Memory commands' shapes are already correct.

These findings inform Phase 2's command-gating + intent design, and shape the `--safe-test-json` / `--self-test-json` output that Phase 3 wraps.

---

## Target 2 — Linux freeze reference (`qga/commands-posix.c` + `qga/commands-linux.c`)

**Sources consulted:**
- `https://raw.githubusercontent.com/qemu/qemu/master/qga/commands-posix.c` (wrappers, freeze hooks)
- `https://raw.githubusercontent.com/qemu/qemu/master/qga/commands-linux.c` (actual FIFREEZE loop, mount enumeration)
- `https://raw.githubusercontent.com/qemu/qemu/master/qga/commands-bsd.c` (BSD/FreeBSD UFSSUSPEND path, for contrast)

### Wrapper (`commands-posix.c`)

```c
int64_t qmp_guest_fsfreeze_freeze_list(bool has_mountpoints,
                                       strList *mountpoints, Error **errp)
{
    int ret;
    FsMountList mounts;
    Error *local_err = NULL;
    slog("guest-fsfreeze called");
    execute_fsfreeze_hook(FSFREEZE_HOOK_FREEZE, &local_err);
    if (local_err) { error_propagate(errp, local_err); return -1; }
    QTAILQ_INIT(&mounts);
    if (!build_fs_mount_list(&mounts, &local_err)) {
        error_propagate(errp, local_err); return -1;
    }
    ga_set_frozen(ga_state);                       /* (1) set state BEFORE ioctl loop */
    ret = qmp_guest_fsfreeze_do_freeze_list(has_mountpoints, mountpoints, mounts, errp);
    free_fs_mount_list(&mounts);
    if (ret == 0) {
        ga_unset_frozen(ga_state);                 /* (2) zero filesystems → unset state */
    } else if (ret < 0) {
        qmp_guest_fsfreeze_thaw(NULL);             /* (3) error → rollback */
    }
    return ret;
}
```

Two important sequencing decisions:
- `ga_set_frozen` is called **before** the freeze ioctl loop. This means command gating kicks in immediately — any concurrent request that arrives mid-freeze is rejected.
- If the ioctl loop returns `0` (no volumes successfully frozen), the agent **unsets** the frozen state. The state is only "frozen" if at least one volume actually got frozen.

### The actual ioctl loop (`commands-linux.c:191-237`)

```c
int64_t qmp_guest_fsfreeze_do_freeze_list(bool has_mountpoints,
                                          strList *mountpoints,
                                          FsMountList mounts,
                                          Error **errp)
{
    struct FsMount *mount;
    strList *list;
    int fd, ret, i = 0;

    QTAILQ_FOREACH_REVERSE(mount, &mounts, next) {   /* (1) reverse order: children before parents */
        if (has_mountpoints) {
            for (list = mountpoints; list; list = list->next) {
                if (strcmp(list->value, mount->dirname) == 0) break;
            }
            if (!list) continue;
        }
        fd = qga_open_cloexec(mount->dirname, O_RDONLY, 0);
        if (fd == -1) {
            error_setg_errno(errp, errno, "failed to open %s", mount->dirname);
            return -1;
        }
        ret = ioctl(fd, FIFREEZE);
        if (ret == -1) {
            if (errno != EOPNOTSUPP && errno != EBUSY) {    /* (2) **only tolerated errnos** */
                error_setg_errno(errp, errno, "failed to freeze %s", mount->dirname);
                close(fd);
                return -1;
            }
            /* EOPNOTSUPP or EBUSY: silently skip, do not count, continue */
        } else {
            i++;                                            /* (3) count only successful freezes */
        }
        close(fd);
    }
    return i;
}
```

### Mount enumeration (`commands-linux.c:81-167`)

`build_fs_mount_list` parses `/proc/self/mountinfo` and falls back to `build_fs_mount_list_from_mtab` (which uses `getmntent`). The mtab fallback explicitly pre-filters:

```c
if ((ment->mnt_fsname[0] != '/') ||           /* non-device-backed: tmpfs, sysfs, proc */
    (strcmp(ment->mnt_type, "smbfs") == 0) ||  /* network share */
    (strcmp(ment->mnt_type, "cifs") == 0)) {   /* network share */
    continue;
}
```

The mountinfo path additionally dedupes by `devmajor:devminor` (bind mounts of the same device only counted once) and filters btrfs subvolumes via a special-case.

### BSD path (`commands-bsd.c`) — for contrast

BSD's QGA uses `UFSSUSPEND` ioctl on `/dev/ufssuspend` for FreeBSD; thaw closes the device. **No `__APPLE__` / Darwin conditional code.** No CPU stats. No `F_FULLFSYNC`. macOS dropped UFS support in 10.7, so even the BSD path is unusable on us.

### Verdict

**Upstream QEMU does not implement a guest agent for macOS.** There is no FIFREEZE on Darwin, no UFSSUSPEND (UFS was removed in 10.7), no equivalent kernel-level freeze primitive. Our `sync()` + `F_FULLFSYNC` + APFS-snapshot approach is the best available substitute, and we are on our own — but we should adopt the reference's **error-handling pattern**, which directly resolves vit9696's report:

1. **Treat `ENOTSUP` / `EOPNOTSUPP` as "skip, don't count, continue."** The reference does exactly this for FIFREEZE; we should do the same for `F_FULLFSYNC` on foreign filesystems. Log at INFO (or DEBUG), not WARN — it is by-design, not a failure.

2. **Pre-filter mounts before attempting the operation.** The reference skips network mounts and non-device-backed mounts. We should consider analogous filters on macOS: skip `smbfs`/`afpfs`/`nfs`/`cddafs`, skip `autofs` placeholders, skip read-only mounts (we already do), skip FUSE mounts where the daemon may itself be on a volume we're trying to flush.

3. **Sequence: set the frozen state before the per-volume loop, unset it if zero volumes succeeded.** We currently set `freeze_status = 1` after `sync_all_volumes()` returns. That's a minor inversion vs. the reference — switching to "set first" gives the same immediate command-gating the reference provides. (Our gating still works because the freeze handler is itself synchronous; in practice no concurrent command sneaks in. But the reference's ordering is more defensive.)

4. **Return the count of *successfully synced* volumes, not the count attempted.** We already do this (`synced` counter in `sync_all_volumes`). The misleading "1 volumes synced" in vit9696's log is not a wrong number — there really was 1 volume where F_FULLFSYNC succeeded — it just *reads* alarming because we log a WARN for the FAT32 skip. Fix the WARN, not the count.

5. **`F_FULLFSYNC` is a per-FD `fcntl`, not a per-mount `ioctl` — and there is no Linux equivalent.** This is a macOS-only mechanism. The reference's FIFREEZE actually suspends writes; F_FULLFSYNC only forces a flush to media. They are not the same primitive, and our freeze cannot offer the same guarantees the Linux reference does. Worth being explicit about that in the docs and the response payload.

---

## Target 3 — Linux command-gating state machine (`qga/main.c`)

**Source consulted:** `https://raw.githubusercontent.com/qemu/qemu/master/qga/main.c` (QEMU `master`, fetched 2026-05-23).

### The freeze allowlist (`main.c:545-553`)

```c
static const char *ga_freeze_allowlist[] = {
    "guest-ping",
    "guest-info",
    "guest-sync",
    "guest-sync-delimited",
    "guest-fsfreeze-status",
    "guest-fsfreeze-thaw",
    NULL
};
```

**Six commands.** Notably absent: `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list`. The reference does **not** allow re-issuing a freeze while already frozen. Once frozen, only `thaw` can exit the state.

### Our allowlist (`src/cmd-fs.c:410-420`)

```c
static const char *allowed[] = {
    "guest-ping",
    "guest-sync",
    "guest-sync-id",                /* extension, not in upstream spec */
    "guest-sync-delimited",
    "guest-info",
    "guest-fsfreeze-status",
    "guest-fsfreeze-freeze",        /* upstream does NOT allow this */
    "guest-fsfreeze-freeze-list",   /* upstream does NOT allow this */
    "guest-fsfreeze-thaw",
    NULL
};
```

**Nine commands.** Three differences vs. upstream:
- We add `guest-sync-id` — fine, it's a sync variant.
- We allow `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` during freeze (our handler is idempotent: returns the current count). Upstream blocks them.

### Gating mechanism (`main.c:569-605`)

```c
static bool ga_command_is_allowed(const QmpCommand *cmd, GAState *state)
{
    /* ... allowedrpcs / blockedrpcs config-list logic ... */

    if (state->frozen) {                              /* (1) freeze filter takes priority over all */
        allowed = false;
        while (ga_freeze_allowlist[i] != NULL) {
            if (strcmp(name, ga_freeze_allowlist[i]) == 0) {
                allowed = true;
            }
            i++;
        }
    }
    return allowed;
}
```

The check is consulted by `ga_apply_command_filters` (`main.c:621-623`), which iterates **all registered commands** and enables/disables them at the QMP-command-table level when the freeze state transitions. Subsequent dispatches go through the QMP machinery's standard "command is disabled" error path, not through a custom check.

### Frozen-state machinery (`main.c:717-770`)

`ga_set_frozen` does **three** things beyond setting the flag:
1. `g_warning("disabling logging due to filesystem freeze");` — and then `ga_disable_logging(s);` — because writing to the log might write to a frozen volume.
2. `ga_create_file(s->state_filepath_isfrozen)` — writes a **persistent on-disk marker** so that if the agent crashes during freeze, the next instance can detect prior-frozen-state on startup.
3. `ga_apply_command_filters(s)` — re-evaluates the command table.

`ga_unset_frozen` is the inverse. It also handles deferred log/pid file creation that was held back due to the frozen state.

### Dispatch path (`main.c:875-897`)

```c
static void process_event(void *opaque, QObject *obj, Error *err)
{
    /* ... */
    rsp = qmp_dispatch(&ga_commands, obj, false, NULL);
    /* ... send_response ... */
}
```

Plain `qmp_dispatch`. Because `ga_apply_command_filters` has already disabled non-allowed commands at the QMP table level when freeze was entered, `qmp_dispatch` returns the standard "command is disabled" error for blocked requests. **The freeze check is enforced *before* dispatch by mutating the command table, not at dispatch time.**

### Verdict

Two architectural lessons that bear on our agent:

1. **Our allowlist is more permissive than upstream on `freeze` itself.** Upstream blocks `guest-fsfreeze-freeze` and `guest-fsfreeze-freeze-list` once frozen — only `thaw` can exit the state. Our handler accepts re-freeze as a no-op that returns the current count. This is a deliberate design choice (idempotent), but worth being aware of: it's a divergence from the spec's intent, and a buggy client could end up "double-freezing" without realising it stayed in the same state. Phase 2 should decide: keep idempotent re-freeze (and document the divergence), or align with upstream (and break any client that relies on idempotence).

2. **Upstream disables logging while frozen.** We don't. Our log path (`/var/log/mac-guest-agent.log`) sits on the root volume that we're freezing. Because our "freeze" is `sync() + F_FULLFSYNC` — not a true I/O suspension — writes still complete (they just may not be on physical media until the next sync). So our log writes during freeze are functionally fine. But if Phase 2 ever introduces a real I/O-suspension primitive (e.g. for APFS via `apfs.kext` private APIs, hypothetically), the logging-during-freeze deadlock that upstream guards against becomes relevant.

3. **Persistent frozen-state marker.** Upstream writes `state_filepath_isfrozen`. We don't. If our agent crashes mid-freeze, on restart `freeze_status = 0` and we lose the fact that we issued a freeze. Because our freeze doesn't actually suspend writes, "lost frozen state" is mostly harmless (no I/O is blocked); the worst case is an APFS snapshot that wasn't cleaned up, which the next freeze cycle's `delete_apfs_snapshot()` handles. But the divergence is worth documenting.

4. **The third command we should consider for the allowlist: `guest-get-fsinfo`.** Reading filesystem metadata is read-only and doesn't break freeze semantics; a monitoring client might want to query it during a freeze window. Upstream doesn't allow it, so we'd be more permissive — Phase 2 call.

5. **`fsfreeze_command_allowed` runtime check vs. command-table mutation.** Functionally equivalent for us. Our runtime check is simpler given we don't have QMP's command-flag infrastructure. No change needed.

---

## Target 3 — Linux command-gating state machine (`qga/main.c`)

**Questions:**
- During freeze, which commands does the reference agent allow?
- Where is the allowed-list defined and how is it consulted?
- What error class / desc does it return for a blocked command?
- Does the reference allow `guest-info` and `guest-ping` during freeze (we do)?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 4 — Proxmox `qm agent` wrapper

**Sources consulted:**
- `https://raw.githubusercontent.com/proxmox/qemu-server/master/src/PVE/QemuServer/Agent.pm` (wrapper helpers)
- `https://raw.githubusercontent.com/proxmox/qemu-server/master/src/PVE/API2/Qemu/Agent.pm` (REST API + CLI dispatcher)
- `https://raw.githubusercontent.com/proxmox/qemu-server/master/src/PVE/CLI/qm.pm` (CLI registration)

### The CLI surface (`PVE/CLI/qm.pm:1286-1305`)

```perl
agent => { alias => 'guest cmd' }, # FIXME: remove with PVE 8.0

guest => {
    cmd => ["PVE::API2::Qemu::Agent", 'agent', ['vmid', 'command'], {%node}, $print_agent_result],
    passwd => ["PVE::API2::Qemu::Agent", 'set-user-password', ['vmid', 'username'], {%node}],
    exec => [__PACKAGE__, 'exec', ['vmid', 'extra-args'], {%node}, $print_agent_result],
    'exec-status' => ["PVE::API2::Qemu::Agent", 'exec-status', ['vmid', 'pid'], {%node}, $print_agent_result],
},
```

`qm agent` is **just an alias** for `qm guest cmd`. Both route to `PVE::API2::Qemu::Agent->agent($vmid, $command)`. **There is no behavioural difference between the two forms.**

### The dispatcher (`PVE/API2/Qemu/Agent.pm`, `register_command()`)

The dispatch code for standard commands (the path `qm agent <vmid> get-osinfo` follows):

```perl
code => sub {
    my ($param) = @_;
    my $vmid = $param->{vmid};
    my $conf = PVE::QemuConfig->load_config($vmid);
    PVE::QemuServer::Agent::assert_agent_available($vmid, $conf);
    my $cmd = $param->{command} // $command;
    my $res = mon_cmd($vmid, "guest-$cmd");
    return { result => $res };
},
```

**Critical observation:** there is no `eval { ... }`, no `check_agent_error($res, ...)`, no inspection of `$res` for a `->{error}` key. The dispatcher takes whatever `mon_cmd` returns and wraps it in `{result => $res}` with no error handling.

### What `mon_cmd` returns on a QGA error response

`mon_cmd` is `PVE::QemuServer::Monitor::mon_cmd`, which wraps QMP. For QGA commands sent via QMP's `guest-exec`/QGA path, when the agent responds with `{"error": {"class": "GenericError", "desc": "..."}}`, the body of `$res` becomes that error hash. We can confirm `mon_cmd` returns error hashes (rather than always dying) by inspecting the *other* helper in the same project — `PVE::QemuServer::Agent::check_agent_error` (lines 75-93):

```perl
sub check_agent_error($result, $errmsg, $noerr = 0) {
    my $error = '';
    if (ref($result) eq 'HASH' && $result->{error} && $result->{error}->{desc}) {
        $error = "Agent error: $result->{error}->{desc}\n";
    } elsif (!defined($result)) {
        $error = "Agent error: $errmsg\n";
    }
    if ($error) {
        die $error if !$noerr;
        warn $error;
        return;
    }
    return 1;
}
```

This helper exists *because* `mon_cmd` can return a `{error => {desc => ...}}` hash for agent-side errors. Other helpers (`agent_cmd`, `guest_fs_thaw`, `guest_fs_is_frozen`, `file-read`, `file-write`) explicitly call `check_agent_error` after `mon_cmd`. **The `register_command` dispatch path used by `qm agent <cmd>` does not.**

### Consequence

When our agent returns `{"error": {"class": "GenericError", "desc": "Command not allowed while filesystem is frozen"}}` for `guest-get-osinfo`:

1. `mon_cmd($vmid, "guest-get-osinfo")` returns the error hash as-is.
2. The dispatcher wraps it: `return { result => {error => {class => "GenericError", desc => "Command not allowed while filesystem is frozen"}} };`
3. The API returns **HTTP 200** with that JSON body.
4. The CLI's `$print_agent_result` callback prints the JSON to stdout.
5. **The shell exit code is 0.**

`scripts/pve-verify.sh`'s behavioural check does:

```bash
if qm agent "$VMID" get-osinfo >/dev/null 2>&1; then
    fail "answered while frozen"
```

`>/dev/null 2>&1` discards the JSON body. Exit code is 0. The check declares failure — but the failure is in the check itself, not in the agent. **Our agent's gating in `src/agent.c:73` is doing exactly what it's supposed to do.** The "error" payload reaches the CLI; the CLI just doesn't translate it into a non-zero exit code for this dispatch path.

### Why El Cap's `pve-verify.sh` run "passed" the behavioural check

It didn't, actually — because the behavioural check did not exist when the user ran `pve-verify.sh 107` against El Cap. That was the *first* `pve-verify.sh` run, before commit `efcb712` (which added the behavioural check). The only "El Cap pass" of the behavioural check came from a stateful **mock** I ran locally where `qm` was replaced by a bash script that explicitly exited 1 on the "frozen" code path. The mock test proves the check *can* distinguish honest from lying agents — when `qm` exits non-zero on agent error. The real PVE wrapper for the dispatch path used by `qm agent <cmd>` does not.

So Target 4's resolution: there is **no Tier difference** between El Cap and Tiger here. The real-PVE behavioural check has never passed against an honest gating agent; it has only ever been mock-validated.

### Verdict

**`scripts/pve-verify.sh`'s freeze-behavioural check is fundamentally broken.** It relies on `qm agent ...` exiting non-zero on a QGA error response, but the PVE dispatch path used by `qm agent <cmd>` doesn't translate QGA errors into non-zero exits — it wraps them in `{result: ...}` and the CLI prints + exits 0. This was confirmed against PVE source at master (commit-pinned in the URL).

**The fix is in our script, not in the agent.** The check must inspect response content, not exit code. Two reliable signals:

1. **Positive signal of rejection:** the JSON contains `"error"` or the `"desc"` string `"Command not allowed while filesystem is frozen"`. If present → rejection → PASS the check.
2. **Positive signal of (unwanted) success:** the JSON contains `"pretty-name"` (a field unique to a successful `get-osinfo` response). If present → the agent answered → FAIL the check.

The cleanest implementation is to check for both and only pass when the rejection signal is present (or fail when the success signal is present). This is robust regardless of whether future PVE versions change the wrapper behaviour.

**Secondary verdict:** the `register_command` dispatcher's lack of error handling is arguably a Proxmox-side bug — a contributor noticing this could file an upstream patch to call `check_agent_error` from the standard dispatch path so `qm agent <cmd>` exits non-zero on agent errors. That's out of scope for this project, but worth noting as upstream context: we should not rely on PVE evolving here, because the current behaviour has been frozen behind the `FIXME: remove with PVE 8.0` comment for some time without change.

---

## Target 5 — Proxmox UI / RRD CPU+memory gauges

**Sources consulted:**
- `https://raw.githubusercontent.com/proxmox/pve-manager/master/PVE/Service/pvestatd.pm` (the collector daemon — confirmed to be a thin relay)
- `https://raw.githubusercontent.com/proxmox/qemu-server/master/src/PVE/QemuServer.pm` `sub vmstatus` (the actual data-collection function)

### `pvestatd` is a relay

`pvestatd`'s `update_qemu_status` calls `PVE::QemuServer::vmstatus(undef, 1)` and broadcasts the result. No QGA calls in the daemon.

### `vmstatus` (PVE::QemuServer.pm, ~line 4600) — the canonical data sources

```perl
# Per-VM CPU%
my $pstat = PVE::ProcFSTools::read_proc_pid_stat($pid);
my $used = $pstat->{utime} + $pstat->{stime};
# ... two samples, delta over time ...
$d->{cpu} = (($dutime / $dtime) * $cpucount) / $d->{cpus};
```

```perl
# Per-VM memory (default)
my $cgroup_mem = eval { $cgroup->get_memory_stat() } // {};
$d->{memhost} = $cgroup_mem->{mem} // 0;
$d->{mem} = $d->{memhost};
```

```perl
# Per-VM memory (full mode, overlaid with QMP query-balloon when stats are present)
my $ballooncb = sub {
    my ($vmid, $resp) = @_;
    my $info = $resp->{'return'};
    return if !$info->{max_mem};
    $d->{maxmem} = $info->{max_mem};
    $d->{balloon} = $info->{actual};
    if (defined($info->{total_mem}) && defined($info->{free_mem})) {
        $d->{mem}     = $info->{total_mem} - $info->{free_mem};
        $d->{freemem} = $info->{free_mem};
    }
    $d->{ballooninfo} = $info;
};
```

```perl
# Network bytes
my $netdev = PVE::ProcFSTools::read_proc_net_dev();
# sums tap<vmid>i* interfaces
```

```perl
# Disk I/O — only in full mode
$qmpclient->queue_cmd($qmp_peer, $blockstatscb, 'query-blockstats');
```

### The data-source matrix for the PVE UI's per-VM gauges

| Gauge | Canonical source |
|---|---|
| **CPU%** | `/proc/<qemu-pid>/stat` — QEMU process `utime + stime`, delta over time, normalised to vCPU count. **Host-side**, never QGA. |
| **Memory (default)** | Linux cgroup memory stat for the VM's scope — QEMU process RSS on the host. |
| **Memory (full mode + balloon stats present)** | QMP `query-balloon`: `total_mem - free_mem`. These fields are only populated when the guest has a virtio-balloon driver that has negotiated `VIRTIO_BALLOON_F_STATS_VQ` (see Target 7). For a macOS guest: absent. |
| **Memory (full mode + no balloon stats)** | Falls back to the cgroup RSS path. |
| **Network bytes** | `/proc/net/dev`, summing per-VM tap interfaces. Host-side. |
| **Disk I/O bytes** | QMP `query-blockstats`. Host-side (QEMU's view). |

### Verdict

**Conclusively: the PVE web UI does not call our QGA commands for any of its data gauges.** No `guest-get-cpustats`, no `guest-get-memory-blocks`, no `guest-get-memory-block-info` is ever invoked by `vmstatus`. Three consequences:

1. **Our `guest-get-cpustats` shape mismatch (Target 1) does not affect the PVE UI.** The UI's CPU% is QEMU process CPU time on the host. That's why @vit9696 sees one core at 100% on his Tiger VM — the QEMU vCPU thread is genuinely consuming a host core (old-macOS-on-QEMU idle quirk), and PVE is faithfully reporting it. Our agent's CPU response shape is irrelevant to this gauge.

2. **Our `guest-get-memory-blocks` (and its derived "accurate memory reporting") does not feed the PVE UI either.** The memory gauge is cgroup RSS of QEMU (host RAM footprint) when no balloon stats are present. For a macOS guest, that's what shows up — not our agent's data. The reason @vit9696's gauge "looks correct" is that QEMU's RSS on the host happens to correlate roughly with guest memory pressure, not because PVE reads our agent.

3. **The "Accurate Memory Reporting Without Balloon Driver" rationale (`docs/PVE.md`) is misleading as written.** It implies PVE's memory gauge benefits from our agent. It doesn't. What we provide is *direct* memory reporting via `qm agent <vmid> get-memory-blocks` or our own `pve-verify.sh` — a separate path from the gauge. Phase 2 should re-word this so contributors aren't misled into thinking the UI gets prettier when our agent is installed.

So the answer to the original @vit9696 CPU question is now fully evidenced and closed: **the 100%-of-one-core figure is QEMU's host-side measurement of an idle-but-not-HLT'ing Tiger guest.** Stopping the agent will not change it. Our `get-cpustats` shape will not change it. Only the guest kernel's idle behaviour under KVM can change it.

---

## Synthesis

Phase 1 complete. Across seven targets, the consolidated picture:

### What the spec says we should look like

- `guest-get-cpustats` is `CONFIG_LINUX`-only in upstream and returns `[GuestCpuStats]` — a per-CPU array of discriminated-union structs with a `type` field and a `cpu` index. **Our aggregate-object shape is not spec-conformant.** Phase 2 must decide: extend the union with a `darwin` variant and produce per-CPU rows, or stop registering the command on macOS.
- `guest-fsfreeze-freeze-list` accepts an optional `mountpoints` argument we silently ignore (we route both `freeze` and `freeze-list` to the same `(void)args` handler). Phase 2 must decide: implement the subset behaviour or drop `freeze-list` from our registration.
- Memory block schemas (`get-memory-blocks`, `get-memory-block-info`) are correct. No action.
- Freeze/thaw/status enum shapes are correct.

### How the reference implementation handles what bit us

- **Foreign-FS sync failure** is treated as "skip silently, don't count, continue" by Linux QGA when `EOPNOTSUPP`/`EBUSY` come back from `FIFREEZE`. Adopt the same pattern for `F_FULLFSYNC` `ENOTSUP`/`EOPNOTSUPP` on macOS. Move from WARN to INFO/DEBUG; consider the volume "best-effort flushed" (the global `sync()` already covered it).
- **Pre-filter mounts** before the operation: skip network mounts (`smbfs`, `cifs`/`afpfs`/`nfs`), skip non-device-backed mounts. Linux QGA does this; we should add an analogous filter (statfs `f_fstypename` based).
- **Set the frozen flag before the ioctl loop, unset if zero succeeded.** A minor sequencing change vs. our current "set after success."
- **Allowlist divergence** with upstream: ours is 9, theirs is 6. We allow re-freeze (idempotent); they block it. Phase 2 should decide whether to align or document the divergence. The other extra entries (`guest-sync-id`) are fine.
- **Persistent frozen-state marker on disk + logging disabled while frozen** — upstream does both for crash-safety and to avoid writing to a frozen volume. We do neither. Mostly harmless given our freeze isn't a true I/O suspension, but Phase 2 should at least decide explicitly.

### Why `pve-verify.sh`'s behavioural check failed for @vit9696

`qm agent <vmid> <cmd>` is an alias for `qm guest cmd <vmid> <cmd>`. Both route through `PVE::API2::Qemu::Agent`'s `register_command` dispatcher, which wraps the QGA response as `{result: $res}` with **no error handling**. When our agent returns `{"error":{"class":"GenericError","desc":"Command not allowed while filesystem is frozen"}}`, PVE wraps that as `{result:{error:{desc:"..."}}}`, returns HTTP 200, and the CLI exits 0. Our `pve-verify.sh` check that reads exit code therefore declares failure even when the agent is doing the right thing.

**Our agent's freeze gating in `src/agent.c:73` is correct.** The fix is in our script: inspect response content for `"error"` / `"pretty-name"` instead of exit code.

### Why @vit9696's PVE CPU% gauge shows 100%

PVE's per-VM CPU gauge comes from `/proc/<qemu-pid>/stat` (`utime + stime`), not from our agent. Tiger's idle loop doesn't trap to KVM in a way that lets it park the vCPU thread, so the QEMU process keeps that thread spinning on the host. By El Cap that had been fixed. Our agent is not the source. Stopping the agent will not change the gauge.

Same path for memory: cgroup RSS, optionally overlaid by `query-balloon` if the guest has a balloon-stats driver — which macOS does not, and structurally never will without a kext that doesn't exist (Target 7).

### What this means for our project's positioning vs Apple's QGA

Apple's `AppleQEMUGuestAgent` (16 commands) is a minimal QGA — exec, file I/O, sync, ping, time, network-get-interfaces, shutdown. **No freeze, no observability, no OS-info, no SSH, no memory, no CPU stats.** Our 45-command surface is genuine extension, not duplication. PVE backup consistency on a macOS guest is impossible without our agent (or one like it).

Apple's QGA is also only launched when an `AppleVirtIOAgentDevice` IOKit property is matched, which is set by Apple's `applevirtio.console` driver loaded on Virtualization.framework hosts. On Proxmox/QEMU/OpenCore guests, that property is not set and Apple's QGA never runs. The "ISA-because-Apple-claims-VirtIO" rationale in our README is right for VZ environments but oversimplified for QEMU.

### What Phase 2 needs to decide

Translating the above into the design questions Phase 2's matrix has to answer:

1. **`F_FULLFSYNC` failure handling on foreign FS** — adopt the Linux pattern (skip-not-count, INFO log). Settle: at what statfs `f_fstypename` values do we pre-skip vs let `F_FULLFSYNC` decide? List per FS type.
2. **`guest-get-cpustats` shape** — extend with a `darwin` variant and produce per-CPU rows, or drop the command? If extend, settle the union discriminator naming and field set (probably matching `GuestLinuxCpuStats`'s shape minus Linux-only fields).
3. **`guest-fsfreeze-freeze-list` mountpoints arg** — implement subset freeze or unregister the command?
4. **Allowlist** — keep idempotent re-freeze or align with upstream (block re-freeze)? Add `get-fsinfo` (read-only) to allowed?
5. **Persistent frozen-state marker** — write one (for crash recovery) or document the divergence?
6. **Documentation honesty** — update the "Accurate Memory Reporting" claim, refine the "ISA-because-Apple" rationale to distinguish VZ from QEMU.

### What Phase 3 needs to wrap

- The corrected `pve-verify.sh` behavioural check (content inspection, not exit code).
- The `qm agent exec` one-shot embedding of `--self-test-json` + `--safe-test-json`.
- Whatever shape changes come out of Phase 2 (especially for `cpustats`).

### What was *not* answered and may need re-research in Phase 2

- Whether PVE has any per-command behaviour difference between `qm agent` and `qm guest cmd` beyond the alias relationship. (Unlikely based on the code, but only verified at the dispatch level — the surrounding CLI framework's exit-code logic was not read.)
- Whether any other QGA consumer in the PVE ecosystem (pve-backup, pve-firewall, pve-cluster) calls our commands with shape expectations that differ from the spec. Likely no, but not exhaustively verified.
- The `PVE::QMPClient::mon_cmd` implementation's exact behaviour for QGA error responses (whether it dies in some configurations and returns in others). The empirical evidence (other helpers' explicit `check_agent_error` calls) implies it returns the error hash, but the implementation itself was not read.

These can be filled in Phase 2 if a design decision specifically depends on them.

---

## Target 6 — Apple's built-in QGA

**Source consulted:** local inspection on macOS 26.5 host (`/usr/libexec/AppleQEMUGuestAgent`). Universal Mach-O (x86_64 + arm64e), 254 KB, code-signed `com.apple.AppleQEMUGuestAgent`, built April 17 2026.

### Linked frameworks

```
SystemAdministration.framework (weak)
SecurityFoundation.framework
RemoteServiceDiscovery.framework
IOKit.framework
Foundation.framework
CoreFoundation.framework
libobjc, libc++, libSystem
```

Notably: `RemoteServiceDiscovery` (Apple's RPC layer), `SystemAdministration` (privileged operations), `SecurityFoundation` (auth). The agent runs as a privileged service for VZ-host → guest communication.

### Commands implemented

Extracted via `strings | grep -E '^guest-[a-z-]+$'`:

```
guest-exec
guest-exec-status
guest-file-close
guest-file-flush
guest-file-open
guest-file-read
guest-file-seek
guest-file-write
guest-get-time
guest-info
guest-network-get-interfaces
guest-ping
guest-set-time
guest-shutdown
guest-sync
guest-sync-delimited
```

**Sixteen commands.** A minimal QGA implementation covering: identify the guest, exec commands and read their output, transfer files, basic networking lookup, time get/set, and graceful shutdown.

### Commands NOT implemented by Apple's agent

Comparing against the QGA spec and our 45-command surface, Apple's QGA explicitly does NOT implement:

- **No freeze/thaw** — `guest-fsfreeze-freeze`, `-freeze-list`, `-thaw`, `-status` all absent. PVE-style backup consistency on a macOS guest with **only Apple's agent gets no quiesce.**
- **No `guest-get-osinfo`** — they have `guest-info` (the version handshake) but not the OS-identification command.
- **No observability:** `guest-get-host-name`, `guest-get-users`, `guest-get-load`, `guest-get-vcpus`, `guest-get-cpustats`, `guest-get-memory-blocks`, `guest-get-memory-block-info`, `guest-get-fsinfo`, `guest-get-disks`, `guest-get-diskstats` — all absent.
- **No fstrim, no network-get-route.**
- **No SSH key management:** `guest-ssh-*` commands absent.
- **No `set-user-password`.**

Strings search for freeze-related identifiers (`freeze`, `thaw`, `FIFREEZE`, `F_FULLFSYNC`) returns nothing — confirming the absence is structural, not just an unregistered command name.

### Launch mechanism (`/System/Library/LaunchDaemons/com.apple.AppleQEMUGuestAgent.plist`)

```
"Label" => "com.apple.AppleQEMUGuestAgent"
"ProgramArguments" => [ 0 => "/usr/libexec/AppleQEMUGuestAgent" ]
"ProcessType" => "Interactive"
"KeepAlive" => { "Crashed" => true }
"LaunchEvents" => {
    "com.apple.iokit.matching" => {
        "com.apple.driver.applevirtio.console.match" => {
            "IOMatchAll" => true
            "IOMatchLaunchStream" => true
            "IOPropertyMatch" => { "AppleVirtIOAgentDevice" => true }
        }
    }
}
"RemoteServices" => {
    "com.apple.AppleQEMUGuestAgent" => {
        "ExposedToUntrustedDevices" => true
        "LimitExposedToDeviceType" => "virtualmachine-host"
        "RequireEntitlement" => "com.apple.private.AppleQEMUGuestAgent"
    }
}
```

Two critical properties:

1. **The daemon is IOKit-launched on-demand.** It only starts when a device with `AppleVirtIOAgentDevice = 1` is matched by IOKit. This property is set by Apple's `applevirtio.console` driver, which only loads on guests running under Apple's Virtualization.framework. **On QEMU/OpenCore guests (vit9696's and the user's setups), this property is not present and Apple's agent never launches.**
2. **The transport is Apple's `RemoteServices`** (not raw serial). The agent listens on a `virtualmachine-host`-only Mach service, gated by the entitlement `com.apple.private.AppleQEMUGuestAgent`. This is the VZ host ↔ guest agent channel; it has nothing to do with QGA's traditional serial channel.

### Verdict

Three concrete implications:

1. **The "ISA-because-Apple-claims-VirtIO" rationale is more nuanced than we've been documenting.** Apple's QGA only launches on VZ hosts (UTM, Apple's own `vz_run`, etc.) where `AppleVirtIOAgentDevice` is matched. On a Proxmox/QEMU/OpenCore guest, Apple's QGA never runs — there's no `AppleVirtIOAgentDevice` IOKit property. So on QEMU we could technically use VirtIO too. We deliberately don't, because on Apple VZ hosts our agent and Apple's would conflict. The honest documentation: "ISA, because some macOS guests run under Apple's Virtualization.framework and we don't want to conflict with Apple's agent there." Phase 2 should refine the README on this point.

2. **Apple's QGA does not implement filesystem freeze, on any version.** PVE backup consistency for a macOS guest is impossible with Apple's agent alone. Anyone who wants quiesce-during-backup on a macOS VM must run our agent (or accept crash-consistent backups). This is a feature gap we fill, not a duplication.

3. **No QGA command we implement clashes in shape with Apple's**, because we don't overlap on `guest-get-cpustats`, `guest-get-memory-blocks`, `guest-fsfreeze-*`, `get-osinfo`, etc. — Apple doesn't have them. On the 7 commands we both implement (`ping`, `sync`, `sync-delimited`, `info`, `get-time`, `set-time`, `shutdown`, `network-get-interfaces`, `exec*`, `file-*`), there's no PVE consumer that would notice our shape vs theirs. Spec-conformance for our extension commands (cpustats, memory blocks, freeze) is purely about being a good QGA citizen, not about competing with Apple.

---

## Target 7 — virtio-balloon stats protocol

**Source consulted:** `https://raw.githubusercontent.com/qemu/qemu/master/hw/virtio/virtio-balloon.c` (QEMU `master`, fetched 2026-05-23).

### The stat field set (lines 194-209)

```c
static const char *balloon_stat_names[] = {
   [VIRTIO_BALLOON_S_SWAP_IN]         = "stat-swap-in",
   [VIRTIO_BALLOON_S_SWAP_OUT]        = "stat-swap-out",
   [VIRTIO_BALLOON_S_MAJFLT]          = "stat-major-faults",
   [VIRTIO_BALLOON_S_MINFLT]          = "stat-minor-faults",
   [VIRTIO_BALLOON_S_MEMFREE]         = "stat-free-memory",
   [VIRTIO_BALLOON_S_MEMTOT]          = "stat-total-memory",
   [VIRTIO_BALLOON_S_AVAIL]           = "stat-available-memory",
   [VIRTIO_BALLOON_S_CACHES]          = "stat-disk-caches",
   [VIRTIO_BALLOON_S_HTLB_PGALLOC]    = "stat-htlb-pgalloc",
   [VIRTIO_BALLOON_S_HTLB_PGFAIL]     = "stat-htlb-pgfail",
   [VIRTIO_BALLOON_S_OOM_KILL]        = "stat-oom-kills",
   [VIRTIO_BALLOON_S_ALLOC_STALL]     = "stat-alloc-stalls",
   [VIRTIO_BALLOON_S_ASYNC_SCAN]      = "stat-async-scans",
   [VIRTIO_BALLOON_S_DIRECT_SCAN]     = "stat-direct-scans",
   [VIRTIO_BALLOON_S_ASYNC_RECLAIM]   = "stat-async-reclaims",
   [VIRTIO_BALLOON_S_DIRECT_RECLAIM]  = "stat-direct-reclaims",
};
```

### Feature negotiation (lines 1076-1080)

```c
static uint64_t virtio_balloon_get_features(VirtIODevice *vdev,
                                            uint64_t f, Error **errp)
{
    VirtIOBalloon *dev = VIRTIO_BALLOON(vdev);
    f |= dev->host_features;
    virtio_add_feature(&f, VIRTIO_BALLOON_F_STATS_VQ);
    return f;
}
```

The host advertises `VIRTIO_BALLOON_F_STATS_VQ`. **The guest driver must accept this feature flag to enable stats reporting.**

### Stats reception (lines 583-610) — **guest-push only**

```c
static void virtio_balloon_receive_stats(VirtIODevice *vdev, VirtQueue *vq)
{
    ...
    elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
    ...
    while (iov_to_buf(elem->out_sg, elem->out_num, offset, &stat, sizeof(stat)) == sizeof(stat)) {
        uint16_t tag = virtio_tswap16(vdev, stat.tag);
        uint64_t val = virtio_tswap64(vdev, stat.val);
        offset += sizeof(stat);
        if (tag < VIRTIO_BALLOON_S_NR)
            s->stats[tag] = val;
    }
    ...
}
```

The guest pushes stats into the stats virtqueue at the host's request, but there is no mechanism for the host to obtain stats without a cooperating guest driver. If no driver, the stats slots are initialised to `-1` (line 217: `dev->stats[i++] = -1`).

### What `query-balloon` returns without a driver (line 1070-1074)

```c
static void virtio_balloon_stat(void *opaque, BalloonInfo *info)
{
    VirtIOBalloon *dev = opaque;
    info->actual = get_current_ram_size() - ((uint64_t) dev->actual <<
                                             VIRTIO_BALLOON_PFN_SHIFT);
}
```

Only `actual` (the ballooned-away amount, defaulting to total RAM when no balloon is active). No `stats` array.

### Verdict

Three concrete answers:

1. **Without a guest-side virtio-balloon driver, the host gets no per-process memory telemetry.** Period. macOS has no virtio-balloon driver — never has, and Apple has no incentive to ship one. `query-balloon` returns the assigned-RAM number and an empty/-1 stats array for a macOS guest.

2. **There is no host-pull alternative.** The protocol is entirely guest-push via the stats virtqueue. No QMP command exists that asks the guest for stats outside the negotiated virtqueue path.

3. **Our `guest-get-memory-blocks` is the only path to *any* memory observability from a macOS guest, given the virtio-balloon dead end.** That validates keeping the command, but it also means we should be honest in documentation: this is a structural macOS-on-QEMU limitation, not a project-specific gap. PVE's "memory: 0 GB / 0 GB" gauge for a macOS guest *cannot* be made accurate via stats-from-balloon — it would require a balloon-stats kext, which doesn't exist and would be a multi-year project.

   The proper future direction (if anyone cared enough) is **either** write a macOS virtio-balloon-stats kext (huge effort, kext deprecation makes it questionable), **or** convince PVE upstream to surface a "use guest agent for memory" path that reads our `guest-get-memory-blocks` for the gauge. The latter is a Proxmox feature request, not our code.

---

## Target 7 — virtio-balloon stats protocol

**Questions:**
- What stats does the virtio-balloon device expose via QMP?
- What driver-side support is required in the guest to populate them?
- Is there any path to memory telemetry on macOS without writing a virtio-balloon-stats kext?
- Should our agent stop pretending to provide memory usage via `get-memory-blocks` and document the limitation instead?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Synthesis

_(filled when all targets are answered)_

A short consolidated statement of what we now know, what's still unknown, and what specific design questions Phase 2 needs to answer.
