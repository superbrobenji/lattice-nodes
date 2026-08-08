# Phase C — Repo-Wide Sweep Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute Phase C of the clean-code-refactor umbrella — 12 audit findings across `main.cpp`, `EepromManager`, `Logger`, `ButtonHandler`, and 6 trivial dead-code/consistency fixes.

**Architecture:** Six independent work areas, each closing one or more audit findings. No new collaborator classes beyond what the design spec locks down (`persistence/eeprom/`'s 8 domain files). Every other change is in-place decomposition of an existing file.

**Tech Stack:** ESP-IDF C++17, GoogleTest (host unit tests), the existing e2e simulation harness (`tests/e2e/`).

## Global Constraints

- **Firmware-only. No wire-format changes. No backwards-compat shims.** Persisted NVS state may reset on reflash where a task touches it (none do — every persisted key name is unchanged).
- **Encapsulation yes, inheritance sparingly.**
- **Single responsibility per file.** Every file this plan creates or splits must have one clear reason to change — this is binding on every task, not just Task 3.
- **Library-caution clause** — not invoked by this plan; no new libraries are added or removed (Task 4 removes arduino-esp32, which is a dependency drop, not a swap-for-library).
- **Tiger Style preserved** — static allocation, WDT feeding, no new heap churn. Task 1 in particular must not reorder any boot-sequencing statement relative to its neighbors — ESP32 driver bring-up is order-sensitive (GPIO config before ISR service install, UART driver install before `Serial.begin()`, etc.) and the existing comments in `main.cpp` document *why* each ordering constraint exists. Decompose into functions along the existing contiguous blocks; do not hoist or reorder individual statements.
- **CMakeLists dual-registration.** Any new `.cpp` must be registered in BOTH `tests/CMakeLists.txt`'s `FIRMWARE_SOURCES`/`add_unit_test` list AND `firmware/main/CMakeLists.txt`'s `SRCS` list. This has been missed by every phase's final review so far (Phase A, Round 1, and it was explicitly guarded against in Round 2) — check both files, not just one.
- **clang-format:** use `/opt/homebrew/opt/llvm@18/bin/clang-format` (CI pins v18; local Homebrew is v22 and reformats differently).
- **Full unit + e2e regression required after every task.** `cmake --build tests/build --parallel 2` then `ctest --test-dir tests/build --output-on-failure --label-exclude e2e` and `--label-regex e2e` (parallelism capped at 2 — this machine OOMs above that).
- **Logger's UART migration (Task 4) reports its own before/after flash/RAM size delta**, separate from the rest of Phase C's size-neutral changes.
- **Finding 11's `LED_ADAPTER` removal is pre-verified safe** (Task 6): `adapter_types` has explicit enumerator values, so removing `LED_ADAPTER = 3` doesn't renumber the others, and nothing in nodes ever transmits value 3 (unreachable in `AdapterFactory::createAdapter()`'s switch). No cross-repo coordination needed.

## Sequencing

Run **Task 3 (EepromManager split) first, alone** — it touches the `#include` block of nearly every other file this plan modifies (`main.cpp`, `ButtonHandler.h`, `Adapter.cpp`, `Adapter.h`'s neighbors via `AdapterFactory.cpp`, `Mesh.cpp`, `Enrollment.cpp`, `PeerRegistry.cpp`, `BootManager.h`), so landing it before the others avoids a second wave of include churn and avoids same-file worktree conflicts.

After Task 3 lands:

```
Task 1 (main.cpp boot decomposition)
  -> Task 2 (enrollment-broadcast extraction, same file)
    -> Task 4 (Logger UART migration, touches main.cpp's initDrivers())
Task 5 (ButtonHandler dedup)          -- parallel-safe with the chain above
Task 6 (trivial batch)                -- parallel-safe with the chain above
```

Tasks 1→2→4 are sequential (same file, `main.cpp`). Tasks 5 and 6 touch disjoint files from the 1→2→4 chain and from each other once Task 3 has landed — safe for worktree parallelism.

---

### Task 1: `main.cpp` boot decomposition

**Files:**
- Modify: `firmware/main/main.cpp:234-579` (`app_main()`)

**Interfaces:**
- Produces: `static void initDrivers()`, `static void initHardwareOutputs()`, `static bool initSubsystems()` (returns `isMaster`), `static void spawnTasks(bool isMaster)`, `static void haltOnRedLedFailure(lattice::hardware::Led& indicator)` — all file-static in `main.cpp`, not declared in any header.

This task only regroups existing statements into named functions, in their exact original order — no statement is reordered, no logic changes. Each new function is a verbatim cut of a contiguous block of the current `app_main()`.

- [ ] **Step 1: Extract `initDrivers()`**

Cut `app_main()`'s NVS init through `Serial.begin(115200)` (current lines 240-322) into:

```cpp
static void initDrivers() {
  // Phase I Task 4: nvs_flash direct — initialize the NVS partition before
  // anything touches lattice::eeprom (which opens nvs_flash handles directly
  // instead of going through Arduino's Preferences wrapper). Erase-and-retry
  // on a stale/incompatible partition (fresh flash, or an NVS layout version
  // bump), matching the standard ESP-IDF boot idiom.
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  // Phase I Task 7 (QQ + RR): bundled gpio_config_t calls replace the
  // scattered per-component pinMode() calls that used to run inside each of
  // SevenSegDisplay::init()/Button::init()/GpioInput::init()/
  // GpioOutput::init() — those now only validate the pin + set _initialized.
  // Must run before any of those init() calls below, and gpio_install_isr_
  // service() must run before PirAdapter::init() (reached via adapter->init()
  // further down) calls Pir::attachInterrupt().
  gpio_config_t outCfg = {
      .pin_bit_mask = (1ULL << RED_LED_PIN) | (1ULL << GREEN_LED_PIN) |
                      (1ULL << lattice::config::SEVSEG_DATA_PIN) |
                      (1ULL << lattice::config::SEVSEG_CLK_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&outCfg));

  // Config/reset buttons: internal pull-DOWN (matches Button::init()'s prior
  // pinMode(_pin, INPUT_PULLDOWN) — line is LOW unless actively driven HIGH).
  gpio_config_t buttonInCfg = {
      .pin_bit_mask = (1ULL << CONFIG_BUTTON_PIN) | (1ULL << RESET_BUTTON_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&buttonInCfg));

  // PIR sensor: internal pull-UP (matches GpioInput::init()'s prior
  // pinMode(_pin, INPUT_PULLUP) default). Edge-interrupt type is armed
  // separately by Pir::attachInterrupt() once PirAdapter::init() runs, below.
  gpio_config_t pirInCfg = {
      .pin_bit_mask = (1ULL << lattice::adapter::PIR_ADAPTER_DEFAULT_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&pirInCfg));

  // Phase I Task 7: ISR service uses flags=0 (no ESP_INTR_FLAG_IRAM) because
  // the PIR ISR chain (Pir::isrTrampoline -> Pir::detectMotion -> Pir::signalMotion,
  // Pir::detachInterrupt) is only partially IRAM_ATTR'd — the trampolines are,
  // but the downstream methods reach gpio_isr_handler_remove and other non-IRAM
  // code paths. DO NOT change flags to ESP_INTR_FLAG_IRAM without first
  // completing the IRAM audit on the full chain — a flash-cache-disabled window
  // (e.g. during NVS commit) would crash the ISR.
  ESP_ERROR_CHECK(gpio_install_isr_service(0));

  // Phase I Task 5: uart_driver — install the native ESP-IDF UART driver for
  // UART_NUM_0 before anything touches Serial (Logger) or SerialAdapter.
  // arduino-esp32's Serial.begin() below detects the driver is already
  // installed (uart_is_driver_installed()) and skips re-installing it,
  // calling only uart_param_config() to apply matching settings — so Logger
  // (hand-rolled Serial.print path, intentionally left alone — see
  // Logger.cpp) and SerialAdapter (uart_read_bytes/uart_write_bytes) share
  // one underlying driver instance with no conflict. Baud rate here MUST
  // match Serial.begin(115200) below.
  uart_config_t uartCfg = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0)); // RX 1024, TX unbuffered
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uartCfg));

  // Phase I Task 10: with CONFIG_AUTOSTART_ARDUINO gone, nothing calls
  // arduino-esp32's initArduino() for us anymore — do it explicitly so
  // Logger.cpp's Serial.print/println/vprintf calls have a working
  // HardwareSerial underneath. Must run before Serial.begin() below.
  initArduino();
  Serial.begin(115200);
}
```

- [ ] **Step 2: Extract `haltOnRedLedFailure()` and `initHardwareOutputs()`**

`haltOnRedLedFailure` names the inlined failure loop (kept as boot-sequencing logic — the error-signaling itself depends on the LED that just failed, same reasoning the current inline comment gives):

```cpp
// Legitimate special case: the red LED itself just failed to initialize, so
// the only way to signal the failure is to try the green one and pump its
// non-blocking pulse() state machine inline (there is no other task running
// yet to do it). If neither LED works, halt silently.
static void haltOnRedLedFailure(lattice::hardware::Led& greenLed) {
  Logger::logln("MAIN", "FATAL: Failed to initialize red LED!", LogLevel::LOG_ERROR);
  if (greenLed.init()) {
    while (true) {
      greenLed.pulse(6, 100, 100);
      while (greenLed.isBusy()) {
        uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
        greenLed.update(nowMs);
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } else {
    Logger::logln("MAIN", "FATAL: No LEDs available. System halted.", LogLevel::LOG_ERROR);
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

static void initHardwareOutputs() {
  Logger::setLogLevel(lattice::config::DEFAULT_LOG_LEVEL);

  // Phase I Task 10: was a raw, manually-gated Serial.println("Lattice
  // Starting..."); LATTICE_LOGLN folds to nothing under LOG_NONE the same way
  // the old manual `if (DEFAULT_LOG_LEVEL != LOG_NONE)` guard did, so this is
  // behavior-preserving and removes one more direct Arduino Serial call site
  // from main.cpp — Logger.cpp remains the only file that touches Serial
  // directly.
  LATTICE_LOGLN("MAIN", "Lattice Starting...", LogLevel::LOG_INFO);

  // Check and log reset reason; escalate if WDT looping
  // Must init EEPROM before BootManager::check — saveRebootReason/saveRebootCount no-op if not
  // initialized
  lattice::eeprom::init();
  lattice::app::BootManager::check();

  Logger::logln("MAIN", "Logger initialized", LogLevel::LOG_INFO);

  Led::setSystemErrorLed(&redLed);

  if (!redLed.init()) {
    haltOnRedLedFailure(greenLed);
  }

  // Seven segment conditional init
  if (lattice::config::ENABLE_SEVSEG_DISPLAY) {
    sevenSeg.init();
    lattice::err_core::init(&redLed, &sevenSeg);
  } else {
    lattice::err_core::init(&redLed, nullptr);
  }

  if (!greenLed.isInitialized()) {
    if (!greenLed.init()) {
      Logger::logln("MAIN", "FATAL: Failed to initialize green LED!", LogLevel::LOG_ERROR);
      lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE,
                          1, "MAIN: Failed to initialize green LED");
    }
  }

  if (!configButton.init()) {
    Logger::error("Config button initialization failed!");
    lattice::err::fail(lattice::utils::ErrorType::HARDWARE_FAILURE, "Config button init failed!");
  }

  if (!resetButton.init()) {
    Logger::error("Reset button initialization failed!");
    lattice::err::fail(lattice::utils::ErrorType::HARDWARE_FAILURE, "Reset button init failed!");
  }

  // Initialize EEPROM Manager
  // (finding 18: this is the authoritative checked call — the earlier
  // lattice::eeprom::init() above is an unchecked early probe needed only so
  // BootManager::check()'s reboot-reason/-count calls aren't no-ops; init()
  // is idempotent, so calling it twice is safe.)
  if (!lattice::eeprom::init()) {
    Logger::logln("MAIN", "Failed to initialize EEPROM Manager", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::CORE, 2,
                        "EEPROM Manager init failed!");
  }
}
```

Note the added comment above the second `eeprom::init()` call — this is finding 18's fix, folded into this task per the design spec.

- [ ] **Step 3: Extract `initSubsystems()`**

Cut from `// Bluetooth disabled...` through the pubkey-print block and `mesh.setIsMaster(isMaster)`/adapter transmit fn/`mesh.linkDataRecvCallback` (current lines 402-534), returning `isMaster`:

```cpp
static bool initSubsystems() {
  // Bluetooth disabled via CONFIG_BT_ENABLED=n in sdkconfig

  // Check if we're in dev mode (compile-time constant takes precedence)
  isDevMode = DEV_MODE;
  if (!isDevMode) {
    // If not compile-time dev mode, check EEPROM
    isDevMode = lattice::eeprom::loadDevFlag();
  }

  // Phase I Task 7 (XX): String concat eliminated — Logger now takes
  // const char*, and both branches are fixed literals, so a ternary of two
  // string literals covers it with no snprintf/buffer needed.
  Logger::logln("MAIN", isDevMode ? "Running in DEV mode" : "Running in PRODUCTION mode",
                LogLevel::LOG_INFO);

  // Set dev mode in AdapterFactory and EEPROM Manager
  lattice::adapter::AdapterFactory::setDevMode(isDevMode);
  lattice::eeprom::setDevMode(isDevMode);

  // Declare peers to EEPROM (only if not in dev mode and EEPROM is empty)
  if (!isDevMode && !lattice::eeprom::hasPeers()) {
    // Write default peers to EEPROM
    lattice::eeprom::savePeerList(reinterpret_cast<const uint8_t*>(defaultPeerList),
                                  NUM_DEFAULT_PEERS);
    Logger::logln("MAIN", "Wrote default peer MACs to EEPROM.", LogLevel::LOG_INFO);
  }

  // Initialize EEPROM defaults if not set (only if not in dev mode)
  if (!isDevMode) {
    lattice::adapter::AdapterFactory::initializeDefaultsIfUnset();
  }

  // Create adapter (from EEPROM if production mode, or default if dev mode)
  if (isDevMode) {
    // In dev mode, create default adapter from config
    adapter.reset(lattice::adapter::AdapterFactory::createAdapter(
        lattice::config::DEFAULT_ADAPTER, lattice::adapter::AdapterFactory::getDefaultPinForAdapter(
                                              lattice::config::DEFAULT_ADAPTER)));
    Logger::logln("MAIN", "Created default adapter (DEV mode)", LogLevel::LOG_INFO);
  } else {
    // In production mode, create from EEPROM
    adapter.reset(lattice::adapter::AdapterFactory::createFromEEPROM());
    Logger::logln("MAIN", "Created adapter from EEPROM (PRODUCTION mode)", LogLevel::LOG_INFO);
  }

  if (!adapter) {
    Logger::logln("MAIN", "Failed to create adapter", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE,
                        3, "MAIN: Failed to create PIR adapter");
  }
  Logger::logln("MAIN", "Adapter created", LogLevel::LOG_INFO);

  if (!adapter->init()) {
    Logger::logln("MAIN", "Adapter failed to initialize", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE,
                        4, "MAIN: Adapter failed to initialize");
  }
  Logger::logln("MAIN", "Adapter initialized", LogLevel::LOG_INFO);

  // Phase I Task 3 (BB + ZZ): mesh.init() below owns the raw esp_netif_init()
  // + esp_event_loop_create_default() + esp_wifi_init() +
  // esp_wifi_set_mode(WIFI_MODE_STA) + esp_wifi_start() sequence that used to
  // be WiFi.mode(WIFI_STA) (see Mesh.cpp:init()) — no separate call needed
  // here.
  if (!mesh.init()) {
    Logger::logln("MAIN", "Mesh init failed", LogLevel::LOG_ERROR);
    lattice::err::fatal(lattice::core::ErrorTypeDigit::COMM, lattice::core::ModuleDigit::MESH, 1,
                        "MAIN: Mesh init failed — cannot operate without mesh");
  }

  mesh.setEnrollmentRelayFn(SerialAdapter::relayEnrollmentToServer);

  // Nodes must always receive — modem sleep drops ESP-NOW packets without AP sync
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (isDevMode) {
    mesh.debugDumpRadio();
  }

  // Print device public key for provisioning (admin copies this to server).
  // Only printed when not yet enrolled — enrolled nodes have already been
  // provisioned. The private key is NEVER printed — only the public key is
  // output here.
  //
  // Phase I Task 10: was Serial.print()/Serial.print(byte, HEX) — this
  // output is provisioning protocol, not a log line, so it must NOT go
  // through LATTICE_LOG* (which folds away entirely under the production
  // LOG_NONE default). Rewritten onto the native uart_write_bytes() path
  // that SerialAdapter already uses (Task 5) — one fewer thing routed
  // through Arduino's Serial wrapper, and it shares the same
  // already-installed UART_NUM_0 driver.
  if (!mesh.isEnrolled()) {
    const uint8_t* pubKey = mesh.getDevicePublicKey();
    char pubKeyLine[16 + 64 + 2]; // "LATTICE_PUBKEY:" + 64 hex chars + "\n" + NUL
    int len = snprintf(pubKeyLine, sizeof(pubKeyLine), "LATTICE_PUBKEY:");
    for (int i = 0; i < 32 && len < static_cast<int>(sizeof(pubKeyLine)); ++i) {
      len += snprintf(pubKeyLine + len, sizeof(pubKeyLine) - len, "%02X", pubKey[i]);
    }
    if (len < static_cast<int>(sizeof(pubKeyLine))) {
      len += snprintf(pubKeyLine + len, sizeof(pubKeyLine) - len, "\n");
    }
    uart_write_bytes(UART_NUM_0, pubKeyLine, len);
    Logger::logln("MAIN", "Public key printed to serial for provisioning", LogLevel::LOG_INFO);
  }

  bool isMaster;
  if (isDevMode) {
    isMaster = devMasterFlag;
    Logger::logln("MAIN", isMaster ? "DEV mode: starting as MASTER" : "DEV mode: starting as NODE",
                  LogLevel::LOG_INFO);
  } else {
    // In production mode, load from EEPROM
    isMaster = lattice::eeprom::loadMasterFlag();
  }

  mesh.setIsMaster(isMaster);
  Logger::logln("MESH", "Mesh initialized", LogLevel::LOG_INFO);
  Logger::logln("MAIN", isMaster ? "Booted as: MASTER" : "Booted as: NODE", LogLevel::LOG_INFO);

  adapter->setTransmitFn(&lattice::mesh::Mesh::transmit);

  mesh.linkDataRecvCallback(dataRecvCallback);

  return isMaster;
}
```

- [ ] **Step 4: Extract `spawnTasks()`**

Cut the mesh-drain-task creation through `esp_pm_configure()` (current lines 472-574):

```cpp
static void spawnTasks(bool isMaster) {
  // Phase I Task 9 (item EE): create the dedicated mesh-drain task and hand
  // its handle to Mesh so the RX-ISR trampoline (MeshTransport.cpp's
  // onDataRecvCallback) can wake it via vTaskNotifyGiveFromISR. Pinned to
  // core 0 alongside the WiFi/ESP-NOW stack that delivers the RX callback;
  // priority above tskIDLE_PRIORITY so it preempts idle immediately on
  // notify, but well below time-critical ISR-adjacent work.
  mesh_task_handle = xTaskCreateStaticPinnedToCore(
      mesh_task_fn, "mesh", sizeof(mesh_task_stack) / sizeof(StackType_t), NULL,
      tskIDLE_PRIORITY + 3, mesh_task_stack, &mesh_task_tcb, 0);
  mesh.setDrainNotifyHandle(mesh_task_handle);

  // Configure task watchdog: 10-second timeout. Registration
  // (esp_task_wdt_add) happens inside housekeeping_task_fn itself, below —
  // app_main()'s own task is transient and returns once boot completes, so it
  // cannot be the long-lived registrant the old Arduino loop task used to be.
  esp_task_wdt_config_t wdtConfig = {
      .timeout_ms = 10000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtConfig);

  // Validate configuration for potential server communication issues
  validateServerConfiguration();

  // Phase I Task 10: housekeeping task absorbs the entire old loop() body
  // (adapter->loop() + DisplayManager::tick() + ButtonHandler::tick(), plus
  // everything else loop() used to run every iteration — see
  // housekeeping_task_fn's comment above). Static stack + TCB, no heap.
  xTaskCreateStaticPinnedToCore(
      housekeeping_task_fn, "hk", sizeof(housekeeping_stack) / sizeof(StackType_t), NULL,
      tskIDLE_PRIORITY + 2, housekeeping_stack, &housekeeping_tcb, tskNO_AFFINITY);

  // Phase I Task 9 (item EE) / Task 10: esp_pm_configure() is now the LAST
  // call in app_main(), made once every subsystem (GPIO, UART, NVS, radio,
  // adapter, mesh, both tasks) is fully up, rather than "as early as
  // possible" (Task 9's original placement, right after nvs_flash_init()).
  // This also folds in what used to be the separate, later
  // `if (!isMaster) setCpuFrequencyMhz(80)` arduino-esp32 call (Phase-G-era):
  // that call is gone entirely now — a leaf simply gets a pinned 80/80 DFS
  // range instead of master's dynamic 80-240 range, in one native
  // esp_pm_configure() call instead of two (one native, one
  // arduino-esp32-internal) that could otherwise fight each other (see Task
  // 9 report's "Concerns" #2).
  esp_pm_config_t pm_cfg = {
      .max_freq_mhz = isMaster ? 240 : 80,
      .min_freq_mhz = 80,
      .light_sleep_enable = true,
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
}
```

- [ ] **Step 5: Rewrite `app_main()` as the 4-call sequence**

```cpp
extern "C" void app_main(void) {
  initDrivers();
  initHardwareOutputs();
  bool isMaster = initSubsystems();
  spawnTasks(isMaster);
  // app_main() returns here; the FreeRTOS scheduler owns the runtime from
  // this point on, via the mesh-drain task and housekeeping task spawned
  // above.
}
```

- [ ] **Step 6: Build and test**

Run: `cmake --build tests/build --parallel 2 && ctest --test-dir tests/build --output-on-failure --label-exclude e2e && ctest --test-dir tests/build --output-on-failure --label-regex e2e`
Expected: same counts as before this task (this is a pure regrouping — no behavior or test-surface change). `main.cpp` is not exercised by host tests directly (it's the ESP-IDF entry point), so the real signal here is that everything else still builds and passes; also visually diff the reconstructed `app_main()` call sequence against the step-by-step cut above to confirm no statement was dropped or reordered.

- [ ] **Step 7: Commit**

```bash
git add firmware/main/main.cpp
git commit -m "refactor(phaseC): decompose app_main() into boot-phase functions"
```

---

### Task 2: `main.cpp` enrollment-broadcast extraction

**Files:**
- Modify: `firmware/main/src/mesh/Mesh.h`, `firmware/main/src/mesh/Mesh.cpp`, `firmware/main/main.cpp`
- Test: `tests/unit/test_mesh_logic.cpp`

**Interfaces:**
- Consumes: `Mesh::isEnrolled()`, `Mesh::getIsMaster()`, `Mesh::sendEnrollmentRequest()` (all already exist on `Mesh`).
- Produces: `bool Mesh::tickEnrollmentBroadcast(uint64_t nowMs)` — returns `true` if data-forwarding should be skipped this tick (mirrors `housekeeping_task_fn`'s current `skipDataForwarding` semantics exactly).

- [ ] **Step 1: Add the method to `Mesh.h`**

Add to the public section (near `sendEnrollmentRequest`/`isEnrolled`):

```cpp
// Periodic re-broadcast of this node's enrollment request while unenrolled
// (every 10s). Returns true if data-forwarding should be skipped this tick
// (not yet enrolled and not master) — callers use this to gate the rest of
// their per-tick work, same as the old inline housekeeping_task_fn logic.
bool tickEnrollmentBroadcast(uint64_t nowMs);
```

Add to the private section:

```cpp
uint64_t lastEnrollmentBroadcastMs_ = 0;
```

- [ ] **Step 2: Implement in `Mesh.cpp`**

```cpp
bool Mesh::tickEnrollmentBroadcast(uint64_t nowMs) {
  if (isEnrolled() || getIsMaster()) {
    return false;
  }
  if (nowMs - lastEnrollmentBroadcastMs_ > 10000) {
    lastEnrollmentBroadcastMs_ = nowMs;
    sendEnrollmentRequest();
    Logger::logln("MAIN", "Enrollment request sent (awaiting server approval)", LogLevel::LOG_INFO);
  }
  return true;
}
```

(`Mesh.cpp` already includes `Logger.h` and uses `Logger::logln`/`LogLevel` elsewhere — no new include needed. Confirm this before writing the method; if for some reason it doesn't, add `#include "src/logging/Logger.h"`.)

- [ ] **Step 3: Write the test**

Add to `tests/unit/test_mesh_logic.cpp` (find the existing `MeshTest`-style fixture that constructs a `Mesh` and controls `getIsMaster()`/enrollment state — reuse it):

```cpp
TEST_F(MeshTest, TickEnrollmentBroadcast_ReturnsFalseWhenMaster) {
  mesh.setIsMaster(true);
  EXPECT_FALSE(mesh.tickEnrollmentBroadcast(1000));
}

TEST_F(MeshTest, TickEnrollmentBroadcast_ReturnsFalseWhenEnrolled) {
  mesh.setIsMaster(false);
  // Use the fixture's existing enrollment-forcing helper if one exists
  // (grep the file for how other tests get mesh.isEnrolled() == true); if
  // none exists, drive it through the real JOIN_ACK path already used by
  // other enrollment tests in this file rather than adding a test-only
  // backdoor.
  ASSERT_TRUE(mesh.isEnrolled());
  EXPECT_FALSE(mesh.tickEnrollmentBroadcast(1000));
}

TEST_F(MeshTest, TickEnrollmentBroadcast_ReturnsTrueAndBroadcastsWhenNeitherMasterNorEnrolled) {
  mesh.setIsMaster(false);
  ASSERT_FALSE(mesh.isEnrolled());
  EXPECT_TRUE(mesh.tickEnrollmentBroadcast(1000));
}

TEST_F(MeshTest, TickEnrollmentBroadcast_RespectsTenSecondInterval) {
  mesh.setIsMaster(false);
  ASSERT_FALSE(mesh.isEnrolled());
  EXPECT_TRUE(mesh.tickEnrollmentBroadcast(1000));
  // Second call inside the 10s window still returns true (skip forwarding)
  // but must not re-broadcast — verify via whatever observable the file's
  // other sendEnrollmentRequest tests already use (e.g. a sent-message
  // counter on the test transport double).
  EXPECT_TRUE(mesh.tickEnrollmentBroadcast(5000));
  EXPECT_TRUE(mesh.tickEnrollmentBroadcast(11001)); // > 1000 + 10000, re-broadcasts
}
```

If `test_mesh_logic.cpp`'s fixture doesn't expose a way to count `sendEnrollmentRequest` calls or force enrollment, read the fixture first and adapt these sketches to its actual helpers rather than adding new test-only hooks to `Mesh` — this file already has extensive enrollment/master-state test coverage to model from.

- [ ] **Step 4: Rewire `main.cpp`**

Replace `housekeeping_task_fn`'s enrollment-broadcast block (the `static uint64_t lastEnrollmentBroadcast = 0; ... skipDataForwarding = true; }` block, currently lines 168-178) with:

```cpp
    bool skipDataForwarding = mesh.tickEnrollmentBroadcast(nowMs);
```

- [ ] **Step 5: Build and test**

Run the full unit + e2e suite. Expected: prior count + 4 new unit tests, e2e unchanged (this is a pure behavior-preserving move — the e2e suite's enrollment-broadcast-dependent scenarios, if any, must still pass identically).

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/mesh/Mesh.h firmware/main/src/mesh/Mesh.cpp firmware/main/main.cpp tests/unit/test_mesh_logic.cpp
git commit -m "refactor(phaseC): extract enrollment-broadcast tick into Mesh::tickEnrollmentBroadcast"
```

---

### Task 3: `EepromManager` split into `persistence/eeprom/`

**Files:**
- Create: `firmware/main/src/persistence/eeprom/EepromCore.{h,cpp}`, `EepromIdentity.{h,cpp}`, `EepromRole.{h,cpp}`, `EepromSecurity.{h,cpp}`, `EepromPeers.{h,cpp}`, `EepromDiagnostics.{h,cpp}`, `EepromEnrollment.{h,cpp}`, `EepromDeviceConfig.{h,cpp}` (8 pairs, 16 files)
- Delete: `firmware/main/src/persistence/EepromManager.{h,cpp}`
- Modify (include lines + `eeprom::` call sites unchanged, only which header is included): `firmware/main/main.cpp`, `firmware/main/src/mesh/Mesh.cpp`, `firmware/main/src/mesh/Enrollment.cpp`, `firmware/main/src/mesh/PeerRegistry.cpp`, `firmware/main/src/adapter/Adapter.cpp`, `firmware/main/src/adapter/AdapterFactory.cpp`, `firmware/main/src/app/ButtonHandler.h`, `firmware/main/src/app/BootManager.h`
- Modify: `firmware/main/CMakeLists.txt`, `tests/CMakeLists.txt`
- Split: `tests/unit/test_eeprom_manager.cpp` into 8 new test files (delete the original)
- Check (may need include swap): `tests/e2e/harness/NodeContext.{h,cpp}`, `tests/e2e/harness/SimNode.{h,cpp}`, `tests/e2e/scenarios/test_harness_smoke.cpp`, `tests/e2e/scenarios/test_seq_wrap.cpp`, `tests/unit/test_pir_adapter.cpp` — these reference `EepromManager`/`eeprom::` today; find each reference and point it at the new domain header(s) it actually needs.

**Interfaces:** Every function keeps its exact name, signature, and `lattice::eeprom::` namespace — this task moves code between files, it does not rename or re-namespace anything. `NVS_KEYS`, `EEPROM_SIZES`, and `detail::State`/`detail::debugStateForTest()` move to `EepromCore.h` (still `lattice::utils::NVS_KEYS`/`lattice::utils::EEPROM_SIZES`/`lattice::eeprom::detail::State` — same fully-qualified names, new file location).

- [ ] **Step 1: Create `EepromCore.h`**

```cpp
#ifndef LATTICE_EEPROM_CORE_H
#define LATTICE_EEPROM_CORE_H

#include <cstdint>
#include <cstddef>
#include <nvs.h>
#include <nvs_flash.h>
#include "src/logging/Logger.h"
#include "../../../project_config.h"

namespace lattice {
namespace utils {

namespace NVS_KEYS {
constexpr const char* NAMESPACE = "lattice";
constexpr const char* MASTER_FLAG = "master";
constexpr const char* DEV_FLAG = "dev";
constexpr const char* ADAPTER_TYPE = "adapter";
constexpr const char* MESH_KEY = "meshkey";
// Phase I Task 6 (JJ): retired — the peer list moved from this single combined
// blob to per-record "peer0".."peer9" keys (built at runtime by
// EepromPeers.cpp's peerKey() helper) so PeerRegistry can load/save one
// record at a time instead of a 380-byte stack buffer. Kept (unused) as a
// migration/rollback reference, not read or written anywhere anymore.
constexpr const char* PEER_LIST = "peers";
constexpr const char* REBOOT_REASON = "rbt_reason";
constexpr const char* REBOOT_COUNT = "rbt_count";
constexpr const char* PRIVATE_KEY = "privkey";
constexpr const char* PUBLIC_KEY = "pubkey";
constexpr const char* KEYPAIR_CRC = "kp_crc";
constexpr const char* ENROLLED_FLAG = "enrolled";
constexpr const char* BOOT_EPOCH = "epoch";
constexpr const char* KNOWN_MASTER_MAC = "master_mac";
constexpr const char* KNOWN_MASTER_MAC_SEC = "master_mac2";
constexpr const char* TX_POWER_PRESET = "txpower";
constexpr const char* NODE_ID = "node_id";
} // namespace NVS_KEYS

namespace EEPROM_SIZES {
constexpr uint8_t MESH_KEY_SIZE = 16;
constexpr uint8_t MAX_PEERS = 10;
constexpr uint8_t PEER_MAC_SIZE = 6;
constexpr uint8_t PEER_PUBLIC_KEY_SIZE = 32;
constexpr uint8_t PEER_RECORD_SIZE = PEER_MAC_SIZE + PEER_PUBLIC_KEY_SIZE; // 38 bytes
constexpr uint16_t PEER_LIST_SIZE = MAX_PEERS * PEER_RECORD_SIZE;          // 380 bytes
} // namespace EEPROM_SIZES

} // namespace utils
} // namespace lattice

// Phase C: EepromManager split into persistence/eeprom/ domain files (audit
// finding 4). This header is the internal KV-primitive layer — the other
// eeprom/*.cpp files include it; consumers outside persistence/eeprom/ never
// should (each includes only the domain header(s) it actually calls into,
// which is the fix for "any consumer can reach any persistence function").
namespace lattice {
namespace eeprom {

namespace detail {
// Groups the module's mutable state into one struct so the e2e test harness
// (tests/e2e/harness/NodeContext.cpp) can still snapshot/restore it as a flat
// byte image per simulated node -- the same technique it used against the
// old singleton object.
struct State {
  bool isInitialized = false;
  bool isDevMode = false;
  uint32_t devEpoch = 0; // DEV_MODE RAM-only monotonic boot-epoch seed (issue #43)
};
} // namespace detail

bool init();
void setDevMode(bool devMode);
bool getDevMode();
inline void flushIfDirty() {}
inline void forceFlush() {}
void clearAll();
void dumpEEPROM();

#ifdef UNIT_TEST
bool isInitializedForTest();
detail::State& debugStateForTest();
#endif

namespace core_internal {
// Shared by every eeprom/*.cpp domain file. Not part of the public
// lattice::eeprom API — declared here so those files can call into it
// without each re-declaring its own copy.
bool ensureInitialized();
void logOperation(const char* operation, const char* details = nullptr);
uint16_t crc16(const uint8_t* data, size_t len);
bool persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant);
uint8_t nvsGetU8(const char* key, uint8_t defaultValue);
size_t nvsPutU8(const char* key, uint8_t value);
uint32_t nvsGetU32(const char* key, uint32_t defaultValue);
size_t nvsPutU32(const char* key, uint32_t value);
bool nvsGetBool(const char* key, bool defaultValue);
size_t nvsPutBool(const char* key, bool value);
size_t nvsGetBytes(const char* key, void* buf, size_t maxLen);
size_t nvsPutBytes(const char* key, const void* buf, size_t len);
bool nvsRemove(const char* key);
bool nvsHasKey(const char* key);
void peerKey(uint8_t index, char* out, size_t outSize);
bool isDevModeInternal(); // domain files need this without calling public getDevMode() semantics
} // namespace core_internal

} // namespace eeprom
} // namespace lattice

#endif // LATTICE_EEPROM_CORE_H
```

Note: `core_internal` functions were `anonymous namespace`-private to the single old `.cpp` file; splitting across files requires them to be declared (not anonymous) so sibling `.cpp` files can call them, while staying out of the public `lattice::eeprom` surface other consumers use. `isDevModeInternal()` is new — a tiny accessor domain `.cpp` files use instead of reaching into `detail::State` directly (which stays private to `EepromCore.cpp`).

- [ ] **Step 2: Create `EepromCore.cpp`**

```cpp
#include "EepromCore.h"
#include "src/error/Error.h"
#include <cstdio>
#include <cstring>

namespace lattice {
namespace eeprom {

using namespace lattice::utils;

namespace {
detail::State _state;
} // namespace

namespace core_internal {

bool ensureInitialized() {
  if (!_state.isInitialized) {
    LATTICE_LOGLN("NVS", "NVS not initialized", lattice::utils::LogLevel::LOG_ERROR);
    return false;
  }
  return true;
}

void logOperation(const char* operation, const char* details) {
  if (details) {
    LATTICE_LOGF("NVS", lattice::utils::LogLevel::LOG_DEBUG, "%s: %s", operation, details);
  } else {
    LATTICE_LOGLN("NVS", operation, lattice::utils::LogLevel::LOG_DEBUG);
  }
}

uint16_t crc16(const uint8_t* data, size_t len) {
  // Phase I Task 4 (opportunistic UU): NOT swapped to esp_rom_crc16_le — see
  // Phase A audit finding 25 for the re-confirmed reasoning (not a bit-exact
  // match without an unverified transform; value is self-referential only).
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int j = 0; j < 8; ++j)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

bool persistOrEscalate(const char* key, size_t got, size_t want, bool securityRelevant) {
  if (got == want) {
    return true;
  }
  if (securityRelevant) {
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 5,
                       "NVS write failed (security-relevant key)");
    return false; // unreachable outside UNIT_TEST
  }
  LATTICE_LOGF("NVS", lattice::utils::LogLevel::LOG_ERROR, "write failed key=%s got=%u want=%u",
               key, (unsigned)got, (unsigned)want);
  return false;
}

esp_err_t nvsOpenRW(nvs_handle_t& handle) {
  return nvs_open(NVS_KEYS::NAMESPACE, NVS_READWRITE, &handle);
}

uint8_t nvsGetU8(const char* key, uint8_t defaultValue) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return defaultValue;
  }
  uint8_t value = defaultValue;
  esp_err_t err = nvs_get_u8(h, key, &value);
  nvs_close(h);
  return (err == ESP_OK) ? value : defaultValue;
}

size_t nvsPutU8(const char* key, uint8_t value) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return 0;
  }
  err = nvs_set_u8(h, key, value);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return (err == ESP_OK) ? sizeof(uint8_t) : 0;
}

uint32_t nvsGetU32(const char* key, uint32_t defaultValue) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return defaultValue;
  }
  uint32_t value = defaultValue;
  esp_err_t err = nvs_get_u32(h, key, &value);
  nvs_close(h);
  return (err == ESP_OK) ? value : defaultValue;
}

size_t nvsPutU32(const char* key, uint32_t value) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return 0;
  }
  err = nvs_set_u32(h, key, value);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return (err == ESP_OK) ? sizeof(uint32_t) : 0;
}

