# DRAFT — PR #3 merge reply

**Status:** draft for review. Not posted. Intended for
https://github.com/mav2287/mac-guest-agent/pull/3 once approved.

**Context:** vit9696's PR #3 ("docs(evidence): add 10.4.11 info") merged at
commit `a0ee5d5`. He included a substantial NOTES.md with PVE config,
OpenCore Booter quirks, e1000 i386 kernel patch, NVRAM, and a
DummyPowerManagement / screen-resolution / no-SSD note — exactly the
reproduction context the docs/evidence/README.md asks for. PR comment
also raised a real format question: NOTES.md doesn't render inline on
GitHub directory listings, README.md does.

**Tone:** peer-to-peer (per the external-comms-tone memory). No
"claim", "we believe", or "you'd know better".

---

Merged — thanks for the OpenCore section especially. The e1000 i386
patch and `DummyPowerManagement`-on note are exactly the
reproduction context we wanted the per-version directories to carry,
and we didn't have a working example until you wrote one.

On NOTES.md vs README.md — you're right. Renamed yours (and our
10.11.6 one, for consistency) in [b49bbe5](https://github.com/mav2287/mac-guest-agent/commit/b49bbe5)
and updated the per-version layout doc in `docs/evidence/README.md`
to make README.md the new convention. Existing NOTES.md files are
still accepted, but new ones default to README.md so the
reproduction context renders on the directory page without an
extra click.

Two small hygiene fixes folded into the same commit, just so you're
aware: (1) `selftest.json` had a trailing space in the filename
(`selftest.json ` — silent on the FS, awkward downstream); (2)
prepended a header to your README explaining the one FAIL was a
verifier-script bug, not an agent bug. Your content (OpenCore +
hardware) is preserved verbatim except for converting the original
reStructuredText-style `##############` heading to a Markdown H1
so GitHub renders it. Diff is in the commit if you want to verify
I didn't touch anything else.

On the FAIL itself — covered in more detail in the issue #2 reply
that's coming next, but the short version: the script was checking
`qm agent`'s exit code to decide whether the agent had rejected the
freeze-gated command. PVE's `register_command` dispatcher wraps QGA
error envelopes as `{result:{error:{...}}}` and exits 0 either way,
so the exit-code check could never distinguish honest rejection
from a silently-served reply. Your agent was correctly rejecting
`get-osinfo` during the freeze window — the script was lying about
what it saw. Fixed in v2.4.3 + the new `scripts/verify.sh` which
inspects response content instead.

If you have cycles for a v2.4.3 re-run against the same VM, the
output format is now schema 2.0 (`verify.txt` + `verify.json` —
single host-side command, drives the in-VM diagnostics for you via
`qm guest exec`, captures host_environment, runs multi-cycle
freeze, computes a mount-dispatch cross-check). Strictly optional;
your existing evidence is what made this entire cycle happen.

Tier 1 for 10.4.11 stays — your data + the verifier rewrite your
report enabled = a strong story.
