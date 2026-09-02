# Bringing Up a Real Master Node on macOS (Dev Workflow)

This is a from-scratch walkthrough of flashing and bringing up a **real ESP32 master node**
against a **real `lattice-hub` instance**, on **macOS**, for local development and testing. It's
narrower than [`docs/getting_started.md`](getting_started.md) (which covers the generic
build/flash path and assumes you already know what a master is) and narrower than
`lattice-hub`'s own
[`docs/macos_native_dev.md`](https://github.com/superbrobenji/lattice-hub/blob/main/docs/macos_native_dev.md)
(which covers the hub side of this same workflow) — read this one alongside that one.

Everything here was worked out and verified end-to-end against real hardware; every gotcha
below is something that actually happened, not a hypothetical.

**Requires firmware built from commit `15bf003` or later** (merged via
[lattice-nodes#112](https://github.com/superbrobenji/lattice-nodes/pull/112)). Two bugs fixed
there — a blank-EEPROM adapter-type sentinel mismatch and a config-button deadlock — otherwise
make first-boot bring-up impossible in production mode. If you hit a fatal boot loop at display
code `0513`, or the config button does nothing when held, you're on an older build.

## Why macOS needs a different path

`lattice-hub`'s normal Docker Compose stack expects to bind-mount a Linux serial device
(`/dev/ttyUSB0`) straight into the `orchestrator` container. Docker Desktop on macOS has no way to
pass a host USB-serial device into a container at all — there's no `/dev/cu.usbserial-*` inside
the Linux VM it runs containers in. The practical workaround (detailed on the hub side) is running
the orchestrator process **natively** on the Mac, talking to the real board directly, while
everything else (Kafka, dashboard, etc.) stays in Docker.

## Prerequisites

- ESP-IDF v5.5.1 installed (see [`docs/getting_started.md`](getting_started.md) §3).
- Board plugged in over USB. Find its device path:
  ```bash
  ls /dev/cu.usbserial-*
  ```
- `lattice-hub` cloned as a sibling directory (`../lattice-hub` relative to this repo), with its
  own prerequisites done per its `docs/macos_native_dev.md`.

## Step 1: Read the board's real MAC address

The hub needs this MAC *before* it can generate a working JOIN_ACK, and the pin file generated in
Step 3 needs it too. Get it directly from the chip — no firmware needs to be running yet:

```bash
source ~/esp/esp-idf/export.sh
esptool.py --port /dev/cu.usbserial-0001 read_mac
```

Note the `MAC: xx:xx:xx:xx:xx:xx` line.

## Step 2: Get the master board's own public key (`LATTICE_PUBKEY`)

The pin every leaf compiles in is the **master board's own on-device key** — the one it prints
over serial at boot as `LATTICE_PUBKEY:<64 hex chars>` — together with its MAC from Step 1. It is
**not** the hub's `data/masterkey.json`: that file is the hub process's *own* identity, no board
holds its private half, and a pin derived from it can never match the key a real master sends in
its JOIN_ACK (the leaf drops every ACK with `JOIN_ACK master pubkey mismatch pin`; see
[lattice-nodes#126](https://github.com/superbrobenji/lattice-nodes/issues/126)). Leave
`masterkey.json` where the hub put it — you never need to copy it anywhere.

The board only prints its key once it's running firmware, so the very first build of the master
uses a placeholder pin (the master never checks the pubkey pin against itself, so this is safe):

```bash
cp firmware/main/config/master_pubkey_pin.h.example firmware/main/config/master_pubkey_pin.h
```

then build, flash and boot the master once (Steps 4–6 below) and copy its `LATTICE_PUBKEY:` line
from `idf.py monitor`. A master prints this line on **every** boot (it never "enrolls", so the
line never goes away) — reset the board with the monitor open if you missed it. Keeping the
monitor output in a file is handy: `idf.py -p /dev/cu.usbserial-0001 monitor | tee master-boot.log`.

## Step 3: Generate the pin file

With the key from Step 2 and the MAC from Step 1:

```bash
python3 tools/gen_master_pubkey_pin.py LATTICE_PUBKEY:<64 hex chars> <MAC from Step 1>
# or let it pull the line out of a monitor capture:
python3 tools/gen_master_pubkey_pin.py master-boot.log <MAC from Step 1>
```

This (re)writes `firmware/main/config/master_pubkey_pin.h` (gitignored, per-deployment). Rebuild,
and flash **every leaf** with this build. Re-flash the master too so all boards share one build —
a plain `idf.py flash` keeps the master's keypair (it lives in NVS), so its `LATTICE_PUBKEY` does
not change.

> **Gotcha — the master's key is not permanent.** A factory reset (reset-button double hold, see
> `getting_started.md` §11) or `idf.py erase-flash` on the master regenerates its keypair. Every
> leaf's compiled-in pin is then stale and enrollment silently fails until you repeat Steps 2–3
> and re-flash all leaves. Prefer `idf.py flash` over `erase-flash` on a working master.

> **Gotcha:** if you're on a checkout older than this section, `gen_master_pubkey_pin.py` took a
> `masterkey.json` and produced a pin that could never match a real board — that was #126. The
> current tool refuses `.json` input outright and tells you what to pass instead.

## Step 4: Configure `project_config.h`

At minimum, set a real `DEFAULT_MESH_KEY` (see `getting_started.md` §6 for how) — every node in
the mesh must share it. `DEFAULT_ADAPTER` should already default to `SERIAL_ADAPTER`, which is
correct for a master; leave it.

## Step 5: Build and flash

```bash
cd firmware
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-0001 flash
```

> **Gotcha:** some USB-serial adapters' auto-reset circuitry (the one that's supposed to
> automatically enter the ROM bootloader via DTR/RTS toggling) is unreliable, especially right
> after a hot-plug rather than a fresh power-on. If `idf.py flash` hangs at "Connecting..." or
> otherwise fails to talk to the chip, manually hold the physical **BOOT** button down, tap **EN**
> once while still holding BOOT, release EN, then release BOOT once "Writing at..." starts
> scrolling. See [lattice-hub#162](https://github.com/superbrobenji/lattice-hub/issues/162) for
> the deeper root-cause writeup — this same unreliability also affects reconnecting to an
> already-running board after a replug, not just flashing.

## Step 6: First boot

```bash
idf.py -p /dev/cu.usbserial-0001 monitor
```

A fresh board boots as a **leaf**, not master — there is no first-boot-as-master shortcut in
production mode (`DEV_MODE=false`, which you should leave alone for anything beyond a quick bench
test). You'll see:

```
LATTICE_PUBKEY:<64 hex chars>
```

and the seven-segment display (if wired) will alternate flashing dashes — "not yet enrolled,
waiting for the hub."

## Step 7: Become master

**A leaf never needs hub approval to become master** — the config button bypasses enrollment
entirely (`main.cpp`: `enrolled = isEnrolled() || isMaster()`). Hold the config button (GPIO32,
wired 3.3V → button → GPIO32, no external resistor needed) for a full, continuous **5 seconds**.

Watch the green LED when you release:

| Blinks | Meaning |
|---|---|
| 3× | Now master |
| 2× | Now leaf |

**This is a toggle, not a "set to master."** If the board was already master (e.g. from an earlier
hold you don't remember, or one that happened mid-flash-noise), holding it again flips it back to
leaf. If you get 2 blinks and wanted master, hold it again.

The device restarts automatically ~2 seconds after the blink. Once back up, the display should
show a steady value (not flashing dashes) — the hub side will confirm connectivity via
`/api/v1/status`'s `mesh.masterOnline`.

## Verifying it worked

The cleanest, unambiguous proof a role-flip actually happened: watch `idf.py monitor` live across
the button hold and look for `rst:0xc (SW_CPU_RESET)` in the reboot banner — that specific reset
reason only comes from a software-initiated `esp_restart()`, and the config-button role-flip is
one of only two places in the firmware that ever call it (the other being the reset-button
factory-wipe double-hold). A `rst:0x1 (POWERON_RESET)` means something else caused the reboot
(power cycle), not the button.

## Dual-master setup

Bench-verified end to end this session with two real boards. `DUAL_MASTER_MODE` is a **compile-time**
constant (`Mesh.cpp`: `_dualMasterMode(lattice::config::DUAL_MASTER_MODE)`), not something the
config button or EEPROM can toggle at runtime (see
[lattice-nodes#116](https://github.com/superbrobenji/lattice-nodes/issues/116), which proposes
changing that) — so setting it up means:

1. **Set `DUAL_MASTER_MODE = true` in `project_config.h` and rebuild.** This is a local, uncommitted
   edit for bench testing — not something to push to `main` (the shipped default is `false`,
   single-master).
2. **Reflash *every* board with this build — including your already-working primary master.**
   Easy to miss: the primary doesn't need the flag to keep functioning as primary, but every node
   that needs to correctly recognize a *second* master (beacon-processing logic in
   `MasterBeacon.cpp`/`FrameAuthorizer.cpp`) needs it compiled in. A primary still running an old
   `DUAL_MASTER_MODE=false` build won't misbehave on its own, but the mesh as a whole isn't
   correctly in dual-master mode until every board is.
3. **Both boards get the exact same `master_pubkey_pin.h`** — the one generated in Step 3 above,
   from the *primary's* `LATTICE_PUBKEY` and MAC. Do **not** regenerate a pin file from the second
   board's own key or MAC; the pin is a single mesh-wide primary identity, not a per-board thing (see
   [lattice-nodes#118](https://github.com/superbrobenji/lattice-nodes/issues/118) for how a board
   knows whether *it itself* is primary or secondary — it's a local comparison against this same
   pinned MAC, no second pin needed).
4. **Read the second board's MAC and flash it**, same as Steps 1/5/6 above, using its own device
   path.
5. **Bring it up as master via the config button**, same as Step 7. It boots as leaf first, same
   as any fresh board.
6. **Wire up the hub side** — see `lattice-hub`'s
   [`docs/macos_native_dev.md`](https://github.com/superbrobenji/lattice-hub/blob/main/docs/macos_native_dev.md)
   "Dual-master setup" section for `SECONDARY_MASTER_MAC`/`DUAL_MASTER_ENABLED`/`SERIAL_PORT_SECONDARY`.

### Worked example (this session's actual boards)

| Role | MAC | Device path (at time of testing — **can shift on replug**, always re-check with `esptool.py read_mac`, don't assume) |
|---|---|---|
| Primary master | `ec:64:c9:5d:ac:18` | `/dev/cu.usbserial-3` (later `/dev/cu.usbserial-4` after a replug) |
| Secondary master | `ec:64:c9:5d:22:20` | `/dev/cu.usbserial-0001` |

Both boards were flashed from the identical build (`DUAL_MASTER_MODE=true`, same
`master_pubkey_pin.h` pinned to the primary's key and MAC above). Bench-verified failover in both
directions: unplugging either master left the other's health-report stream to the hub completely
uninterrupted (confirmed via `/tmp/orchestrator.log` — each board's reports kept arriving on
schedule regardless of the other's state). Replugging never self-healed on the hub side in either
direction — see the hub doc's "Known issues" for why and how to recover.

### Gotcha: verify your fixes actually survived, if you've been juggling git branches

Hit this directly this session: after opening a PR from a fix branch, running
`git reset --hard origin/main` to keep the local branch clean **reverted the fix in the working
tree** — the PR wasn't merged yet at that point, so `origin/main` didn't have it. Every subsequent
build silently used the unfixed code. It only surfaced because a *second* board with blank EEPROM
hit the exact bug the fix was supposed to prevent — a board with already-populated EEPROM (like an
existing working primary) won't show the regression at all, since the buggy code path only
triggers on a truly first-time boot.

If you've been resetting/switching branches mid-session and something that used to work stops
working (or a bug that was supposedly fixed comes back) — grep the actual file content for the fix
before assuming it's a new bug:
```bash
git log --oneline -1 -- <file>   # confirms which commit last touched it
grep -n '<a line from the fix>' <file>   # confirms the fix is actually there right now
```

## Known issues affecting this workflow

- [lattice-nodes#111](https://github.com/superbrobenji/lattice-nodes/issues/111) — if a node's
  adapter hardware fails to initialize (e.g. a leaf configured for `PIR_ADAPTER` without a real
  sensor attached), the device halts *permanently*, with no button-based recovery at all. Doesn't
  affect a `SERIAL_ADAPTER` master, but will bite if you're also bringing up leaf/sensor nodes.
- [lattice-hub#161](https://github.com/superbrobenji/lattice-hub/issues/161) /
  [#162](https://github.com/superbrobenji/lattice-hub/issues/162) — the orchestrator does not
  auto-reconnect after a board is unplugged/replugged, even on the exact same device path. After
  any physical disconnect, restart the orchestrator process manually.
- [lattice-hub#167](https://github.com/superbrobenji/lattice-hub/issues/167) — occasionally a
  *restart* itself doesn't fully recover a connection (read loop gets stuck silently). If a fresh
  restart doesn't produce a health report within the normal ~30-60s interval, try restarting again
  rather than assuming the board itself is broken.
- [lattice-nodes#126](https://github.com/superbrobenji/lattice-nodes/issues/126) — the pin and
  E2E crypto share one JOIN_ACK field, so the pin *must* be the master board's own key (Steps 2–3
  above). Pins generated from `masterkey.json` by older checkouts never enrolled anything on a real
  bench. A cleaner split of the two identities is tracked in
  [#120](https://github.com/superbrobenji/lattice-nodes/issues/120) /
  [#121](https://github.com/superbrobenji/lattice-nodes/issues/121).
- [lattice-nodes#116](https://github.com/superbrobenji/lattice-nodes/issues/116) — `DUAL_MASTER_MODE`
  should be a runtime setting, not a compile-time flag requiring a mesh-wide reflash to toggle.
- [lattice-nodes#118](https://github.com/superbrobenji/lattice-nodes/issues/118) — the display's
  master indicator (decimal point) doesn't currently render for a master whose node ID is 0, which
  is every master today. Don't rely on the display to distinguish master from leaf yet, let alone
  primary from secondary.
