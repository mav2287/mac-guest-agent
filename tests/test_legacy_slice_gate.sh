#!/bin/bash
# Sabotage tests for scripts/verify-legacy-slices.sh
# ===================================================
#
# Why this exists: scripts/verify-legacy-slices.sh is the CI gate that keeps
# issue #4 from recurring. But a verifier that says "PASS" on every happy-path
# build doesn't actually prove it would reject a regression — a future
# refactor of the verifier's awk/grep could silently make every check a no-op
# and the next bad build would ship.
#
# These tests deliberately produce broken universal binaries — one per gate
# the verifier promises to enforce — and assert that the verifier exits
# non-zero with the expected error pattern. Each sabotage matches a real
# failure mode that has actually shipped or could ship:
#
#   A. LC_MAIN on x86_64                — exact v2.4.3 / issue #4 regression
#   B. wrong min version on x86_64      — wrong deployment-target floor
#   C. universal missing i386 slice     — incomplete lipo
#   D. symbol baseline drift            — undef-symbol set changed undetected
#
# Prerequisites:
#   - LEGACY_SDK env var pointing at MacOSX10.13.sdk (or default
#     /tmp/MacOSX10.13.sdk).
#   - `make build-all LEGACY_SDK=...` has already produced
#     build/mac-guest-agent-{i386,x86_64,arm64}. The script does NOT rebuild
#     the good slices; it only rebuilds the bad x86_64 needed for each test.
#
# Exit code: 0 if all sabotages were correctly rejected; non-zero on any
# unexpected acceptance or wrong-message rejection.

set -e

cd "$(dirname "$0")/.."

PROGRAM="mac-guest-agent"
BUILD_DIR="build"
TESTS_DIR="tests"
SDK="${LEGACY_SDK:-/tmp/MacOSX10.13.sdk}"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

PASS=0
FAIL=0

# Sources list — kept in lockstep with Makefile build-x86_64.
SRCS=(src/main.c src/agent.c src/channel.c src/protocol.c src/commands.c
      src/cmd-info.c src/cmd-system.c src/cmd-power.c src/cmd-hardware.c
      src/cmd-disk.c src/cmd-fs.c src/cmd-network.c src/cmd-file.c
      src/cmd-exec.c src/cmd-ssh.c src/cmd-user.c src/util.c src/log.c
      src/compat.c src/service.c src/selftest.c
      src/third_party/cJSON.c)
LDFLAGS=(-framework CoreFoundation -framework IOKit)
COMMON_CFLAGS=(-Wall -O2 -std=c99 '-DVERSION="sabotage"'
               -Isrc -Isrc/third_party -Wno-deprecated-declarations)

# --- preflight ---------------------------------------------------------------

if [ ! -d "$SDK" ]; then
    echo "::error::test_legacy_slice_gate: LEGACY_SDK not present at $SDK"
    exit 2
fi
if [ ! -f "$BUILD_DIR/$PROGRAM-i386" ] || \
   [ ! -f "$BUILD_DIR/$PROGRAM-x86_64" ] || \
   [ ! -f "$BUILD_DIR/$PROGRAM-arm64" ]; then
    echo "::error::test_legacy_slice_gate: need build/$PROGRAM-{i386,x86_64,arm64} from 'make build-all LEGACY_SDK=$SDK'"
    exit 2
fi
if [ ! -x "scripts/verify-legacy-slices.sh" ]; then
    echo "::error::test_legacy_slice_gate: scripts/verify-legacy-slices.sh not found or not executable"
    exit 2
fi

# --- helpers -----------------------------------------------------------------

# assert_rejects <name> <error-pattern> <universal-binary> [symbol-baseline-dir]
#   Runs the verifier; succeeds iff the verifier exits non-zero AND its
#   combined output matches the expected error pattern.
assert_rejects() {
    local name="$1"
    local needle="$2"
    local bin="$3"
    local symdir="${4:-$TESTS_DIR}"

    local out exit_code=0
    out=$(./scripts/verify-legacy-slices.sh "$bin" "$symdir" 2>&1) || exit_code=$?

    if [ "$exit_code" -eq 0 ]; then
        echo "  FAIL: $name — verifier ACCEPTED a sabotaged binary"
        echo "        (expected exit !=0 with message matching: $needle)"
        FAIL=$((FAIL + 1))
        return
    fi
    if echo "$out" | grep -qE "$needle"; then
        echo "  PASS: $name — verifier rejected ($needle)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name — verifier rejected but with wrong message"
        echo "        expected pattern: $needle"
        echo "        actual tail:"
        echo "$out" | tail -3 | sed 's/^/          /'
        FAIL=$((FAIL + 1))
    fi
}

# --- sabotage A: LC_MAIN on x86_64 (issue #4 reproduction) -------------------

echo "=== Sabotage A: LC_MAIN on x86_64 slice (issue #4 regression mode) ==="
# Build x86_64 WITHOUT -Wl,-ld_classic and WITHOUT explicit min<10.7. The new
# ld-prime emits LC_MAIN regardless of -mmacosx-version-min on x86_64; setting
# min=10.8 also fits — 10.8 is the version where LC_MAIN was introduced.
MACOSX_DEPLOYMENT_TARGET=10.8 clang "${COMMON_CFLAGS[@]}" \
    -mmacosx-version-min=10.8 \
    -arch x86_64 -isysroot "$SDK" \
    -o "$TMPDIR/x86_64-LC_MAIN" "${SRCS[@]}" "${LDFLAGS[@]}" 2>/dev/null
