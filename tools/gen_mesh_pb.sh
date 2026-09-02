#!/usr/bin/env bash
# Regenerate the serial-link nanopb codec
#   firmware/main/src/mesh/serialization/mesh.pb.{h,c}
# from the lattice-protocol submodule's proto/mesh.proto + proto/mesh.options.
#
# The submodule is the single source of truth for the wire schema. The
# generated files are checked in so the firmware builds without a Python
# toolchain, which means they can silently drift from the submodule pin
# (that is how retired v0.6.0 fields lingered in firmware, see #113). CI
# (.github/workflows/proto-sync.yml) runs this script with --check on every
# PR so a submodule bump without a matching regen fails the build.
#
# Usage:
#   tools/gen_mesh_pb.sh          # regenerate in place
#   tools/gen_mesh_pb.sh --check  # regenerate, then exit 1 if the result
#                                 # differs from the committed files
#
# Requirements:
#   - nanopb_generator on PATH at the SAME version as the vendored runtime
#     (NANOPB_VERSION in firmware/main/src/mesh/serialization/nanopb/pb.h);
#     the script refuses to run on a mismatch. Install with:
#         python3 -m pip install "nanopb==<that version>"
#     (or point NANOPB_GENERATOR at a specific binary)
#   - protoc: either on PATH, or `python3 -m pip install grpcio-tools`
#     (nanopb_generator falls back to the protoc bundled with grpc_tools).
#
# The generator emits no timestamp (nanopb >= 0.4 default), so the output is
# byte-for-byte reproducible for a given nanopb version + proto + options.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTO_DIR="$REPO_ROOT/firmware/main/lib/lattice-protocol/proto"
OUT_DIR="$REPO_ROOT/firmware/main/src/mesh/serialization"
RUNTIME_PB_H="$OUT_DIR/nanopb/pb.h"
GENERATOR="${NANOPB_GENERATOR:-nanopb_generator}"

die() {
  echo "gen_mesh_pb: $*" >&2
  exit 1
}

check=0
case "${1:-}" in
"") ;;
--check) check=1 ;;
*) die "usage: $0 [--check]" ;;
esac

[ -f "$PROTO_DIR/mesh.proto" ] ||
  die "$PROTO_DIR/mesh.proto missing — run: git submodule update --init --recursive"
[ -f "$PROTO_DIR/mesh.options" ] || die "$PROTO_DIR/mesh.options missing"

# Generator and vendored runtime must agree: a mismatch trips the generated
# header's PB_PROTO_HEADER_VERSION #error at compile time, and even same-major
# versions can differ in emitted layout, which would show up as spurious drift.
runtime_ver="$(sed -n 's/^#define NANOPB_VERSION "nanopb-\(.*\)"$/\1/p' "$RUNTIME_PB_H")"
[ -n "$runtime_ver" ] || die "could not read NANOPB_VERSION from $RUNTIME_PB_H"
command -v "$GENERATOR" >/dev/null 2>&1 ||
  die "$GENERATOR not found — install with: python3 -m pip install \"nanopb==$runtime_ver\""
gen_ver="$("$GENERATOR" --version 2>&1 | sed -n 's/^nanopb-\(.*\)$/\1/p')"
[ "$gen_ver" = "$runtime_ver" ] ||
  die "nanopb_generator is '$gen_ver' but the vendored runtime is $runtime_ver" \
    "— install with: python3 -m pip install \"nanopb==$runtime_ver\""

# Run from proto/ with -I . so the descriptor's file name is a bare
# "mesh.proto" — that is what the include guard / type prefix derive from.
(cd "$PROTO_DIR" && "$GENERATOR" --quiet -I . -D "$OUT_DIR" -f mesh.options mesh.proto)

if [ "$check" -eq 1 ]; then
  if ! git -C "$REPO_ROOT" diff --exit-code -- "$OUT_DIR/mesh.pb.h" "$OUT_DIR/mesh.pb.c"; then
    die "mesh.pb.h/.c are out of date relative to the lattice-protocol submodule" \
      "— run tools/gen_mesh_pb.sh and commit the result"
  fi
  submodule_rev="$(git -C "$PROTO_DIR" describe --tags --always 2>/dev/null || echo unknown)"
  echo "gen_mesh_pb: mesh.pb.h/.c are in sync with lattice-protocol $submodule_rev" \
    "(nanopb-$gen_ver)"
fi
