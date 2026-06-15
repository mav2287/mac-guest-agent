# Reclaiming thin-provisioned disk space on a macOS guest

macOS has **no on-demand TRIM** — no `fstrim`/`FITRIM` equivalent, no public
volume-level "discard all free extents" API. That is why this agent does **not**
register `guest-fstrim` (it follows upstream QEMU, which gates the command behind
`CONFIG_FSTRIM` and omits it where unavailable; calling it returns
`CommandNotFound`). Reclaim is still entirely possible — it's just a deliberate,
out-of-band operation, not a fast QGA call.

## How TRIM works on macOS (by version)

| macOS | TRIM availability |
|---|---|
| 10.4–10.6 (HFS+) | None. No `trimforce`. Reclaim only via zero-fill (below). |
| 10.10.4+ | `sudo trimforce enable` turns on automatic TRIM for third-party SATA/AHCI disks (reboot required). Apple internal/NVMe SSDs are on by default. |
| 10.13+ (APFS) | APFS trims at mount; `secureErase freespace` is discouraged on APFS/SSD. Prefer `trimforce` + `discard`. |

A QEMU **virtual** disk is always "third-party," and the legacy macOS this agent
targets runs on **IDE** (AHCI panics Tiger) — IDE can't carry guest TRIM at all.
So for the common case, zero-fill is the path that works.

## The reclaim procedure (works on every macOS, including 10.4–10.6)

This is the established technique (it's what VMware Fusion's "Clean Up Virtual
Machine" runs inside macOS guests). Two parts: **zero the free space in the
guest**, then **reclaim host-side**.

### 1. In the guest — zero free space with Apple's own tool
Driven from the host via the agent's `guest-exec` (no SSH needed); it's a
long-running job, so poll `guest-exec-status` rather than expecting an immediate
result:

```sh
# kick off (returns a pid)
qm guest exec <vmid> -- /usr/sbin/diskutil secureErase freespace 0 /
# poll until exited
qm guest exec-status <vmid> <pid>
```

`secureErase freespace 0` = single-pass **zeros** over all free space. (Level 0
only — levels 1–4 write random data, which the host cannot reclaim.) Apple's tool
walks the free extents for you; do **not** hand-roll a `dd` zero-fill.

### 2. On the host — turn those zeros into freed blocks
- **Online:** the VM disk must be configured `discard=on,detect-zeroes=unmap`.
  QEMU then converts each zero-write into an UNMAP against the backing store
  (LVM-thin / qcow2 / zvol) — this works regardless of the guest bus (IDE
  included), because the detection is host-side, not guest TRIM.
- **Offline:** stop the VM and `qemu-img convert -O qcow2 in.qcow2 out.qcow2`
  (drops the zero clusters), or use the storage's thin-reclaim path.

## Caveats (honest limits)
- **APFS local / Time Machine snapshots pin "deleted" space.** Reclaim is partial
  until they're thinned. (Real TRIM has the same limit.)
- **`secureErase freespace` is discouraged by Apple on APFS/SSD** (10.13+). On
  modern guests prefer enabling TRIM (`trimforce` + `discard`) so deletes
  reclaim continuously.
- **It temporarily consumes free space** while zeroing — Apple's tool manages its
  own scratch, but leave headroom and don't run it on a near-full volume.
- **Never run it during a filesystem freeze.**
- The host knobs (`detect-zeroes=unmap` / offline convert) are **not visible from
  inside the guest** — they're the operator's responsibility on the PVE/host side.

## Why not implement this as `guest-fstrim`?
`guest-fstrim` is synchronous and fast on Linux (`FITRIM` is a metadata
operation — seconds). The macOS equivalent is a full-bandwidth zero-write of all
free space (minutes to hours), which would blow PVE's agent-command timeout and
look like a hang. It's also not the QGA operation. So the agent exposes the
*capability* it already has (`guest-exec`) plus this documented procedure, rather
than a command that can't honor its contract.
