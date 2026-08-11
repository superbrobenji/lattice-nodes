# Close Open PRs Umbrella Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Root-cause every failing check on the 25 open PRs across `lattice-protocol`/`lattice-hub`/`lattice-nodes`, fix the one real regression (hub react-router 8.3.0), merge everything safe, and close the one stale PR (nodes #69).

**Architecture:** Three read-only investigation tasks (one per failure cluster) produce verdicts in a shared findings ledger; one fix-forward task resolves the real regression; three merge tasks consume the ledger verdicts to decide what's safe to land, in dependency order (protocol → nodes → hub); one close task retires nodes #69; one final task updates persistent memory.

**Tech Stack:** `gh` CLI (PR/run/log inspection, merging, closing), `git` (worktree for the one code-fix task), `npm`/`docker` (hub's typecheck/build/Docker verification).

## Global Constraints

- Repos live at `/Users/benji/projects/personal/{lattice-protocol,lattice-hub,lattice-nodes}`, remote `superbrobenji/<repo>`.
- Merge order across repos: **protocol → nodes → hub** (protocol is the shared dependency).
- No PR merges on a stale or red check — every merge must cite a check run that completed *after* the fix/rebase that made it green, not the original creation-time run.
- **Every `git push`, `gh pr merge`, and `gh pr close` is a shared-state action and requires explicit user confirmation immediately before it runs** — batch the list of what's about to happen and wait for a go-ahead. Do not merge/close/push silently even if a task's earlier steps were pre-approved.
- Findings ledger (shared across Task 1-3): `lattice-nodes/docs/superpowers/specs/2026-08-11-close-open-prs-findings.md`, append-only, one `##` section per task.
- PR numbers referenced below are as of 2026-08-11; if a task's first step shows a PR already closed/merged/changed, stop and report rather than guessing.

---

### Task 1: Investigate protocol CodeQL `Analyze (go)` failure

**Files:**
- Create: `lattice-nodes/docs/superpowers/specs/2026-08-11-close-open-prs-findings.md` (if it doesn't exist yet — first task to run creates it; if Task 2 or 3 ran first, append a new `##` section instead)

**Interfaces:**
- Consumes: nothing (first investigation lens)
- Produces: `## A1 — protocol Analyze (go)` section in the findings ledger with a `Verdict:` line of exactly `real-bug`, `stale-branch`, or `known-safe-flake`, plus the evidence (log excerpt, quoted) backing it. Task 5 (protocol merge) reads this verdict.

- [ ] **Step 1: Confirm current failing PRs and pull the live run ID**

```bash
cd /Users/benji/projects/personal/lattice-protocol
gh pr view 33 --json statusCheckRollup \
  --jq '.statusCheckRollup[] | select(.name=="Analyze (go)") | {conclusion, detailsUrl}'
```
Expected: `conclusion` is `FAILURE` and `detailsUrl` contains a `runs/<id>/job/<id>` path. If `conclusion` is not `FAILURE` (already fixed/rebased since this plan was written), record that in the ledger as `Verdict: already-resolved` and stop this task.

- [ ] **Step 2: Pull the failed job log**

Extract the run ID from the `detailsUrl` (`.../actions/runs/<RUN_ID>/job/...`), then:

```bash
gh run view <RUN_ID> --log-failed | tail -100
```

- [ ] **Step 3: Cross-check against the other two failing PRs (#32, #31) to see if it's the same root cause**

```bash
for n in 32 31; do
  echo "=== PR $n ==="
  gh pr view $n --json statusCheckRollup \
    --jq '.statusCheckRollup[] | select(.name=="Analyze (go)") | .detailsUrl'
done
```
Fetch and skim those job logs too (same `gh run view <id> --log-failed` pattern). Expected: all three show the identical failure signature (protocol is a small repo — these are 3 near-simultaneous dependabot GH-Actions bumps, likely hitting the exact same CodeQL issue).

- [ ] **Step 4: Classify the failure**

Read the log's actual error line (not just "FAILURE" — the specific CodeQL message, e.g. a real alert on Go source, a `codeql-action` version/Go-toolchain incompatibility, or an infra timeout). Decide:
- If it's a real CodeQL alert on repo Go code unrelated to the PR's own diff (these PRs only touch `.github/workflows/*.yml`) → `stale-branch` (the alert predates the PR and rebasing won't fix a real code alert, so also add a one-line note on whether it needs a separate follow-up issue).
- If it's a `codeql-action`/Go-version incompatibility introduced by *this specific* dependency bump → `real-bug`, note whether it self-resolves once merged (i.e. it's dependabot bumping toward a version that needs a matching Go toolchain bump too) or needs a manual pin.
- If it's a timeout/network/infra error with no code signature → `known-safe-flake`.

- [ ] **Step 5: Write the ledger section and commit**

```bash
cd /Users/benji/projects/personal/lattice-nodes
git add docs/superpowers/specs/2026-08-11-close-open-prs-findings.md
git commit -m "docs: A1 findings — protocol Analyze (go) failure"
```

---

### Task 2: Investigate nodes CodeQL `Analyze (cpp)` failure

**Files:**
- Modify: `lattice-nodes/docs/superpowers/specs/2026-08-11-close-open-prs-findings.md`

**Interfaces:**
- Consumes: nothing (independent lens, can run in parallel with Task 1 and Task 3)
- Produces: `## A2 — nodes Analyze (cpp)` section, same verdict vocabulary as Task 1. Task 6 (nodes merge) reads this verdict.

- [ ] **Step 1: Confirm current failing PRs and pull the live run ID**

```bash
cd /Users/benji/projects/personal/lattice-nodes
gh pr view 73 --json statusCheckRollup \
  --jq '.statusCheckRollup[] | select(.name=="Analyze (cpp)") | {conclusion, detailsUrl}'
```
Same already-resolved escape hatch as Task 1 Step 1.

- [ ] **Step 2: Pull the failed job log**

```bash
gh run view <RUN_ID> --log-failed | tail -100
```

- [ ] **Step 3: Cross-check PR #71 (the other failing one)**

```bash
gh pr view 71 --json statusCheckRollup \
  --jq '.statusCheckRollup[] | select(.name=="Analyze (cpp)") | .detailsUrl'
```
Fetch that log too, same pattern.

- [ ] **Step 4: Classify the failure**

Same three-way classification as Task 1 Step 4, applied to C++ CodeQL output. Note: these PRs (`codeql-action/upload-sarif`, `analyze`, `init` bumps) only touch `.github/workflows/*.yml` — any C++ finding is pre-existing, not introduced by the diff.

- [ ] **Step 5: Write the ledger section and commit**

```bash
cd /Users/benji/projects/personal/lattice-nodes
git add docs/superpowers/specs/2026-08-11-close-open-prs-findings.md
git commit -m "docs: A2 findings — nodes Analyze (cpp) failure"
```

---

### Task 3: Investigate hub codeql-action-bump stale-branch hypothesis

**Files:**
- Modify: `lattice-nodes/docs/superpowers/specs/2026-08-11-close-open-prs-findings.md`

**Interfaces:**
- Consumes: nothing (independent lens, can run in parallel with Task 1 and Task 2)
- Produces: `## A3 — hub codeql-action bumps (#118/#119/#120)` section with a per-PR verdict line (`Verdict #120:`, `Verdict #119:`, `Verdict #118:`), each one of `stale-branch` (rebase fixes it), `real-bug` (fails even against current main), or `known-safe-flake`. Task 7 (hub merge) reads these.

- [ ] **Step 1: Compare each PR's base commit to current main**

```bash
cd /Users/benji/projects/personal/lattice-hub
git fetch origin main
for n in 120 119 118; do
  base_sha=$(gh pr view $n --json baseRefOid --jq .baseRefOid)
  echo "PR $n base: $base_sha ... commits behind main: $(git rev-list --count $base_sha..origin/main)"
done
```
Expected: a non-zero "commits behind main" count supports the staleness hypothesis. A count of 0 rules it out for that PR — proceed to Step 3's real-log read for it instead of assuming staleness.

- [ ] **Step 2: For any PR with a non-trivial gap, check whether the specific failing checks (Go test, Go lint, TypeScript typecheck, Playwright) are things this PR's diff could plausibly break**

```bash
gh pr diff 120 --name-only
gh pr diff 119 --name-only
gh pr diff 118 --name-only
```
Expected: all three touch only `.github/workflows/*.yml`. A GitHub-Actions-only diff cannot break Go compilation, TypeScript types, or Playwright behavior — that's strong corroborating evidence for `stale-branch` over `real-bug`, independent of the commit-count check.

- [ ] **Step 3: Spot-check one actual failing job log per PR to rule out a shared infra issue (e.g. a broken main-branch fixture) rather than plain staleness**

```bash
gh pr view 118 --json statusCheckRollup --jq '.statusCheckRollup[] | select(.conclusion=="FAILURE") | {name, detailsUrl}'
```
Pull that job's log (`gh run view <RUN_ID> --log-failed | tail -60`) and read the actual error. If it's a genuine assertion/compile failure that also reproduces on current `main` (check via `git log --oneline -5 origin/main` and recent main CI runs: `gh run list --branch main --limit 5`), that's `real-bug`, not staleness — flag it as a separate issue, out of scope for this umbrella's merge decision (don't silently merge past a broken main).

- [ ] **Step 4: Write the ledger section and commit**

```bash
cd /Users/benji/projects/personal/lattice-nodes
git add docs/superpowers/specs/2026-08-11-close-open-prs-findings.md
git commit -m "docs: A3 findings — hub codeql-action bump staleness"
```

---

### Task 4: Fix forward hub react-router 8.3.0 regression

**Files:**
- Worktree: create via `superpowers:using-git-worktrees` off `lattice-hub` main, branch name `fix/react-router-8.3.0`
- Modify: whatever `server/dashboard/` and/or `server/artist-portal/` source the investigation in Step 1-2 identifies (unknown until reproduced — do not guess file paths before reproducing)
- Reference: `gh pr diff 96 --repo superbrobenji/lattice-hub` for the exact dependency version diff dependabot proposed (react-router `/node` and `/serve` → 8.3.0 in dashboard) and `gh pr diff 99 --repo superbrobenji/lattice-hub` for artist-portal's copy

**Interfaces:**
- Consumes: nothing directly, but this is the code-touching task — run it before Task 7 (hub merge), since Task 7 needs this branch's fix pushed to decide #93/#96/#99's fate.
- Produces: a pushed branch (or updated dependabot branches) with dashboard + artist-portal green on react-router 8.3.0. Task 7 reads whichever PR/branch ends up carrying the fix.

**This task is a live bug reproduction, not a known fix — invoke `superpowers:systematic-debugging` for the reproduce/isolate/fix loop instead of guessing the change up front.**

- [ ] **Step 1: Set up an isolated worktree**

Follow `superpowers:using-git-worktrees` to create a worktree for `lattice-hub` on a new branch `fix/react-router-8.3.0` off current `main`. If the automated worktree provisioning branches from a stale base (see the `feedback-sdd-worktree-base-bug` pattern — verify ancestry with `git merge-base --is-ancestor origin/main HEAD`), fall back to a manual `git worktree add`.

- [ ] **Step 2: Reproduce the dashboard failure locally**

```bash
cd server/dashboard
npm install react-router@8.3.0 @react-router/node@8.3.0 @react-router/serve@8.3.0
npm run typecheck
```
Expected: FAIL — capture the exact TypeScript error output (this is the "failing test" for this task).

- [ ] **Step 3: Read the react-router 8.3.0 changelog for the specific breaking change**

```bash
gh api repos/remix-run/react-router/releases/tags/react-router@8.3.0 --jq .body
```
Match the changelog's breaking-change entries against the Step 2 error output to confirm which change is responsible (don't fix blind — confirm the changelog explains the exact error seen).

- [ ] **Step 4: Apply the minimal fix in dashboard**

Fix depends on Step 2/3's actual findings — implement whatever the changelog-confirmed breaking change requires (e.g. an updated type import, a renamed config key, an updated loader/action signature). Re-run after each attempt:

```bash
npm run typecheck && npm run build
```
Expected: both PASS.

- [ ] **Step 5: Verify the full dashboard check suite locally**

```bash
npm run lint
docker build -f ../../Dockerfile -t lattice-hub-dashboard-test . # adjust path to match repo's actual Dockerfile location — check with: find /Users/benji/projects/personal/lattice-hub -name Dockerfile
```
Expected: lint and Docker build both PASS. If Playwright e2e is runnable locally (check `server/e2e/README.md` or `package.json` scripts for a local-run command), run it too; if it requires the full stub stack, note that CI will be the real gate for that check.

- [ ] **Step 6: Repeat Steps 2-5 for artist-portal**

```bash
cd ../artist-portal
npm install react-router@8.3.0 @react-router/fs-routes@8.3.0 @react-router/dev@8.3.0
npm run typecheck && npm run build && npm run lint
```
Apply whatever the equivalent fix is (likely the same root cause as dashboard, per Step 3's changelog read — confirm before assuming).

- [ ] **Step 7: Commit the fix**

```bash
git add server/dashboard server/artist-portal
git commit -m "fix(dashboard,artist-portal): resolve react-router 8.3.0 breaking change"
```

- [ ] **Step 8: STOP — present the diff and ask the user before pushing**

Do not push yet. Summarize what broke, what the fix was, and the verification results (Steps 4-6 pass/fail). Ask the user whether to push this branch and open a PR (or push directly to one of the dependabot branches #93/#96/#99 if `gh` permissions allow rewriting a dependabot-owned branch — check first with `gh pr view 96 --json headRepositoryOwner` to confirm it's push-able, since dependabot branches sometimes live in a fork).

---

### Task 5: Merge protocol PRs

**Files:** none (GitHub API operations only)

**Interfaces:**
- Consumes: Task 1's `## A1` verdict from the findings ledger.
- Produces: nothing later tasks read; this is a leaf action.

- [ ] **Step 1: Re-fetch current state of all 5 protocol PRs**

```bash
cd /Users/benji/projects/personal/lattice-protocol
gh pr list --state open --json number,title,mergeable,statusCheckRollup \
  --jq '.[] | {number, title, mergeable, failing: [.statusCheckRollup[]? | select(.conclusion=="FAILURE") | .name]}'
```

- [ ] **Step 2: Build the merge list**

From the Step 1 output plus Task 1's ledger verdict:
- #34, #29: already clean → include.
- #33, #32, #31: include only if Task 1's verdict was `stale-branch` (rebase, confirm green, then include) or `known-safe-flake` (include as-is with the documented reason). If `real-bug`, exclude and leave open with a comment linking the ledger finding.

If a rebase is needed: `gh pr comment <N> --body "@dependabot rebase"`, wait for the new run, re-check via Step 1's command before including.

- [ ] **Step 3: STOP — present the merge list to the user for confirmation**

State exactly which PR numbers will merge and which will stay open (and why). Wait for explicit go-ahead.

- [ ] **Step 4: Merge**

```bash
for n in <confirmed PR numbers>; do
  gh pr merge $n --squash --delete-branch
done
```

- [ ] **Step 5: Verify residual state**

```bash
gh pr list --state open --json number,title
```
Expected: only the excluded (real-bug) PRs remain, if any.

---

### Task 6: Merge nodes PRs (excluding #69)

**Files:** none (GitHub API operations only)

**Interfaces:**
- Consumes: Task 2's `## A2` verdict from the findings ledger.
- Produces: nothing later tasks read.

- [ ] **Step 1: Re-fetch current state of the 5 dependabot PRs (#75, #74, #73, #72, #71) — exclude #69, handled in Task 8**

```bash
cd /Users/benji/projects/personal/lattice-nodes
gh pr list --state open --json number,title,mergeable,statusCheckRollup \
  --jq '.[] | select(.number != 69) | {number, title, mergeable, failing: [.statusCheckRollup[]? | select(.conclusion=="FAILURE") | .name]}'
```

- [ ] **Step 2: Build the merge list**

Same logic as Task 5 Step 2, applied to #73/#71 using Task 2's `## A2` verdict. #75/#74/#72 are already clean → include.

- [ ] **Step 3: STOP — present the merge list to the user for confirmation**

- [ ] **Step 4: Merge**

```bash
for n in <confirmed PR numbers>; do
  gh pr merge $n --squash --delete-branch
done
```

- [ ] **Step 5: Verify residual state**

```bash
gh pr list --state open --json number,title
```
Expected: only #69 (handled separately in Task 8) and any excluded real-bug PRs remain.

---

### Task 7: Merge hub PRs

**Files:** none (GitHub API operations only)

**Interfaces:**
- Consumes: Task 3's `## A3` per-PR verdicts, and Task 4's fix (must be pushed/merged into #93/#96/#99 or their replacement PR before those three can be included here).
- Produces: nothing later tasks read.

- [ ] **Step 1: Re-fetch current state of all 14 hub PRs**

```bash
cd /Users/benji/projects/personal/lattice-hub
gh pr list --state open --json number,title,mergeable,statusCheckRollup \
  --jq '.[] | {number, title, mergeable, failing: [.statusCheckRollup[]? | select(.conclusion=="FAILURE") | .name]}'
```

- [ ] **Step 2: Build the merge list**

- Already-clean PRs (#108, #103, #102, #101, #97, #95, #90 as of 2026-08-11 — re-verify against Step 1's fresh output, don't trust this list if it's changed): include.
- #92, #104: fail only Playwright e2e. Check Task 3's investigation notes for whether this is the same systemic flake pattern seen on #118/#119/#120; if so, treat as `known-safe-flake` and include with a documented reason. If Task 3 didn't cover these (they weren't in its scope), do a quick single-log read here before deciding — don't include on an unexamined red check.
- #118, #119, #120: include per Task 3's per-PR verdict (rebase-and-recheck if `stale-branch`, exclude with comment if `real-bug`).
- #93, #96, #99: include only once Task 4's fix has landed on their branches (or their replacement PR) and a fresh CI run is green.

- [ ] **Step 3: STOP — present the merge list to the user for confirmation**

- [ ] **Step 4: Merge**

```bash
for n in <confirmed PR numbers>; do
  gh pr merge $n --squash --delete-branch
done
```

- [ ] **Step 5: Verify residual state**

```bash
gh pr list --state open --json number,title
```

---

### Task 8: Close nodes #69

**Files:** none (GitHub API operation only)

**Interfaces:**
- Consumes: confirmation that hub #123 and protocol #41 (Phase D docs legs) are merged — already verified true as of 2026-08-11 during brainstorming (`gh pr view 123`/`gh pr view 41` both show `state: MERGED`). Re-verify at execution time in case time has passed.

- [ ] **Step 1: Re-verify #69 is still open and still superseded**

```bash
cd /Users/benji/projects/personal/lattice-nodes
gh pr view 69 --json state,title
cd /Users/benji/projects/personal/lattice-hub && gh pr view 123 --json state
cd /Users/benji/projects/personal/lattice-protocol && gh pr view 41 --json state
```
Expected: #69 `OPEN`, #123 and #41 both `MERGED`.

- [ ] **Step 2: STOP — present the close reasoning and comment text to the user for confirmation**

Draft comment: "Superseded — Phase D docs shipped via hub #123 and protocol #41 (both merged 2026-08-11); this repo's own Phase D docs already merged via #102. Closing without merge."

- [ ] **Step 3: Close**

```bash
cd /Users/benji/projects/personal/lattice-nodes
gh pr close 69 --comment "Superseded — Phase D docs shipped via hub #123 and protocol #41 (both merged 2026-08-11); this repo's own Phase D docs already merged via #102. Closing without merge."
```

---

### Task 9: Update persistent memory

**Files:**
- Create: `/Users/benji/.claude/projects/-Users-benji-projects-personal/memory/project_close_open_prs_umbrella.md`
- Modify: `/Users/benji/.claude/projects/-Users-benji-projects-personal/memory/project_clean_code_refactor_umbrella.md` (flip status to CLOSED)
- Modify: `/Users/benji/.claude/projects/-Users-benji-projects-personal/memory/MEMORY.md` (add one index line, update the clean-code-refactor line's status)

**Interfaces:**
- Consumes: final results from Tasks 1-8 (which PRs merged, which stayed open and why, the fix-forward summary from Task 4).

- [ ] **Step 1: Write the new umbrella memory**

Content: what the effort was, final counts (merged/closed/left-open per repo with reasons), the react-router 8.3.0 root cause and fix summary, links to the spec/plan/findings-ledger paths. Follow the frontmatter format (`name`, `description`, `metadata.type: project`) used by `project_clean_code_refactor_umbrella.md`.

- [ ] **Step 2: Flip `clean-code-refactor-umbrella` to CLOSED**

Update its `description` and final status paragraph to note hub #123 + protocol #41 merged 2026-08-11, umbrella fully closed.

- [ ] **Step 3: Update `MEMORY.md` index**

Add: `- [Close-open-PRs umbrella](project_close_open_prs_umbrella.md) — <one-line result>`. Update the clean-code-refactor line to say `CLOSED` instead of listing PRs as open.

- [ ] **Step 4: No git commit needed** — memory files are outside all 3 repos, tracked by the memory system itself, not git.

---

## Self-Review Notes

- **Spec coverage:** Phase A → Tasks 1-3. Phase B → Task 4. Phase C → Tasks 5-7. Phase D → Task 8. Memory/tracking goal → Task 9. All spec sections covered.
- **No placeholders:** Task 4's exact fix content is intentionally left to Step 3's changelog read + Step 4's iterate-until-green loop — this is a live bug reproduction (per `systematic-debugging`), not an unresolved planning gap; every other task has concrete commands.
- **Type/name consistency:** the findings ledger's verdict vocabulary (`real-bug` / `stale-branch` / `known-safe-flake`) is used identically across Tasks 1-3 (producers) and 5-7 (consumers).