bool nvsGetBool(const char* key, bool defaultValue) {
  return nvsGetU8(key, defaultValue ? 1 : 0) != 0;
}

size_t nvsPutBool(const char* key, bool value) {
  return nvsPutU8(key, value ? 1 : 0);
}

size_t nvsGetBytes(const char* key, void* buf, size_t maxLen) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return 0;
  }
  size_t len = maxLen;
  esp_err_t err = nvs_get_blob(h, key, buf, &len);
  nvs_close(h);
  return (err == ESP_OK) ? len : 0;
}

size_t nvsPutBytes(const char* key, const void* buf, size_t len) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return 0;
  }
  err = nvs_set_blob(h, key, buf, len);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return (err == ESP_OK) ? len : 0;
}

bool nvsRemove(const char* key) {
  nvs_handle_t h;
  esp_err_t err = nvsOpenRW(h);
  if (err != ESP_OK) {
    return false;
  }
  err = nvs_erase_key(h, key);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err == ESP_OK;
}

bool nvsHasKey(const char* key) {
  nvs_handle_t h;
  if (nvsOpenRW(h) != ESP_OK) {
    return false;
  }
  size_t size = 0;
  esp_err_t err = nvs_get_blob(h, key, nullptr, &size);
  nvs_close(h);
  return err == ESP_OK;
}

