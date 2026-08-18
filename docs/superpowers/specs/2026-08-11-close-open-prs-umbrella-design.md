# Close Open PRs Umbrella — Design

**Date:** 2026-08-11
**Scope:** All 3 Lattice repos — `lattice-protocol`, `lattice-hub`, `lattice-nodes`
**Filed under `lattice-nodes`** per this ecosystem's convention (cross-repo umbrella specs live here; see `2026-08-11-phaseD-docs-rewrite-design.md` and the 2026-08-03 recovery plan).

## Problem

25 PRs open across the 3 repos as of 2026-08-11:

- **lattice-protocol** (5): all dependabot GitHub Actions bumps (#34, #33, #32, #31, #29). #33/#32/#31 each fail one check: `Analyze (go)` (CodeQL).
- **lattice-nodes** (6): 5 dependabot bumps (#75, #74, #73, #72, #71 — #73/#71 fail `Analyze (cpp)` CodeQL), plus #69 `docs(phaseD): implementation plan` — a real, all-green PR.
- **lattice-hub** (14): all dependabot. Most pass; several fail real (non-CodeQL) checks:
  - #118/#119/#120 (codeql-action init/autobuild/analyze bumps) each fail a broad, seemingly-unrelated set (Go test, Go lint, TypeScript typecheck, Playwright) despite touching only `.github/workflows/`.
  - #93/#96/#99 (react-router 8.3.0 family: `/node`, `/serve` in dashboard; root package in artist-portal) each fail TypeScript typecheck, Build, Docker build, and/or Playwright — a real regression, not noise.
  - #104/#92 fail only Playwright e2e.

No unifying repo-of-record for "all open PRs" — each repo's PRs are independent dependabot streams plus, in nodes' case, one leftover docs PR from the now-otherwise-closed `clean-code-refactor-umbrella` (hub #123 + protocol #41, that umbrella's last two open legs, merged earlier today — confirmed via `gh pr view`).

## Goals

1. Root-cause every CI failure before merging past it — no blind merges on red.
2. Fix forward the real react-router 8.3.0 regression in hub, not defer it.
3. Land every PR that's safe to land; close nodes #69 (superseded, per user decision).
4. Leave a clean memory trail (umbrella + phase ledger) matching how `clean-code-refactor-umbrella` was tracked, and close that umbrella out now that its final 2 PRs merged.

## Non-goals

- No new feature work. No refactor beyond what's needed to un-break react-router 8.3.0.
- No speculative CodeQL alert triage beyond explaining the specific failing checks on these PRs.

## Phases

### Phase A — Root-cause investigation (read-only, parallel across repos)

Three independent lenses, one per repo/cluster:

- **A1 (protocol):** Read one failing `Analyze (go)` job log (e.g. PR #33). Determine: real CodeQL finding, action-version incompatibility, or transient infra flake.
- **A2 (nodes):** Same for `Analyze (cpp)` on PR #73 or #71.
- **A3 (hub, codeql-action bumps):** Read job logs for #118/#119/#120. Hypothesis: these branches are stale relative to `main` (created before some unrelated main-branch fix landed) and a rebase alone turns them green. Test by checking each PR's base-vs-head commit distance from current main; if confirmed stale, the fix is "rebase," not "investigate further."

Each lens must end in one of three verdicts, backed by an actual log/diff read (no guessing):
- **Real bug** → needs a code fix (folds into Phase B if it's the react-router cluster, otherwise handled inline).
- **Stale-branch artifact** → rebase, re-run CI, confirm green.
- **Known-safe flake** → documented reason, merge past it.

### Phase B — Fix-forward: hub react-router 8.3.0 regression

One task, one worktree (touches source, unlike A/C/D which are read-only or merge-only):

- Pull a branch that includes the react-router 8.3.0 bump (start from #93 or #96 — dashboard leg).
- Identify what 8.3.0 actually changed that breaks TypeScript typecheck / build / Docker build / Playwright.
- Fix forward in dashboard and artist-portal as needed (the fix likely applies to both, since #99 is artist-portal's copy of the same bump family).
- Verify: `npm run typecheck`, `npm run build`, Docker build, Playwright e2e all green locally/in CI before calling this done.
- End state: dependabot PRs #93/#96/#99 are either updated in place (push fix to their branches) or superseded by a manual PR carrying the same version bump + fix — whichever is cleaner given dependabot branch-push permissions.

### Phase C — Bulk merge

Merge order: protocol → nodes → hub (protocol is the shared dependency; nodes and hub both consume it, so landing it first avoids any theoretical drift, even though these are CI-only GitHub Actions bumps with no cross-repo runtime coupling).

Per PR: merge only if either (a) already green, or (b) Phase A/B brought it to green and that's been freshly re-verified — not assumed from a stale check run. Each merge logged (PR#, repo, verdict from A/B, merge method).

### Phase D — nodes #69

Close without merging. Comment explaining it's superseded by the now-merged Phase D docs legs (hub #123, protocol #41) and nodes' own already-merged Phase D docs (#102) from `clean-code-refactor-umbrella`.

## Verification

- No PR merges on a red or stale-green check. Phase A/B verdicts must be freshly re-verified (a new CI run, not the cached one from creation time) before Phase C touches that PR.
- After all merges, re-run `gh pr list --state open` per repo to confirm the expected residual set (should be empty, or only newly-arrived dependabot PRs created after this effort started).

## Memory / tracking

- New umbrella memory: `project-close-open-prs-umbrella`, phase ledger A→D, same style as `clean-code-refactor-umbrella`.
- Update `clean-code-refactor-umbrella` memory to CLOSED (hub #123 + protocol #41 both confirmed merged 2026-08-11).
