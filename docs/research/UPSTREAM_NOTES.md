# Upstream research: QGA spec, Linux reference impl, PVE wrapper behaviour

**Status:** in progress. Phase 1 of `../PLAN.md`.

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

## Target 4 — Proxmox `qm agent` wrapper (`PVE/QemuServer/Agent.pm`, `PVE/CLI/qm.pm`)

**Questions:**
- When QGA returns `{"error": {"class": "...", "desc": "..."}}`, does `qm agent <cmd>` exit non-zero?
- Does `qm guest cmd <cmd>` behave the same way? Different?
- What does each print to stdout vs stderr on agent error?
- Has this behaviour changed between PVE versions (the user's PVE host vs @vit9696's PVE 9.1.9)?
- Definitive answer for why `pve-verify.sh`'s behavioural check passed on El Cap and failed on Tiger: is it the wrapper, or our gating?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 5 — Proxmox UI / RRD CPU+memory gauges

**Questions:**
- Does the PVE web UI's per-VM CPU% gauge call `guest-get-cpustats`?
- Does the memory gauge call `guest-get-memory-blocks` or use balloon stats?
- Does it use QMP host-side (`query-cpus-fast`, `query-balloon`) regardless of guest agent?
- Implication: does our `get-cpustats` shape matter for the UI, or is the UI fed from a different path?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

---

## Target 6 — Apple's built-in QGA (Big Sur+, 18 commands)

**Questions:**
- What commands does Apple's agent implement?
- What response shapes does it produce — especially for any commands we both implement?
- Where does it live on disk? Is it a binary we can `nm` / `otool -L` / extract strings from?
- What does it do for `guest-fsfreeze-freeze` on a Big Sur+ APFS guest (if anything)?
- Is there a relationship between Apple's agent claiming the VirtIO channel and any cross-version freeze behaviour we should know about?

**Source:** _(to be filled)_

**Findings:** _(to be filled)_

**Verdict:** _(to be filled)_

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
