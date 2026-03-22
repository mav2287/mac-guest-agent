PROGRAM_NAME := mac-guest-agent
VERSION := 2.1.0
BUILD_DIR := build
DIST_DIR := dist

CC := clang
CFLAGS := -Wall -Wextra -Werror -O2 -std=c99 -DVERSION=\"$(VERSION)\"
LDFLAGS := -framework CoreFoundation -framework IOKit

SRCS := src/main.c src/agent.c src/channel.c src/protocol.c src/commands.c \
        src/cmd-info.c src/cmd-system.c src/cmd-power.c src/cmd-hardware.c \
        src/cmd-disk.c src/cmd-fs.c src/cmd-network.c src/cmd-file.c \
        src/cmd-exec.c src/cmd-ssh.c src/cmd-user.c \
        src/util.c src/log.c src/compat.c src/service.c \
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

# Install to system
install: build
	@echo "Installing..."
	sudo mkdir -p /usr/local/bin
	sudo cp $(BUILD_DIR)/$(PROGRAM_NAME) /usr/local/bin/$(PROGRAM_NAME)
	sudo chmod +x /usr/local/bin/$(PROGRAM_NAME)
	sudo /usr/local/bin/$(PROGRAM_NAME) --install

# Uninstall
uninstall:
	@echo "Uninstalling..."
	@sudo /usr/local/bin/$(PROGRAM_NAME) --uninstall 2>/dev/null || echo "Not installed"

# Run tests
test: build
	@echo "Running tests..."
	@echo '{"execute":"guest-ping"}' | $(BUILD_DIR)/$(PROGRAM_NAME) --test 2>/dev/null | grep -q '"return"' && echo "PASS: guest-ping" || echo "FAIL: guest-ping"
	@echo '{"execute":"guest-info"}' | $(BUILD_DIR)/$(PROGRAM_NAME) --test 2>/dev/null | grep -q 'supported_commands' && echo "PASS: guest-info" || echo "FAIL: guest-info"
	@echo '{"execute":"guest-sync","arguments":{"id":12345}}' | $(BUILD_DIR)/$(PROGRAM_NAME) --test 2>/dev/null | grep -q '12345' && echo "PASS: guest-sync" || echo "FAIL: guest-sync"
	@echo "Tests complete"

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
