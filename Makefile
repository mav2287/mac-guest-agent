PROGRAM_NAME := mac-guest-agent
VERSION := 2.5.0
BUILD_DIR := build
DIST_DIR := dist

CC := clang
CFLAGS := -Wall -Wextra -Werror -O2 -std=c99 -DVERSION=\"$(VERSION)\"
LDFLAGS := -framework CoreFoundation -framework IOKit

SRCS := src/main.c src/agent.c src/channel.c src/protocol.c src/commands.c \
        src/cmd-info.c src/cmd-system.c src/cmd-power.c src/cmd-hardware.c \
        src/cmd-disk.c src/cmd-fs.c src/cmd-network.c src/cmd-file.c \
        src/cmd-exec.c src/cmd-ssh.c src/cmd-user.c \
        src/util.c src/log.c src/compat.c src/service.c src/selftest.c \
        src/third_party/cJSON.c

INCLUDES := -Isrc -Isrc/third_party

.PHONY: all build clean install uninstall test help \
        build-i386 build-x86_64 build-arm64 build-universal build-all plist-header

all: build

# Generate plist header from plist file
plist-header:
	@xxd -i configs/com.macos.guest-agent.plist > src/plist_data.h.tmp
	@sed 's/configs_com_macos_guest_agent_plist/plist_data/g' src/plist_data.h.tmp > src/plist_data.h
	@rm -f src/plist_data.h.tmp

# Detect current architecture and set appropriate deployment target
ARCH := $(shell uname -m)
ifeq ($(ARCH),arm64)
    BUILD_DEPLOY := 11.0
else
    BUILD_DEPLOY := 10.6
endif

# Build for current architecture with proper deployment target
build: plist-header docs/mac-guest-agent.8
	@echo "Building $(PROGRAM_NAME) v$(VERSION) ($(ARCH), $(BUILD_DEPLOY)+)..."
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=$(BUILD_DEPLOY) $(CC) $(CFLAGS) $(INCLUDES) -arch $(ARCH) \
		-o $(BUILD_DIR)/$(PROGRAM_NAME) $(SRCS) $(LDFLAGS)
	@echo "Build complete: $(BUILD_DIR)/$(PROGRAM_NAME)"

# Generate the man page from the .in template, substituting $(VERSION)
# from this Makefile (single source of truth — audit finding 4) and the
# current month/year. Triggered when the template OR the Makefile changes.
# CI has a "no-diff" check that fails if a VERSION bump landed without
# regenerating this file.
docs/mac-guest-agent.8: docs/mac-guest-agent.8.in Makefile
	@sed -e 's/@VERSION@/$(VERSION)/g' \
	     -e 's/@DATE@/$(shell date "+%B %Y")/g' \
	     < docs/mac-guest-agent.8.in > docs/mac-guest-agent.8
	@echo "Regenerated docs/mac-guest-agent.8 (version $(VERSION), date $$(date '+%B %Y'))"

# Legacy SDK used for both i386 (10.4+) and x86_64 (10.6+) builds.
# Explicit -mmacosx-version-min flags below force ld64 to emit LC_UNIXTHREAD
# (instead of LC_MAIN, introduced 10.8) so 10.6/10.7 dyld can load the binary.
# Download SDK: curl -L -o /tmp/sdk.tar.xz https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz && tar xf /tmp/sdk.tar.xz -C /tmp
# SHA256: 1d2984acab2900c73d076fbd40750035359ee1abe1a6c61eafcd218f68923a5a
LEGACY_SDK ?= /tmp/MacOSX10.13.sdk
I386_SDK ?= $(LEGACY_SDK)

# i386 build links libclang_rt.osx.a's divdi3.S.o / udivdi3.S.o (64-bit
# integer division helpers). Modern Xcode stamps those .o files with
# `LC_VERSION_MIN_MACOSX 10.7`, so the linker prints a "newer macOS version
# than being linked" warning. They are pure integer-arithmetic assembly with
# no runtime OS dependency and are statically linked into our binary (verified:
# `nm` shows them as local text symbols `t`). vit9696 ran v2.4.2 (same helpers)
# on Tiger 10.4.11 successfully, confirming runtime compatibility. Warning is
# cosmetic; suppression would require either eliminating 64-bit division on
# i386 (intrusive) or linking against an older compiler-rt (build complexity).
build-i386: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) (i386, 10.4+)..."
	@if [ ! -d "$(I386_SDK)" ]; then echo "Error: i386 SDK not found at $(I386_SDK)"; echo "Download: curl -L https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz | tar xJ -C /tmp"; exit 1; fi
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=10.4 $(CC) -Wall -Wextra -Werror -O2 -std=c99 -DVERSION=\"$(VERSION)\" -mmacosx-version-min=10.4 \
		-Wno-deprecated-declarations $(INCLUDES) -arch i386 -isysroot $(I386_SDK) \
		-Wl,-ld_classic -Wl,-platform_version,macos,10.4,10.13 \
		-o $(BUILD_DIR)/$(PROGRAM_NAME)-i386 $(SRCS) $(LDFLAGS)
	@echo "i386 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-i386"

