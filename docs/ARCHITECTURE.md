# Architecture

## Data Flow

```
Proxmox VE Host                          macOS Guest VM
┌─────────────┐                         ┌─────────────────┐
│  qm agent   │                         │  mac-guest-agent │
│  107 ping   │                         │  (LaunchDaemon)  │
└──────┬──────┘                         └────────┬────────┘
       │                                         │
       │ JSON-RPC                    JSON-RPC     │
       │ over Unix socket    over serial port     │
       ▼                                         ▼
┌──────────────┐    ISA Serial    ┌──────────────────┐
│ QEMU chardev │◄────16550 UART───►│ /dev/cu.serial1  │
│ socket       │                  │ (Apple16X50Serial)│
│ 107.qga      │                  └──────────────────┘
└──────────────┘
```

## Protocol

The agent speaks the standard QEMU Guest Agent protocol (QGA/QMP):

1. **PVE connects** to QEMU's Unix socket (`/var/run/qemu-server/<vmid>.qga`)
2. **PVE sends** sync-delimited + actual command in one write:
   ```
   {"execute":"guest-sync-delimited","arguments":{"id":12345}}\n
   {"execute":"guest-ping"}\n
   ```
3. **QEMU bridges** the data through the ISA serial device to the guest
4. **Agent reads** from `/dev/cu.serial1`, parses JSON lines
5. **Agent responds** with `\xFF` + sync response + `\n`, then command response + `\n`:
   ```
   \xFF{"return":12345}\n
   {"return":{}}\n
   ```
6. **QEMU bridges** the response back to the Unix socket
7. **PVE reads** the response, matches the sync ID, parses the command result

## Serial Port Configuration

The agent opens `/dev/cu.serial1` (or auto-detected device) with:
- **No O_NONBLOCK** — blocking I/O with poll() for timeouts
- **Full raw termios**: `c_iflag=0, c_oflag=0, c_lflag=0, c_cflag=CS8|CREAD|CLOCAL`
- This disables ALL terminal processing (no echo, no ICANON, no OPOST, no ISTRIP)

Critical: PVE sends sync + command in ONE write (103 bytes). The agent must:
1. Read all bytes into a buffer
2. Extract the first line (sync), process it, respond
3. **Check the buffer for the next line BEFORE polling** (buffer-check-before-poll)
4. Extract the second line (command), process it, respond

Without step 3, the agent would wait for a poll() timeout (1 second) before processing the command, causing PVE to time out.

## Command Dispatch

```
channel_read_message()
    │
    ▼
protocol_parse_request()  →  cJSON parse
    │
    ▼
commands_dispatch()
    │
    ├─ find_command() in registry (44 commands)
    │
    ├─ check enabled (block-rpcs / allow-rpcs)
    │
    ├─ call handler(args, &err_class, &err_desc)
    │
    └─ return cJSON result or error
    │
    ▼
protocol_build_response() or protocol_build_error()
    │
    ▼
channel_send_response() or channel_send_delimited_response()
```

## File Layout

```
src/
├── main.c          Entry point, CLI parsing, config file, signal handling
├── agent.c         Main loop: read → parse → dispatch → respond
├── channel.c       Serial port I/O: open, raw mode, poll, read, write
├── protocol.c      JSON message framing: parse requests, build responses
├── commands.c      Command registry, dispatch, block/allow filtering
├── cmd-info.c      ping, sync, sync-delimited, info
├── cmd-system.c    osinfo, hostname, timezone, time, users, load
├── cmd-power.c     shutdown, suspend-disk/ram/hybrid
├── cmd-hardware.c  vcpus, memory-blocks, cpustats (Mach APIs + sysctl)
├── cmd-disk.c      disks (diskutil), fsinfo (getmntinfo), diskstats
├── cmd-fs.c        fsfreeze (simulated), fstrim (no-op)
├── cmd-network.c   interfaces (getifaddrs + AF_LINK for MAC)
├── cmd-file.c      file open/close/read/write/seek/flush (handle table)
├── cmd-exec.c      exec (fork/execvp with pipe capture), exec-status
├── cmd-ssh.c       SSH authorized_keys get/add/remove
├── cmd-user.c      set-user-password (dscl via stdin pipe)
├── util.c          run_command, base64, file I/O, string helpers
├── log.c           Logging: file + syslog (os_log on 10.12+)
├── compat.c        macOS version detection, polyfills
├── service.c       LaunchDaemon install/uninstall/update
└── third_party/
    └── cJSON.c/h   JSON parser (embedded, MIT license)
```

## macOS API Usage

| Need | API | Available Since |
|---|---|---|
| Memory stats | `host_statistics64()` (Mach) | 10.0 |
| CPU stats | `host_statistics(HOST_CPU_LOAD_INFO)` (Mach) | 10.0 |
| Total memory | `sysctlbyname("hw.memsize")` | 10.0 |
| CPU count | `sysctlbyname("hw.logicalcpu")` | 10.4 |
| Network interfaces | `getifaddrs()` + `AF_LINK` | 10.2 |
| Mount info | `getmntinfo()` + `statfs()` | 10.0 |
| Disk info | `diskutil` (command) | 10.4 |
| OS info | `sw_vers` (command) + `uname()` | 10.0 |
| Users | `getutxent()` | 10.3 |
| Load averages | `getloadavg()` | POSIX |
| Time | `gettimeofday()` / `settimeofday()` | POSIX |
| Hostname | `gethostname()` | POSIX |
| Process exec | `fork()` / `execvp()` | POSIX |
| Serial port | `Apple16X50Serial.kext` (ISA 16550) | 10.0 |
| Password | `dscl` (command, stdin pipe) | 10.2 |
| Shutdown | `osascript` / `shutdown` (commands) | 10.0 |
| Sleep | `pmset` (command) | 10.0 |
| Logging | `syslog()` + file | 10.0 |