lipo -create \
    "$BUILD_DIR/$PROGRAM-i386" \
    "$TMPDIR/x86_64-LC_MAIN" \
    "$BUILD_DIR/$PROGRAM-arm64" \
    -output "$TMPDIR/sab-a.universal"
assert_rejects "LC_MAIN on x86_64 slice" \
    "x86_64.*(LC_MAIN|LC_DYLD_INFO_ONLY)" \
    "$TMPDIR/sab-a.universal"
# Note: caught by gate 3c (disallowed required-load-command) before reaching
# gate 3d (the issue-#4-specific message). With the post-issue-#9 allowlist
# the x86_64 floor is 10.4 so LC_DYLD_INFO_ONLY is also disallowed and may
# fire first. Either rejection is valid — both indicate a load command
# the post-issue-#9 x86_64 slice must not carry. Pattern accepts either.

# --- sabotage B: wrong min version on x86_64 --------------------------------

echo "=== Sabotage B: x86_64 with wrong min version (10.6 vs required 10.4) ==="
MACOSX_DEPLOYMENT_TARGET=10.6 clang "${COMMON_CFLAGS[@]}" \
    -mmacosx-version-min=10.6 \
    -arch x86_64 -isysroot "$SDK" \
    -fno-stack-protector -D_FORTIFY_SOURCE=0 \
    -Wl,-ld_classic -Wl,-platform_version,macos,10.6,10.13 \
    -Wl,-weak_framework,CoreFoundation -Wl,-weak_framework,IOKit \
    -o "$TMPDIR/x86_64-min10.6" "${SRCS[@]}" 2>/dev/null
lipo -create \
    "$BUILD_DIR/$PROGRAM-i386" \
    "$TMPDIR/x86_64-min10.6" \
    "$BUILD_DIR/$PROGRAM-arm64" \
    -output "$TMPDIR/sab-b.universal"
assert_rejects "x86_64 min version drift" \
    "x86_64.*(min=10\.6.*expected 10\.4|LC_DYLD_INFO_ONLY)" \
    "$TMPDIR/sab-b.universal"
# Note: a min=10.6 build also emits LC_DYLD_INFO_ONLY (since 10.6 introduced
# the dyld-info shape), so the disallowed-load-command rejection may fire
# before the min-version rejection. Either is a valid catch.

# --- sabotage C: universal missing i386 slice -------------------------------

echo "=== Sabotage C: universal missing i386 slice (incomplete lipo) ==="
lipo -create \
    "$BUILD_DIR/$PROGRAM-x86_64" \
    "$BUILD_DIR/$PROGRAM-arm64" \
    -output "$TMPDIR/sab-c.universal"
# The verifier's top-level architecture check catches this before per-slice
# inspection: "Universal binary missing one of [i386, x86_64, arm64]".
# Either that message or a per-slice "i386 slice missing LC_UNIXTHREAD"
# from gate 3d would be acceptable rejection — the pattern accepts both.
assert_rejects "missing i386 slice" \
    "(missing one of \[i386|i386 slice missing LC_UNIXTHREAD)" \
    "$TMPDIR/sab-c.universal"

# --- sabotage D: symbol baseline drift --------------------------------------

echo "=== Sabotage D: symbol baseline drift (undef-symbol set changed) ==="
# Tamper the x86_64 baseline by injecting a symbol that isn't in the build.
# The diff against the actual build's undef set will show the injected symbol
# as "only in baseline" — gate 3g rejects.
mkdir -p "$TMPDIR/sab-baselines"
cp "$TESTS_DIR/legacy_slice_symbols_i386.txt"   "$TMPDIR/sab-baselines/"
{
    echo "_fake_sabotage_symbol_only_in_baseline"
    cat "$TESTS_DIR/legacy_slice_symbols_x86_64.txt"
} | sort > "$TMPDIR/sab-baselines/legacy_slice_symbols_x86_64.txt"
assert_rejects "symbol baseline drift (x86_64)" \
    "x86_64 slice (undefined-symbol set drifted|symbol drift)" \
    "$BUILD_DIR/$PROGRAM-universal" \
    "$TMPDIR/sab-baselines"

# --- sabotage E: arm64 symbol baseline drift --------------------------------

echo "=== Sabotage E: arm64 symbol baseline drift (Big Sur API floor guard) ==="
# Audit wave 5 MED-1: arm64 had no baseline before, so a future direct
# import of a macOS 12+ symbol would slip past CI even though minos=11.0
# was still declared. Now that tests/legacy_slice_symbols_arm64.txt exists
# and is required-if-present, drift in arm64 imports must be caught the
# same way it is for legacy slices.
mkdir -p "$TMPDIR/sab-baselines-arm64"
cp "$TESTS_DIR/legacy_slice_symbols_i386.txt"   "$TMPDIR/sab-baselines-arm64/"
cp "$TESTS_DIR/legacy_slice_symbols_x86_64.txt" "$TMPDIR/sab-baselines-arm64/"
{
    echo "_fake_arm64_macos12_symbol"
    cat "$TESTS_DIR/legacy_slice_symbols_arm64.txt"
} | sort > "$TMPDIR/sab-baselines-arm64/legacy_slice_symbols_arm64.txt"
assert_rejects "symbol baseline drift (arm64)" \
    "arm64 slice (undefined-symbol set drifted|symbol drift)" \
    "$BUILD_DIR/$PROGRAM-universal" \
    "$TMPDIR/sab-baselines-arm64"

# --- summary ----------------------------------------------------------------

echo ""
echo "=============================================="
echo "Sabotage tests: $PASS passed, $FAIL failed"
echo "=============================================="

[ "$FAIL" -eq 0 ]