# x86_64 targeting 10.6+ (LC_UNIXTHREAD via legacy SDK + explicit min-version
# + ld-classic linker). Xcode 15-16's new ld-prime linker hardcodes LC_MAIN
# for x86_64 regardless of -mmacosx-version-min; Apple kept the older
# ld-classic as a fallback that DOES honor the min-flag for entry-point
# selection. `-Wl,-ld_classic` invokes it. Layered signals (env var + flag
# + explicit platform_version directive + ld-classic) for belt-and-suspenders.
build-x86_64: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) (x86_64, 10.6+, LC_UNIXTHREAD via ld-classic + legacy SDK)..."
	@if [ ! -d "$(LEGACY_SDK)" ]; then echo "Error: legacy SDK not found at $(LEGACY_SDK)"; echo "Download: curl -L https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.13.sdk.tar.xz | tar xJ -C /tmp"; exit 1; fi
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=10.6 $(CC) $(CFLAGS) -mmacosx-version-min=10.6 \
		-Wno-deprecated-declarations $(INCLUDES) -arch x86_64 -isysroot $(LEGACY_SDK) \
		-Wl,-ld_classic -Wl,-platform_version,macos,10.6,10.13 \
		-o $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(SRCS) $(LDFLAGS)
	@echo "x86_64 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64"

# arm64 targeting 11.0+
build-arm64: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) (arm64, 11.0+)..."
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=11.0 $(CC) $(CFLAGS) $(INCLUDES) -arch arm64 \
		-o $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 $(SRCS) $(LDFLAGS)
	@echo "arm64 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-arm64"

# Tri-fat universal binary (i386 + x86_64 + arm64) — the v2.5.0+ canonical artifact.
# dyld picks the right slice at load time across macOS 10.4 Tiger → 26 Tahoe.
build-universal: build-i386 build-x86_64 build-arm64
	@echo "Creating tri-fat universal binary (i386 + x86_64 + arm64)..."
	lipo -create \
		$(BUILD_DIR)/$(PROGRAM_NAME)-i386 \
		$(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 \
		$(BUILD_DIR)/$(PROGRAM_NAME)-arm64 \
		-output $(BUILD_DIR)/$(PROGRAM_NAME)-universal
	@echo "Tri-fat universal binary: $(BUILD_DIR)/$(PROGRAM_NAME)-universal"

# All architectures (i386 + x86_64 + arm64 thin slices + tri-fat universal).
# build-universal already depends on all three thin slices, so listing them
# here is redundant in terms of the make DAG — but the explicit list makes
# the intent visible to readers and keeps `make help` honest.
build-all: build-i386 build-x86_64 build-arm64 build-universal
	@echo "All builds complete"
	@ls -la $(BUILD_DIR)/$(PROGRAM_NAME)-*

# Clean
clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD_DIR) $(DIST_DIR)
	@echo "Clean complete"

# Generate dSYM debug symbols for crash analysis
dsym: build-all
	@echo "Generating dSYM files..."
	@dsymutil $(BUILD_DIR)/$(PROGRAM_NAME)-universal -o $(BUILD_DIR)/$(PROGRAM_NAME)-universal.dSYM 2>/dev/null || true
	@echo "dSYM files generated"

# Install to system
install: build
	@echo "Installing..."
	sudo mkdir -p /usr/local/bin
	sudo cp $(BUILD_DIR)/$(PROGRAM_NAME) /usr/local/bin/$(PROGRAM_NAME)
	sudo chmod +x /usr/local/bin/$(PROGRAM_NAME)
	sudo /usr/local/bin/$(PROGRAM_NAME) --install
	@echo "Installing man page..."
	sudo mkdir -p /usr/local/share/man/man8
	sudo cp docs/mac-guest-agent.8 /usr/local/share/man/man8/
	@echo "Man page installed (try: man mac-guest-agent)"

# Build .pkg installer — universal only (v2.5.0+).
# The produced pkg is unsigned by default; supported install path is
# `sudo installer -pkg build/mac-guest-agent-<ver>-universal.pkg -target /`.
# For Finder-double-click distribution, set PRODUCTSIGN_IDENTITY to a
# Developer ID Installer identity before invoking — see scripts/build-pkg.sh
# header for the full sign / notarize flow.
pkg: build-all
	@./scripts/build-pkg.sh universal

