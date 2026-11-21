#!/usr/bin/env bash
# Build and run gen-a740-fill against the host Mesa build (ext/mesa-25.0.7 + ext/mesa-host-build) and hakios-metal's headers.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); EXT=$HERE/../../ext
S=$EXT/mesa-25.0.7; B=$EXT/mesa-host-build; H=$EXT/hakios/hakios-metal
DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' $B/build.ninja | grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
/usr/bin/c++ -std=c++17 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -Wno-c99-designator $DEFS \
  -I"$HERE" -I"$H/include" -isystem $S/src -isystem $S/include -isystem $S/src/freedreno -isystem $S/src/freedreno/common \
  -isystem $B/src -isystem $B/src/freedreno -isystem $B/src/freedreno/common -isystem $B/src/freedreno/registers/adreno \
  -isystem $S/src/freedreno/registers/adreno \
  -o "$HERE/out/gen-a740-fill" "$HERE/gen-a740-fill.cc" $B/src/freedreno/common/libfreedreno_common.a $B/src/util/libmesa_util.a \
  $B/src/c11/impl/libmesa_util_c11.a $B/src/util/blake3/libblake3.a -lz -lm -pthread
"$HERE/out/gen-a740-fill" "$HERE/out/A740FillProgram.h"
