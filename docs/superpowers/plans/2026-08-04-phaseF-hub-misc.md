# Phase F — Hub misc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close hub #63 (dashboard approve posts empty name) + hub #64 (meshsim writeLocked deadlock) as one hub PR.

**Architecture:** Dashboard adds a modal-per-row approve dialog for name/zone entry; orchestrator `AssignNode` defaults empty name to MAC string (belt-and-suspenders). meshsim's `SerialComm.WriteFrame` type-asserts an optional `deadliner` interface and sets a 2-second `SetWriteDeadline` on TCP transports only (production serial no-op).

**Tech Stack:** Go 1.23 (orchestrator + meshsim), TypeScript / React Router v8 (dashboard), Playwright (e2e).

## Global Constraints

- No wire-format changes.
- No cross-repo work. All 3 fixes land in `lattice-hub`.
- `SerialPort` interface unchanged; deadline applied via optional-interface type-assert.
- E2E spec must not weaken assertions.
- Design doc (canonical): `lattice-nodes/docs/superpowers/specs/2026-08-04-phaseF-hub-misc-design.md`.

---

### Task 1: Orchestrator `AssignNode` MAC-default + tests

**Repo:** `/Users/benji/projects/personal/lattice-hub` — branch `feat/phaseF-hub-misc`.

**Files:**
- Modify: `server/orchestrator/mesh/node_registry.go` — 1-line default in `AssignNode`.
- Modify: `server/orchestrator/mesh/node_registry_test.go` (or create if absent) — 2 new cases.

**Interfaces:**
- Consumes: `macToString(mac []byte) string` (existing).
- Produces: `AssignNode` now writes `node.Name = macToString(mac)` when caller passes `name == ""`. Signature unchanged.

- [ ] **Step 1: Write failing tests**

Append to `server/orchestrator/mesh/node_registry_test.go`:

```go
func TestAssignNode_EmptyName_DefaultsToMACString(t *testing.T) {
    nr := NewNodeRegistry()
    mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}
    nr.AssignNode(mac, 1, "", "")
    node, ok := nr.GetNode(mac)
    if !ok {
        t.Fatalf("AssignNode did not register node")
    }
    if node.Name != macToString(mac) {
        t.Errorf("Name = %q, want %q (MAC fallback)", node.Name, macToString(mac))
    }
}

func TestAssignNode_ExplicitName_Preserved(t *testing.T) {
    nr := NewNodeRegistry()
    mac := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02}
    nr.AssignNode(mac, 2, "kitchen-motion", "kitchen")
    node, ok := nr.GetNode(mac)
    if !ok {
        t.Fatalf("AssignNode did not register node")
    }
    if node.Name != "kitchen-motion" {
        t.Errorf("Name = %q, want %q (preserved)", node.Name, "kitchen-motion")
    }
}
```

Verify the fixture pattern matches existing `node_registry_test.go`; if `GetNode` accessor doesn't exist, use whatever accessor the file already uses (grep first).

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd server/orchestrator
go test -race -run "TestAssignNode_" ./mesh/... -v
```

Expected: `TestAssignNode_EmptyName_DefaultsToMACString` FAILS (Name is empty); `TestAssignNode_ExplicitName_Preserved` PASSES.

- [ ] **Step 3: Add the default**

In `server/orchestrator/mesh/node_registry.go::AssignNode`, immediately after `macStr := macToString(mac)`:

```go
if name == "" {
    name = macStr   // #63 belt-and-suspenders: never store an empty name
}
```

- [ ] **Step 4: Run tests to verify pass**

```bash
go test -race -run "TestAssignNode_" ./mesh/... -v
```

Expected: both PASS.

- [ ] **Step 5: Full suite regression**

```bash
go test -race -count=1 ./...
```

- [ ] **Step 6: Commit**

```bash
git add server/orchestrator/mesh/node_registry.go server/orchestrator/mesh/node_registry_test.go
git commit -m "fix(orchestrator): AssignNode defaults empty name to MAC string