# Code signing (for users with a Developer ID)
sign: build-all
	@echo "Signing universal binary..."
	@if security find-identity -v -p basic 2>/dev/null | grep -q "Developer ID"; then \
		IDENTITY=$$(security find-identity -v -p basic | grep "Developer ID Application" | head -1 | awk -F'"' '{print $$2}'); \
		codesign --sign "$$IDENTITY" --timestamp $(BUILD_DIR)/$(PROGRAM_NAME)-universal; \
		echo "Signed with: $$IDENTITY"; \
	else \
		echo "No Developer ID found. Install one from developer.apple.com"; \
		exit 1; \
	fi

# Uninstall
uninstall:
	@echo "Uninstalling..."
	@sudo /usr/local/bin/$(PROGRAM_NAME) --uninstall 2>/dev/null || echo "Not installed"

# Run all tests
test: build test-unit test-proactive test-fuzz test-integration test-verify-transports

# Shell-shim integration tests for scripts/verify.sh (PVE, libvirt,
# UTM, qga-serial). Mocks the host CLIs and a local QGA socket; no
# real hypervisor needed.
test-verify-transports:
	@bash tests/test_verify_transports.sh

# Sabotage tests for scripts/verify-legacy-slices.sh — deliberately
# produce broken universal binaries and assert the verifier rejects
# them. Without this, the verifier's happy-path "PASS" doesn't prove
# it would catch a regression. Requires `make build-all LEGACY_SDK=...`
# to have produced the good i386 + x86_64 + arm64 thin slices first.
test-legacy-slice-gate: build-all
	@bash tests/test_legacy_slice_gate.sh

