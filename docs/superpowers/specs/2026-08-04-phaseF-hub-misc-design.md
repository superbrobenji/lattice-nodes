# Phase F — Hub misc (#63 + #64)

**Status:** Approved
**Date:** 2026-08-04
**Repo:** lattice-hub (only).
**Scope:** dashboard React fix + orchestrator Go guard + meshsim TCP write-deadline. No wire-format changes. No cross-repo work.
**Parent:** `lattice-nodes/docs/superpowers/specs/2026-07-22-close-all-open-issues-design.md` (Phase F).
**Issues:** hub #63, hub #64.

## Context

Two independent hub-side items, bundled to close Phase F in a single PR.

### #63 — Dashboard approve posts empty name
`server/dashboard/app/routes/_auth.enrollments.tsx:28` posts `approveEnrollment(mac, {})`. Node registers with empty name → blank NodeCard title on `/nodes`. Artist-portal has correct pattern (`server/artist-portal/app/components/features/EnrollmentTable.tsx`). E2E spec `e2e/tests/dashboard/enrollments.spec.ts:39-47` currently asserts the broken empty-name behaviour — must be updated.

### #64 — meshsim writeLocked deadlock
`server/orchestrator/meshsim/sim.go:166-173` holds `s.mu` while calling `writeLocked → SerialComm.WriteFrame → tcpPort.Write`. If the orchestrator's TCP reader stalls, the write blocks in kernel send-buffer; `s.mu` never releases; the whole sim hangs. Test-infra only, but causes flaky CI hangs.

## Design

### 1. `#63` Part A — Dashboard slide-panel approve UI

Add a name (required) + zone (optional) input to the dashboard's Approve action.