Belt-and-suspenders per Phase F: any caller (currently dashboard,
possibly future) that skips the name field yields a readable label
rather than a blank NodeCard title.

Part of Phase F (issue #63)."
```

---

### Task 2: meshsim TCP write deadline

**Repo:** hub — same branch.

**Files:**
- Modify: `server/orchestrator/mesh/serial.go` — add `deadliner` interface + `SetWriteDeadline` type-assert in `WriteFrame`.
- Test: `server/orchestrator/mesh/serial_test.go` — optional deadline test.

**Interfaces:**
- Produces: file-local `type deadliner interface { SetWriteDeadline(time.Time) error }`.

- [ ] **Step 1: Add the type-assert in `WriteFrame`**

In `server/orchestrator/mesh/serial.go`, at file scope near other types:

```go
// deadliner is optionally implemented by transports (net.Conn does).
// serial.Port does not — deadline is a no-op on real serial hardware.
type deadliner interface {
    SetWriteDeadline(time.Time) error
}
```

Inside `SerialComm::WriteFrame`, before each `s.port.Write(...)` call (there are two — header + data at ~lines 86 and 91):

```go
if d, ok := s.port.(deadliner); ok {
    _ = d.SetWriteDeadline(time.Now().Add(2 * time.Second))
}
```

Two insertions (once before each `Write`). Do NOT change the `Write` error handling — existing `if _, err := s.port.Write(...); err != nil { return err }` is what surfaces the deadline error to the caller.

- [ ] **Step 2: Verify meshsim disconnect path**

Grep meshsim: `grep -n "writeLocked\|s.comm = nil\|comm ==" server/orchestrator/meshsim/sim.go`. Verify that a `WriteFrame` error propagated by `writeLocked` triggers cleanup (`s.comm = nil` or equivalent). If not, add a minimal `s.comm = nil` on error in `writeLocked` so a stalled TCP peer causes a clean disconnect rather than a repeated fail. Report the finding; commit the disconnect wiring if needed.

- [ ] **Step 3: Optional — write a deadline test**

If a paired listener can be set up trivially in `server/orchestrator/mesh/serial_test.go` (existing tests use `net.Pipe`, `bytes.Buffer`, or similar — grep first), add:

```go
func TestWriteFrame_TCPDeadlineExpires_ReturnsError(t *testing.T) {
    // Pair a net.Conn with a peer that never reads. WriteFrame's Write
    // should return a deadline-exceeded error within ~2.5 seconds.
    // Skip if the test infra doesn't allow this cleanly.
}
```

If the test infra doesn't easily allow this, skip the test — mark as follow-up in the task report. The main fix's correctness is verifiable by code inspection.

- [ ] **Step 4: Run full orchestrator test suite**

```bash
go test -race -count=1 ./...
```

- [ ] **Step 5: Commit**

```bash
git add server/orchestrator/mesh/serial.go
# plus serial_test.go if the optional test was added
# plus meshsim/sim.go if Step 2 required a disconnect fix
git commit -m "fix(mesh): TCP write deadline via optional-interface type-assert

meshsim's writeLocked holds s.mu while calling WriteFrame → tcpPort.Write.
If the peer stops reading, the write blocks in the kernel send-buffer;
s.mu never releases; the sim hangs and every e2e test times out.

Adds a file-local deadliner interface (Go idiom for optional behaviour).
WriteFrame type-asserts on s.port; TCP transports (tcpPort, meshsim's
connPort) implement SetWriteDeadline via the embedded net.Conn; real
serial ports don't and get a compile-time-safe no-op. Deadline is 2s
per issue recommendation.

Part of Phase F (issue #64)."
```

---

### Task 3: Dashboard modal approve UI + e2e update

**Repo:** hub — same branch.

**Files:**
- Modify: `server/dashboard/app/routes/_auth.enrollments.tsx` — swap inline `<form>` for a modal per pending enrollment; add name (required) + zone (optional) inputs; action handler reads them.
- Modify: `e2e/tests/dashboard/enrollments.spec.ts` — drive the new dialog; assert on real name.

**Interfaces:**
- Consumes: existing `orchestrator.approveEnrollment(mac, { name?, zone? })` service — no signature change (both are optional per `services/orchestrator.server.ts:34`).

- [ ] **Step 1: Add modal state + inputs to `_auth.enrollments.tsx`**

Convert the default export to a client component that tracks `openMac: string | null`. When the Approve button is clicked, set `openMac = e.mac`; render a modal overlay containing:

```tsx
<div className="fixed inset-0 bg-black/50 flex items-center justify-center z-50">
  <div className="bg-surface rounded p-6 min-w-[320px]">
    <h2 className="text-lg font-bold mb-4">Approve {openMac}</h2>
    <form method="post" onSubmit={() => setOpenMac(null)}>
      <input type="hidden" name="intent" value="approve" />
      <input type="hidden" name="mac" value={openMac} />
      <label className="block mb-3">
        <span className="text-sm">Name</span>
        <input name="name" required autoFocus className="w-full mt-1 px-2 py-1 border rounded" />
      </label>
      <label className="block mb-4">
        <span className="text-sm">Zone (optional)</span>
        <input name="zone" className="w-full mt-1 px-2 py-1 border rounded" />
      </label>
      <div className="flex justify-end gap-2">
        <button type="button" onClick={() => setOpenMac(null)} className="px-3 py-1 text-sm rounded bg-muted/20">Cancel</button>
        <button type="submit" className="px-3 py-1 text-sm rounded bg-ok/20 text-ok hover:bg-ok/30">Confirm</button>
      </div>
    </form>
  </div>
</div>
```

Adjust Tailwind class names to match the dashboard's actual palette utilities (verify with `grep -n "className.*bg-surface\|bg-muted" server/dashboard/app/` before writing).

Preserve the Reject button's existing inline `<form>` — no change (rejection has no metadata to collect).

- [ ] **Step 2: Update the action handler to read name/zone**

In the `action` function at `_auth.enrollments.tsx:22-35`, replace:

```ts
if (intent === "approve") {
  await orchestrator.approveEnrollment(mac, {});
}
```

with:

```ts
if (intent === "approve") {
  const name = String(form.get("name") ?? "").trim();
  const zone = String(form.get("zone") ?? "").trim() || undefined;
  await orchestrator.approveEnrollment(mac, { name: name || undefined, zone });
}
```

`name || undefined` lets the orchestrator's Task-1 default fire if the dialog is somehow bypassed.

- [ ] **Step 3: Add `useState` import + client-boundary if needed**

React Router v8 route modules are server-first; if a `useState` hook is added, mark the component with the appropriate client directive (check dashboard convention — grep for `useState` in `server/dashboard/app/routes/` to see the existing pattern).

- [ ] **Step 4: Update the e2e spec**

In `e2e/tests/dashboard/enrollments.spec.ts:39-47`, replace the `n.name === ''` search with dialog interaction + real-name assertion:

```ts
// After the pending enrollment appears in the table:
await page.getByRole("button", { name: "Approve" }).first().click();
await page.getByLabel(/^name$/i).fill("test-node-42");
await page.getByRole("button", { name: /confirm/i }).click();

// ...wait for the redirect / refresh...

// Assert the node registered with the submitted name:
const nodesRes = await request.get("/api/v1/nodes", { headers });
const node = (await nodesRes.json()).nodes.find(n => n.name === "test-node-42");
expect(node).toBeDefined();
```

Adjust labels + assertions to match the actual accessible-name of dialog elements and the existing enrollments-spec test-scaffolding conventions (imports, `request` fixture, headers helper).

- [ ] **Step 5: Run dashboard build + e2e**

```bash
cd server/dashboard
npm ci
npm run build
npm run typecheck

# From repo root:
cd -
make stub-seed   # or existing e2e bring-up per Makefile
npx playwright test e2e/tests/dashboard/enrollments.spec.ts
```

Expected: build/typecheck clean, e2e green.

- [ ] **Step 6: Commit**

```bash
git add server/dashboard/app/routes/_auth.enrollments.tsx \
        e2e/tests/dashboard/enrollments.spec.ts
git commit -m "fix(dashboard): approve dialog collects node name (closes #63)

Dashboard enrollment Approve now opens a modal with required name and
optional zone inputs, posting them to the existing
orchestrator.approveEnrollment endpoint. Matches the artist-portal
pattern in intent; uses a modal-per-row rather than a slide panel
because dashboard has no slide-panel primitive.

Task-1's AssignNode MAC-default is the defense-in-depth guard: any
future client that bypasses the dialog still gets a readable name.

E2E spec updated: instead of asserting the previously-broken
name === '' behaviour, it drives the dialog and asserts on the
submitted name.

Part of Phase F (issue #63)."
```

---

### Task 4: Push + PR

**Files:**
- No production changes.
- Modify: `.superpowers/sdd/phaseF-hub-misc/progress.md` (gitignored).

- [ ] **Step 1: Full go test regression**

```bash
cd server/orchestrator && go test -race -count=1 ./...
cd ../sidecar && go test -race -count=1 ./...
```

- [ ] **Step 2: E2E full pass**

```bash
cd $(git rev-parse --show-toplevel)
make stub-seed
npx playwright test
```

Full playwright suite — not just enrollments.spec.ts — to catch any collateral regressions from the modal change.

- [ ] **Step 3: Push + PR**

```bash
git push -u origin feat/phaseF-hub-misc
gh pr create --title "feat(phaseF): hub misc (closes #63, #64)" \
             --body "$(cat <<'EOF'
Implements docs/superpowers/plans/2026-08-04-phaseF-hub-misc.md
(canonical spec/plan live in lattice-nodes).

Closes #63, #64.

## Summary
- #63 dashboard: modal approve dialog collects name (required) + zone;
  orchestrator AssignNode defaults empty name to MAC string
  (belt-and-suspenders).
- #64 meshsim: SerialComm.WriteFrame applies 2s SetWriteDeadline via
  optional-interface type-assert. TCP transports get the deadline;
  real serial ports are unaffected.

## Test plan
- [x] `go test -race -count=1 ./...` clean across orchestrator + sidecar.
- [x] Dashboard build + typecheck clean.
- [x] Playwright e2e suite green.
- [ ] CI green.
EOF
)"
```

---

## Self-review

**Spec coverage:**
- §Design/1 (dashboard UI) → Task 3.
- §Design/2 (orchestrator default) → Task 1.
- §Design/3 (e2e spec update) → Task 3 Step 4.
- §Design/4 (meshsim TCP deadline) → Task 2.
- §Testing → Task 1 Step 1 (2 Go tests), Task 2 Step 3 (optional deadline test), Task 3 Steps 4-5.
- §Non-goals — respected: no wire changes, no SerialPort interface change, no cross-repo.

**Type consistency:**
- `AssignNode(mac []byte, nodeId uint8, name, zone string)` — signature unchanged (Task 1).
- `deadliner interface { SetWriteDeadline(time.Time) error }` — file-local in serial.go (Task 2).
- `orchestrator.approveEnrollment(mac, { name?, zone? })` — signature unchanged (Task 3).

**Placeholder scan:** none. Multiple "grep first / verify at impl time" hooks (Tailwind class names, useState pattern, e2e scaffolding conventions) are legitimate — each has a concrete grep command.

**Scope check:** 4 tasks, one repo, one PR — appropriate for one plan.
