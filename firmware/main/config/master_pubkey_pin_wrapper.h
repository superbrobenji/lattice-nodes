#pragma once
// Deployer must generate firmware/main/config/master_pubkey_pin.h via
// tools/gen_master_pubkey_pin.py. Host-test builds and DEV_MODE firmware
// builds may set -DLATTICE_ALLOW_EXAMPLE_PIN=1 to fall back to the
// placeholder (compilable but not shippable).
#if __has_include("master_pubkey_pin.h")
  #include "master_pubkey_pin.h"
#elif defined(LATTICE_ALLOW_EXAMPLE_PIN)
  #include "master_pubkey_pin.h.example"
#else
  #error "firmware/main/config/master_pubkey_pin.h not found. Generate it via tools/gen_master_pubkey_pin.py or build with -DLATTICE_ALLOW_EXAMPLE_PIN=1 (DEV_MODE only)."
#endif

#ifdef UNIT_TEST
namespace lattice { namespace mesh { namespace pin {
// Test-only: when true, production check sites skip the pin comparison.
// Off by default so pin-active tests behave as production would.
inline bool& _testBypass() { static bool b = false; return b; }
inline void setTestBypass(bool on) { _testBypass() = on; }
inline bool isTestBypassed() { return _testBypass(); }
}}}
#else
namespace lattice { namespace mesh { namespace pin {
inline constexpr bool isTestBypassed() { return false; }
}}}
#endif
