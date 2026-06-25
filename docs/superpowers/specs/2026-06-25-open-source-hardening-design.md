# Open Source Hardening & Documentation Refresh — Design Spec

**Date:** 2026-06-25  
**Status:** Approved

## Goal

Prepare the Planetopia-nodes repo for public open-source release.  
Two sub-goals: (1) make all docs accurate, (2) add everything expected of a healthy OSS repo.

## Decisions Made

| Decision | Choice | Reason |
|----------|--------|--------|
| License | GPL v3 | Copyleft — derivatives must stay GPL |
| CI clang-format rollout | Auto-fix existing source, then add CI gate | Keeps CI green from day one |
| REFACTORING_GUIDE.md | Replace with accurate architecture guide | Current doc describes non-existent classes |
| Enrollment protocol docs | Document fully in server_requirements.md | Security model relies on server approving nodes, not obscuring the protocol |

## Scope

Three tracks, executed in order. Each task = one commit, independently reviewable.

### Track A — Legal & Security (blocks public release)

1. **LICENSE** — GPL v3
2. **Sanitize project_config.h** — replace real device MACs (`EC:64:C9:5D:…`) with fictitious examples; add warning comment to `DEFAULT_MESH_KEY`
3. **SECURITY.md** — responsible disclosure policy

### Track B — Community Health

4. **CODE_OF_CONDUCT.md** — Contributor Covenant 2.1
5. **GitHub templates** — issue templates (bug, feature) + PR template
6. **.clang-format** — LLVM base, 2-space indent, 100-col limit
7. **CI hardening** — (a) run `clang-format --in-place` on all `main/src/` source, commit; (b) add `lint-format` and `static-analysis` jobs to `unit-tests.yml`, rename workflow to `CI`

### Track C — Documentation

8. **README rewrite** — fix duplicate "Project Structure" section; correct file paths (no `src/utils/`); add enrollment/nanopb/replay-protection/WDT; fix `<repository-url>` placeholder; add license badge
9. **REFACTORING_GUIDE → architecture guide** — remove references to `MessageRouter`, `PeerManager`, `ProtobufCodec`, `ConfigurationManager`, `NetworkManager` (none exist); document actual module map
10. **docs/server_requirements.md** — append full enrollment protocol section: message type 2/4, public-key format, JOIN_ACK construction, server responsibilities, flow diagram
11. **docs/adapter_development_guide.md** — fix `src/utils/Logger.h` → `src/core/Logger.h`; remove raw `EEPROM.write()` patterns
12. **CONTRIBUTING.md** — align CI claims with actual workflow (note Arduino compile is local-only)

## Out of Scope

- No source code changes (firmware logic, tests)
- No Arduino compile job in CI (toolchain is too heavy; document as local-only)
- No GitHub branch protection rules (cannot configure via code)
- No dependabot configuration (no package manager; not needed for vendored deps)

## Success Criteria

- `LICENSE` present and GPL v3
- `grep -r "0xEC,0x64,0xC9" .` returns no matches
- CI passes: unit-tests + lint-format + static-analysis all green
- README has no duplicate sections, no `src/utils/` references, no placeholder URLs
- REFACTORING_GUIDE references no non-existent classes
- `docs/server_requirements.md` covers enrollment end-to-end
- `docs/adapter_development_guide.md` has correct Logger path
