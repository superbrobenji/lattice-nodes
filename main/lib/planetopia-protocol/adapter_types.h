/* Copied from: planetopia-protocol/c/adapter_types.h
 * Source repo: https://github.com/benjaminswanepoel/planetopia-protocol
 * Keep in sync with that repo — do not edit values here directly.
 * For production, set up a git submodule or CI step to keep this file current.
 *
 * NOTE: These wire-protocol identifiers are distinct from the C++ adapter_types enum
 * in src/Adapter/Adapter.h. The C++ enum uses different int32_t values for internal
 * routing; these defines are the canonical wire-level identifiers used in mesh frames. */

#pragma once

/* Planetopia adapter type identifiers.
 * Keep in sync with adapter/types.go in this repo. */

#define ADAPTER_TYPE_UNKNOWN  0
#define ADAPTER_TYPE_SERIAL   1
#define ADAPTER_TYPE_PIR      2
#define ADAPTER_TYPE_LED      3
#define ADAPTER_TYPE_RELAY    4
