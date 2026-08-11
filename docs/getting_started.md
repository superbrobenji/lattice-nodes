# Getting Started: Building and Flashing Lattice Firmware From Source

This guide walks you from an empty computer to a working, flashed ESP32 running
Lattice firmware — building everything from source, no shortcuts skipped. It is
written for someone who has **never used ESP32 tools before**. You don't need
prior embedded/firmware experience. You do need to be comfortable copying a
command, pasting it into a terminal window, and pressing Enter — every command
below is written to be copied exactly as shown.

A "terminal" is the text-based command window: **Terminal** on macOS, your
distribution's terminal app on Linux (e.g. GNOME Terminal, Konsole), or
**Command Prompt** / **PowerShell** on Windows. All the commands in this guide
are typed into one of those.

If something goes wrong at any step, skip ahead to [Troubleshooting](#12-troubleshooting) —
the most common failure (a compile error mentioning `master_pubkey_pin.h`) has
its own explicit entry there.

## Contents

1. [What you'll end up with](#1-what-youll-end-up-with)
2. [Hardware you'll need](#2-hardware-youll-need)
3. [Step 1: Install ESP-IDF](#3-step-1-install-esp-idf)
4. [Step 2: Clone the repo and initialize the submodule](#4-step-2-clone-the-repo-and-initialize-the-submodule)
5. [Step 3: Generate your master-pubkey pin file](#5-step-3-generate-your-master-pubkey-pin-file)
6. [Step 4: Configure `project_config.h`](#6-step-4-configure-project_configh)
7. [Step 5: Build](#7-step-5-build)
8. [Step 6: Flash](#8-step-6-flash)
9. [Step 7: First boot — provisioning](#9-step-7-first-boot--provisioning)
10. [Step 8: Understanding the LEDs and display](#10-step-8-understanding-the-leds-and-display)
11. [Step 9: Using the buttons](#11-step-9-using-the-buttons)
12. [Troubleshooting](#12-troubleshooting)
13. [What's next](#13-whats-next)

---

## 1. What you'll end up with

By the end of this guide you'll have a physical ESP32 board flashed with
Lattice firmware. Depending on how you configure it, that board will either:

- act as a **sensor node** — for example watching a PIR (motion) sensor and
  reporting events into the mesh, or
- act as the **master node** — the one board that bridges the wireless mesh
  to a server over a USB cable.

All Lattice nodes talk to each other wirelessly over ESP-NOW (a low-latency,
router-free WiFi-radio protocol — you do **not** need a WiFi network or
router for the mesh itself). This repository (`lattice-nodes`) only covers the
firmware — the code that runs *on* the ESP32 boards.

**A complete, operating system also needs a server.** The master node's job is
to relay mesh traffic over USB serial to `lattice-hub`, a separate Go server
application that handles enrollment approval, a REST API, and (optionally) a
dashboard. Getting `lattice-hub` running is **out of scope for this guide** —
if you intend to operate a full system (not just flash and bench-test a single
node), you will also need to set up that separate repository. This guide will
get you to a firmware build that boots, prints its identity, and is ready to
be enrolled — that's the finish line here.

## 2. Hardware you'll need

See [`docs/hardware_requirements.md`](hardware_requirements.md) for the full
parts list (board model, LEDs, buttons, optional seven-segment display, and
wiring). At minimum you will need an ESP32 development board (the original
ESP32, not an ESP32-S3/C3/etc. — see [Step 5](#7-step-5-build) below for why
that matters) and a USB cable that supports data transfer (not a
charge-only cable).

## 3. Step 1: Install ESP-IDF

Lattice firmware is built with **ESP-IDF v5.5.1** — Espressif's official
ESP32 development framework and toolchain. This is a one-time setup per
computer; you do not need to repeat it for future builds.

> **Why this exact version?** `firmware/dependencies.lock` pins the toolchain
> to `5.5.1`. Using a different major/minor version can silently change build
> settings and is not guaranteed to produce a working image. Install exactly
> `v5.5.1`.

### macOS / Linux

Open a terminal and run:

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32
```

- `mkdir -p ~/esp` creates a folder named `esp` in your home directory (if it
  doesn't already exist) — this is just a convenient, conventional place to
  keep the ESP-IDF toolchain; you can use a different folder if you prefer.
- `git clone -b v5.5.1 --recursive ...` downloads ESP-IDF itself, at the
  `v5.5.1` tag, including its own sub-dependencies (`--recursive`). This step
  downloads a fair amount of data and can take several minutes.
- `./install.sh esp32` downloads and installs the actual compiler toolchain
  and Python packages needed to target the ESP32 chip specifically. This can
  also take several minutes and will print a long stream of installation
  progress; a line ending in something like `All done! You can now run:` means
  it succeeded.

**Linux only:** ESP-IDF's installer expects a handful of common system
packages (Python 3, Git, CMake, Ninja, etc.) to already be present. On
Debian/Ubuntu-based systems, install them first with:

```bash
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

(Other distributions use their own package manager — see
[Espressif's Linux setup docs](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/get-started/linux-macos-setup.html)
if `apt-get` isn't available on your system.)

Every time you open a **new** terminal window and want to build Lattice, you
need to load ESP-IDF's environment into that terminal by running:

```bash
. ~/esp/esp-idf/export.sh
```

(Note the leading `.` and space — this "sources" the script into your current
shell rather than just running it. This is required every new terminal
session; if you forget it, `idf.py` commands below will fail with a
"command not found" error.)

### Windows

The simplest path on Windows is Espressif's **ESP-IDF Tools Installer**, a
graphical installer that sets up Python, Git, the toolchain, and ESP-IDF
itself in one step:

1. Download the ESP-IDF Windows installer from
   [Espressif's official ESP-IDF Windows installer page](https://dl.espressif.com/dl/esp-idf/).
2. Run it, and when prompted for the ESP-IDF version, select **`v5.5.1`**
   (or "Find Espressif IDF" → download `release/v5.5.1` if it's not offered
   directly in the list of pre-set versions).
3. Accept the defaults for install location and components.

The installer also creates an **"ESP-IDF 5.5.1 CMD"** (or PowerShell)
shortcut in your Start Menu — use that shortcut to open a terminal with
ESP-IDF already loaded, instead of a plain Command Prompt. Every command in
this guide should be run from that shortcut's terminal window.

If you prefer the manual/command-line route instead of the graphical
installer (equivalent to the macOS/Linux steps above), from a regular Command
Prompt:

```bat
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
install.bat
export.bat
```

`export.bat` (like `export.sh` on macOS/Linux) must be re-run in every new
Command Prompt/PowerShell window before using `idf.py`.

### Verify the install

In your ESP-IDF-enabled terminal (after running `export.sh`/`export.bat`, or
from the Start Menu shortcut on Windows), run:

```bash
idf.py --version
```

A successful install prints a line like:

```
ESP-IDF v5.5.1
```

If instead you see `command not found` (macOS/Linux) or `'idf.py' is not
recognized...` (Windows), the environment script wasn't sourced in this
terminal — re-run `. ~/esp/esp-idf/export.sh` (or `export.bat`) and try again.

## 4. Step 2: Clone the repo and initialize the submodule

In your ESP-IDF-enabled terminal, run:

```bash
git clone https://github.com/superbrobenji/lattice-nodes.git
cd lattice-nodes
git submodule update --init --recursive
```

- `git clone` downloads a copy of this repository to your computer.
- `cd lattice-nodes` moves your terminal into the newly downloaded folder —
  every remaining command in this guide assumes you're inside this folder
  (or a subfolder of it, as noted).
- `git submodule update --init --recursive` is the important one to not skip.
  A **submodule** is a second, separate git repository that lives nested
  inside this one — in this case, `firmware/main/lib/lattice-protocol`, which
  holds the shared network-message definitions both the firmware and the
  server use. Cloning the main repo does **not** automatically download
  submodule contents; this command does that as a separate step.

**How do you know it worked?** Check that the submodule folder is populated,
not empty:

```bash
ls firmware/main/lib/lattice-protocol
```

If the submodule step succeeded, this prints a handful of files and folders
(headers, a `message/` directory, etc.). If it prints nothing (an empty
folder), the submodule step was skipped or failed — re-run
`git submodule update --init --recursive` from the `lattice-nodes` folder.

(If you'd rather do it in one step next time, `git clone --recurse-submodules
https://github.com/superbrobenji/lattice-nodes.git` clones the repo and
initializes submodules together.)

## 5. Step 3: Generate your master-pubkey pin file

**Do not skip this step.** This is the single most important step in this
guide — skip it, and your very first build will fail with a compile error.

Lattice firmware verifies the identity of the master/hub it's talking to
using a pinned public key baked into the firmware at compile time. That
pinned key lives in a file, `firmware/main/config/master_pubkey_pin.h`, which
**does not exist in the repository** — it's listed in `.gitignore`
specifically because it's unique to each deployment and must never be
committed to source control. The firmware will not compile without it: there
is a deliberate `#error` in
`firmware/main/config/master_pubkey_pin_wrapper.h` that stops the build cold
if this file is missing, with a message telling you exactly what to do:

```
error: "firmware/main/config/master_pubkey_pin.h not found. Generate it via
tools/gen_master_pubkey_pin.py or build with -DLATTICE_ALLOW_EXAMPLE_PIN=1
(DEV_MODE only)."
```

This is a security feature, not a bug — it exists so firmware can't
accidentally ship without a way to verify it's talking to the real master.

### How to generate it

The file is generated by `tools/gen_master_pubkey_pin.py`, which takes two
inputs: a `masterkey.json` file (containing a base64-encoded 32-byte public
key) and the master node's MAC address, and writes out a compiled-in C++
header.

**In a real deployment**, `masterkey.json` is generated by the `lattice-hub`
server the first time it starts up, and you would copy that file from your
hub server to your firmware checkout before running the generator. Since
this guide is firmware-only and doesn't walk through setting up the hub (see
[section 1](#1-what-youll-end-up-with)), you may not have a real
`masterkey.json` yet.

**For a first-time "just get it building" experience**, you can generate a
throwaway key locally — this is enough to compile and test the firmware on
the bench, but it will **not** let a real node talk to a real hub (they need
to share the same master identity). From the `lattice-nodes` folder, run:

```bash
python3 -c "import json,base64,os; json.dump({'publicKey': base64.b64encode(os.urandom(32)).decode()}, open('masterkey.json','w'))"
python3 tools/gen_master_pubkey_pin.py masterkey.json aa:bb:cc:dd:ee:ff
```

- The first command creates a throwaway `masterkey.json` file containing 32
  random bytes, base64-encoded, in the field the generator script expects.
- The second command runs the generator script against that file and a
  placeholder MAC address (`aa:bb:cc:dd:ee:ff` — a conventional placeholder,
  not a real device's address), and writes the compiled header.

A successful run prints exactly one line:

```
wrote /path/to/lattice-nodes/firmware/main/config/master_pubkey_pin.h
```

You can also confirm the file now exists with `ls firmware/main/config/` —
you should see `master_pubkey_pin.h` in the listing.

**Before a real deployment against a real hub**, delete this throwaway file
and regenerate it using the actual `masterkey.json` your `lattice-hub`
instance produced, and the actual master node's real MAC address — every
node in a real mesh must be built against the same real master identity, or
they will refuse to trust the master's `JOIN_ACK` messages.

**Do not commit `masterkey.json` or `master_pubkey_pin.h`.** Both are
per-deployment secrets/identifiers. `master_pubkey_pin.h` is already covered
by `.gitignore`; treat `masterkey.json` the same way and delete it (or keep
it somewhere outside the repo) once you're done generating the header.

## 6. Step 4: Configure `project_config.h`

All of the firmware's compile-time settings — pins, radio channel, the mesh
encryption key, logging verbosity, and more — live in one file:
`firmware/main/project_config.h`. Open it in any text editor before your
first build.

For a **first-time, single-node bring-up**, most of these can be left at
their defaults. The one you should change is `DEFAULT_MESH_KEY` (below) —
everything else is safe to leave as-is until you're deploying for real or
have a specific reason to change it.

### What you should change now

| Constant | What it is | What to do |
|---|---|---|
| `DEFAULT_MESH_KEY` | The shared 16-byte AES key every node uses to encrypt mesh radio traffic. The file ships with a placeholder value and an explicit `WARNING: Change this before deployment` comment. | Generate a random one (see below) and use the **same** key on every node in your mesh. |

**Generating a mesh key:** run this in your terminal:

```bash
python3 -c "import os; print([hex(b) for b in os.urandom(16)])"
```

This prints a Python-style list of 16 random hex bytes, for example:

```
[0x4a, 0xf1, 0x0c, 0x9b, 0x77, 0x2e, 0x88, 0x03, 0x5d, 0xe6, 0x1f, 0xaa, 0x90, 0x4c, 0x33, 0x67]
```

(Your actual output will be different random bytes every time you run it —
that's expected and correct.) Open `firmware/main/project_config.h`, find the
`DEFAULT_MESH_KEY` array (in the "Radio / ESP-NOW" section), and replace the
16 values between the curly braces with the 16 values you just generated —
**copy only the `0x..` values, not the square brackets** (the file needs
`{ }`, the Python output gives you `[ ]`). It should end up looking like:

```cpp
inline constexpr uint8_t DEFAULT_MESH_KEY[16] = {0x4a, 0xf1, 0x0c, 0x9b, 0x77, 0x2e, 0x88, 0x03,
                                                 0x5d, 0xe6, 0x1f, 0xaa, 0x90, 0x4c, 0x33, 0x67};
```

Save the file. Every node you build must use this same array of 16 values —
mismatched keys can't talk to each other.

### Everything else, for reference

You don't need to change any of the below for a first single-node build —
this table is here so you know what each setting does when you're ready to
tune it.

| Constant | Purpose | Default / first-timer guidance |
|---|---|---|
| `DEV_MODE` | Compile-time switch: `true` skips EEPROM writes and always boots into the role set by `DEFAULT_DEV_MASTER` below, ignoring any previously-saved role. | Leave `false` for anything you intend to actually deploy; `true` only for quick bench tests where you don't want settings to persist. |
| `DEFAULT_DEV_MASTER` | Only used when `DEV_MODE` is `true`: whether this node boots as the master or as a leaf node. | `true` if this is your one hub-connected board; `false` for every other dev-mode board. |
| `DEFAULT_ADAPTER` | Which sensor/IO "adapter" the node uses on first boot (or always, in `DEV_MODE`). **Must stay `SERIAL_ADAPTER` for any node that talks to the hub over USB.** | Leave as `SERIAL_ADAPTER` for your master/hub-connected node. |
| `MASTER_BEACON_INTERVAL_MS` | How often (in milliseconds) the master broadcasts a "hello, I'm here" beacon to the mesh. | Leave at `3000` (3 seconds) unless tuning RF behavior. |
| `STALE_MASTER_THRESHOLD_MS` | How long a node waits without hearing a beacon before deciding the master is gone. | Leave at `9000` — it's derived from the interval above; change both together if you do change it. |
| `DUAL_MASTER_MODE` | Enables a redundant, two-master setup. | Leave `false` unless you're specifically deploying two masters. |
| `RELAY_JITTER_MAX_MS` | Random delay window nodes use before relaying, to avoid radio collisions when many nodes relay at once. | Leave at `64`. |
| `WIFI_CHANNEL` | The WiFi/ESP-NOW radio channel. **Every node in the mesh must use the identical value.** | `1` is a fine default — pick any 2.4 GHz channel (1/6/11 are common choices) that's relatively clear in your environment, and use the same number on every node. |
| `RED_LED_PIN` / `GREEN_LED_PIN` | GPIO pins for the two status LEDs. | Defaults are `33` (red) / `26` (green) — wire your LEDs there, or change these to match your wiring. |
| `CONFIG_BUTTON_PIN` / `RESET_BUTTON_PIN` | GPIO pins for the two buttons (see [Step 9](#11-step-9-using-the-buttons)). | Defaults are `32` / `25`. |
| `SEVSEG_DATA_PIN` / `SEVSEG_CLK_PIN` | GPIO pins for the optional seven-segment status display. | Defaults are `23` / `22`. |
| `ENABLE_SEVSEG_DISPLAY` | Whether the firmware drives a seven-segment display at all. | `true` if you have the display wired up; `false` if you don't (saves a little overhead and frees those pins). |
| `DEFAULT_PEERS` | The initial list of other nodes' MAC addresses, written into storage on first boot. Ships with two obvious placeholder addresses and a `TODO: Replace these` comment. | Fine to leave as placeholders for your very first single-board smoke test (a lone node has no peers to talk to yet). Once you have real hardware to pair, get each node's real MAC address from its own serial output on first boot (see [Step 7](#9-step-7-first-boot--provisioning)) and update this list, then reflash. |
| `DEFAULT_LOG_LEVEL` | How much debug text the firmware prints over serial. **For any node using `SERIAL_ADAPTER` to talk to the hub, this must stay `LOG_NONE`** — any extra text corrupts the binary protocol the hub expects on that same wire. | Leave at `LOG_NONE`. Only change this (to `LOG_DEBUG` or similar) for bench-testing a node that is *not* simultaneously talking to a hub over the same USB connection. |
| `DEFAULT_TX_POWER_PRESET` | Named radio transmit-power preset: `SHORT_RANGE` (same room), `INDOOR` (through walls), `OUTDOOR` (maximum range). | Ships as `OUTDOOR` — reasonable to leave as-is unless you specifically want to reduce range/interference. |
| `SIMULATE_MODE` | Enables fake, serial-injected sensor events for testing without real sensor hardware. | Leave at `0` (off) for a real flash. |

### Advanced: mesh-scale limits (near the bottom of the file)

The last section of `project_config.h` ("Global Limits") is a set of
bounded-resource constants that cap the maximum scale of a mesh — how many
peers, routes, and cached keys the firmware will ever allocate memory for.
You do not need to touch any of these for a first build; they're listed here
only so you know what they're for if you ever need to grow a deployment
beyond its defaults.

| Constant | Purpose | Default |
|---|---|---|
| `LATTICE_E2E_KEYCACHE_MAX` | Master-side cap on cached end-to-end derived-key entries (one per enrolled node). | `10` |
| `LATTICE_E2E_KEYCACHE_MAX_LEAF` | Leaf-side cap (a leaf only ever needs keys for its master(s)). | `2` |
| `LATTICE_NEIGHBOR_MAX` | Max beacon-learned forwarding neighbors tracked per node. | `8` |
| `LATTICE_REPLAY_MAX_ORIGINS` | Per-origin replay-protection cache slot count. | `12` |
| `LATTICE_ROUTE_TABLE_MAX` | Master's downlink source-routing table size (raise this for large deployments with many nodes). | `16` |
| `LATTICE_DOWNLINK_PEER_MAX` | Cap on auto-registered downlink-forwarding ESP-NOW peers (a security bound against peer-table exhaustion). | `4` |
| `MAX_HOPS` | Maximum routing hops allowed across the mesh. | `8` |
| `STALE_PEER_THRESHOLD_MS` | How long before an unresponsive peer is considered offline. | `8000` ms |
| `ROUTING_TIMEOUT_MS` | Timeout used by the message router. | `5000` ms |
| `HEALTH_REPORT_INTERVAL_MS` | How often a node sends a periodic health report. | `30000` ms |
| `ROUTE_REPORT_INTERVAL_MS` | How often route reports are sent (derived: 2× the health-report interval). | `60000` ms |

## 7. Step 5: Build

From the `lattice-nodes` folder, move into the `firmware` folder and build:

```bash
cd firmware
idf.py set-target esp32
idf.py build
```

- `cd firmware` — the build tooling operates from this subfolder, not the
  repo root.
- `idf.py set-target esp32` tells the build system which chip you're
  targeting. **This project targets the original ESP32 chip specifically**
  (not an ESP32-S3, C3, or other variant) — use exactly `esp32`. You only
  need to run this once per fresh checkout (or if you ever delete the
  `build/` folder); it's safe to run again if you're unsure whether you
  already did it.
- `idf.py build` does the actual compile. **The first time**, this also
  downloads/compiles a large number of dependencies and can take several
  minutes; it prints a long, scrolling stream of compiler output. This is
  normal — a wall of text scrolling by is expected, not an error.

### What success looks like

A successful build ends with something like:

```
lattice-nodes.bin binary size 0xa22b0 bytes. Smallest app partition is 0x100000 bytes. 0x5dd50 bytes (37%) free.
[100%] Built target app_check_size
[100%] Built target app

Project build complete. To flash, run:
 idf.py flash
or
 idf.py -p PORT flash
```

The exact line `Project build complete.` is the signal you're looking for.
The line above it (binary size / free space) is just informational — it
means your compiled firmware fits comfortably inside the 1 MiB flash
partition reserved for it.

### If the build fails

The two most common first-time failures are forgetting an earlier step:

- **Error mentions `master_pubkey_pin.h` or `MASTER_PUBKEY`/`MASTER_MAC`
  "is not a member of"**: you skipped [Step 3](#5-step-3-generate-your-master-pubkey-pin-file).
  Go back and generate the file.
- **Error mentions missing headers from somewhere under `lattice-protocol`
  or `lib/`**: you skipped the submodule step in
  [Step 2](#4-step-2-clone-the-repo-and-initialize-the-submodule). Run
  `git submodule update --init --recursive` from the repo root, then
  re-run `idf.py build`.

See also [Troubleshooting](#12-troubleshooting) below for more.

## 8. Step 6: Flash

"Flashing" means copying your freshly built firmware onto the physical board
over USB.

1. **Connect the board** to your computer with a USB data cable.
2. **Find its serial port name.** Your computer identifies the board as a
   serial device with a specific name/number, and you need that name for the
   next command.

   - **macOS**: run `ls /dev/tty.*` in a terminal, note the list, then
     physically plug in the board (if not already plugged in) and run
     `ls /dev/tty.*` again — the new entry that appears is your board,
     typically named something like `/dev/cu.usbserial-0001` or
     `/dev/cu.SLAB_USBtoUART`.
   - **Linux**: the board typically appears as `/dev/ttyUSB0` (or `ttyUSB1`,
     etc. if you have other serial devices attached). You can confirm with
     `ls /dev/ttyUSB*` before and after plugging in, the same way as macOS
     above.
   - **Windows**: open **Device Manager** (search for it in the Start Menu),
     expand **"Ports (COM & LPT)"**, and look for an entry that appears when
     you plug the board in — it will show a name like `Silicon Labs CP210x
     USB to UART Bridge (COM4)`. The `COM4` part (yours will likely be a
     different number) is the port name you need.

3. **Flash it**, substituting your actual port name for `PORT`:

   ```bash
   idf.py -p PORT flash
   ```

   For example: `idf.py -p /dev/cu.usbserial-0001 flash` (macOS),
   `idf.py -p /dev/ttyUSB0 flash` (Linux), or `idf.py -p COM4 flash`
   (Windows).

### What success looks like

You'll see esptool (the flashing tool ESP-IDF uses internally) connect to
the board, then write each of three pieces (bootloader, partition table, and
your application) with a progress indicator for each, e.g.:

```
Writing at 0x00010000... (100 %)
Wrote 664240 bytes (app image)...
Hash of data verified.

Hard resetting via RTS pin...
```

**`Hard resetting via RTS pin...`** is the final line of a successful flash —
it means the tool finished writing and reset the board so your new firmware
starts running immediately.

If the command instead fails to even connect (e.g. it hangs at "Connecting..."
or reports a permission/access error), see
[Troubleshooting](#12-troubleshooting) — "can't find my serial port" below.

## 9. Step 7: First boot — provisioning

Once flashed, watch the board's serial output to see it boot and to capture
the information you'll need to enroll it with a hub. Run:

```bash
idf.py -p PORT monitor
```

(same `PORT` as the flash step — you can also combine both:
`idf.py -p PORT flash monitor`.)

This opens a live view of everything the firmware prints over USB. To leave
the monitor when you're done, press **Ctrl+]** (this is a standard ESP-IDF
convention, not specific to this repo).

**What to look for:** if the node isn't enrolled with a hub yet (which is
true for every freshly flashed node), it prints a single line that looks
like this:

```
LATTICE_PUBKEY:3A7F2B91C4D06E5F8A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F607A
```

That's `LATTICE_PUBKEY:` followed by exactly 64 uppercase hexadecimal
characters — the node's public identity key (its private key is never
printed anywhere). **Copy that 64-character value** — it's what gets
registered with your hub server to approve/enroll this specific node into
the mesh. The technical detail of how a hub approves an enrollment and sends
back the acknowledgement the node is waiting for is covered in
[`docs/server_requirements.md`](server_requirements.md)'s "Enrollment
Protocol" section — that part happens on the hub/server side, outside this
firmware repo.

Once a hub approves the node and the node receives its acknowledgement, this
line stops appearing on future boots — the node now considers itself
enrolled.

## 10. Step 8: Understanding the LEDs and display

Lattice nodes communicate status through two LEDs (red and green) and,
optionally, a small seven-segment numeric display. None of these require any
special tools to read — this section explains what you'll see and what it
means, in plain language.

### LED patterns

Both LEDs blink in short, distinct patterns rather than staying simply on or
off — count the blinks (and which color) to know what happened.

| What you see | When it happens | What it means |
|---|---|---|
| Green **and** red both blink twice (fast) | Once, right after boot | **Startup complete.** The firmware booted successfully and reached its main loop. This is not an error — it's a normal "I'm alive" signal. |
| Green blinks twice (fast), repeating | Every time the node receives mesh data | **Mesh activity.** The node just received a message from another node. Frequent green blinking during operation means the mesh is actively talking. |
| Green blinks 2 or 3 times, device restarts a couple seconds later | You held the config button for 5 seconds (see [Step 9](#11-step-9-using-the-buttons)) | **Role changed.** 2 blinks = the node is now a plain node (leaf); 3 blinks = the node is now the master. The device automatically restarts a moment later to apply the change. |
| Red blinks 3 times | You held the reset button for 5 seconds, first time | **Reset armed.** Hold the reset button again, for another 5 seconds, within the next 3 seconds, to actually confirm the wipe. If you don't, nothing happens — it's silently cancelled and you'd need to start over. |
| Red **and** green both blink 5 times together, device restarts a few seconds later | You confirmed a reset (see above) | **Factory wipe in progress.** All saved settings (role, paired devices, identity, enrollment status) are being erased. The device restarts as a blank, unenrolled node. |
| Red blinks a specific number of times (1 through 8), repeating | An internal error occurred | **Error code**, by blink count: 1 = generic error, 2 = sensor failure, 3 = communication failure, 4 = memory error, 5 = configuration error, 6 = hardware failure, 7 = user error, 8 = timeout. A memory error or hardware failure additionally restarts the device automatically after blinking. |
| Green blinks 6 times, pausing and repeating forever | The red LED itself failed to turn on at boot | **Fatal fault, fallback signal.** The board couldn't use its normal red error LED, so green is repurposed to signal the fault instead. The device halts here — it will not proceed with normal operation. Double-check your LED wiring against the pin numbers in `project_config.h`. |
| No LEDs blink at all, board unresponsive | Neither LED could be initialized | **Total hardware failure**, with no visual indicator. If nothing lights up at all after flashing, check that your LEDs are wired to the correct GPIO pins from `project_config.h` (defaults: red = GPIO 33, green = GPIO 26). |

### Seven-segment display (optional)

If you have the optional TM1637 seven-segment display wired up
(`ENABLE_SEVSEG_DISPLAY = true`), it shows:

| What you see | What it means |
|---|---|
| Four dashes (`----`), alternating with blank, about twice a second | **Not yet enrolled.** The node is waiting for a hub to approve it — this is the same window during which it's printing `LATTICE_PUBKEY:...` to the serial monitor (see [Step 7](#9-step-7-first-boot--provisioning)). |
| A plain number | **Enrolled, regular node.** The number is the node ID assigned by the hub when it was enrolled. |
| A number with a decimal point lit | **Enrolled, master node.** Same node-ID display, but the lit decimal point marks this board specifically as the master. |
| A different numeric code appears whenever an error LED pattern (above) fires | **Error code.** Gives a specific numeric code for troubleshooting, in addition to the LED blink count — see [`docs/error_codes.md`](error_codes.md) for the registry of specific codes. (Note: this numeric code system and the red-LED blink-count system above are two related but separately-numbered schemes in the current firmware — if the two ever seem to disagree on a specific error, trust the more specific numeric code from the display.) |

## 11. Step 9: Using the buttons

If you have the two optional buttons wired up (config and reset), here's
exactly what each does. **Both require a full, continuous 5-second hold** —
a quick tap does nothing.

### Config button (role toggle)

Wired to GPIO 32 by default (`CONFIG_BUTTON_PIN`).

- **Hold it down for 5 seconds.** The green LED blinks (2 or 3 times — see
  the LED table above) to confirm the new role, and the board restarts a
  couple of seconds later to boot into that new role.
- **Practical consequence:** this flips the board between "regular node" and
  "master." Only do this deliberately — if this was your master (the one
  connected to your hub over USB), flipping it means it stops acting as the
  bridge to your server until you flip it back.
- There's no cancel gesture — if you let go before the full 5 seconds, it
  simply does nothing and you can try again.

### Reset button (factory wipe)

Wired to GPIO 25 by default (`RESET_BUTTON_PIN`). This erases **everything**
saved on the device — its role, its enrollment status, its paired devices,
its identity — so it's deliberately a two-step process to avoid triggering
it by accident.

1. **Hold the reset button for 5 seconds.** The red LED blinks 3 times. This
   **arms** the reset — nothing has been erased yet.
2. **Within the next 3 seconds, hold the reset button again for another full
   5 seconds.** Both LEDs blink 5 times together, and a few seconds later the
   device restarts as a completely blank, unenrolled node.
3. If you don't start (and complete) that second 5-second hold within the
   3-second window, the arming silently expires — nothing is erased, and you
   have to start over from step 1.

A simple way to think about the timing: **hold... count "one-Mississippi"
through "five-Mississippi"... let go (red blinks 3 times)... within 3
seconds, hold again... count to five again... let go (both LEDs blink 5
times, then it restarts).**

**Practical consequence:** this wipes your keys, your role, and every paired
device's MAC address. Your `master_pubkey_pin.h` configuration is unaffected
(that's compiled into the firmware, not stored on the device) — you do not
need to reflash the board — but you will need to re-enroll the node with
your hub afterward, as if it were brand new (it will print a fresh
`LATTICE_PUBKEY:...` line on its next boot).

## 12. Troubleshooting

**Build fails, error message mentions `master_pubkey_pin.h`.**
You skipped [Step 3](#5-step-3-generate-your-master-pubkey-pin-file). Run the
two commands in that section (generating a throwaway `masterkey.json`, then
running `tools/gen_master_pubkey_pin.py`) before building again.

**Build fails with errors about missing headers, or symbols from
`lattice-protocol`.**
You likely skipped the submodule step. From the repo root, run
`git submodule update --init --recursive`, confirm
`firmware/main/lib/lattice-protocol` is populated (see
[Step 2](#4-step-2-clone-the-repo-and-initialize-the-submodule)), and rebuild.

**`idf.py` says "command not found" (or similar).**
You need to load the ESP-IDF environment into your current terminal window
first: `. ~/esp/esp-idf/export.sh` on macOS/Linux, or `export.bat` on
Windows (or just open the "ESP-IDF 5.5.1 CMD" shortcut if you used the
Windows graphical installer). This has to be done again every time you open
a new terminal window.

**I can't find my serial port / the flash command can't connect.**
Re-check the per-OS instructions in [Step 6](#8-step-6-flash) for finding
the port name. If no new device shows up at all when you plug the board in,
your operating system may be missing a driver for the board's
USB-to-serial chip. Most ESP32 dev boards use either a **Silicon Labs
CP210x** or a **WCH CH340/CH341** USB-to-serial chip; if plugging in the
board doesn't produce a new serial device, search for and install the driver
matching your board's chip from the chip manufacturer's site (this repo
doesn't ship or require a specific driver — it depends entirely on which
board you bought). Also make sure you're using a USB cable that supports
data transfer, not a charge-only cable.

**Flashing succeeds, but I never see `LATTICE_PUBKEY:...` or anything else
in the monitor.**
Make sure `DEFAULT_LOG_LEVEL` in `project_config.h` wasn't left at something
other than intended, and that you're opening the monitor at the same port
you flashed to. If truly nothing prints at all and no LED lights either,
see the "Total hardware failure" row in the [LED table](#10-step-8-understanding-the-leds-and-display) —
double check your board is genuinely powered and your LED/board wiring
matches `project_config.h`'s pin numbers.

**My node won't enroll / stays showing `----` on the display forever.**
This almost always means the hub side hasn't approved it yet, or isn't
running. Confirm your `lattice-hub` server is running and reachable, and
that you gave it the exact 64-character `LATTICE_PUBKEY` value the node
printed on first boot (see [Step 7](#9-step-7-first-boot--provisioning)).
The approval process itself happens on the server —
[`docs/server_requirements.md`](server_requirements.md)'s "Enrollment
Protocol" section has the full technical flow if you're debugging the server
side.

## 13. What's next

- If you want to **contribute code** rather than just flash a board, see the
  [`README.md`](../README.md) Development section for how to build and run
  this repo's unit and end-to-end test suites.
- Building from source, generating your own pin file, and hand-editing
  `project_config.h` is admittedly a lot of steps for a first-time,
  non-technical setup. A simpler path — a pre-built release binary plus a
  one-click flashing tool that wouldn't require installing ESP-IDF or
  touching C++ source at all — is tracked as future work in
  [GitHub issue #101](https://github.com/superbrobenji/lattice-nodes/issues/101),
  "Pre-built release binary + simple flasher tool for non-technical setup."
  If that's the workflow you were hoping for, that issue is the place to
  watch (or contribute to).
