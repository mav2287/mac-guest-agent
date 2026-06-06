#!/bin/bash
# Verify per-slice invariants for the tri-fat universal binary.
# Called by .github/workflows/build.yml and .github/workflows/release.yml.
#
# Usage: ./scripts/verify-legacy-slices.sh [path-to-universal-binary] [path-to-symbol-baseline-dir]
# Defaults: build/mac-guest-agent-universal, tests
# Exit code: 0 if all invariants hold; non-zero (with ::error::) on first failure.
#
# Invariants are the contract that the v2.5.x universal binary must satisfy
# to be safe for macOS 10.4 through current. The check set is grounded in
# Apple's mach-o/loader.h LC_REQ_DYLD semantics + the v2.5.0 BREAKING
# entries in CHANGELOG.md (universal asset shape, ISA-only transport).

set -euo pipefail

# --- PIPELINE CONVENTION (do not violate) -------------------------------------
# Pipelines `producer | consumer-that-exits-early` (awk with `exit`, grep -q,
# head -N, sed 'Nq', etc.) under `set -o pipefail` are SIGPIPE traps:
# the consumer exits early, the producer's next write gets EPIPE, dies with
# status 141, pipefail propagates non-zero. Observed once on CI run
# 26532052157 (macos-14, 2026-05-27) in tests/test_verify_transports.sh;
# see that file's ASSERTION-HELPER CONVENTION block for the full write-up.
#
# Rule for this file: awk extracts that need "the first matching value" use
# the `flag=1 ... flag && /match/{print; flag=0}` pattern (no `exit`) so awk
# reads to EOF and the producer finishes cleanly. Substring checks on bash
# variables use `case "$var" in *pattern*)` instead of `echo $var | grep -q`.
# ------------------------------------------------------------------------------

BIN="${1:-build/mac-guest-agent-universal}"
SYMBOL_DIR="${2:-tests}"
[ -f "$BIN" ] || { echo "::error::$BIN not found"; exit 1; }

fail() { echo "::error::$1"; exit 1; }
pass() { echo "  PASS  $1"; }
info() { echo "  INFO  $1"; }

# --- Helpers ------------------------------------------------------------

# Extract ALL load commands per slice including the unknown `cmd ?(0x...)` form.
# Returns one command per line. We use $2 (not $NF) because for the line
# "   cmd ?(0x80000028) Unknown load command", $NF would return "command"
# (the last word) — silently dropping the actual cmd value.
slice_loads_all() {
  otool -arch "$1" -l "$BIN" \
    | awk '/^[[:space:]]*cmd[[:space:]]+/{print $2}'
}

# NOTE on awk usage below: the `print $2; exit` idiom would short-circuit
# the otool producer, risking SIGPIPE under `set -o pipefail`. Same class
# as the test-harness flake fixed in tests/test_verify_transports.sh —
# see that file's ASSERTION-HELPER CONVENTION block for the full story.
# Here we drop `exit` and instead set+clear a flag so awk prints exactly
# the first matching value and then reads to EOF without acting. otool
# finishes writing cleanly; no SIGPIPE possible.
slice_min_macosx() {
  otool -arch "$1" -l "$BIN" \
    | awk '/cmd LC_VERSION_MIN_MACOSX/{flag=1; next} flag && /version/{print $2; flag=0}'
}

# Parse LC_BUILD_VERSION minos (used by arm64 instead of LC_VERSION_MIN_MACOSX).
# Format: "    minos 11.0"
slice_build_version_minos() {
  otool -arch "$1" -l "$BIN" \
    | awk '/cmd LC_BUILD_VERSION/{flag=1; next} flag && /minos/{print $2; flag=0}'
}

slice_deps() { otool -arch "$1" -L "$BIN" | tail -n +2 | awk '{print $1}'; }
slice_undefs() { nm -arch "$1" -u "$BIN" 2>/dev/null | awk '{print $NF}' | sort; }
# nm -m gives full undefined-symbol metadata including weak/strong status.
# Used by gate 3h below to assert host_statistics64 stays weak-imported.
slice_undefs_full() { nm -m -arch "$1" -u "$BIN" 2>/dev/null; }

