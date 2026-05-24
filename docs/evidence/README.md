# Runtime Validation Evidence

This directory stores per-version evidence supporting Tier 1 / Tier 2 claims in [`../COMPATIBILITY.md`](../COMPATIBILITY.md). Anyone running a macOS version we haven't fully validated yet can drop their results here and the maintainer will fold the version into the matrix.

## Per-version layout

Each subdirectory is named for the **exact macOS build** the evidence comes from — for example `10.4.11/`, `10.11.6/`, `15.7.5/`.

```
docs/evidence/<version>/
├── pve-verify.txt    # text output of: scripts/pve-verify.sh <vmid> | tee pve-verify.txt
├── pve-verify.json   # the JSON appendix from the same run — embeds the
│                     # in-VM --self-test-json and --safe-test-json as
│                     # parsed objects, plus host-side check records and
│                     # the freeze-event "Filesystem frozen:" log line
└── NOTES.md          # (optional) host hardware, PVE version, OpenCore
                      # config notes, contributor name
```

The two files come out of a single `pve-verify.sh` invocation: the human-readable text section ends, then a `JSON Appendix (paste into docs/evidence/<version>/pve-verify.json)` header, then the JSON object. Split the output at that header.

The previous three-file layout (separate `selftest.json` + `safetest.json` + `pve-verify.txt`) is still accepted — existing per-version directories won't be rewritten. New submissions should prefer the two-file form because everything comes from one host-side command.

`NOTES.md` is genuinely optional — only include it if there's context that would help someone reproducing your setup (Apple Xserve vs commodity hardware, specific QEMU CPU type, OpenCore Serial settings, etc.).

## How to capture the evidence

The full validation sequence is documented in [COMPATIBILITY.md → Step 2: Runtime Validation](../COMPATIBILITY.md#step-2-runtime-validation-tier-2--tier-1). Short version:

**One-time setup inside the VM (install the agent):**

```bash
sudo cp mac-guest-agent /usr/local/bin/mac-guest-agent
sudo chmod +x /usr/local/bin/mac-guest-agent
sudo /usr/local/bin/mac-guest-agent --install
```

**On the PVE host (one command — drives the in-VM diagnostics for you):**

```bash
curl -fsSL -o /tmp/pve-verify.sh \
  https://raw.githubusercontent.com/mav2287/mac-guest-agent/main/scripts/pve-verify.sh
chmod +x /tmp/pve-verify.sh
/tmp/pve-verify.sh <vmid> | tee pve-verify.txt
# split the output at the "JSON Appendix" header — the JSON below it
# is what goes into pve-verify.json
```

PII (IPv4 addresses, MAC addresses, the VM ID) is redacted by default; pass `--no-redact` if you want raw values. `--help` lists the rest of the flags (`--no-appendix`, `--no-in-vm`, `--agent-path`, `--log-path`, `--exec-timeout`).

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

`selftest.json` and `safetest.json` are guest-side outputs and are generally safe — they describe the agent's environment inside the VM. `pve-verify.txt` is host-side and includes things like detected interface addresses, so give it a quick read before committing.
