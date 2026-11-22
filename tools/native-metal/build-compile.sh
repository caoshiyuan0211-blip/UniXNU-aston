#!/usr/bin/env bash
# Build a740-compile against the host Mesa build (ext/mesa-25.0.7 + ext/mesa-host-build).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); S=$HERE/../../ext/mesa-25.0.7; B=$HERE/../../ext/mesa-host-build
DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' $B/build.ninja | grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
L=""
for a in src/freedreno/ir3/libfreedreno_ir3.a src/compiler/spirv/libvtn.a src/freedreno/isa/libir3encode.a src/freedreno/isa/libir3decode.a \
         src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/freedreno/fdl/libfreedreno_layout.a \
         src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a; do
  L="$L $B/$a"
done
/usr/bin/cc -std=c11 -O1 -g $DEFS -I$S/src -I$S/include -I$S/src/freedreno -I$S/src/freedreno/common -I$S/src/compiler \
  -I$S/src/compiler/nir -I$S/src/gallium/include -I$B/src -I$B/src/compiler -I$B/src/compiler/nir -I$B/src/compiler/spirv \
  -I$B/src/freedreno -I$B/src/freedreno/common -I$B/src/freedreno/isa -I$B/src/freedreno/registers/adreno \
  -I$S/src/compiler/isaspec -I$B/src/compiler/isaspec -o "$HERE/out/a740-compile" "$HERE/a740-compile.c" $L $L -lz -lm -pthread -lc++