void peerKey(uint8_t index, char* out, size_t outSize) {
  snprintf(out, outSize, "peer%u", static_cast<unsigned>(index));
}

bool isDevModeInternal() {
  return _state.isDevMode;
}

} // namespace core_internal

namespace {
void nvsClearAll() {
  nvs_handle_t h;
  esp_err_t err = core_internal::nvsOpenRW(h);
  if (err != ESP_OK) {
    return;
  }
  err = nvs_erase_all(h);
  if (err == ESP_OK) {
    nvs_commit(h);
  }
  nvs_close(h);
}
} // namespace

bool init() {
  if (_state.isInitialized)
    return true;
  nvs_handle_t probe;
  esp_err_t err = nvs_open(NVS_KEYS::NAMESPACE, NVS_READWRITE, &probe);
  if (err != ESP_OK) {
    LATTICE_LOGLN("NVS", "Failed to open NVS namespace", lattice::utils::LogLevel::LOG_ERROR);
    lattice::err::fail(lattice::core::ErrorTypeDigit::MEMORY, lattice::core::ModuleDigit::EEPROM, 1,
                       "EepromManager: NVS begin failed");
    return false;
  }
  nvs_close(probe);
  _state.isInitialized = true;

  uint8_t reason = core_internal::nvsGetU8(NVS_KEYS::REBOOT_REASON, 0xFF);
  if (reason == 0x00) {
    size_t n = core_internal::nvsPutU8(NVS_KEYS::REBOOT_REASON, 0xFF);
    core_internal::persistOrEscalate(NVS_KEYS::REBOOT_REASON, n, sizeof(uint8_t), false);
  }
  uint8_t count = core_internal::nvsGetU8(NVS_KEYS::REBOOT_COUNT, 0);
  if (count > 10) {
    size_t n = core_internal::nvsPutU8(NVS_KEYS::REBOOT_COUNT, 0);
    core_internal::persistOrEscalate(NVS_KEYS::REBOOT_COUNT, n, sizeof(uint8_t), false);
  }

  core_internal::logOperation("Initialized", "NVS ready");
  return true;
}