# Code coverage report (llvm-cov)
test-coverage:
	@echo "Building with coverage instrumentation..."
	@mkdir -p $(BUILD_DIR)/coverage
	@$(CC) -Isrc -Isrc/third_party -fprofile-instr-generate -fcoverage-mapping \
		-o $(BUILD_DIR)/test_unit_cov tests/test_unit.c \
		src/util.c src/protocol.c src/compat.c src/third_party/cJSON.c \
		-framework CoreFoundation
	@$(CC) $(CFLAGS) $(INCLUDES) -fprofile-instr-generate -fcoverage-mapping \
		-o $(BUILD_DIR)/mac-guest-agent-cov $(SRCS) $(LDFLAGS)
	@echo "Running unit tests with coverage..."
	@LLVM_PROFILE_FILE=$(BUILD_DIR)/coverage/unit.profraw $(BUILD_DIR)/test_unit_cov >/dev/null
	@echo "Running integration tests with coverage..."
	@LLVM_PROFILE_FILE=$(BUILD_DIR)/coverage/integration-%p-%m.profraw \
		./tests/run_tests.sh $(BUILD_DIR)/mac-guest-agent-cov >/dev/null 2>&1 || true
	@echo "Merging coverage data..."
	@xcrun llvm-profdata merge -sparse \
		$(BUILD_DIR)/coverage/*.profraw \
		-o $(BUILD_DIR)/coverage/merged.profdata
	@echo ""
	@echo "=== Coverage Report ==="
	@xcrun llvm-cov report $(BUILD_DIR)/mac-guest-agent-cov \
		-instr-profile=$(BUILD_DIR)/coverage/merged.profdata \
		-ignore-filename-regex='third_party' \
		src/util.c src/protocol.c src/compat.c src/log.c \
		src/cmd-info.c src/cmd-system.c src/cmd-power.c src/cmd-hardware.c \
		src/cmd-disk.c src/cmd-fs.c src/cmd-network.c src/cmd-file.c \
		src/cmd-exec.c src/cmd-ssh.c src/cmd-user.c \
		src/commands.c src/channel.c src/agent.c src/main.c src/service.c src/selftest.c
	@echo ""
	@echo "Detailed HTML report: make coverage-html"

# HTML coverage report
coverage-html: test-coverage
	@xcrun llvm-cov show $(BUILD_DIR)/mac-guest-agent-cov \
		-instr-profile=$(BUILD_DIR)/coverage/merged.profdata \
		-ignore-filename-regex='third_party' \
		-format=html -output-dir=$(BUILD_DIR)/coverage/html \
		src/util.c src/protocol.c src/compat.c src/log.c \
		src/cmd-info.c src/cmd-system.c src/cmd-power.c src/cmd-hardware.c \
		src/cmd-disk.c src/cmd-fs.c src/cmd-network.c src/cmd-file.c \
		src/cmd-exec.c src/cmd-ssh.c src/cmd-user.c \
		src/commands.c src/channel.c src/agent.c src/main.c src/service.c src/selftest.c
	@echo "HTML report: $(BUILD_DIR)/coverage/html/index.html"

# Unit tests (individual functions)
test-unit:
	@echo "Building unit tests..."
	@$(CC) -Isrc -Isrc/third_party -o $(BUILD_DIR)/test_unit tests/test_unit.c \
		src/util.c src/protocol.c src/compat.c src/third_party/cJSON.c \
		-framework CoreFoundation
	@echo "Running unit tests..."
	@$(BUILD_DIR)/test_unit

# Proactive tests (PTY channel, SSH temp files, hook validation, fs dispatch).
# Links src/cmd-fs.c so tests can exercise fs_dispatch_class. The test file
# stubs command_register (the only commands.c symbol cmd-fs.c references at
# link time) so we don't have to drag in all cmd-*.c files for tests that
# never call cmd_fs_init.
test-proactive:
	@echo "Building proactive tests..."
	@$(CC) -Isrc -Isrc/third_party -o $(BUILD_DIR)/test_proactive tests/test_proactive.c \
		src/channel.c src/util.c src/protocol.c src/compat.c src/log.c \
		src/cmd-fs.c src/cmd-ssh.c \
		src/third_party/cJSON.c -framework CoreFoundation -framework IOKit
	@echo "Running proactive tests..."
	@$(BUILD_DIR)/test_proactive

# Fuzz tests (random/malformed input with ASAN)
test-fuzz:
	@echo "Building fuzz tests with ASAN..."
	@$(CC) -Isrc -Isrc/third_party -fsanitize=address,undefined -O1 -g \
		-o $(BUILD_DIR)/fuzz_protocol tests/fuzz_protocol.c \
		src/util.c src/protocol.c src/compat.c src/third_party/cJSON.c \
		-framework CoreFoundation
	@echo "Running fuzz tests (210k rounds)..."
	@$(BUILD_DIR)/fuzz_protocol

# Integration tests (full agent via --test mode)
test-integration: build
	@./tests/run_tests.sh ./$(BUILD_DIR)/$(PROGRAM_NAME)

# Run in test mode
run-test: build
	@$(BUILD_DIR)/$(PROGRAM_NAME) --test --verbose

# Check binary deployment target
check: build
	@echo "Binary info:"
	@file $(BUILD_DIR)/$(PROGRAM_NAME)
	@otool -l $(BUILD_DIR)/$(PROGRAM_NAME) | grep -A 3 "LC_VERSION_MIN\|LC_BUILD_VERSION" || true
	@echo "Size: $$(du -h $(BUILD_DIR)/$(PROGRAM_NAME) | cut -f1)"

# Create release distribution
dist: build-all
	@echo "Creating distribution (universal-only, v2.5.0+)..."
	@rm -rf $(DIST_DIR) && mkdir -p $(DIST_DIR)
	@cp $(BUILD_DIR)/$(PROGRAM_NAME)-universal $(DIST_DIR)/$(PROGRAM_NAME)-darwin-universal
	@cd $(DIST_DIR) && shasum -a 256 * > checksums.sha256
	@echo "Distribution ready in $(DIST_DIR)/"

# Service management shortcuts
status:
	@sudo launchctl list com.macos.guest-agent 2>/dev/null || echo "Service not running"

logs:
	@tail -f /var/log/mac-guest-agent.log

restart:
	@sudo launchctl stop com.macos.guest-agent 2>/dev/null || true
	@sudo launchctl start com.macos.guest-agent

help:
	@echo "macOS Guest Agent Build System"
	@echo ""
	@echo "Build targets:"
	@echo "  build           Build for current architecture"
	@echo "  build-i386      Build i386 (10.4+)"
	@echo "  build-x86_64    Build x86_64 (10.6+)"
	@echo "  build-arm64     Build arm64 (11.0+)"
	@echo "  build-universal Build tri-fat (i386+x86_64+arm64) — the v2.5.0+ canonical artifact"
	@echo "  build-all       Build all architectures"
	@echo ""
	@echo "Other targets:"
	@echo "  install         Install as system service"
	@echo "  uninstall       Remove system service"
	@echo "  test            Run automated tests"
	@echo "  run-test        Run in interactive test mode"
	@echo "  check           Show binary info and deployment target"
	@echo "  dist            Create release distribution"
	@echo "  clean           Remove build artifacts"
	@echo "  status          Check service status"
	@echo "  logs            Tail service log"
	@echo "  restart         Restart service"