**Approach:** stay React-Router-v8-idiomatic like the rest of the dashboard. Two viable UI shapes; picking a **modal/dialog** over a slide panel because the dashboard has no existing slide-panel primitive (artist-portal has `EnrollmentTable`'s inline expand — different component library). A modal-per-row keeps changes local and doesn't introduce a new slide-panel component.

**Files touched:**
- `server/dashboard/app/routes/_auth.enrollments.tsx` — replace the inline `<form>` with a small `ApproveDialog` state block: clicking Approve opens a dialog with `<input name="name" required />` + `<input name="zone" />`; Submit posts `{ intent, mac, name, zone }`.
- Action handler at `_auth.enrollments.tsx:22-35` reads `form.get("name")` + `form.get("zone")`, passes to `orchestrator.approveEnrollment(mac, { name, zone })`. Fallback: if `name` is empty (defense-in-depth vs. bypassed dialog), send undefined so the orchestrator default fires.

Modal styling reuses existing `Badge` / `DataTable` conventions and Tailwind utility classes; no new UI dependencies.

### 2. `#63` Part B — Orchestrator MAC-default guard

`server/orchestrator/mesh/node_registry.go::AssignNode` (~line 70-87): if `name == ""`, default to `macToString(mac)`.

```go
func (nr *NodeRegistry) AssignNode(mac []byte, nodeId uint8, name, zone string) {
    nr.mu.Lock()
    defer nr.mu.Unlock()
    macStr := macToString(mac)
    if name == "" {
        name = macStr   // #63 belt-and-suspenders: never store an empty name
    }
    // ... rest unchanged ...
}
```

Belt-and-suspenders per umbrella. Any caller that skips name still yields a readable label.

### 3. `#63` — E2E spec update

`e2e/tests/dashboard/enrollments.spec.ts:39-47` currently identifies the approved node by `n.name === ''`. Update to submit an explicit name via the new dialog and assert on it:

```ts
// Enter node name in the approve dialog
await page.getByRole("button", { name: /^Approve$/ }).first().click();
await page.getByLabel(/name/i).fill("test-node-42");
await page.getByRole("button", { name: /Confirm|Submit|OK/i }).click();
// ... later ...
expect(node.name).toBe("test-node-42");
```

Wording matches whichever labels the dialog actually uses. Must NOT weaken — asserting a real name is stricter than the old `=== ''` check.

### 4. `#64` — TCP write deadline

`server/orchestrator/mesh/serial.go::SerialComm.WriteFrame` (~line 72-95): before each `s.port.Write(...)` call, type-assert `s.port` to a `deadliner` interface and set a 2-second write deadline; on write error, disconnect (existing return-err pattern; caller in meshsim already handles by clearing `s.comm`).

```go
// serial.go — add near top of file or near WriteFrame
type deadliner interface {
    SetWriteDeadline(time.Time) error
}

// inside WriteFrame, before each Write:
if d, ok := s.port.(deadliner); ok {
    _ = d.SetWriteDeadline(time.Now().Add(2 * time.Second))
}
```

The type-assert-on-optional-interface pattern is idiomatic Go and applies the fix ONLY to TCP transports (meshsim's `tcpPort` and meshsim's own `connPort`) — real serial ports don't implement `SetWriteDeadline` and don't have the deadlock (serial `Write` is bounded by device baud rate, not peer read).

**Alternative considered:** extend the `SerialPort` interface with `SetWriteDeadline`. Rejected — would require adding no-op implementations to `realSerialPort` and any other adapter, spreading concern; the type-assert keeps the fix localised to WriteFrame.

Deadline choice: 2 seconds per issue recommendation. Sim writes are single-frame ≤250 B, so 2s is generous for functional writes; any deadline hit is genuine peer stall.

### 5. Non-goals

- No wire-format changes.
- No `serial.go` interface change (SerialPort interface unchanged).
- No SetReadDeadline (issue scope is write-side deadlock).
- No orchestrator ApproveEnrollment signature change (nil name still works via Part B default).
- Not fixing dashboard slide-panel primitive; ad-hoc modal per row is sufficient.

### 6. Error handling

- **#63 dashboard:** dialog `required` attribute on the name input prevents submit with empty name (client-side); orchestrator default (Part B) catches any bypass.
- **#63 orchestrator:** the default silently substitutes MAC for empty name; no log needed (correct-by-construction; MAC is already stored on `node.MACString`).
- **#64 write deadline:** on deadline error, `WriteFrame` returns the error unchanged; meshsim's existing `s.writeLocked` catches errors, logs, and disconnects via `s.comm = nil` (already in place — verify at implementation time).

### 7. Testing

**Hub Go orchestrator:**
- Extend `server/orchestrator/mesh/api_v1_enrollments_test.go` (or `node_registry_test.go` if closer to `AssignNode`):
  - `AssignNode_EmptyName_DefaultsToMACString` — call with `name == ""`, assert stored `node.Name == macToString(mac)`.
  - `AssignNode_ExplicitName_PreservesIt` — call with a real name, assert unchanged.
- If a fake `deadliner` type exists (or can be added trivially in a test helper), extend `serial_test.go` with `WriteFrame_TCPDeadlineExpires_ReturnsError`. Otherwise defer to integration coverage.

**Dashboard:**
- No Vitest existing on `_auth.enrollments.tsx` per current CI; if there IS component-level test coverage (grep first), add a case for the dialog. If none exists, rely on the e2e update below.

**E2E (Playwright):**
- Update `e2e/tests/dashboard/enrollments.spec.ts` per §3 above: dialog interaction + real-name assertion.

**meshsim:**
- Existing `meshsim/sim_test.go` covers frame-write happy paths. Add if trivial: a test that simulates a stalled TCP reader (peer that never reads) and asserts `WriteFrame` returns within ~2.5s (with the deadline). Only if the test infra allows a paired listener without a lot of setup — if it doesn't, skip the direct test and document the fix in a comment.

### 8. Files touched (estimate)

- `server/dashboard/app/routes/_auth.enrollments.tsx` — dialog UI + action wiring (~40 LOC delta).
- `server/orchestrator/mesh/node_registry.go` — 1-line name default.
- `server/orchestrator/mesh/serial.go` — deadliner interface + 2× type-assert (~10 LOC delta).
- `server/orchestrator/mesh/node_registry_test.go` — 2 new cases.
- `server/orchestrator/mesh/serial_test.go` — optional deadline test.
- `e2e/tests/dashboard/enrollments.spec.ts` — dialog interaction (~15 LOC delta).

Rough size: ~80 LOC production + ~50 LOC tests + ~15 LOC e2e.
