# Tiger ISA-serial inbound drain — measured

The QGA channel on Tiger loses bytes on larger inbound messages. This is the
measurement of *where* the limit is and whether the agent can fix it.

Detector (`tests/uart-measure.sh`): send a single `guest-file-write` of N random
bytes; the agent's returned `count` equals N iff every inbound byte of the JSON
message arrived. Loss shows as empty reply (truncated JSON, no newline),
base64-decode error, or count<N.

## Stage A — baseline (current binary: READ_BUF_SIZE=4096, 1 Hz poll, one read/wake)

| payload N | wire bytes ~ | result (3 reps) |
|-----------|--------------|-----------------|
| 256       | 437          | OK OK OK |
| 512       | 778          | OK OK OK |
| 768       | 1119         | OK OK OK |
| 1024      | 1461         | OK OK OK |
| 1280      | 1802         | OK OK OK |
| 1536      | 2143         | LOSS LOSS OK |
| 1792      | 2485         | LOSS LOSS LOSS |
| 2048+     | 2826+        | LOSS (always) |

**Threshold: ~1280–1536 byte payload ≈ ~1.8–2.1 KB on the wire.** Loss begins far
BELOW the 4096 agent buffer, so the buffer is NOT the limiter. The ~2 KB number
is close to the macOS tty input-queue (clist) size.

### Disambiguation: true loss, not slow assembly

A failing size (N=2048, wire ~2827 B) was re-sent and the response awaited for
**12 seconds** → still nothing. If the agent were merely assembling slowly at
1 Hz, 12 s (≈12 reads × up to 4 KB) would have completed a 2.8 KB message easily.
It never completed → bytes were genuinely dropped mid-message, leaving the agent
with an incomplete JSON (no newline) it can never finish. **Confirmed: byte loss,
not latency.** Dropped bytes can't be recovered by waiting — they can only be
prevented by draining the queue before it overflows (if the overflow is
drain-rate-driven) — which Stage C tests.

## Stage C — combined agent-side fix (READ_BUF_SIZE 64K + drain-until-line/EAGAIN + 50ms poll)

Stage C binary CONFIRMED live before measuring (fstrim returns the honest error,
proving it is not the old binary — the first attempt silently ran the old binary
and was discarded).

| payload N | Stage A | Stage C |
|-----------|---------|---------|
| ≤1280     | OK      | OK |
| 1536      | LOSS LOSS OK | OK OK LOSS |
| 1792      | LOSS    | LOSS |
| 2048+     | LOSS    | LOSS |

**Threshold essentially unchanged (~1.3–1.5 KB).** The 1280→1536 shift is one
sample of noise; N=1792 is 0/3 intact in BOTH. Draining harder and polling 20x
faster did NOT meaningfully raise the limit.

## Conclusion — the loss is BELOW the agent; not agent-fixable

If the bytes were sitting in the macOS tty input queue waiting for the agent to
read them, faster/harder draining would have recovered them. It didn't. Combined
with the Stage-A finding that the loss is *real* (12 s wait never completes the
message), the evidence says the bytes are dropped **before they reach the tty
queue the agent reads from** — i.e. a receive overrun at the emulated 16550 ↔
macOS serial-driver boundary, because single-vCPU Tiger can't service the UART RX
interrupt fast enough while QEMU feeds a multi-KB burst at memory speed. The
agent never sees those bytes, so no read-loop change can recover them.

**The ~1.5 KB cap is a hard property of this guest (Tiger 10.4, 1 vCPU, emulated
16550), not an agent bug.** The fix must live at the protocol/sender layer:

1. **Keep inbound QGA messages ≤ ~1.3 KB.** Normal commands are tiny (automatic).
   The only large inbound payload is `guest-file-write`; chunk it ≤1 KB. The
   deploy tooling already does this; the agent acks each write (byte count), so a
   disciplined sender gets request/response flow control for free.
2. Modern VMs (faster driver, multi-core) have no such cap — they receive large
   messages fine.

## What of the Stage C code changes?

- **READ_BUF_SIZE 4096 → 65536: KEEP.** Doesn't help Tiger (can't receive >1.5 KB
  anyway) but lets the *modern* VMs receive large single messages — the original
  "match Linux qemu-ga's growable buffer" win, and what makes big file-write
  chunks work there.
- **drain-until-line loop: KEEP.** Harmless hygiene; empties the queue in one
  pass; speeds large-message receipt on modern VMs.
- **50 ms idle poll: REVERTED to 1000 ms.** No measured benefit on Tiger (within
  noise) and it adds ~20 idle wakeups/s on a Tiger that is specifically sensitive
  to idle CPU. Not worth it.
