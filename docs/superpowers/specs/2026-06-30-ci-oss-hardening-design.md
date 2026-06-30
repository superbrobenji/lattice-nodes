# CI OSS Hardening + Integration Test Fix — Design

**Date:** 2026-06-30  
**Scope:** Planetopia-nodes repo only  
**Approach:** Option C — full security hardening with Dependabot automation

---

## Problem

Planetopia-nodes CI is missing three OSS table-stakes controls that both sibling repos (motionSensorServer, planetopia-protocol) already have:

1. No `permissions:` blocks — jobs inherit broad GITHUB_TOKEN write access by default
2. No CodeQL — C++ firmware is unscanned for memory-safety and security vulnerabilities
3. No dependency-review — PRs can introduce vulnerable dependencies silently

Additionally:
- All actions use mutable `@v4` tags — a supply-chain attack vector
- GoogleTest fetched by URL with no SHA256 integrity check
- `tests/integration/harness.py::send_enrollment_approve` raises `NotImplementedError` — stale code referencing a removed protocol path

---

## Design

### 1. Least-privilege permissions

Add a `permissions:` block to every job in `unit-tests.yml`:

```yaml
permissions:
  contents: read
```

Jobs that upload artifacts also need:

```yaml
permissions:
  contents: read
  actions: read   # required by upload-artifact
```

This follows the principle of least privilege: GITHUB_TOKEN can only read repo contents, not write to packages, issues, or deployments.

### 2. SHA-pinned actions

Replace all mutable `@v4` / `@v3` tags with immutable SHA pins. Affected actions:

- `actions/checkout`
- `actions/upload-artifact`
- `github/codeql-action/init`, `/autobuild`, `/analyze`
- `actions/dependency-review-action`

SHAs are resolved at implementation time via:
```sh
gh api repos/{owner}/{repo}/git/refs/tags/{tag} --jq '.[-1].object.sha'
```

Each pinned line includes a trailing comment with the human-readable tag for maintainability:
```yaml
uses: actions/checkout@<SHA> # v4.x.x
```

### 3. GoogleTest integrity check

`tests/CMakeLists.txt` fetches GoogleTest by URL. Add `URL_HASH` to lock the download:

```cmake
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=<hash>
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
```

SHA256 resolved at implementation time via `curl -sL <url> | sha256sum`.

### 4. New `codeql.yml` workflow

Mirrors `planetopia-protocol/codeql.yml` with `cpp` added:

- **Trigger:** push/PR to `main` + weekly schedule (`cron: '0 2 * * 1'`)
- **Language:** `cpp` with `autobuild` build mode — CMake test build serves as proxy (same `ubuntu-latest` environment the `unit-tests` job already uses)
- **Queries:** `security-and-quality`
- **Permissions:** `security-events: write, contents: read, actions: read, packages: read`
- **`fail-fast: false`** on matrix so one language failure doesn't skip the other

### 5. New `dependency-review.yml` workflow

PR-only. Identical structure to motionSensorServer's file:

- **Trigger:** `pull_request` targeting `main`
- **Permissions:** `contents: read`
- No `allow-ghsas` entries initially (added as needed when justified false positives arise)

### 6. New `.github/dependabot.yml`

Two ecosystems:

```yaml
version: 2
updates:
  - package-ecosystem: github-actions
    directory: /
    schedule:
      interval: weekly
    labels: [dependencies, ci]

  - package-ecosystem: git-submodules
    directory: /
    schedule:
      interval: weekly
    labels: [dependencies, submodule]
```

Dependabot opens auto-PRs when action SHAs drift from latest tags, and when the `planetopia-protocol` submodule has new commits. CI gates on these PRs like any other.

### 7. Stale integration test fix

**`tests/integration/harness.py`**

Replace `send_enrollment_approve` with a working HTTP implementation:

```python
def send_enrollment_approve(self, mac: bytes, pub_key: bytes, server_url: str, admin_key: str) -> None:
    """Approve enrollment via server HTTP API (POST /api/v1/enrollments/{mac}/approve)."""
    import requests
    mac_str = ':'.join(f'{b:02X}' for b in mac)
    url = f"{server_url}/api/v1/enrollments/{mac_str}/approve"
    resp = requests.post(url, headers={"Authorization": f"Bearer {admin_key}"}, timeout=5.0)
    resp.raise_for_status()
```

Server URL and admin key passed as parameters (callers supply from env vars — no secrets in source).

**`tests/integration/requirements.txt`**

Add `requests>=2.31.0`.

**`tests/integration/test_enrollment.py`**

Update `test_server_approval_triggers_join_ack` to pass `SERVER_URL` and `ADMIN_KEY` from environment:

```python
SERVER_URL = os.getenv('SERVER_URL', 'http://localhost:8080')
ADMIN_KEY  = os.getenv('ADMIN_KEY', '')

def test_server_approval_triggers_join_ack(master, node):
    pub_key = node.get_public_key(timeout=5.0)
    assert pub_key is not None
    test_mac = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01])
    master.send_enrollment_approve(test_mac, pub_key, SERVER_URL, ADMIN_KEY)
    approved = node.wait_for_log('Enrollment approved', timeout=5.0)
    assert approved, "Node did not receive JOIN_ACK after server approval"
```

---

## Files changed

| File | Change |
|------|--------|
| `.github/workflows/unit-tests.yml` | Add `permissions:`, SHA-pin actions |
| `.github/workflows/codeql.yml` | **New** — C++ CodeQL scan |
| `.github/workflows/dependency-review.yml` | **New** — PR dependency audit |
| `.github/dependabot.yml` | **New** — Actions + submodule auto-update |
| `tests/CMakeLists.txt` | Add `URL_HASH SHA256` to GoogleTest fetch |
| `tests/integration/harness.py` | Fix `send_enrollment_approve` → HTTP POST |
| `tests/integration/requirements.txt` | Add `requests>=2.31.0` |
| `tests/integration/test_enrollment.py` | Pass `SERVER_URL`/`ADMIN_KEY` from env |

---

## Out of scope

- Arduino/ESP32 toolchain compilation in CI (documented in CONTRIBUTING.md as local-only)
- Hardware-in-the-loop tests in CI (requires physical ESP32 serial connections)
- SPDX license headers on workflow files (cosmetic, no security impact)
