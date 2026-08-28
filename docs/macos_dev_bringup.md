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

## Step 2: Get a real `masterkey.json` from the hub

Follow `lattice-hub`'s doc to get its orchestrator running with `MASTER_MAC` set to the value from
Step 1. Once it's started once, it will have generated
`../lattice-hub/server/orchestrator/data/masterkey.json` (or wherever `MASTER_KEY_PATH` points).

Copy it into this repo temporarily:

```bash
cp ../lattice-hub/server/orchestrator/data/masterkey.json ./masterkey.json
```

**⚠️ This file contains the hub's private key.** Delete it again as soon as Step 3 is done — don't
commit it, don't leave it lying around.

## Step 3: Generate the pin file

```bash
python3 tools/gen_master_pubkey_pin.py masterkey.json <MAC from Step 1>
rm masterkey.json
```

This writes `firmware/main/config/master_pubkey_pin.h` (gitignored, per-deployment).

> **Gotcha:** the real `masterkey.json` a running hub produces is `{"public_key": [int, int, ...],
> "private_key": [...]}` — plain JSON int arrays, from Go's `[32]byte` struct fields. It is **not**
> the `{"publicKey": "<base64>"}` shape used by `getting_started.md`'s throwaway-key bench-test
> example. `gen_master_pubkey_pin.py` handles both formats as of `15bf003` — if you're on an older
> checkout, this step will fail with `publicKey field not found in masterkey.json` against a real
> hub key even though it works fine against the docs' synthetic example.

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

## Known issues affecting this workflow

- [lattice-nodes#111](https://github.com/superbrobenji/lattice-nodes/issues/111) — if a node's
  adapter hardware fails to initialize (e.g. a leaf configured for `PIR_ADAPTER` without a real
  sensor attached), the device halts *permanently*, with no button-based recovery at all. Doesn't
  affect a `SERIAL_ADAPTER` master, but will bite if you're also bringing up leaf/sensor nodes.
- [lattice-hub#161](https://github.com/superbrobenji/lattice-hub/issues/161) /
  [#162](https://github.com/superbrobenji/lattice-hub/issues/162) — the orchestrator does not
  auto-reconnect after a board is unplugged/replugged. After any physical disconnect, restart the
  orchestrator process manually.
