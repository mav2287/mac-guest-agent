# Release Notes Template

Use this template when cutting a new release. Copy the relevant sections into the GitHub Release body and CHANGELOG.md.

## Template

```markdown
## mac-guest-agent vX.Y.Z

### Changes
- [summary of changes]

### Compatibility Updates

**Newly runtime-tested:**
- [version] — [evidence: self-test, safe_test, PVE integration, freeze]

**Newly installer-verified:**
- [version] — [evidence: kext, symbols, frameworks, PCI class]

**Newly PVE-integrated:**
- [version] — [evidence: qm agent ping, network-get-interfaces, freeze/thaw]

**Downgraded or caveated:**
- [version] — [reason]

### Quality Metrics
- Static analysis: [0 bugs / N bugs found and fixed]
- Memory leaks: [0 leaks]
- Test suite: [N unit + N proactive + Nk fuzz + N integration]
- Code coverage: [N% line, N% function]

### Downloads
| Binary | Arch | Min macOS |
|---|---|---|
| `mac-guest-agent-darwin-amd64` | x86_64 | 10.6 Snow Leopard |
| `mac-guest-agent-darwin-arm64` | arm64 | 11.0 Big Sur |
| `mac-guest-agent-darwin-universal` | x86_64 + arm64 | 10.6 / 11.0 |
```

## Checklist

Before cutting a release:

- [ ] All tests pass (`make test`)
- [ ] Static analysis clean (`clang --analyze`)
- [ ] Memory leak check clean (macOS `leaks` tool)
- [ ] CHANGELOG.md updated with new version section
- [ ] COMPATIBILITY.md updated with any new verification results
- [ ] Version bumped in: `src/agent.h`, `Makefile`, `scripts/build-pkg.sh`
- [ ] All binaries build (`make build-all`)
- [ ] .pkg installers build (`make pkg`)
- [ ] Git tag created: `git tag vX.Y.Z`
- [ ] Tag pushed: `git push origin vX.Y.Z`