void setDevMode(bool devMode) {
  _state.isDevMode = devMode;
  core_internal::logOperation("Dev mode set",
                              devMode ? "Development mode enabled" : "Production mode enabled");
}

bool getDevMode() {
  return _state.isDevMode;
}

void clearAll() {
  if (!core_internal::ensureInitialized())
    return;
  nvsClearAll();
  core_internal::logOperation("All NVS cleared");
}

void dumpEEPROM() {
  LATTICE_LOGLN("NVS", "NVS dump not implemented (use idf.py nvs-dump)",
                lattice::utils::LogLevel::LOG_INFO);
}

#ifdef UNIT_TEST
bool isInitializedForTest() {
  return _state.isInitialized;
}

detail::State& debugStateForTest() {
  return _state;
}
#endif

} // namespace eeprom
} // namespace lattice
```

Also add `nvsOpenRW` to `EepromCore.h`'s `core_internal` declarations (used by `nvsClearAll` above and every domain file's own get/put calls do NOT need it directly — only the `nvsGet*`/`nvsPut*` helpers use it internally, so it can stay declared in the header for `EepromCore.cpp`'s own use only if no domain file calls it directly; check this while writing domain files in Step 3 and add the declaration to the header only if a domain file needs it — none of the mappings below do, since every domain file goes through `core_internal::nvsGetU8`/`nvsPutBytes`/etc., not `nvsOpenRW` directly).

- [ ] **Step 3: Create the 7 domain files**

Each domain `.cpp` follows the same shape: `#include "EepromDomainName.h"`, `#include "EepromCore.h"`, `namespace lattice { namespace eeprom {`, functions calling `core_internal::*` and `lattice::utils::NVS_KEYS::*`/`EEPROM_SIZES::*` instead of the old file's private helpers, using `core_internal::isDevModeInternal()` wherever the old code read `_state.isDevMode` directly. Header files declare just that domain's public functions, `#include "EepromCore.h"` is NOT needed in headers unless a signature needs a type from it (none do beyond fundamental types already in `<cstdint>`).

