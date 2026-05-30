#!/bin/bash
# macOS Guest Agent - Bootstrap Installer
#
# Thin wrapper that fetches the binary from GitHub (or uses --local PATH),
# copies it to /usr/local/bin/mac-guest-agent, and exec's the binary with
# the install action requested by the caller. All orchestration logic
# (install/upgrade state machine, VirtIO override, marker handling,
# rollback) lives in the binary itself (src/service.c) as of v2.5.3.
#
# Use cases:
#   sudo bash install.sh                       # fresh standard install
#   sudo bash install.sh --virtio              # fresh install with VirtIO override
#   sudo bash install.sh --virtio-force        # fresh install, force mode (unsupported)
#   sudo bash install.sh --upgrade             # in-place upgrade
#   sudo bash install.sh --uninstall           # remove agent
#   sudo bash install.sh --local PATH ...      # use a local binary (skip download)
#   sudo bash install.sh --dry-run ...         # preview without side effects
#
# The binary's --help lists what each flag means once installed.

set -e

REPO="mav2287/mac-guest-agent"
BINARY_NAME="mac-guest-agent"
INSTALL_PATH="/usr/local/bin/${BINARY_NAME}"
RELEASE_URL="https://github.com/${REPO}/releases/latest/download/${BINARY_NAME}"

info() { echo "[INFO] $1" >&2; }
err()  { echo "[ERR]  $1" >&2; }

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    cat <<EOF
macOS Guest Agent Installer (bootstrap wrapper, v2.5.3+)

Usage: sudo $0 [options]

This script fetches the binary (or uses --local PATH) and forwards the
install/upgrade/uninstall action to the binary itself, where all
orchestration logic lives.

Options recognized by this wrapper:
  --local [PATH]    Use a local binary instead of downloading. PATH is
                    optional; if omitted, searches ./mac-guest-agent,
                    /tmp/mac-guest-agent, build/mac-guest-agent-universal,
                    and a couple of legacy fallbacks.
  --dry-run         Preview the actions without touching the filesystem.
                    Forwarded to the binary, which gates its side effects.

Forwarded to the binary's --install (see 'mac-guest-agent --help'):
  --virtio          Gated VirtIO override install (macOS 11+, SIP off,
                    unloads AppleQEMUGuestAgent). Unsupported config —
                    see docs/NO_ISA_OVERRIDE.md.
  --virtio-force    VirtIO install bypassing all safety checks. Unsupported.

Forwarded as their own binary operations:
  --upgrade         In-place upgrade. Detects current mode, swaps binary,
                    regenerates plist, restarts, verifies. Rolls back on
                    failure.
  --uninstall       Remove agent. Marker-aware: reloads AppleQEMUGuestAgent
                    if installed via --virtio (mode=full).

  -h, --help        Show this help

Standard install (default — no flags):
  sudo bash $0

Operators can also run the binary directly after copying it to
/usr/local/bin/ themselves; see the README for the manual workflow.
EOF
    exit 0
fi

# Parse only the wrapper-specific flags. Everything else is forwarded to
# the binary verbatim.
DRY_RUN=0
USE_LOCAL=0
LOCAL_PATH=""
FORWARD_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --local)
            USE_LOCAL=1
            shift
            # Optional explicit path
            if [ $# -gt 0 ] && [ "${1:0:2}" != "--" ]; then
                LOCAL_PATH="$1"
                shift
            fi
            ;;
        --dry-run)
            DRY_RUN=1
            FORWARD_ARGS+=("--dry-run")
            shift
            ;;
        --uninstall)
            # --uninstall doesn't need a binary download/copy — just shell out
            # to the already-installed binary's --uninstall (which is
            # marker-aware in v2.5.3+).
            if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
                err "Root privileges required. Run with sudo."
                exit 1
            fi
            if [ ! -x "$INSTALL_PATH" ]; then
                err "Binary not found at $INSTALL_PATH — nothing to uninstall."
                exit 1
            fi
            shift
            exec "$INSTALL_PATH" --uninstall "$@"
            ;;
        --upgrade)
            # --upgrade needs a PATH. We support two forms:
            #   --upgrade            (then --local PATH separately)
            #   (default flow downloads new binary; upgrade calls --upgrade
            #    on the binary against the downloaded file)
            FORWARD_ARGS+=("--upgrade")
            shift
            ;;
        *)
            FORWARD_ARGS+=("$1")
            shift
            ;;
    esac
