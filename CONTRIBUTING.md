# Contributing

## Building

```bash
git clone https://github.com/mav2287/mac-guest-agent.git
cd mac-guest-agent
make build          # current architecture
make build-all      # x86_64 + arm64 + universal
make test           # quick tests
```

Full test suite:

```bash
./tests/run_tests.sh ./build/mac-guest-agent
```

## Code Style

- C99
- 4-space indentation
- `snake_case` for functions and variables
- No external dependencies beyond cJSON (embedded in `src/third_party/`)
- Every function that can fail returns an error indicator
- Log levels: ERROR for failures, INFO for startup/shutdown only, DEBUG for per-command details

## Adding a Command

1. Create or edit `src/cmd-<category>.c`
2. Write the handler: `static cJSON *handle_<name>(cJSON *args, const char **err_class, const char **err_desc)`
3. Register in the `cmd_<category>_init()` function: `command_register("guest-<name>", handle_<name>, 1);`
4. Add a test in `tests/run_tests.sh`
5. Update the command count check in the test suite

## Testing

- All tests run via `--test` mode (stdin/stdout, no serial port or QEMU needed)
- Tests must pass on both arm64 and x86_64
- PVE integration tests are manual (require a Proxmox VE host with a macOS VM)

## Pull Requests

- One feature or fix per PR
- Include tests for new functionality
- Run the full test suite before submitting
- CI must pass (GitHub Actions builds and tests on every push)