# Numeric cmd value parser. Returns 0 (true) if a command has LC_REQ_DYLD bit set.
# Input format: "?(0xHEXVALUE)" — quoted via case glob below.
has_req_dyld_bit() {
  case "$1" in
    "?("*)
      hex=$(echo "$1" | sed 's/^?(\(0x[0-9a-fA-F]*\)).*/\1/')
      [ $((hex & 0x80000000)) -ne 0 ] && return 0 || return 1
      ;;
    *) return 1 ;;
  esac
}

# --- 1. Architecture set ------------------------------------------------
# lipo arg order: binary path BEFORE -verify_arch.
# `lipo -verify_arch ... PATH` fails: "unknown architecture specification flag"
# because lipo interprets the path as another arch token.
lipo "$BIN" -verify_arch i386 x86_64 arm64 \
  || fail "Universal binary missing one of [i386, x86_64, arm64]"
pass "Architectures: i386 + x86_64 + arm64"

# --- 2. Allowed system dylibs -------------------------------------------
SAFE_DEPS=(
  "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"
  "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit"
  "/usr/lib/libSystem.B.dylib"
)

# --- 3. Per-slice allowlists --------------------------------------------
# Closed enumeration of all currently-known load commands. Sourced from
# apple-oss-distributions/xnu EXTERNAL_HEADERS/mach-o/loader.h. Update when
# ld64 emits a new one and confirm whether it's REQ_DYLD-bit (must be added
# to NAMED_REQ_DYLD_COMMANDS too if so).
KNOWN_LCS="
LC_SEGMENT LC_SEGMENT_64 LC_SYMTAB LC_DYSYMTAB
LC_LOAD_DYLINKER LC_UUID LC_VERSION_MIN_MACOSX LC_VERSION_MIN_IPHONEOS
LC_VERSION_MIN_TVOS LC_VERSION_MIN_WATCHOS LC_BUILD_VERSION
LC_SOURCE_VERSION LC_FUNCTION_STARTS LC_DATA_IN_CODE LC_DYLIB_CODE_SIGN_DRS
LC_LINKER_OPTION LC_LINKER_OPTIMIZATION_HINT LC_NOTE LC_ENCRYPTION_INFO
LC_ENCRYPTION_INFO_64 LC_CODE_SIGNATURE LC_SEGMENT_SPLIT_INFO
LC_LOAD_DYLIB LC_ID_DYLIB LC_PREBOUND_DYLIB LC_SUB_FRAMEWORK
LC_SUB_CLIENT LC_SUB_UMBRELLA LC_SUB_LIBRARY LC_TWOLEVEL_HINTS
LC_PREBIND_CKSUM LC_THREAD LC_UNIXTHREAD LC_LOADFVMLIB LC_IDFVMLIB
LC_IDENT LC_FVMFILE LC_PREPAGE LC_ROUTINES LC_ROUTINES_64
LC_FUNCTION_VARIANTS LC_FUNCTION_VARIANT_FIXUPS LC_TARGET_TRIPLE
LC_MAIN LC_DYLD_INFO LC_DYLD_INFO_ONLY LC_DYLD_ENVIRONMENT
LC_LOAD_WEAK_DYLIB LC_REEXPORT_DYLIB LC_LOAD_UPWARD_DYLIB LC_RPATH
LC_DYLD_CHAINED_FIXUPS LC_DYLD_EXPORTS_TRIE LC_FILESET_ENTRY
LC_ATOM_INFO
"