**`EepromIdentity.h` / `.cpp`** — `loadKeypair`/`saveKeypair`, `loadNodeId`/`saveNodeId`:

```cpp
// EepromIdentity.h
#ifndef LATTICE_EEPROM_IDENTITY_H
#define LATTICE_EEPROM_IDENTITY_H
#include <cstdint>
namespace lattice {
namespace eeprom {
bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32);
void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32);
uint8_t loadNodeId();
void saveNodeId(uint8_t nodeId);
} // namespace eeprom
} // namespace lattice
#endif
```

```cpp
// EepromIdentity.cpp
#include "EepromIdentity.h"
#include "EepromCore.h"
#include <cstring>

namespace lattice {
namespace eeprom {
using namespace lattice::utils;

bool loadKeypair(uint8_t* privateKey32, uint8_t* publicKey32) {
  if (!core_internal::ensureInitialized())
    return false;
  size_t privRead = core_internal::nvsGetBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  size_t pubRead = core_internal::nvsGetBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  if (privRead != 32 || pubRead != 32)
    return false;
  uint32_t stored = core_internal::nvsGetU32(NVS_KEYS::KEYPAIR_CRC, 0xFFFFFFFF);
  if (stored == 0xFFFFFFFF)
    return false;
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t computed = core_internal::crc16(both, 64);
  if (static_cast<uint16_t>(stored) != computed) {
    LATTICE_LOGLN("NVS", "Keypair CRC mismatch", lattice::utils::LogLevel::LOG_WARN);
    return false;
  }
  return true;
}

void saveKeypair(const uint8_t* privateKey32, const uint8_t* publicKey32) {
  if (!core_internal::ensureInitialized() || core_internal::isDevModeInternal())
    return;
  size_t nPriv = core_internal::nvsPutBytes(NVS_KEYS::PRIVATE_KEY, privateKey32, 32);
  core_internal::persistOrEscalate(NVS_KEYS::PRIVATE_KEY, nPriv, 32, true);
  size_t nPub = core_internal::nvsPutBytes(NVS_KEYS::PUBLIC_KEY, publicKey32, 32);
  core_internal::persistOrEscalate(NVS_KEYS::PUBLIC_KEY, nPub, 32, true);
  uint8_t both[64];
  memcpy(both, privateKey32, 32);
  memcpy(both + 32, publicKey32, 32);
  uint16_t crc = core_internal::crc16(both, 64);
  size_t nCrc = core_internal::nvsPutU32(NVS_KEYS::KEYPAIR_CRC, static_cast<uint32_t>(crc));
  core_internal::persistOrEscalate(NVS_KEYS::KEYPAIR_CRC, nCrc, sizeof(uint32_t), true);
  core_internal::logOperation("Keypair saved");
}

uint8_t loadNodeId() {
  if (!core_internal::ensureInitialized())
    return 0;
  return core_internal::nvsGetU8(NVS_KEYS::NODE_ID, 0);
}

void saveNodeId(uint8_t nodeId) {
  if (!core_internal::ensureInitialized())
    return;
  size_t n = core_internal::nvsPutU8(NVS_KEYS::NODE_ID, nodeId);
  core_internal::persistOrEscalate(NVS_KEYS::NODE_ID, n, sizeof(uint8_t), false);
  core_internal::logOperation("saveNodeId");
}

} // namespace eeprom
} // namespace lattice
```