done

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    err "Root privileges required. Run with sudo."
    exit 1
fi

# Architecture validation. Catches "running on PowerPC" before download.
case "$(uname -m)" in
    x86_64|i386|i486|i586|i686|arm64|arm64e) ;;
    *)
        err "Unsupported architecture: $(uname -m)"
        err "This project ships i386, x86_64, and arm64 slices only."
        exit 1
        ;;
esac

# Resolve the source binary.
if [ "$USE_LOCAL" -eq 1 ]; then
    if [ -n "$LOCAL_PATH" ]; then
        if [ ! -f "$LOCAL_PATH" ]; then
            err "Local binary not found at: $LOCAL_PATH"
            exit 1
        fi
        BINARY="$LOCAL_PATH"
    elif [ -f "./${BINARY_NAME}" ];                       then BINARY="./${BINARY_NAME}"
    elif [ -f "/tmp/${BINARY_NAME}" ];                    then BINARY="/tmp/${BINARY_NAME}"
    elif [ -f "build/${BINARY_NAME}-universal" ];         then BINARY="build/${BINARY_NAME}-universal"
    elif [ -f "build/${BINARY_NAME}" ];                   then BINARY="build/${BINARY_NAME}"
    elif [ -f "./${BINARY_NAME}-darwin-universal" ];      then BINARY="./${BINARY_NAME}-darwin-universal"
    elif [ -f "/tmp/${BINARY_NAME}-darwin-universal" ];   then BINARY="/tmp/${BINARY_NAME}-darwin-universal"
    else
        err "No local binary found."
        err "Searched: ./${BINARY_NAME}, /tmp/${BINARY_NAME}, build/${BINARY_NAME}-universal, build/${BINARY_NAME}, ./${BINARY_NAME}-darwin-universal, /tmp/${BINARY_NAME}-darwin-universal"
        err "Pass an explicit path: sudo $0 --local /path/to/${BINARY_NAME}"
        exit 1
    fi
    info "Using local binary: $BINARY"
else
    if [ "$DRY_RUN" -eq 1 ]; then
        info "DRY RUN: would download ${BINARY_NAME} from GitHub releases."
        BINARY="<dry-run-pending-download>"
    else
        info "Downloading latest release..."
        TMPDIR=$(mktemp -d)
        trap "rm -rf $TMPDIR" EXIT
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -o "$TMPDIR/$BINARY_NAME" "$RELEASE_URL" || {
                err "Download failed. On older macOS, download from another machine and use: sudo $0 --local"
                exit 1
            }
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$TMPDIR/$BINARY_NAME" "$RELEASE_URL" || { err "Download failed"; exit 1; }
        else
            err "curl or wget required. Or download manually and use: sudo $0 --local"
            exit 1
        fi
        BINARY="$TMPDIR/$BINARY_NAME"
        info "Downloaded"
    fi
fi

# Determine if the user wants an upgrade vs a fresh install. Both paths
# now exec the BINARY at its downloaded/local location — the binary
# self-installs to /usr/local/bin/ during --install, and --upgrade uses
# the running binary as the source.
IS_UPGRADE=0
for arg in "${FORWARD_ARGS[@]}"; do
    if [ "$arg" = "--upgrade" ]; then
        IS_UPGRADE=1
        break
    fi
done

if [ "$IS_UPGRADE" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        info "DRY RUN: would exec: $BINARY ${FORWARD_ARGS[*]}"
        if [ -x "$BINARY" ]; then
            exec "$BINARY" "${FORWARD_ARGS[@]}"
        fi
        exit 0
    fi
    if [ ! -x "$BINARY" ]; then
        err "Binary not executable: $BINARY"
        exit 1
    fi
    exec "$BINARY" "${FORWARD_ARGS[@]}"
fi

# Fresh install path: exec the binary with --install (and any --virtio
# / --virtio-force / --dry-run modifiers). The binary self-copies to
# /usr/local/bin/ as part of --install.
if [ "$DRY_RUN" -eq 1 ]; then
    info "DRY RUN: would exec: $BINARY --install ${FORWARD_ARGS[*]}"
    if [ -x "$BINARY" ]; then
        exec "$BINARY" --install "${FORWARD_ARGS[@]}"
    fi
    info "DRY RUN complete."
    exit 0
fi

if [ ! -x "$BINARY" ]; then
    err "Binary not executable: $BINARY"
    exit 1
fi
exec "$BINARY" --install "${FORWARD_ARGS[@]}"