# Per-slice REQ_DYLD-bit allowlist (named form).
# i386 floor: 10.4 dyld — NONE expected (classic relocations only)
# x86_64 floor: 10.4 dyld — LC_LOAD_WEAK_DYLIB ONLY (post-issue-#9: x86_64
#   lowered to 10.4 with `-Wl,-ld_classic` + `-platform_version,macos,10.4,10.13`
#   so Tiger's 2007-era dyld can parse it; classic LC_SYMTAB binding. The
#   sole LC_REQ_DYLD entry is `LC_LOAD_WEAK_DYLIB` from the weak-linked
#   CoreFoundation and IOKit frameworks — required because Tiger ships
#   those frameworks as i386-only, so the x86_64 slice's references must
#   be weak to allow the load.)
# arm64 floor: 11.0 dyld — modern set
ALLOWED_REQ_DYLD_i386=""
ALLOWED_REQ_DYLD_x86_64="LC_LOAD_WEAK_DYLIB"
ALLOWED_REQ_DYLD_arm64="LC_DYLD_INFO_ONLY LC_MAIN LC_BUILD_VERSION LC_DYLD_CHAINED_FIXUPS LC_DYLD_EXPORTS_TRIE LC_FUNCTION_VARIANTS LC_FUNCTION_VARIANT_FIXUPS"

# Subset of KNOWN_LCS that carry the LC_REQ_DYLD bit (per loader.h annotations).
NAMED_REQ_DYLD_COMMANDS="LC_MAIN LC_DYLD_INFO_ONLY LC_DYLD_CHAINED_FIXUPS LC_DYLD_EXPORTS_TRIE LC_BUILD_VERSION LC_FUNCTION_VARIANTS LC_FUNCTION_VARIANT_FIXUPS LC_LOAD_UPWARD_DYLIB LC_LOAD_WEAK_DYLIB LC_REEXPORT_DYLIB LC_RPATH LC_FILESET_ENTRY LC_ATOM_INFO LC_TARGET_TRIPLE"

