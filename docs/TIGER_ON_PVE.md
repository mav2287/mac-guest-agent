# Mac OS X 10.4 Tiger (Intel) on Proxmox VE / QEMU — Definitive Setup Guide

A reproducible, copy-pasteable recipe for running **Mac OS X 10.4.10 Tiger
Intel** as a guest under **Proxmox VE 9.x** on **modern QEMU/KVM (x86_64
host)**, with working SSH access for validation work.

Last verified: 2026-06-04 on PVE 9.1.1, QEMU 10.1.2, host Xserve3,1
(Intel Xeon W5590 / Nehalem).

---

## What this guide gets you

- Tiger 10.4.10 booting from a real Apple retail install on a 30 GB virtual
  disk
- Mouse, keyboard, display, sound (optional)
- SSH access from outside the VM (e.g. for running test binaries)
- A *known good* `qm` config you can copy into `/etc/pve/qemu-server/<vmid>.conf`
- Documented OpenCore quirks (which ones matter, which ones don't)

## What this guide is honest about

- The networking situation on modern QEMU/Q35 is **publicly unsolved** for
  Tiger 10.4 Intel: Tiger's `AppleIntel82545Ethernet` driver does not bind
  link-up from QEMU's e1000 PHY emulation, so the standard
  bridge/tap path delivers zero RX packets to the guest. We diagnose this
  and document the **slirp (user-mode NAT) workaround** — which to our
  knowledge is the only path that actually works end-to-end on PVE for
  Tiger Intel as of this writing.
- The pointer offset issue in noVNC (cursor drifts toward the edges of the
  guest framebuffer) is real and cosmetic; the keyboard works perfectly, and
  the QEMU monitor's `sendkey` / `mouse_move` can drive the GUI without it.
- This recipe targets *Tiger 10.4.10 Intel*. PowerPC Tiger has a completely
  different path (QEMU `-M mac99` / `sungem` NIC) and is out of scope.

---

## Critical findings up front

If you read nothing else:

1. **Machine type**: `pc-q35-7.0` works for boot. `pc-q35-6.1` also works.
   Do not use `pc-i440fx` on PVE — PVE always force-includes
   `pve-q35-4.0.cfg` via `-readconfig` and that defines q35-only buses
   (`pcie.0`, `pci.0`), which i440fx rejects with `Bus 'pcie.0' not found`.
2. **OpenCore** (not Clover, not bare OVMF) is the bootloader. Quirks listed
   below are mandatory.
3. **Disk bus**: IDE (`ide0`, `ide1`). AHCI/SATA panics Tiger's AppleAHCIPort.
   The OpenCore boot image can stay on `sata2` as a CD-ROM — that's fine.
4. **Disk wrapping**: the Tiger install image must be GPT-wrapped before
   `boot-uuid-media` will publish in OF. A raw HFS+ partition won't boot.
5. **USB**: EHCI (`usb-ehci`) only. No XHCI. Tiger has no USB 3 driver.
6. **CPU**: `Nehalem` or `Penryn`, both with `vendor=GenuineIntel`.
   Add `kvm=off` to hide the KVM hypervisor signature — Tiger's APIC code
   predates PV-EOI and certain hypervisor-aware behaviors. Strip
   `+kvm_pv_eoi,+kvm_pv_unhalt` from the PVE default CPU line.
7. **Networking**: `-netdev user,...` (slirp). **Do not** use `-netdev tap`
   for Tiger — Tiger sees the link as `inactive` and RX is dead. Slirp
   forces link-up at the e1000 device boundary internally. See *Networking*
   section for full rationale and the exact `hostfwd` rule for SSH.
8. **Boot args** (in OpenCore NVRAM): `keepsyms=1 -v cpus=1`. Tiger SMP
   on emulated CPUs is unstable; pin to single CPU.
9. **SMBIOS**: `iMac19,1` is fine. Tiger doesn't care for SMBIOS-keyed
   features; `iMac19,1` keeps OpenCore happy.
10. **Block `AppleTyMCEDriver`** for Xserve3,1 SMBIOS hosts (mandatory if
    your host SMBIOS triggers MCE pathways Tiger panics on).

---

## Prerequisites

- Proxmox VE 9.x host with KVM available (`/dev/kvm` exists, user has
  permission)
- Host CPU is Intel Nehalem-class or newer (EM64T + SSE4.1 + SSSE3) — these
  are the CPUID bits Tiger's grade_binary keys on
- ~15 GB of storage for the Tiger install ISO + boot image + EFI disk
- ~30 GB of storage for the Tiger root partition
- A Mac OS X 10.4.10 retail Install DVD ISO (we used `Tiger-10.4-anymachine.iso`,
  GPT-wrapped — see *Step 2.5: GPT-wrap the install ISO* below)
- An OpenCore bundle preconfigured for legacy macOS. We used a customized
  LongQT-sea OpenCore boot image (4.4 MB FAT16 raw disk image, mounted as
  CD-ROM)

---

## Step 1 — Stage assets on the PVE host

```sh
# On the PVE host
mkdir -p /var/lib/vz/template/iso

# 1. Mac OS X 10.4.10 install ISO (you must source this yourself — it's
#    Apple media). GPT-wrap it if it's a raw HFS+ image, so OpenFirmware
#    can publish boot-uuid-media. The HFS+J Apple_HFS partition must sit
#    inside a GPT.
ls -la /var/lib/vz/template/iso/Tiger-10.4-anymachine.iso

# 2. OpenCore boot image (4.4 MB FAT16 — see Step 2 for the quirks it must
#    contain). Drop the prebuilt image at:
ls -la /var/lib/vz/template/iso/oc-tiger-boot.img
```

---

## Step 2 — OpenCore boot image quirks

These are the values **inside** `EFI/OC/config.plist` of the OpenCore boot
image, *not* in the PVE VM config. They are mandatory and discovered by
elimination.

### `Kernel.Quirks`

| Key | Value | Why |
|---|---|---|
| `DisableLinkeditJettison` | `True` | XNU 792 jettisons __LINKEDIT differently from XNU 8000+; this preserves it. |
| `LegacyCommpage` | `True` | **Mandatory.** Tiger maps the commpage at addresses XNU >= 10.6 cannot tolerate; this remaps. Without it Tiger panics with `vm_map_create`. |
| `LapicKernelPanic` | `True` | Suppresses LAPIC-related kernel panics on emulated CPUs. |
| `IncreasePciBarSize` | `True` | Q35 ICH9 PCI BAR sizing — Tiger's PCI scan needs the larger window. |
| `ForceSecureBootScheme` | `True` | OpenCore housekeeping. |
| `ProvideCurrentCpuInfo` | `True` | XNU's CPUID parsing pre-10.6 needs OpenCore to provide synthesized cpuinfo. |

Everything else under `Kernel.Quirks` = `False`.

### `Booter.Quirks`

| Key | Value | Why |
|---|---|---|
| `SetupVirtualMap` | **`False`** | **Critical inversion vs every other modern macOS recipe.** vit9696 documented this in AcidAnthera bugtracker #2418 — `SetupVirtualMap=True` (which is the universal default for 10.5+) is **incompatible with 32-bit macOS**. Tiger panics with `Unable to find driver for this platform ACPI` if this is on. |
| `AllowRelocationBlock` | `True` | OpenCore must shift the kernel relocation block down for Tiger's address map. |
| `ProvideCustomSlide` | `True` | OpenCore picks a working slide value Tiger boots at. |
| `AvoidRuntimeDefrag` | `True` | Standard for legacy macOS guests. |
| `EnableWriteUnprotector` | `True` | Standard for legacy macOS guests. |
| `RebuildAppleMemoryMap` | `True` | Standard for legacy macOS guests. |

Everything else under `Booter.Quirks` = `False`. In particular do **not**
enable `DevirtualiseMmio`, `EnableSafeModeSlide`, `ProtectMemoryRegions` —
each causes a different panic on Tiger.

### `Kernel.Block`

Enable a block entry for:
```
Identifier  = com.apple.driver.AppleTyMCEDriver
Arch        = Any
MinKernel   = (empty)
MaxKernel   = (empty)
```

`AppleTyMCEDriver` panics on Xserve3,1-class host SMBIOS that does not
expose Apple's expected machine-check infrastructure. Block it
unconditionally for Tiger.

### `NVRAM` boot-args

```
boot-args = keepsyms=1 -v cpus=1
```

- `keepsyms=1` — keeps symbol table for panic traces
- `-v` — verbose mode; on a healthy boot Tiger still switches to graphical
  after launchd
- `cpus=1` — pin to single CPU. Tiger SMP on emulated CPUs is unstable.
  This is the single biggest source of "boots OK then random hang" issues
  if you forget it.

### `PlatformInfo.Generic.SystemProductName`

```
SystemProductName = iMac19,1
```

Tiger ignores most SMBIOS-keyed feature gates so the exact model is not
critical, but `iMac19,1` keeps OpenCore's own consistency checks happy.

### `Misc.Security.SecureBootModel`

```
SecureBootModel = Disabled
```

Tiger predates Secure Boot. Anything other than `Disabled` causes
OpenCore to refuse to launch the boot.efi.

### Step 2.5 — GPT-wrap the install ISO

If your install media is a raw HFS+ partition image, the OpenCore loader
will boot but Tiger will panic with `Still waiting for root device` because
no `boot-uuid-media` publishes. Wrap it in a GPT:

```sh
# On a Linux host with sgdisk + dd
truncate -s $(stat -c%s Tiger-10.4-anymachine-raw.hfs)+1M Tiger-wrapped.img
sgdisk --new=1:34:0 --typecode=1:af00 --change-name=1:"Apple_HFS" Tiger-wrapped.img
# then dd the HFS+ partition contents into the partition at LBA 34
```

(The OpenCore boot image we ship is already GPT-correct; only the Tiger
install ISO needs wrapping.)

---

## Step 3 — Create the PVE VM

Create VM 111 (or any unused VMID) via `qm create`:

```sh
qm create 111 \
  --name macos-10.4-tiger \
  --memory 2048 \
  --cores 1 --sockets 1 \
  --cpu Nehalem \
  --bios ovmf \
  --machine pc-q35-7.0 \
  --vga std \
  --tablet 0 \
  --ostype other \
  --scsihw virtio-scsi-single \
  --serial0 socket

# EFI disk for OVMF NVRAM
qm set 111 --efidisk0 nvme-storage:0,efitype=4m,pre-enrolled-keys=0

# Install target (30 GB thin)
# IMPORTANT: cache=writethrough (NOT writeback) — Tiger's HFS+ journal
# does not survive a hard host-side reset with cache=writeback. We learned
# this the hard way: multiple `qm reset` during testing with writeback
# corrupted the B-tree (`hfs_swap_BTNode: invalid forward link`, panic at
# init). writethrough costs ~5-10% on write-heavy workloads but is fine
# for an installer + agent VM. Alternative: `cache=none` (safer still,
# requires the storage backend to support O_DIRECT — LVM-thin does).
qm set 111 --ide1 nvme-storage:0,cache=writethrough,size=30G

# Install ISO (Tiger 10.4.10, GPT-wrapped) as ide0
qm set 111 --ide0 local:iso/Tiger-10.4-anymachine.iso,cache=writethrough,size=15G

# OpenCore boot.img as sata2 CD-ROM
qm set 111 --sata2 local:iso/oc-tiger-boot.img,media=cdrom

# Boot order: OC boot image first, then ide1 (installed Tiger)
qm set 111 --boot order=sata2\;ide0
```

### The `args:` line — everything that PVE can't express natively

This is the load-bearing line. **All of it matters.** Replace the
auto-generated `net0:` entry by deleting it and putting the NIC in `args`:

```sh
qm set 111 --delete net0
qm set 111 --args '-machine type=pc-q35-7.0+pve0 -device usb-ehci,id=ehci -device usb-kbd,bus=ehci.0 -device usb-tablet,bus=ehci.0 -cpu Penryn,vendor=GenuineIntel,kvm=off,+sse4.1,+sse4.2,+ssse3 -netdev user,id=net0,hostfwd=tcp:0.0.0.0:22111-10.0.2.15:22 -device e1000-82545em,netdev=net0,mac=XX:XX:XX:XX:XX:XX,bus=pcie.0,addr=0x4,id=net0'
```

Pieces of that args line, line-by-line:

- `-machine type=pc-q35-7.0+pve0` — re-declare the machine type. PVE adds
  one automatically too; the last `-machine` wins. We add this to be
  explicit.
- `-device usb-ehci,id=ehci` — USB 2.0 controller. Tiger HID stack works
  on EHCI's full-speed pass-through.
- `-device usb-kbd,bus=ehci.0` — USB keyboard.
- `-device usb-tablet,bus=ehci.0` — USB pointer in absolute mode. **Note**:
  the noVNC cursor mapping is offset toward edges (cosmetic; pointer
  events still reach Tiger correctly). The QEMU monitor's `mouse_move`
  and `sendkey` work without this offset.
- `-cpu Penryn,vendor=GenuineIntel,kvm=off,+sse4.1,+sse4.2,+ssse3` — explicit
  CPU. Note: PVE will also emit a `-cpu Nehalem,enforce,+kvm_pv_eoi,+kvm_pv_unhalt,vendor=GenuineIntel`
  earlier on the command line; the *last* `-cpu` wins. We strip the
  PV-EOI flags because Tiger's APIC code doesn't expect them.
- `-netdev user,id=net0,hostfwd=tcp:0.0.0.0:22111-10.0.2.15:22` —
  **THE BIG ONE**. Slirp user-mode NAT, with a host port-forward exposing
  Tiger's sshd on PVE-host:22111. See *Networking* below for why.
- `-device e1000-82545em,...,bus=pcie.0,addr=0x4,id=net0` — e1000-82545em
  is the QEMU device variant that exactly matches Tiger's
  `AppleIntel82545Ethernet.kext`. Place it at PCI slot 0x4 on pcie.0
  (not on a pcie-root-port — placement on a root port forces MSI
  capability negotiation that Tiger's driver mishandles).

### Final `qm config 111` for reference

```ini
agent: 0
args: -machine type=pc-q35-7.0+pve0 -device usb-ehci,id=ehci -device usb-kbd,bus=ehci.0 -device usb-tablet,bus=ehci.0 -cpu Penryn,vendor=GenuineIntel,kvm=off,+sse4.1,+sse4.2,+ssse3 -netdev user,id=net0,hostfwd=tcp:0.0.0.0:22111-10.0.2.15:22 -device e1000-82545em,netdev=net0,mac=XX:XX:XX:XX:XX:XX,bus=pcie.0,addr=0x4,id=net0
bios: ovmf
boot: order=sata2;ide0
cores: 1
cpu: Nehalem
efidisk0: nvme-storage:vm-111-disk-4,efitype=4m,pre-enrolled-keys=0,size=4M
ide0: nvme-storage:vm-111-disk-6,cache=writeback,size=15G
ide1: nvme-storage:vm-111-disk-7,cache=writeback,size=30G
machine: pc-q35-7.0
memory: 2048
name: macos-10.4-tiger
ostype: other
sata2: local:iso/oc-tiger-boot.img,media=cdrom,size=4403712
scsihw: virtio-scsi-single
serial0: socket
sockets: 1
tablet: 0
vga: std
```

---

## Step 4 — Install Tiger

> ⚠️ **READ THIS FIRST — the load-bearing rule that cost us two reinstalls:**
> Do **NOT** `qm reset 111`, `qm stop 111`, or pull power on the VM **at
> any point** between "Install Now" and your **first successful login at
> the Tiger desktop**. The Tiger install isn't fully committed until
> Setup Assistant completes AND you've logged in once. A hard reset
> mid-Setup-Assistant produces an unrecoverable `pmap-remove: mapping
> not in pv_list!` panic on next boot (we hit it; only fix is wipe +
> reinstall). If the VM genuinely freezes during this window and you
> must reset, use `qm shutdown 111` (graceful ACPI shutdown) FIRST and
> wait up to 60 s; only fall back to `qm stop` if that fails. With
> `cache=writethrough` on the disks (Step 3) HFS+ won't corrupt from a
> reset, but the *install state itself* still won't be finished.

### Step 4a — Boot the installer

1. `qm start 111`
2. Open the noVNC console from the PVE UI.
3. **OpenCore picker appears for ~5 s** with a countdown timer. If you
   miss it, it auto-boots the last-selected volume (the installer the
   first time, the installed Tiger after that). To interrupt the
   countdown and force the picker: spam **Space** in the noVNC viewport
   (or via QEMU monitor: `echo 'sendkey spc' | qm monitor 111` in a
   loop). The picker shows: `Mac OS X Install Disc 1`, your installed
   volume (if any), `Reset NVRAM`, `Toggle SIP`. Use **Left/Right arrow
   keys + Return** to select. Pick `Mac OS X Install Disc 1`.

### Step 4b — Walk through the installer

4. **Language picker**: English (or your choice) → Return.
5. **Welcome to the Mac OS X Installer** → Continue.
6. **License Agreement** → the **Agree** button is on the right, but
   **Disagree** is the default-highlighted button. Tab to switch focus to
   Agree, then Return. (If you press Return immediately, you cancel the
   install.)
7. **Select Destination**: the screen will be **empty on first install**
   — the new `ide1` 30 GB disk is unformatted and Tiger's installer
   hides unformatted disks.

### Step 4c — Format the destination via Disk Utility

8. From the installer's **menu bar → Utilities → Disk Utility** (also
   reachable by `Ctrl-F2` for menu-bar focus → arrow keys → Return; the
   Disk Utility item is the 4th entry under Utilities, OR press `D`
   inside the open menu to letter-jump).
9. In Disk Utility's left sidebar, select the **unnamed 30 GB QEMU
   HARDDISK** (the device, not any partition under it).
10. Click the **Erase** tab in the right pane.
11. **Volume Format: Mac OS Extended (Journaled)**.
12. **Name: `Tiger`** (or any short alphanumeric name).
13. Click **Erase** at the bottom right. Confirm. Takes ~10 s.
14. **Quit Disk Utility** (Cmd-Q) — you're back at Select Destination.

### Step 4d — Install onto the formatted volume

15. The new `Tiger` volume now appears with a green arrow. Select it →
    **Continue**.
16. **Easy Install** screen → **Customize** (bottom left). Uncheck:
    - Additional Printer Drivers (~600 MB you don't need in a VM)
    - Additional Languages (~600 MB you don't need)
    - X11 (~80 MB)
    - Xcode Tools (~700 MB — adds compiler; if you need build tools on
      Tiger keep it, otherwise skip)
    - All optional bundled apps (iTunes etc — Tiger ships them all)
    Bare-minimum total is ~1.5 GB and installs in ~10-15 min on
    emulated IDE.
17. Click **Install** → confirms space → starts writing.
18. When complete (~10-15 min), Tiger **reboots itself**.
    (The boot order `sata2;ide0` means OC boots first, scans all
    available volumes, and presents the new `Tiger` volume for selection.
    Auto-boot timer picks it after 5 s.)

> ⚠️ **From this point until you complete Setup Assistant — read the
> warning at the top of Step 4. No `qm reset`. No `qm stop`. Let
> Tiger run uninterrupted through the next ~15 minutes.**

### Step 4.5 — Setup Assistant + enable SSH

Tiger boots into Setup Assistant. Walk through:

1. **Welcome video** plays for ~30 s. Wait it out — there is no skip
   button. If your VM was set with `vga: std`, audio is muted by
   default (the Welcome video is silent for you).
2. **"Before You Begin..." keyboard identification wizard** appears
   ("Your keyboard cannot be identified and will not be usable until it
   is identified"). Click **OK** → Tiger prompts you to press specific
   keys (typically the one to the right of left-Shift, then the one to
   the right of left-Shift on another section). Press what it asks.
   This sets the keyboard layout (US ANSI, JIS, ISO, etc).
3. **Welcome screen with language list**: select your locale → Continue.
4. **"Do you already own a Mac?"**: select **"Do not transfer my
   information now"** → Continue.
5. **"Select a wireless service"** (if it appears — depends on Tiger
   minor version): select **"My computer does not connect to the
   Internet"** → Continue.
6. **"Enter your Apple ID"**: leave blank → Continue.
7. **Registration Information**: fill with arbitrary values, or just
   tab past them. (Tiger 10.4 honors **Cmd-Q** to abort registration
   cleanly: hit Cmd-Q → confirm with Skip in the dialog that appears.
   Skipping is the fastest path.)
8. **Create Your Account**:
   - Name: anything (used as full display name)
   - Short Name: **`user`** (must match your SSH harness; lowercase, no
     spaces — once you Continue this is locked)
   - Password: **`password`** (or whatever you'll use; remember it)
   - Hint: optional
9. **Pick a picture for this account**: click any → Continue.
10. **Don't Forget to Register**: Continue.
11. **Thank You** → **Done**. Tiger drops into Finder.

12. **NOW it is safe to `qm reset` if needed** — the install is fully
    committed.

13. **Enable SSH**:
    - Apple menu → **System Preferences → Sharing** (the icon under
      "Internet & Network")
    - **Services** tab → check **"Remote Login"** → status flips to
      "Remote Login: On" within a second
    - This starts Tiger's `OpenSSH 4.5p1` sshd on port 22

14. **(Optional) eject the install DVD**: drag the **Mac OS X Install
    DVD** icon from the desktop into the Trash icon (which turns into
    an Eject icon when dragging a removable disk). Or `diskutil eject
    disk0s2` in Terminal.

15. **(Optional) disable display sleep** for unattended testing:
    System Preferences → Energy Saver → drag both sliders to **Never**.
    Tiger 10.4 has no `caffeinate` command (added in 10.8).

### Step 4.6 — "Erase and Install" if you're reinstalling onto a previously-installed volume

If the destination volume already has a Tiger install (from a failed
attempt) and you want to be sure the new install is clean, the
installer's default behavior is **Upgrade Mac OS X** — it preserves the
old `/Library`, `/Users`, etc and only writes over `/System`. That can
inherit on-disk corruption. To force a clean wipe at install time:

- On **Select Destination**, after selecting the volume, click the
  **Options...** button at the bottom left (NOT Continue).
- The Options sheet shows three radio buttons: **Upgrade Mac OS X**
  (default), **Archive and Install** (saves old system to
  `/Previous System`), **Erase and Install** (full wipe).
- Pick **Erase and Install** → OK.
- Continue with the install normally. This takes ~2 min longer (for
  the wipe) but guarantees no inherited state.

Alternatively (what we do above in Step 4c): wipe via Disk Utility *before*
hitting Continue, then the installer just sees an empty volume and the
distinction doesn't apply.

---

## Step 5 — Networking (the load-bearing section)

### What does *not* work

A standard PVE-managed `net0: e1000,bridge=vmbr0,firewall=0`, or any
direct `-netdev tap,...` configuration, **produces dead networking on
Tiger 10.4**. We exhaustively verified:

| Configuration | Result |
|---|---|
| `e1000` (82540EM) at PVE default slot 0x12 | Tiger never sends DHCP DISCOVER |
| `e1000-82545em` at slot 0x12 | Tiger sends exactly 1 DHCP DISCOVER, receives OFFER from LAN DHCP, never sends DHCP REQUEST, never ARP-replies. RX `Ipkts: 0` lifetime. |
| `e1000-82545em` on dedicated `pcie-root-port` (Somlo IRQ-isolation fix) at addr=0x1c or 0x10 | Same: 1 DISCOVER → silence |
| `rtl8139` (Realtek 8139C+) | Zero packets out from Tiger ever |
| `i82557b` (EEPro100) | `kextd[37]: a link/load error occured for kernel extension AppleIntel8255x.kext` — the Tiger driver is broken in the install |
| `kernel-irqchip=split` machine option | No change |
| `kvm=off` on CPU line | No change |
| Strip `+kvm_pv_eoi`/`+kvm_pv_unhalt` | No change |
| Disable PVE per-VM firewall | No change |
| Disable host-side TX/RX/TSO/GSO offloads on tap111i0 | No change |
| Force fixed media inside Tiger: `sudo ifconfig en0 media 100baseTX mediaopt full-duplex` | No change — `status: inactive` persists |
| QEMU monitor `set_link net0 off; set_link net0 on` | No change |
| Machine type downgrade to `pc-q35-6.1` | No change |
| Machine type swap to `pc-i440fx-7.2` | Rejected by PVE: `pve-q35-4.0.cfg:7: Bus 'pcie.0' not found` (PVE force-includes the q35 config) |

### Diagnosis: PHY auto-neg never completes for Tiger

Inside Tiger:

```
$ ifconfig en0
en0: flags=8863<UP,BROADCAST,SMART,RUNNING,SIMPLEX,MULTICAST> mtu 1500
        ether XX:XX:XX:XX:XX:XX
        media: autoselect status: inactive       ← driver says LINK DOWN
$ netstat -in
Name  Mtu   Network  Address          Ipkts Opkts
en0   1500  <Link#2> XX:XX:XX:XX:XX:XX    0     8    ← RX=0, TX works for DISCOVERs only
$ ioreg -l | grep -A3 AppleIntel82
| +- AppleIntel82545Ethernet <class AppleIntel82545Ethernet, !registered, !matched, active, busy 0, retain count 7>
```

The kext loads, matches the PCI ID `8086:100f`, and TX works at the
descriptor level (DHCP DISCOVER frames egress the host tap). But the kext
reads the PHY-status MII register and concludes "link down" — because
QEMU's `hw/net/e1000.c` MII emulation does not assert
auto-negotiation-completion in a way Tiger's 2005-era driver recognizes.
With link reported down, the driver discards every inbound RX frame.

This is a real QEMU/Tiger interaction bug. Our research agent confirmed
that **no public working config exists** for Tiger 10.4 Intel networking
under modern QEMU/Proxmox with a tap/bridge backend. The community
running Tiger workloads at scale uses real Apple Intel hardware,
VirtualBox, or VMware Fusion — not QEMU.

### The workaround: slirp (user-mode NAT)

QEMU's built-in `-netdev user,...` (slirp) runs an internal TCP/IP stack
that NATs to the host. Its e1000 interaction is different: slirp **forces
link state up** at the QEMU device level regardless of host carrier
state. With slirp, Tiger's driver finally sees an active link, RX path
flows normally, and Tiger's built-in DHCP client gets an IP from slirp's
internal DHCP (`10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`).

Tradeoff: slirp is NAT-only, so external hosts cannot initiate inbound
L2/L3 connections to Tiger. We expose Tiger's sshd through a host
port-forward.

The exact args line (already in *Step 3*):

```
-netdev user,id=net0,hostfwd=tcp:0.0.0.0:22111-10.0.2.15:22
-device e1000-82545em,netdev=net0,mac=XX:XX:XX:XX:XX:XX,bus=pcie.0,addr=0x4,id=net0
```

Now PVE host port 22111 → Tiger:22.

### Verify networking is up

From the PVE host:

```sh
# Confirm slirp port-forward is listening
ss -tlnp | grep 22111
# Output: LISTEN 0  1  0.0.0.0:22111  0.0.0.0:*  users:(("kvm",pid=...,fd=22))

# Confirm Tiger's sshd banner
nc -v -w 3 127.0.0.1 22111
# Output: SSH-1.99-OpenSSH_4.5
```

If you see `SSH-1.99-OpenSSH_4.5` you have a working Tiger network. Done.

---

## Step 6 — SSH access from your workstation

Tiger ships OpenSSH 4.5p1 (May 2007). Modern OpenSSH 9.x clients refuse its
default key-exchange, host-key, cipher, and MAC algorithms. You must
re-enable them per-connection.

### From the PVE host directly

```sh
# Install sshpass once (the password "password" is from your Tiger user
# account; substitute whatever you set).
apt-get install -y sshpass

SSHPASS=password sshpass -e ssh \
  -o KexAlgorithms=+diffie-hellman-group1-sha1 \
  -o HostKeyAlgorithms=+ssh-rsa \
  -o Ciphers=+aes128-cbc,3des-cbc \
  -o MACs=+hmac-sha1 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -p 22111 \
  user@127.0.0.1 \
  "uname -a; sw_vers"
```

Expected:
```
Darwin users-computer.local 8.10.3 Darwin Kernel Version 8.10.3:
  Wed Jun 27 23:29:36 PDT 2007; root:xnu-792.23.3~1/RELEASE_I386 i386 i386
ProductName:    Mac OS X
ProductVersion: 10.4.10
BuildVersion:   8R4088
```

### From a remote workstation through the PVE host

```sh
# Open an SSH tunnel: local 2222 → PVE-host:22111 → Tiger:22
ssh -L 2222:127.0.0.1:22111 root@<pve-host>

# In another terminal, connect to Tiger as if it were localhost:2222
SSHPASS=password sshpass -e ssh \
  -o KexAlgorithms=+diffie-hellman-group1-sha1 \
  -o HostKeyAlgorithms=+ssh-rsa \
  -o Ciphers=+aes128-cbc,3des-cbc \
  -o MACs=+hmac-sha1 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -p 2222 user@127.0.0.1 \
  "uname -a"
```

### Copying files into Tiger

The same legacy options work for `scp`. Important: **use `scp -O`**
(legacy SCP protocol). Modern `scp` defaults to SFTP which Tiger's OpenSSH
4.5 doesn't support.

```sh
SSHPASS=password sshpass -e scp -O \
  -o KexAlgorithms=+diffie-hellman-group1-sha1 \
  -o HostKeyAlgorithms=+ssh-rsa \
  -o Ciphers=+aes128-cbc,3des-cbc \
  -o MACs=+hmac-sha1 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -P 22111 \
  ./mybinary user@127.0.0.1:/tmp/
```

---

## Troubleshooting matrix

| Symptom | Cause | Fix |
|---|---|---|
| Tiger panics at `vm_map_create` | `LegacyCommpage` quirk not enabled | Enable `Kernel.Quirks.LegacyCommpage = True` in OC config.plist |
| `Unable to find driver for this platform ACPI` panic | `SetupVirtualMap` quirk is enabled (it's universally `True` for 10.5+) | Set `Booter.Quirks.SetupVirtualMap = False` — Tiger is the inversion case |
| `Still waiting for root device` | Raw HFS+ install ISO; no `boot-uuid-media` publishes | GPT-wrap the install ISO before attaching |
| `AppleAHCIPort` panic mounting root | Install media on AHCI/SATA bus | Use IDE (`ide0`/`ide1`) for the install + root disk; OC boot image can stay on SATA CD-ROM |
| Tiger boots to console verbose log, never reaches WindowServer / login | Hung kext load (often `AppleTyMCEDriver` on Xserve3,1 host SMBIOS) | Block `com.apple.driver.AppleTyMCEDriver` in `Kernel.Block` |
| Tiger installer "Select Destination" empty | The new disk is unformatted; Tiger hides it | `Utilities → Disk Utility → Erase` the disk as Mac OS Extended (Journaled) first |
| noVNC mouse drifts toward screen edges | Canvas/scaling mismatch between noVNC's logical canvas and guest framebuffer | Cosmetic; keyboard works fine, and `qm monitor` `mouse_move` is exact |
| `kextd: link/load error AppleIntel8255x.kext` | EEPro100 Tiger kext is corrupted/incompatible | Don't use `i82557b` family NIC; use `e1000-82545em` |
| Tiger sends 1 DHCP DISCOVER then never any more outbound traffic | The e1000 PHY-auto-neg-doesn't-bind bug | Switch from `-netdev tap` to `-netdev user` (slirp) — see *Networking* |
| `dyld: unknown required load command 0x80000022` running an Intel binary on Tiger | Binary's x86_64 slice has `LC_DYLD_INFO_ONLY`; Tiger's dyld (<10.5) cannot parse | Re-link x86_64 slice with `-Wl,-ld_classic -mmacosx-version-min=10.5 -Wl,-platform_version,macos,10.5,10.13` — emits classic `LC_SYMTAB`/`LC_DYSYMTAB` instead |
| `dyld: Symbol not found: ___stack_chk_guard` running x86_64 slice on Tiger | clang's default `-fstack-protector` references a symbol Tiger's libSystem doesn't export | Add `-fno-stack-protector` to the x86_64 build command line |
| SCP from modern host fails with "subsystem request failed" | Modern OpenSSH defaults to SFTP; Tiger's OpenSSH 4.5 only does legacy SCP | Use `scp -O` (capital O — force legacy protocol) |
| `ssh: command-line line 0: Bad key types '+ssh-rsa,ssh-dss'` | Modern OpenSSH renamed `PubkeyAcceptedKeyTypes` to `PubkeyAcceptedAlgorithms`; don't try to use the old name | Drop the option — password auth doesn't need it |
| Tiger panics at `hfs_swap_BTNode: invalid forward link` / `pid 1 exited` / `init died` after a `qm reset` or host crash | HFS+ journal corruption from `cache=writeback` losing in-flight writes on hard reset | Use `cache=writethrough` (or `cache=none`) on every Tiger disk. Recover existing corruption by booting the install DVD → Utilities → Disk Utility → First Aid → Repair Disk on the affected volume. If GUI Disk Utility can't fix it, drop to Terminal (Utilities menu) and run `fsck_hfs -fy /dev/disk1s2` (or whatever `diskutil list` shows for the Apple_HFS partition). |
| Tiger panics at `pmap-remove: mapping not in pv_list!` in `_load_init_program` early in boot | Hard reset (`qm reset`/`qm stop`) was performed during Setup Assistant — the Tiger first-boot finalization never completed and the install state on disk is inconsistent | Cannot be repaired by `fsck_hfs` (filesystem is clean, the *install state* is corrupted). Boot install DVD → Disk Utility → Erase the volume → reinstall Tiger. This time do not reset until you've logged in to the Tiger desktop AT LEAST ONCE. |
| OpenCore picker boots into the wrong (corrupted/old) volume automatically | Picker has a 5 s countdown timer with last-selected as the default | At VM start, spam `Space` keystrokes (via noVNC or `sendkey spc` in QEMU monitor) for ~4 s — that interrupts the timer. Then use Left/Right arrows + Return to select Mac OS X Install Disc 1 or the desired volume. |
| Tiger `sshd` stops accepting connections after many rapid SSH attempts (`kex_exchange_identification: read: Connection reset by peer`) but TCP port 22 still answers | Tiger's OpenSSH 4.5 hits its default `MaxStartups` (typically 10) and starts rejecting; the backoff is long and unreliable on Tiger | Reboot the VM (`qm reset 111`, *safe once you have completed first login*). Settles in ~70 s. To avoid: don't loop SSH connection attempts — use a single persistent connection (`-M -S /tmp/tigerctl`) and reuse it. |
| noVNC mouse "dead-on at center, drifts toward edges" (linear-from-center scale error) | **Three layers must all agree on resolution AND noVNC must not draw the cursor itself.** OC's GOP framebuffer, Tiger's WindowServer display, and noVNC's canvas must match. THEN noVNC's "Local Cursor" feature (which renders the cursor via CSS overlay rather than letting Tiger draw it in the framebuffer) introduces an independent scale error in the CSS positioning math. | **All three required**: (1) Pin OC: edit `EFI/OC/config.plist` to set `UEFI.Output.Resolution = "1280x800"` + `ForceResolution = True` + `ProvideConsoleGop = True`. (2) Set Tiger: System Preferences → Displays → 1280×800. (3) noVNC client: open the side panel (▶ arrow on left edge), settings gear, **UNCHECK "Local Cursor"** — this is the load-bearing setting. With Local Cursor off, Tiger draws the cursor in the framebuffer (no CSS overlay = no scaling math = pixel-perfect). The Local Cursor preference persists in browser localStorage. |
| Need to drive Tiger GUI from outside via QEMU monitor (e.g. mouse genuinely broken) | (Driving GUI without using noVNC) | Three options: (a) use `Ctrl-F2` to focus the menu bar, then arrow keys + Return for menu navigation (no mouse needed); (b) hot-add a relative mouse via QEMU monitor — `device_add usb-mouse,bus=ehci.0,id=mouse2`, then `mouse_set 5` to activate it, then click in noVNC to grab focus (relative mice need pointer capture); (c) drive specific clicks via `mouse_move <x> <y>; mouse_button 1; mouse_button 0` where coords are in QEMU tablet space 0-32767. |
| PVE Console dropdown defaults to `xterm.js` instead of noVNC | PVE auto-picks `xterm.js` whenever a VM has `serial0: socket` configured | Per-VM: `qm set <vmid> --delete serial0` (removes the serial device entirely). Cluster-wide: add `console: html5` to `/etc/pve/datacenter.cfg` (sets noVNC as default for all VMs regardless of serial0). |
| Need to run a command from outside but Tiger has no `nm`, `caffeinate`, or modern utilities | Default Tiger 10.4.x install does not include Developer Tools (Xcode) which would provide `nm`, `gcc`, etc | What Tiger DOES ship: `/usr/bin/lipo`, `/usr/bin/otool`, `/usr/bin/file`, `/usr/bin/md5`, `/usr/sbin/diskutil`, `/usr/sbin/fsck_hfs`, `/usr/bin/ssh`, `/usr/bin/scp`. For `nm`-equivalent inspection, scp the binary back to a host with `nm`. To prevent sleep on Tiger 10.4 use `pmset` (not `caffeinate`): `sudo pmset -a sleep 0 displaysleep 0 disksleep 0`. |

---

## What does NOT work (do not try these)

These were tried during diagnosis and either don't fix the problem or
make it worse. Listed so you don't repeat the cycle.

- **Different e1000 PCI slots** (Somlo IRQ-isolation): tried slots 0x05,
  0x10, 0x1c (dedicated `pcie-root-port`). No effect on Tiger.
- **`kernel-irqchip=split`**: tried via `-machine pc-q35-7.0,kernel-irqchip=split`. No effect.
- **`kvm=off`**: tried in `-cpu` line. No effect on networking (but it's
  fine to keep — costs nothing).
- **Strip `+kvm_pv_eoi`/`+kvm_pv_unhalt`**: tried. No effect on networking.
  Keep them off anyway — Tiger's APIC code predates them.
- **`pc-q35-6.1` machine type**: works fine for boot but no effect on
  networking.
- **`pc-i440fx-*` machine types**: rejected by PVE because PVE
  force-includes `pve-q35-4.0.cfg`. Would require hand-running QEMU
  outside PVE.
- **rtl8139 NIC** (`AppleRTL8139Ethernet.kext`): Tiger emits zero packets
  on QEMU's RTL8139C+. The C+ variant has different DMA semantics from
  the plain 8139 that the kext targets.
- **`i82557b` / `i82559er` (EEPro100)**: Tiger's `AppleIntel8255x.kext`
  has a link/load error in the 10.4.10 install we tested. Possibly
  recoverable by reinstalling the kext, but slirp is simpler.
- **VMware vmxnet3 / vmxnet2**: No Tiger 10.4 kext exists.
- **virtio-net**: No Tiger driver — virtio-net for macOS is 10.13+.
- **e1000e (82574L)**: No working Tiger kext (`IntelMausi`/`AppleIntelE1000e`
  are 10.8+).
- **Forcing fixed media via `ifconfig en0 media 100baseTX`**: Tiger's
  driver checks PHY status register directly and ignores user-space
  media override.
- **Host-side `set_link net0 off; set_link net0 on` via QEMU monitor**:
  doesn't trigger Tiger to re-evaluate link state.
- **Host-side ethtool offload disable** (`tx off rx off tso off gso off
  gro off lro off` on tap interface): no effect.
- **PVE firewall disable** (`firewall=0`): no effect (firewall wasn't
  the blocker).

---

## Sources & credits

- Gabriel Somlo, *Running Mac OS X as a QEMU/KVM Guest*: the canonical
  reference for e1000 IRQ isolation on modern QEMU. Documents the
  ICR-self-clears-on-read race condition for `AppleIntel8254XEthernet`.
  https://www.contrib.andrew.cmu.edu/~somlo/OSXKVM/index_old.html
- LongQT-sea, *OpenCore-ISO* README: the Tiger compatibility matrix
  (q35 ≤ 10.0, Nehalem, Intel E1000, SATA, OVMF) and the source of the
  OpenCore boot image we customized. Asserts "Intel E1000" works but
  ships no example config. https://github.com/LongQT-sea/OpenCore-ISO
- vit9696, AcidAnthera bugtracker #2418: documents
  `Booter.Quirks.SetupVirtualMap` incompatibility with 32-bit macOS.
- vit9696, mac-guest-agent issue #9: the upstream report that x86_64-slice
  selection on Tiger/Leopard with `LC_DYLD_INFO_ONLY` is fatal — which
  drove the v2.5.4 fix this VM exists to validate.
- royalgraphx, *LegacyOSXKVM* SLeopard-Boot.sh: the closest known-good
  Snow Leopard reference config (works for 10.6, not 10.4).
  https://github.com/royalgraphx/LegacyOSXKVM
- foxlet/macOS-Simple-KVM Issue #561 (open, no answer):
  https://github.com/foxlet/macOS-Simple-KVM/issues/561 —
  confirms the public knowledge gap.

---

## TL;DR

If you want to skip the explanation and just bring up Tiger:

1. Drop `oc-tiger-boot.img` (with the quirks above) at
   `/var/lib/vz/template/iso/`.
2. Drop your GPT-wrapped Tiger 10.4.10 install ISO at
   `/var/lib/vz/template/iso/`.
3. `qm create` as in Step 3, then paste the `args:` line verbatim.
   **Set `cache=writethrough` on every disk** (Step 3 covers this).
4. Install Tiger:
   a. Boot installer (spam Space to interrupt OC picker timer if needed)
   b. Tab+Return through License → Agree
   c. Utilities → Disk Utility → Erase the 30 GB disk as Mac OS Extended
      Journaled (name `Tiger`) → Cmd-Q
   d. Select Tiger → Customize → uncheck Printer/Languages/X11/Xcode →
      Install (~15 min)
   e. Tiger reboots itself
5. **DO NOT `qm reset` until Setup Assistant + first login complete**.
   Walk through Setup Assistant: keyboard ID, locale, "don't transfer",
   "no internet", Cmd-Q to skip registration, create user
   `user`/`password`.
6. System Preferences → Sharing → check Remote Login.
7. SSH in from PVE host on port 22111 with the legacy cipher options in
   Step 6. Use `scp -O` (capital O) for file copies.
5. Enable Remote Login in System Preferences → Sharing.
6. SSH in from the PVE host on port 22111 with the legacy cipher options
   in Step 6.

That's the whole recipe.
