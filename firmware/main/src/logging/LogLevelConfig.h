#ifndef LATTICE_LOG_LEVEL_CONFIG_H
#define LATTICE_LOG_LEVEL_CONFIG_H

// ---------------------------------------------------------------------------------------------
// Compile-time log level — the single place it is set (issue #117).
//
// Numeric values mirror lattice::utils::LogLevel (Logger.h) exactly, so the preprocessor can
// compare against them before the enum exists:
//   0 = LOG_DEBUG   1 = LOG_INFO   2 = LOG_WARN   3 = LOG_ERROR   4 = LOG_NONE
//
// Both Logger.h (compile-time gating of LATTICE_LOG/LATTICE_LOGLN/LATTICE_LOGF) and
// project_config.h (runtime lattice::config::DEFAULT_LOG_LEVEL, derived from this macro) include
// this header, so every translation unit sees the same value no matter which of those two it
// reaches first. Previously each carried its own #ifndef fallback and whichever was included
// first won per translation unit, so raising the level in project_config.h alone tripped its
// own static_assert in any file that pulled in Logger.h before it.
//
// CRITICAL: for hub communication this MUST stay LATTICE_LOG_LEVEL_NONE. On a SERIAL_ADAPTER node
// Logger and SerialAdapter share UART0, and log text corrupts the framed protocol. Only raise it
// for bench-testing a node that is not talking to a hub over the same USB connection.
//
// To raise it, either edit the default below, or pass a CMake cache entry — no source edit —
//   idf.py -DLATTICE_DEFAULT_LOG_LEVEL=3 build      (main/CMakeLists.txt forwards it as a -D)
// The #ifndef is what lets that command-line definition win over the default here.
#define LATTICE_LOG_LEVEL_NONE 4

#ifndef LATTICE_DEFAULT_LOG_LEVEL
#define LATTICE_DEFAULT_LOG_LEVEL LATTICE_LOG_LEVEL_NONE
#endif

#endif // LATTICE_LOG_LEVEL_CONFIG_H