**`EepromRole.h` / `.cpp`** — `loadMasterFlag`/`saveMasterFlag`, `loadDevFlag`/`saveDevFlag`. Header declares the 4 functions; `.cpp` moves their bodies verbatim from the old `EepromManager.cpp` (lines 274-299 in the original — quoted in Task 3's research above), replacing `ensureInitialized()`/`_state.isDevMode`/`nvsGetBool`/`nvsPutBool`/`logOperation`/`persistOrEscalate` with their `core_internal::` equivalents exactly as `EepromIdentity.cpp` above demonstrates. Same transformation pattern for every remaining domain — apply it identically.

**`EepromSecurity.h` / `.cpp`** — `loadMeshKey`/`saveMeshKey`, `loadKnownMasterMac`/`saveKnownMasterMac`/`clearKnownMasterMac`, `loadKnownMasterMacSecondary`/`saveKnownMasterMacSecondary`/`clearKnownMasterMacSecondary` (original lines 301-318, 536-595).

**`EepromPeers.h` / `.cpp`** — `loadPeerList`/`savePeerList`/`hasPeers`/`clearPeerList`, `loadPeerRecord`/`savePeerRecord`/`erasePeerRecord` (original lines 325-424). Uses `core_internal::peerKey()`.

**`EepromDiagnostics.h` / `.cpp`** — `loadRebootCount`/`saveRebootCount`/`saveRebootReason`/`loadRebootReason`, `loadBootEpoch`/`saveBootEpoch` (original lines 439-463, 517-534). `saveBootEpoch`/`loadBootEpoch`'s dev-mode branch reads/writes `_state.devEpoch` in the old code — this field stays in `EepromCore.h`'s `detail::State`, so add `core_internal::devEpochRef()` returning `uint32_t&` to `EepromCore.h`/`.cpp` (mirrors `isDevModeInternal()`) for `EepromDiagnostics.cpp` to use instead of touching `_state` directly.

**`EepromEnrollment.h` / `.cpp`** — `loadEnrolledFlag`/`saveEnrolledFlag` (original lines 504-515).

**`EepromDeviceConfig.h` / `.cpp`** — `loadAdapterType`/`saveAdapterType`, `loadTxPowerPreset`/`saveTxPowerPreset` (original lines 426-437, 597-612).

- [ ] **Step 4: Delete the old `EepromManager.{h,cpp}`**

```bash
git rm firmware/main/src/persistence/EepromManager.h firmware/main/src/persistence/EepromManager.cpp
```

- [ ] **Step 5: Update every consumer's `#include`**

Based on the grep of `eeprom::` call sites (verified against the tree):

| File | Old include | New includes |
|---|---|---|
| `firmware/main/main.cpp` | `src/persistence/EepromManager.h` | `EepromCore.h`, `EepromIdentity.h`, `EepromRole.h`, `EepromPeers.h` |
| `firmware/main/src/mesh/Mesh.cpp` | (same) | `EepromCore.h`, `EepromDiagnostics.h`, `EepromDeviceConfig.h`, `EepromSecurity.h` |
| `firmware/main/src/mesh/Enrollment.cpp` | (same) | `EepromIdentity.h`, `EepromSecurity.h`, `EepromEnrollment.h` |
| `firmware/main/src/mesh/PeerRegistry.cpp` | (same) | `EepromPeers.h` |
| `firmware/main/src/adapter/Adapter.cpp` | (same) | `EepromIdentity.h`, `EepromDeviceConfig.h` |
| `firmware/main/src/adapter/AdapterFactory.cpp` | (same) | `EepromDeviceConfig.h` |
| `firmware/main/src/app/ButtonHandler.h` | (same) | `EepromCore.h`, `EepromRole.h` |
| `firmware/main/src/app/BootManager.h` | (same) | `EepromDiagnostics.h` |

Use path `"src/persistence/eeprom/EepromX.h"` (matching the existing `"src/persistence/EepromManager.h"` include style) or the shorter `"persistence/eeprom/EepromX.h"` form depending on which the file currently uses — check each file's existing include style before editing (`main.cpp` uses the `src/`-prefixed form; verify others individually, mesh files might use a shorter relative form already).

- [ ] **Step 6: Update `firmware/main/CMakeLists.txt`**

Replace `"src/persistence/EepromManager.cpp"` (line 43) with:

```
    "src/persistence/eeprom/EepromCore.cpp"
    "src/persistence/eeprom/EepromIdentity.cpp"
    "src/persistence/eeprom/EepromRole.cpp"
    "src/persistence/eeprom/EepromSecurity.cpp"
    "src/persistence/eeprom/EepromPeers.cpp"
    "src/persistence/eeprom/EepromDiagnostics.cpp"
    "src/persistence/eeprom/EepromEnrollment.cpp"
    "src/persistence/eeprom/EepromDeviceConfig.cpp"
```

Add `src/persistence/eeprom` to the `target_include_directories` block (alongside the existing `src/persistence`) so the domain `#include "EepromCore.h"`-style relative includes between the eeprom files themselves resolve.

- [ ] **Step 7: Update `tests/CMakeLists.txt`**

Replace `../firmware/main/src/persistence/EepromManager.cpp` (line 62) with the same 8-file list (adjusted for the `../firmware/main/` prefix this file uses), and replace `add_unit_test(test_eeprom_manager unit/test_eeprom_manager.cpp)` (line 112) with 8 `add_unit_test` lines, one per new test file from Step 9 below.

- [ ] **Step 8: Fix the e2e harness and remaining test references**

Grep-check each of `tests/e2e/harness/NodeContext.{h,cpp}`, `tests/e2e/harness/SimNode.{h,cpp}`, `tests/e2e/scenarios/test_harness_smoke.cpp`, `tests/e2e/scenarios/test_seq_wrap.cpp`, `tests/unit/test_pir_adapter.cpp` for `EepromManager`/`eeprom::`/`detail::State`/`debugStateForTest` references and repoint each include at the specific new domain header(s) it needs (`NodeContext.cpp`'s snapshot/restore almost certainly needs only `EepromCore.h` for `detail::State`/`debugStateForTest()` — verify by reading it, since the harness snapshots the whole `_state` blob, not individual domain state).

- [ ] **Step 9: Split `test_eeprom_manager.cpp` into 8 domain test files**

Delete `tests/unit/test_eeprom_manager.cpp`. Create 8 new files, each with the same fixture shape as the original (`SetUp()`: `resetMillis()`, `NvsMock::_store.clear()`, `namespace mgr = lattice::eeprom; mgr::init(); mgr::setDevMode(false);`), including only `EepromCore.h` (for `init`/`setDevMode`) plus its own domain header, `<gtest/gtest.h>`, and whatever else the moved tests need (`Mesh.h` for `PeerInfo`/`PEER_RECORD_SIZE` in the peers file, `error/Error.h` where fatal-path tests need it). Move each `TEST_F(EEPROMMgrTest, ...)` body **verbatim**, renaming the fixture class per file:

| New file | Fixture name | Tests moved (verbatim body, from `test_eeprom_manager.cpp`) |
|---|---|---|
| `test_eeprom_core.cpp` | `EepromCoreTest` | `Init_SucceedsFirstTime`, `Init_IdempotentOnSecondCall`, `ClearAll_RemovesAllData`, `FlushIfDirty_IsNoOp`, `ForceFlush_IsNoOp` |
| `test_eeprom_identity.cpp` | `EepromIdentityTest` | `Keypair_SaveAndLoad_ValidCRC`, `Keypair_Load_NotFound_ReturnsFalse`, `Keypair_Load_CorruptedData_ReturnsFalse`, `NodeId_DefaultIsZero`, `NodeId_SaveAndLoad`, `NodeId_SaveZeroRoundtrips` |
| `test_eeprom_role.cpp` | `EepromRoleTest` | `MasterFlag_DefaultsToFalse`, `MasterFlag_SaveAndLoad_RoundTrip`, `MasterFlag_SkipSaveInDevMode`, `DevFlag_DefaultsToFalse`, `DevFlag_SaveAndLoad_RoundTrip` |
| `test_eeprom_security.cpp` | `EepromSecurityTest` | `MeshKey_SaveAndLoad_RoundTrip`, `MeshKey_Load_NotFound_ReturnsFalse`, `MeshKey_WrongSize_ReturnsFalse`, `KnownMasterMac_UnsetReturnsFalse`, `KnownMasterMac_SaveAndLoad_RoundTrip`, `KnownMasterMac_Clear_ResetsToUnset`, `KnownMasterMac_AllFF_TreatedAsUnset`, `KnownMasterMacSecondary_UnsetReturnsFalse`, `KnownMasterMacSecondary_SaveAndLoad_RoundTrip`, `KnownMasterMacSecondary_Clear_ResetsToUnset`, `SaveKnownMasterMac_ShortWrite_Fatal` |
| `test_eeprom_peers.cpp` | `EepromPeersTest` | `PeerList_LoadWhenEmpty_FillsWithFF_ReturnsFalse`, `PeerList_SaveAndLoad_SinglePeer`, `PeerList_SaveAndLoad_MaxPeers`, `PeerList_SaveZero_ClearsList`, `PeerList_HasPeers_ReturnsFalseWhenEmpty`, `PeerList_HasPeers_ReturnsTrueAfterSave`, `PeerList_ClearPeerList_RemovesAllPeers` |
| `test_eeprom_diagnostics.cpp` | `EepromDiagnosticsTest` | `BootEpoch_StartsAtZeroWhenUnset`, `BootEpoch_SaveAndLoad_RoundTrip`, `BootEpoch_WrapsAtMax`, `RebootCount_DefaultIsZero`, `RebootCount_SaveAndLoad_RoundTrip`, `RebootReason_DefaultIs0xFF`, `RebootReason_SaveAndLoad_RoundTrip`, `SaveBootEpoch_DevMode_UsesRAMSeed`, `SaveBootEpoch_DevMode_DoesNotTouchNVS`, `SaveBootEpoch_ProdMode_Persists`, `SaveBootEpoch_ProdMode_ShortWrite_Fatal`, `SaveBootEpoch_ProdMode_FullWrite_NoFatal`, `SaveRebootCount_ShortWrite_WarnsNoFatal` |
| `test_eeprom_enrollment.cpp` | `EepromEnrollmentTest` | `EnrolledFlag_DefaultsToFalse`, `EnrolledFlag_SaveAndLoad_RoundTrip` |
| `test_eeprom_device_config.cpp` | `EepromDeviceConfigTest` | `AdapterType_DefaultIsZero`, `AdapterType_SaveAndLoad_RoundTrip`, `TxPower_DefaultIsOutdoor`, `TxPower_SaveAndLoad` |

After moving, count `TEST_F` occurrences across all 8 new files and confirm the total equals the original file's count (self-check — do not skip this).

- [ ] **Step 10: Build and test**

Run: `cmake --build tests/build --parallel 2 && ctest --test-dir tests/build --output-on-failure --label-exclude e2e && ctest --test-dir tests/build --output-on-failure --label-regex e2e`
Expected: same total test count as before this task (function bodies moved, not changed), all passing. The e2e harness's `NodeContext.cpp` snapshot/restore is the highest-risk spot — if any e2e test fails with state-corruption-looking symptoms, check that `detail::State` truly stayed a single flat POD in `EepromCore.h` and that `debugStateForTest()` still returns a reference to the same `_state` instance every domain file's functions read/write through `core_internal::*`.

- [ ] **Step 11: Commit**

```bash
git add -A firmware/main/src/persistence firmware/main/CMakeLists.txt firmware/main/main.cpp firmware/main/src/mesh firmware/main/src/adapter firmware/main/src/app tests/CMakeLists.txt tests/unit tests/e2e
git commit -m "refactor(phaseC): split EepromManager into persistence/eeprom/ domain files"
```

---

### Task 4: Logger native UART migration

**Files:**
- Modify: `firmware/main/src/logging/Logger.h`, `firmware/main/src/logging/Logger.cpp`, `firmware/main/main.cpp`, `firmware/main/CMakeLists.txt`, `firmware/main/idf_component.yml`

**Interfaces:** `Logger::debug/info/warn/error/logln/log` keep their exact signatures — only the implementation's transport changes (Arduino `Serial.*` → native `uart_write_bytes`).

- [ ] **Step 1: Rewrite `Logger.h`'s Arduino dependency**

Replace `#include <Arduino.h>` with `#include <driver/uart.h>` and `#include <cstdarg>` (needed for `va_list` now that `Arduino.h` no longer pulls it in transitively).

- [ ] **Step 2: Rewrite `Logger.cpp`'s print path**

Mirror `SerialAdapter.cpp`'s existing static `uart_write_bytes(UART_NUM_0, ...)` pattern (`SerialAdapter.cpp:136-139`). Replace every `Serial.print`/`println`/`vprintf` call with a small local helper:

```cpp
#include "Logger.h"
#include <cstdio>

namespace lattice {
namespace utils {

LogLevel Logger::currentLevel = LogLevel::LOG_DEBUG;

namespace {
void uartWrite(const char* s) {
  uart_write_bytes(UART_NUM_0, s, strlen(s));
}

void uartWriteLine(const char* tag, const char* prefix, const char* message) {
  uartWrite("[");
  uartWrite(tag);
  uartWrite("] ");
  if (prefix) {
    uartWrite(prefix);
  }
  uartWrite(message);
  uartWrite("\r\n");
}

// Mirrors the old Serial.print(prefix); Serial.vprintf(fmt, args); Serial.println();
// sequence — formats into a stack buffer (matches LATTICE_LOGF's existing
// 128-byte convention in Logger.h) then writes it as one native UART call.
void uartWriteFormatted(const char* prefix, const char* fmt, va_list args) {
  char buf[128];
  vsnprintf(buf, sizeof(buf), fmt, args);
  uartWrite(prefix);
  uartWrite(buf);
  uartWrite("\r\n");
}
} // namespace

void Logger::setLogLevel(LogLevel level) {
  currentLevel = level;
}

LogLevel Logger::getLogLevel() {
  return currentLevel;
}

void Logger::debug(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_DEBUG)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[DEBUG] ", fmt, args);
  va_end(args);
}

void Logger::info(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_INFO)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[INFO] ", fmt, args);
  va_end(args);
}

void Logger::warn(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_WARN)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[WARN] ", fmt, args);
  va_end(args);
}

void Logger::error(const char* fmt, ...) {
  if (currentLevel > LogLevel::LOG_ERROR)
    return;
  va_list args;
  va_start(args, fmt);
  uartWriteFormatted("[ERROR] ", fmt, args);
  va_end(args);
}

void Logger::logln(const char* tag, const char* message, LogLevel level) {
  if (currentLevel > level)
    return;
  uartWriteLine(tag, nullptr, message);
}

void Logger::log(const char* tag, const char* message, LogLevel level) {
  if (currentLevel > level)
    return;
  uartWrite("[");
  uartWrite(tag);
  uartWrite("] ");
  uartWrite(message);
}

} // namespace utils
} // namespace lattice
```

Add `#include <cstring>` for `strlen`. Note `logln`'s original behavior used `Serial.println(message)` (adds `\n` only, Arduino's println uses `\r\n` on most cores — check `HardwareSerial`'s actual line ending; if it was bare `\n`, use `"\n"` above instead of `"\r\n"` to stay byte-for-byte compatible with whatever the server/log-consumer side already expects). Verify against any existing e2e/host test that asserts on exact log output bytes (search for `Serial` mocks or log-capturing test doubles before assuming `\r\n` is safe).

- [ ] **Step 3: Remove `main.cpp`'s Arduino calls**

In `initDrivers()` (created by Task 1 — this task must run after Task 1), delete:

```cpp
initArduino();
Serial.begin(115200);
```

and delete the `extern "C" void initArduino();` forward declaration plus its surrounding comment block near the top of `main.cpp` (the comment explaining why `Logger.cpp` was "the one remaining Arduino API user" — no longer true after this task, so the comment is now stale and should go with the code it was explaining).

- [ ] **Step 4: Drop the arduino-esp32 dependency**

In `firmware/main/CMakeLists.txt`, remove `arduino-esp32` from the `REQUIRES` list (line 49) and update the file's header comment (lines 3-6) which currently explains why arduino-esp32 is required — replace with a note that it was removed in Phase C (finding 17) once Logger no longer needs it.

In `firmware/main/idf_component.yml`, remove the `espressif/arduino-esp32` dependency entry, leaving only the `idf` version constraint.

- [ ] **Step 5: Build and measure**

This is a real ESP-IDF build change (component dependency removal) — the host test build (`cmake --build tests/build`) does not exercise this component graph, so this step's real verification is the actual `idf.py build` CI check on the PR, not local host tests. Run the full host unit + e2e suite anyway (regression on `Logger`'s call-site-compatible behavior) — `tests/mocks/` almost certainly stubs `uart_write_bytes` already (`SerialAdapter` already uses it in host tests); confirm the mock exists and captures written bytes if any test wants to assert on Logger output, otherwise this is transport-invisible to host tests and only the real build/flash proves the migration works.

Report the flash/RAM delta from the PR's CI step (`gh pr checks` / the size-report CI job) in the commit message and PR description — do not fold this number into any other task's reporting.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/logging/Logger.h firmware/main/src/logging/Logger.cpp firmware/main/main.cpp firmware/main/CMakeLists.txt firmware/main/idf_component.yml
git commit -m "refactor(phaseC): migrate Logger off Arduino Serial to native UART (finding 17)"
```

---

### Task 5: `ButtonHandler` dedup

**Files:**
- Modify: `firmware/main/src/app/ButtonHandler.h`

**Interfaces:** `ButtonHandler::tick(...)` signature unchanged. New private static helper `detectHold`.

- [ ] **Step 1: Add the shared helper**

```cpp
// Shared "press-and-hold for holdMs" detection, factored out of
// tickConfig/tickReset (both hand-rolled the identical skeleton). Returns
// true exactly once — the instant the hold threshold is crossed — and
// false every other tick (not-pressed, still-pressed-but-under-threshold,
// or already-fired-and-released). wasPressed/holdStart are the caller's own
// static state (each of tickConfig/tickReset keeps its own pair, matching
// today's per-function static locals).
static bool detectHold(lattice::hardware::Button& btn, uint64_t holdMs, bool& wasPressed,
                       uint64_t& holdStart) {
  uint64_t now = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  if (btn.isPressed()) {
    if (!wasPressed) {
      wasPressed = true;
      holdStart = now;
      return false;
    }
    if (now - holdStart >= holdMs) {
      wasPressed = false; // reset before firing, matches tickConfig's existing comment/behavior
      return true;
    }
    return false;
  }
  wasPressed = false;
  return false;
}
```

- [ ] **Step 2: Rewrite `tickConfig` to use it**

```cpp
static void tickConfig(lattice::hardware::Button& btn, lattice::mesh::Mesh& mesh,
                       lattice::hardware::Led& greenLed, bool isDevMode, bool& devMasterFlag) {
  static bool wasPressed = false;
  static uint64_t holdStart = 0;

  if (!detectHold(btn, HOLD_MS, wasPressed, holdStart)) {
    return;
  }

  if (isDevMode) {
    bool newMaster = !mesh.getIsMaster();
    mesh.setIsMaster(newMaster);
    devMasterFlag = newMaster;
    LATTICE_LOGF("MAIN", lattice::utils::LogLevel::LOG_INFO, "DEV MODE: Role toggled. Now %s",
                 newMaster ? "MASTER" : "NODE");
    greenLed.pulse(newMaster ? 3 : 2, 150, 150);
  } else {
    bool wasMaster = lattice::eeprom::loadMasterFlag();
    bool newMaster = !wasMaster;
    lattice::eeprom::saveMasterFlag(newMaster);
    LATTICE_LOGF("MAIN", lattice::utils::LogLevel::LOG_INFO,
                 "Button held 5s: CONFIG TOGGLED. Now %s", newMaster ? "MASTER" : "NODE");
    LATTICE_LOGLN("MAIN", "Restarting in 2 seconds for new role...",
                  lattice::utils::LogLevel::LOG_INFO);
    greenLed.pulse(newMaster ? 3 : 2, 200, 200);
    pumpLedsUntilIdle(greenLed);
    vTaskDelay(pdMS_TO_TICKS(2000));
    lattice::eeprom::forceFlush();
    esp_restart();
  }
}
```

- [ ] **Step 3: Rewrite `tickReset` to use it**

`tickReset` layers a second "confirm within 3s" phase on top of the hold detection — only the hold-detection skeleton is shared, the confirm-phase state (`confirmPending`/`confirmDeadline`) stays local to this function exactly as today:

```cpp
static void tickReset(lattice::hardware::Button& btn, lattice::hardware::Led& greenLed,
                      lattice::hardware::Led& redLed) {
  static bool wasPressed = false;
  static uint64_t holdStart = 0;
  static bool confirmPending = false;
  static uint64_t confirmDeadline = 0;

  if (detectHold(btn, HOLD_MS, wasPressed, holdStart)) {
    if (!confirmPending) {
      confirmPending = true;
      confirmDeadline = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL + 3000;
      LATTICE_LOGLN("MAIN", "Reset armed: hold again within 3s to confirm EEPROM wipe",
                    lattice::utils::LogLevel::LOG_WARN);
      redLed.pulse(3, 100, 100);
    } else if (static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL < confirmDeadline) {
      confirmPending = false;
      LATTICE_LOGLN("MAIN", "EEPROM wipe confirmed. Clearing all...",
                    lattice::utils::LogLevel::LOG_WARN);
      lattice::eeprom::clearAll();
      redLed.pulse(5, 100, 100);
      greenLed.pulse(5, 100, 100);
      pumpLedsUntilIdle(redLed, &greenLed);
      vTaskDelay(pdMS_TO_TICKS(3000));
      lattice::eeprom::forceFlush();
      esp_restart();
    }
    return;
  }

  // detectHold's internal wasPressed=false covers the "still pressed, under
  // threshold" and "released mid-press" cases; the confirm-window timeout
  // (button not pressed at all, deadline elapsed) is the one case detectHold
  // doesn't observe, so it's still checked here exactly as before.
  if (confirmPending && static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL > confirmDeadline) {
    confirmPending = false;
    LATTICE_LOGLN("MAIN", "Reset confirmation timed out", lattice::utils::LogLevel::LOG_INFO);
  }
}
```

Note the behavior-preservation risk here: the original `tickReset`'s final `else` branch (timeout check) only ran when `btn.isPressed()` was false. `detectHold` returning `false` covers BOTH "not pressed" and "pressed but under threshold" — the timeout check must only fire in the "not pressed" case, same as before. Since `detectHold` itself already reads `btn.isPressed()` and the timeout logic doesn't care about press state directly (only about `confirmDeadline` elapsing), re-check against the original: the original's timeout-check `else` branch was gated on `!btn.isPressed()`, not merely "hold not detected". If a caller holds the reset button past `HOLD_MS` again while `confirmPending` is true but the button is held below the deadline check's threshold in a way that never revisits the "not pressed" branch, the two versions could diverge. Write a test for this exact case (Step 4) before trusting the refactor.

- [ ] **Step 4: Add regression tests if `ButtonHandler` has none today**

Check for an existing `test_button_handler.cpp`; if none exists, this refactor is shipping without direct test coverage for a state machine with a subtle timing edge case (flagged above) — write one covering: (a) hold `HOLD_MS` on `resetButton` → `confirmPending` becomes true, (b) release, wait past `confirmDeadline`, verify timeout log fires and `confirmPending` resets to false, (c) hold again within the window → `clearAll()`/`esp_restart()` path taken. If a real `Button` test double doesn't already exist for driving `isPressed()` programmatically, check `tests/mocks/` for one before writing a new one.

- [ ] **Step 5: Build and test**

Run the full unit + e2e suite. Expected: unchanged or +N new tests from Step 4, all passing.

- [ ] **Step 6: Commit**

```bash
git add firmware/main/src/app/ButtonHandler.h tests/unit/
git commit -m "refactor(phaseC): dedupe ButtonHandler's hold-detection into detectHold() (finding 13)"
```

---

### Task 6: Trivial batch (findings 7, 8, 9, 10, 11, 12)

**Files:**
- Delete: `firmware/main/src/network/MacAddress.h`
- Modify: `firmware/main/src/mesh/Mesh.cpp`, `firmware/main/src/mesh/PeerRegistry.h`, `firmware/main/src/error/Error.h`, `firmware/main/main.cpp`, `firmware/main/src/hardware/input/Button.cpp`, `firmware/main/src/adapter/Adapter.cpp`, `firmware/main/src/adapter/Adapter.h`, `firmware/main/src/hardware/input/GpioInput.h`, `firmware/main/src/hardware/output/GpioOutput.h`

Six independent one-to-few-line fixes. No new tests needed — each deletes dead code or replaces one implementation with an equivalent, already-tested one.

- [ ] **Step 1: Finding 7 — delete dead `MacAddress`**

```bash
git rm firmware/main/src/network/MacAddress.h
```

In `firmware/main/src/mesh/Mesh.cpp`, delete the `#include "src/network/MacAddress.h"` line (line 2) and the stale comment at line 22 (`// no longer need macEquals helper – use MacAddress equality directly`).

In `firmware/main/src/mesh/PeerRegistry.h`, delete the `#include "src/network/MacAddress.h"` line (line 5).

- [ ] **Step 2: Finding 8 — delete the legacy `Error::fail` overload**

In `firmware/main/src/error/Error.h`, delete the `toDigit()` helper (lines 30-47ish) and the legacy `fail(utils::ErrorType, const char*)`/`fatal(utils::ErrorType, const char*)` overloads (lines 62-64, 82-84).

Migrate `main.cpp`'s 2 call sites (in `initHardwareOutputs()`, created by Task 1) from:

```cpp
lattice::err::fail(lattice::utils::ErrorType::HARDWARE_FAILURE, "Config button init failed!");
```

to the digit-based form, matching the `ModuleDigit::CORE`/`HARDWARE` precedent already used elsewhere in the same function:

```cpp
lattice::err::fail(lattice::core::ErrorTypeDigit::HARDWARE, lattice::core::ModuleDigit::CORE, 5,
                   "Config button init failed!");
```

and similarly for the reset-button call site (pick the next unused sub-code in that function's existing numbering — check what sub-codes 1-4 are already used for in `initSubsystems()`/`initHardwareOutputs()` per Task 1's extraction and continue the sequence without colliding).

- [ ] **Step 3: Finding 9 — `Button::init()` delegates to `GpioInput::init()`**

In `firmware/main/src/hardware/input/Button.cpp`, replace:

```cpp
bool Button::init() {
  if (!isValidInputPin(_pin)) {
    return false;
  }
  _initialized = true;
  return true;
}
```

with:

```cpp
bool Button::init() {
  return GpioInput::init();
}
```

(matches `Pir::init()`'s existing pattern of delegating to the shared base — verify `Pir.cpp` for the exact call shape before writing this, to keep the two siblings visibly consistent.)

- [ ] **Step 4: Finding 10 — delete `Adapter::init()`'s dead body**

In `firmware/main/src/adapter/Adapter.cpp`, delete:

```cpp
bool Adapter::init() {
  return true;
}
```

`Adapter.h:50` already declares `init()` as pure-virtual (`= 0`) — no header change needed, just remove the unreachable out-of-line definition.

- [ ] **Step 5: Finding 11 — delete the dead `LED_ADAPTER` enumerator**

In `firmware/main/src/adapter/Adapter.h`, delete `LED_ADAPTER = 3,` from `enum adapter_types`. Pre-verified safe (see Global Constraints) — explicit enumerator values mean `UNKNOWN_ADAPTER`/`SERIAL_ADAPTER`/`PIR_ADAPTER` keep their values unchanged, and `AdapterFactory::createAdapter()` never handles this case (falls to `default:`), so nothing in nodes ever transmits it.

- [ ] **Step 6: Finding 12 — drop `virtual` from `GpioInput`/`GpioOutput` destructors**

In `firmware/main/src/hardware/input/GpioInput.h`, change `virtual ~GpioInput() = default;` to `~GpioInput() = default;` and update the adjacent comment (which currently says the destructor is "left virtual/untouched — out of scope for this item") to note it's now done too.

In `firmware/main/src/hardware/output/GpioOutput.h`, same change: `virtual ~GpioOutput() = default;` → `~GpioOutput() = default;`.

- [ ] **Step 7: Build and test**

Run the full unit + e2e suite. Expected: same count, all passing — every change here either deletes genuinely-dead code or swaps in a behaviorally-identical implementation.

- [ ] **Step 8: Commit**

```bash
git add -A firmware/main/src/network firmware/main/src/mesh firmware/main/src/error firmware/main/main.cpp firmware/main/src/hardware firmware/main/src/adapter
git commit -m "refactor(phaseC): trivial dead-code and consistency cleanup (findings 7,8,9,10,11,12)"
```

---

## Self-Review Notes

- **Spec coverage:** all 12 Phase C findings (3, 4, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18) are covered — 3/18 in Task 1, 14 in Task 2, 4 in Task 3, 17 in Task 4, 13 in Task 5, 7/8/9/10/11/12 in Task 6.
- **Sequencing risk:** Task 3 touches the include block of 8 other files this plan modifies — it is sequenced first and alone specifically to avoid this. Tasks 1→2→4 share `main.cpp` and are sequential. Tasks 5 and 6 are worktree-parallel-safe once Task 3 has landed.
- **Type/interface consistency:** `core_internal::*` names introduced in Task 3 are used identically across every domain file's `.cpp` — verified against the single `EepromCore.h` declaration list while drafting each domain file above.
- **Known risk flagged inline:** Task 5's `tickReset` refactor has a subtle behavior-preservation edge case around the confirm-window timeout check, called out explicitly in Step 3 with a required test in Step 4 rather than asserted safe by construction.
