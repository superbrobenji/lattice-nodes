#pragma once
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#include <mbedtls/chachapoly.h>

// RAII guards for mbedtls contexts used across E2ECrypto.h / MeshCrypto.h.
//
// Why: on-device, lattice::err::fail(...) is [[noreturn]] (reboot), so a leaked
// mbedtls context on a fatal path is moot. Under UNIT_TEST, err::fail throws and
// unwinds the stack instead, which leaked every raw context initialised on the
// failing function's stack frame — polluting subsequent tests. These guards call
// the matching mbedtls _init in the default constructor and _free in the
// destructor, so both success and fatal paths clean up automatically via normal
// C++ stack unwind. Header-only, no dynamic allocation, no behaviour change on
// the success path (call sites still pass the same pointer to the same mbedtls
// APIs — see operator T*() below).

namespace lattice {
namespace mesh {
namespace mbedtls_guard {

struct EcdhCtx {
  mbedtls_ecdh_context ctx;
  EcdhCtx() { mbedtls_ecdh_init(&ctx); }
  ~EcdhCtx() { mbedtls_ecdh_free(&ctx); }
  EcdhCtx(const EcdhCtx&) = delete;
  EcdhCtx& operator=(const EcdhCtx&) = delete;
  operator mbedtls_ecdh_context*() { return &ctx; }
};

struct EntropyCtx {
  mbedtls_entropy_context ctx;
  EntropyCtx() { mbedtls_entropy_init(&ctx); }
  ~EntropyCtx() { mbedtls_entropy_free(&ctx); }
  EntropyCtx(const EntropyCtx&) = delete;
  EntropyCtx& operator=(const EntropyCtx&) = delete;
  operator mbedtls_entropy_context*() { return &ctx; }
};

struct CtrDrbgCtx {
  mbedtls_ctr_drbg_context ctx;
  CtrDrbgCtx() { mbedtls_ctr_drbg_init(&ctx); }
  ~CtrDrbgCtx() { mbedtls_ctr_drbg_free(&ctx); }
  CtrDrbgCtx(const CtrDrbgCtx&) = delete;
  CtrDrbgCtx& operator=(const CtrDrbgCtx&) = delete;
  operator mbedtls_ctr_drbg_context*() { return &ctx; }
};

// Post-Phase-G audit item P: MeshCrypto.h::generateKeypair and
// E2ECrypto.h::sealPayload/openPayload were left hand-managed when this file
// was introduced (Phase E) — same UNIT_TEST-unwind leak class: an
// err::fatal() between init and free throws under UNIT_TEST instead of
// halting the device, skipping the free. These four close that gap.

struct EcpGroupCtx {
  mbedtls_ecp_group ctx;
  EcpGroupCtx() { mbedtls_ecp_group_init(&ctx); }
  ~EcpGroupCtx() { mbedtls_ecp_group_free(&ctx); }
  EcpGroupCtx(const EcpGroupCtx&) = delete;
  EcpGroupCtx& operator=(const EcpGroupCtx&) = delete;
  operator mbedtls_ecp_group*() { return &ctx; }
};

struct MpiCtx {
  mbedtls_mpi ctx;
  MpiCtx() { mbedtls_mpi_init(&ctx); }
  ~MpiCtx() { mbedtls_mpi_free(&ctx); }
  MpiCtx(const MpiCtx&) = delete;
  MpiCtx& operator=(const MpiCtx&) = delete;
  operator mbedtls_mpi*() { return &ctx; }
};

struct EcpPointCtx {
  mbedtls_ecp_point ctx;
  EcpPointCtx() { mbedtls_ecp_point_init(&ctx); }
  ~EcpPointCtx() { mbedtls_ecp_point_free(&ctx); }
  EcpPointCtx(const EcpPointCtx&) = delete;
  EcpPointCtx& operator=(const EcpPointCtx&) = delete;
  operator mbedtls_ecp_point*() { return &ctx; }
};

struct ChaChaPolyCtx {
  mbedtls_chachapoly_context ctx;
  ChaChaPolyCtx() { mbedtls_chachapoly_init(&ctx); }
  ~ChaChaPolyCtx() { mbedtls_chachapoly_free(&ctx); }
  ChaChaPolyCtx(const ChaChaPolyCtx&) = delete;
  ChaChaPolyCtx& operator=(const ChaChaPolyCtx&) = delete;
  operator mbedtls_chachapoly_context*() { return &ctx; }
};

// Note: no MdCtx here. mbedtls_md_* usage in E2ECrypto.h (mbedtls_md_info_from_type)
// and RouteMac.h (mbedtls_md_hmac) are free-standing / one-shot calls — neither
// initialises nor frees an mbedtls_md_context_t, so there is nothing to guard.

} // namespace mbedtls_guard
} // namespace mesh
} // namespace lattice
