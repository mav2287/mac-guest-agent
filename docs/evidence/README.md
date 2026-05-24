# Runtime Validation Evidence

This directory stores per-version evidence supporting Tier 1 / Tier 2 claims in [`../COMPATIBILITY.md`](../COMPATIBILITY.md). Anyone running a macOS version we haven't fully validated yet can drop their results here and the maintainer will fold the version into the matrix.

## Per-version layout

Each subdirectory is named for the **exact macOS build** the evidence comes from — for example `10.4.11/`, `10.11.6/`, `15.7.5/`.

```
docs/evidence/<version>/
├── verify.txt    # text output of: scripts/verify.sh <id> | tee verify.txt
├── verify.json   # the JSON appendix from the same run — embeds the
│                 # in-VM --self-test-json and --safe-test-json as parsed
│                 # objects, plus host-side check records and the
│                 # freeze-event "Filesystem frozen:" log line
└── NOTES.md      # (optional) host hardware, hypervisor version, OpenCore
                  # config notes, contributor name
```

The two files come out of a single `verify.sh` invocation: the human-readable text section ends, then a `JSON Appendix (paste into docs/evidence/<version>/verify.json)` header, then the JSON object. Split the output at that header.

`verify.sh` auto-detects the host transport (PVE / libvirt / UTM) or accepts `--transport <name>`, so the same file layout applies regardless of which hypervisor the contributor is on. The previous PVE-only filenames (`pve-verify.txt` / `pve-verify.json`) and three-file layout (`selftest.json` + `safetest.json` + `pve-verify.txt`) are still accepted — existing per-version directories won't be rewritten — but new submissions should use `verify.txt` + `verify.json`.

### Appendix schema 2.0

`verify.json` is JSON object with `schema_version: "2.0"` and these top-level fields:

| Field | Type | Notes |
|---|---|---|
| `schema_version` | string | `"2.0"` for the current schema. Earlier 1.0 appendices (from the Phase 3 `pve-verify.sh`) are still accepted but missing `host_environment`, `freeze_cycles_log`, `mount_dispatch_crosscheck`, and `freeze_cycles`. |
| `script_version` | string | Internal version stamp (date + script revision). |
| `generated_at` | string | UTC timestamp of the run. |
| `transport` | string | `"pve"` / `"libvirt"` / `"utm"` / `"qga-socket"`. |
| `vmid` | string | The supplied identifier (or `<REDACTED-VMID>` if redaction is on). |
| `agent_path`, `log_path` | string | Resolved guest-side paths (default `/usr/local/bin/mac-guest-agent`, `/var/log/mac-guest-agent.log`). |
| `freeze_cycles` | int | Number of freeze/thaw cycles configured (default 3). |
| `counts` | object | `{passed, failed}` rollup of all PASS/FAIL lines. |
| `host_checks` | array | One record per PASS/FAIL/INFO line, with `{section, level, name, detail}`. The structured form of the human-readable text section. |
| `host_environment` | object | Captured guest environment: `sw_vers`, `hardware` (hw_model/ncpu/memsize/cpu_brand), `kexts` (filtered to serial/virtio families), `serial_io_nodes` (ioreg subset), `mounts` (parsed mount table with `{device, mount_point, fstype, options}`), `launchd_status`, `log_file` (size+mtime). `null` if `--no-in-vm` or `--no-env-capture`. |
| `in_vm_selftest` | object | The full `mac-guest-agent --self-test-json` output as a parsed object — `null` if `--no-in-vm` or the agent binary isn't present. |
| `in_vm_safetest` | object | Same shape for `--safe-test-json`. |
| `freeze_log_tail` | string | The LAST `Filesystem frozen:` INFO line emitted to the agent log during the freeze cycles. `""` if freeze didn't run. |
| `freeze_cycles_log` | array | One entry per freeze cycle: `{cycle, frozen_n, thawed_n, fsfreeze_status, behavioural_check, post_thaw_check, freeze_log_line}`. Empty array if `--no-freeze`. |
| `mount_dispatch_crosscheck` | object | Sanity-check on the dispatch: `{expected_freeze_n, actual_freeze_n, upper_bound, match}` — `expected` from the captured mount table (mounts whose fstype isn't categorically skipped), `actual` from the last freeze cycle's wire response. `null` if `--no-freeze` or if env-capture skipped. |

The schema is additive — every 1.0 field is preserved in 2.0. Consumers reading 1.0 fields by name continue to work; consumers wanting the new context read the new fields. No migration of existing evidence files is required.

`NOTES.md` is genuinely optional — only include it if there's context that would help someone reproducing your setup (Apple Xserve vs commodity hardware, specific QEMU CPU type, OpenCore Serial settings, etc.).

## How to capture the evidence

The full validation sequence is documented in [COMPATIBILITY.md → Step 2: Runtime Validation](../COMPATIBILITY.md#step-2-runtime-validation-tier-2--tier-1). Short version:

**One-time setup inside the VM (install the agent):**

```bash
sudo cp mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

**On the host (one command — drives the in-VM diagnostics for you):**

```bash
curl -fsSL -o /tmp/verify.sh \
  https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/verify.sh
chmod +x /tmp/verify.sh
/tmp/verify.sh <identifier> | tee verify.txt
# split the output at the "JSON Appendix" header — the JSON below it
# is what goes into verify.json
```

`<identifier>` is a numeric VMID on PVE, a domain name on libvirt, a VM name on UTM. Pass `--transport pve|libvirt|utm|qga-socket` to force a transport (skip auto-detect). PII (IPv4 addresses, MAC addresses, the supplied identifier) is redacted by default; pass `--no-redact` if you want raw values. `--help` lists the rest of the flags (`--no-appendix`, `--no-in-vm`, `--no-env-capture`, `--no-freeze`, `--freeze-cycles N`, `--qga-socket PATH`, `--agent-path`, `--log-path`, `--exec-timeout`).

## How to submit

Two paths, pick whichever fits:

1. **Open a PR** adding the three files under `docs/evidence/<your version>/` and bumping the corresponding row in `../COMPATIBILITY.md`. Quickest if you're comfortable with PRs.
2. **Drop the outputs in an issue comment** (or a fresh issue) — the maintainer will commit them and update the matrix on your behalf.

## Privacy

Sanitise before committing:

- VM IDs — `<vmid>` placeholder is fine.
- IP addresses, MAC addresses — replace internal ones with `<redacted>` if you'd rather not publish them.
- Hostnames you'd rather not associate with the repo.
- Anything else host-side you'd rather not publish.

`verify.json` includes the in-VM `--self-test-json` and `--safe-test-json` outputs as parsed objects (`in_vm_selftest` / `in_vm_safetest`) — those are guest-side and generally safe. The host-side records (`host_checks`, `freeze_log_tail`) can include detected interface addresses, so give the file a quick read before committing. The default redaction (on unless `--no-redact`) covers IPv4 / MAC / supplied identifier.