check_slice() {
  local arch="$1"
  local expected_min="$2"
  local allow_main="$3"

  local allowed_var="ALLOWED_REQ_DYLD_${arch}"
  local allowed_list="${!allowed_var}"

  local loads min deps
  loads=$(slice_loads_all "$arch")
  min=$(slice_min_macosx "$arch")
  deps=$(slice_deps "$arch")

  # 3a. No unknown numeric-cmd LC_REQ_DYLD entries (catches `?(0x...)` form)
  local unknown_req=""
  while IFS= read -r cmd; do
    if has_req_dyld_bit "$cmd"; then
      unknown_req="$unknown_req $cmd"
    fi
  done <<< "$loads"
  [ -z "$unknown_req" ] || fail "$arch slice has unknown numeric LC_REQ_DYLD command(s):$unknown_req"

  # 3b. Every NAMED LC must be in KNOWN_LCS. Forces explicit review of any
  # new command Apple introduces.
  while IFS= read -r cmd; do
    case "$cmd" in
      LC_*)
        if ! echo " $KNOWN_LCS " | tr '\n' ' ' | grep -q " $cmd "; then
          fail "$arch slice has unrecognized LC command: $cmd (not in KNOWN_LCS; update script after reviewing whether it's REQ_DYLD)"
        fi
        ;;
      "?("*) ;; # already handled by 3a numeric check
      "") ;;    # empty line from awk
      *)
        fail "$arch slice has unparseable cmd line: '$cmd'"
        ;;
    esac
  done <<< "$loads"

  # 3c. Every named LC_REQ_DYLD command must be in the per-slice allowlist
  while IFS= read -r cmd; do
    for known in $NAMED_REQ_DYLD_COMMANDS; do
      if [ "$cmd" = "$known" ]; then
        if ! echo " $allowed_list " | grep -q " $cmd "; then
          fail "$arch slice has disallowed required-load-command: $cmd (allowlist: '$allowed_list')"
        fi
      fi
    done
  done <<< "$loads"

  # 3d. LC_MAIN policy
  if [ "$allow_main" = "no" ]; then
    echo "$loads" | grep -qFx LC_MAIN \
      && fail "$arch slice has LC_MAIN (10.6/10.7 dyld rejects; issue #4)"
    echo "$loads" | grep -qFx LC_UNIXTHREAD \
      || fail "$arch slice missing LC_UNIXTHREAD"
  else
    echo "$loads" | grep -qFx LC_MAIN \
      || fail "$arch slice missing LC_MAIN (unexpected for modern arm64)"
  fi

  # 3e. Min version — handles both LC_VERSION_MIN_MACOSX (legacy slices)
  # and LC_BUILD_VERSION minos (arm64). When expected_min is set, the slice
  # must declare exactly that floor via whichever load command it uses.
  if [ -n "$expected_min" ]; then
    local actual_min=""
    if [ -n "$min" ]; then
      actual_min="$min"
    else
      actual_min=$(slice_build_version_minos "$arch")
    fi
    [ "$actual_min" = "$expected_min" ] \
      || fail "$arch slice min=$actual_min, expected $expected_min"
  fi

  # 3f. Dependency allowlist
  for dep in $deps; do
    local found=no
    for safe in "${SAFE_DEPS[@]}"; do
      [ "$dep" = "$safe" ] && found=yes && break
    done
    [ "$found" = "yes" ] \
      || fail "$arch slice has unexpected dependency: $dep"
  done

  # 3g. Per-slice undefined-symbol baseline diff.
  # Legacy slices (i386, x86_64): missing baseline = HARD FAIL.
  # arm64: informational (modern dyld floor, different risk profile).
  local symfile="$SYMBOL_DIR/legacy_slice_symbols_${arch}.txt"
  if [ -f "$symfile" ]; then
    local current_syms diff_out
    current_syms=$(slice_undefs "$arch")
    diff_out=$(diff <(sort "$symfile") <(echo "$current_syms") || true)
    if [ -n "$diff_out" ]; then
      echo "::error::$arch slice undefined-symbol set drifted from baseline ($symfile)"
      echo "$diff_out"
      fail "$arch slice symbol drift — update baseline if intentional + add CHANGELOG entry"
    fi
  else
    case "$arch" in
      i386|x86_64)
        fail "$arch slice baseline missing at $symfile — required for legacy slices. Generate: nm -arch $arch -u $BIN | sort > $symfile"
        ;;
      *)
        info "$arch slice no baseline at $symfile (informational; not required for modern slices)"
        ;;
    esac
  fi

  # 3h. Weak-import assertion for host_statistics64.
  # The plain baseline (gate 3g) only records symbol NAMES via `nm -u`, not the
  # weak/strong attribute. cmd-hardware.c uses `__attribute__((weak_import))`
  # so dyld resolves the symbol to NULL on Tiger (which lacks it) and the
  # `vm_stat` text fallback runs. If a future edit drops `weak_import`, dyld on
  # Tiger would refuse to load the i386 slice ("Symbol not found"). The plain
  # baseline diff would NOT catch this — the symbol name is unchanged.
  # Gate 3h closes that gap by inspecting `nm -m` (full attribute info).
  # Find the nm -m line mentioning _host_statistics64 using pure bash so
  # the `echo "$nm_full" | grep -q` pattern (a producer | short-circuit-
  # consumer SIGPIPE risk under pipefail — see tests/test_verify_transports.sh
  # ASSERTION-HELPER CONVENTION) is eliminated. nm_full is ~150 lines so the
  # risk was low in practice but the pattern is the same class as the one
  # that actually flaked on CI run 26532052157.
  local nm_full statline=""
  nm_full=$(slice_undefs_full "$arch")
  while IFS= read -r line; do
    case "$line" in
      *"_host_statistics64"*) statline="$line"; break ;;
    esac
  done <<<"$nm_full"
  if [ -n "$statline" ]; then
    case "$statline" in
      *"weak external"*) ;;  # OK — weak-imported as required
      *)
        fail "$arch slice: _host_statistics64 must be 'weak external' for Tiger compatibility (cmd-hardware.c __attribute__((weak_import))). Current nm -m: $statline"
        ;;
    esac
  fi

  pass "$arch slice: LCs allowlisted, min=${expected_min:-<no-min-set>}, deps OK, symbols match baseline, weak-imports preserved"
}

# --- 4. Run per-slice checks --------------------------------------------
check_slice i386   10.4 no
check_slice x86_64 10.4 no
check_slice arm64  11.0 yes

echo ""
echo "All legacy-slice invariants verified for $BIN."
