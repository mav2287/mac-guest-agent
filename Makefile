PROGRAM_NAME := mac-guest-agent
VERSION := 2.2.0
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
build: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) ($(ARCH), $(BUILD_DEPLOY)+)..."
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=$(BUILD_DEPLOY) $(CC) $(CFLAGS) $(INCLUDES) -arch $(ARCH) \
		-o $(BUILD_DIR)/$(PROGRAM_NAME) $(SRCS) $(LDFLAGS)
	@echo "Build complete: $(BUILD_DIR)/$(PROGRAM_NAME)"

# i386 targeting 10.4+ (requires older SDK)
build-i386: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) (i386, 10.4+)..."
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=10.4 $(CC) $(CFLAGS) $(INCLUDES) -arch i386 \
		-o $(BUILD_DIR)/$(PROGRAM_NAME)-i386 $(SRCS) $(LDFLAGS)
	@echo "i386 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-i386"

# x86_64 targeting 10.6+
build-x86_64: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) (x86_64, 10.6+)..."
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=10.6 $(CC) $(CFLAGS) $(INCLUDES) -arch x86_64 \
		-o $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(SRCS) $(LDFLAGS)
	@echo "x86_64 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64"

# arm64 targeting 11.0+
build-arm64: plist-header
	@echo "Building $(PROGRAM_NAME) v$(VERSION) (arm64, 11.0+)..."
	@mkdir -p $(BUILD_DIR)
	MACOSX_DEPLOYMENT_TARGET=11.0 $(CC) $(CFLAGS) $(INCLUDES) -arch arm64 \
		-o $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 $(SRCS) $(LDFLAGS)
	@echo "arm64 build complete: $(BUILD_DIR)/$(PROGRAM_NAME)-arm64"

# Universal binary (x86_64 + arm64)
build-universal: build-x86_64 build-arm64
	@echo "Creating universal binary..."
	lipo -create $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 \
		-output $(BUILD_DIR)/$(PROGRAM_NAME)-universal
	@echo "Universal binary: $(BUILD_DIR)/$(PROGRAM_NAME)-universal"

# All architectures
build-all: build-x86_64 build-arm64 build-universal
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
	@dsymutil $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 -o $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64.dSYM 2>/dev/null || true
	@dsymutil $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 -o $(BUILD_DIR)/$(PROGRAM_NAME)-arm64.dSYM 2>/dev/null || true
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

# Build .pkg installer (double-click or sudo installer -pkg)
pkg: build-all
	@./scripts/build-pkg.sh amd64
	@./scripts/build-pkg.sh arm64
	@./scripts/build-pkg.sh universal

# Code signing (for users with a Developer ID)
sign: build-all
	@echo "Signing binaries..."
	@if security find-identity -v -p basic 2>/dev/null | grep -q "Developer ID"; then \
		IDENTITY=$$(security find-identity -v -p basic | grep "Developer ID Application" | head -1 | awk -F'"' '{print $$2}'); \
		codesign --sign "$$IDENTITY" --timestamp $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64; \
		codesign --sign "$$IDENTITY" --timestamp $(BUILD_DIR)/$(PROGRAM_NAME)-arm64; \
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
test: build test-unit test-proactive test-fuzz test-integration

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
		src/commands.c src/channel.c src/agent.c src/main.c src/service.c
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
		src/commands.c src/channel.c src/agent.c src/main.c src/service.c
	@echo "HTML report: $(BUILD_DIR)/coverage/html/index.html"

# Unit tests (individual functions)
test-unit:
	@echo "Building unit tests..."
	@$(CC) -Isrc -Isrc/third_party -o $(BUILD_DIR)/test_unit tests/test_unit.c \
		src/util.c src/protocol.c src/compat.c src/third_party/cJSON.c \
		-framework CoreFoundation
	@echo "Running unit tests..."
	@$(BUILD_DIR)/test_unit

# Proactive tests (PTY channel, SSH temp files, hook validation)
test-proactive:
	@echo "Building proactive tests..."
	@$(CC) -Isrc -Isrc/third_party -o $(BUILD_DIR)/test_proactive tests/test_proactive.c \
		src/channel.c src/util.c src/protocol.c src/compat.c src/log.c \
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
	@echo "Creating distribution..."
	@mkdir -p $(DIST_DIR)
	@cp $(BUILD_DIR)/$(PROGRAM_NAME)-x86_64 $(DIST_DIR)/$(PROGRAM_NAME)-darwin-amd64
	@cp $(BUILD_DIR)/$(PROGRAM_NAME)-arm64 $(DIST_DIR)/$(PROGRAM_NAME)-darwin-arm64
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
	@echo "  build-universal Build x86_64+arm64 fat binary"
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
