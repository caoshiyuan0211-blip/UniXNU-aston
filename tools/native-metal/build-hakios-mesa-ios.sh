#!/bin/bash
# build-hakios-mesa-ios.sh OUT [JOBS] -- Mesa 25.0.7 with hakios-metal's AIR front end (MIT; hakios-mesa/air, the patches beside it),
# cross-built as static archives for arm64e iOS 27: what hakios-metal's compiler.cpp links on the OnePlus 12R (stage 4c-2), so the
# plugin compiles the OS's Metal shaders (AIR) for the Adreno 740 itself. The Mac build of the same tree passed hakios's 266 tests
# with the compiler in and compiled all 72 of its AIR modules for the A740 (12r/metal/tools/a740-air-check.cpp).
#
# Out: OUT/lib/*.a (the archives compiler.cpp needs), OUT/include.txt (the -isystem dirs), OUT/defs.txt (the -D set Mesa compiled
# ir3 with: its headers need the same), OUT/stamp (what was built from). Needs Xcode's iPhoneOS SDK, python3 (a venv with meson,
# mako, pyyaml is made in OUT), ninja, GNU bison and m4 (Homebrew's on the runner; M4 may name m4).
set -euo pipefail
OUT=$(mkdir -p "$1" && cd "$1" && pwd)
JOBS=${2:-$(sysctl -n hw.ncpu)}
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
STAMP=$( (echo $VERSION; cat "$HERE"/hakios-mesa/*.patch "$HERE"/hakios-mesa/air/* "$0") | shasum -a 256 | cut -d' ' -f1)
if [ -f "$OUT/stamp" ] && [ "$(cat "$OUT/stamp")" = "$STAMP" ]; then echo "hakios Mesa for iOS: up to date ($OUT)"; exit 0; fi
cd "$OUT"
[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
rm -rf src build lib
mkdir src && tar xJf mesa-$VERSION.tar.xz -C src --strip-components=1
for p in "$HERE"/hakios-mesa/air-*.patch "$HERE"/hakios-mesa/ir3-*.patch; do patch -s --no-backup-if-mismatch -d src -p1 < "$p"; done
rm -rf src/src/compiler/air && cp -R "$HERE"/hakios-mesa/air src/src/compiler/air
# ir3's disk cache asks for the ELF build id of its library (dl_iterate_phdr): Darwin has none, and the cache is off here
patch -s --no-backup-if-mismatch -d src -p1 < "$HERE"/hakios-mesa/darwin-ir3-disk-cache.patch
[ -x venv/bin/ninja ] || { python3 -m venv venv && venv/bin/pip -q install meson==1.6.1 ninja mako pyyaml packaging; }
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos --find clang); CXX=$(xcrun --sdk iphoneos --find clang++); AR=$(xcrun --sdk iphoneos --find ar)
FLAGS="'-target', 'arm64e-apple-ios27.0', '-isysroot', '$SDK', '-fstack-protector-strong'"
cat > ios.cross <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
objc = '$CC'
ar = '$AR'
strip = '$(xcrun --sdk iphoneos --find strip)'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$FLAGS]
cpp_args = [$FLAGS]
objc_args = [$FLAGS]
objc_link_args = [$FLAGS]
c_link_args = [$FLAGS]
cpp_link_args = [$FLAGS]
EOF
export PATH=$OUT/venv/bin:$PATH
venv/bin/meson setup build src --cross-file ios.cross --buildtype=release -Db_ndebug=true \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled \
    -Dlibunwind=disabled -Dfreedreno-kmds=kgsl -Dshader-cache=disabled -Dbuild-tests=false > meson-setup.log
ARCHIVES="src/compiler/air/libair.a src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a src/freedreno/isa/libir3decode.a
src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/freedreno/fdl/libfreedreno_layout.a
src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a"
ninja -j"$JOBS" -C build $ARCHIVES > ninja.log
mkdir -p lib && for a in $ARCHIVES; do cp build/$a lib/; done
grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' build/build.ninja | grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | grep -v "^-DNDEBUG$" | tr '\n' ' ' > defs.txt
for d in src/src src/include src/src/freedreno src/src/freedreno/common src/src/compiler src/src/compiler/nir src/src/gallium/include \
         src/src/compiler/air build/src build/src/compiler build/src/compiler/nir build/src/freedreno build/src/freedreno/common \
         build/src/freedreno/isa; do echo -n "-isystem $OUT/$d "; done > include.txt
echo "$STAMP" > stamp
echo "hakios Mesa for iOS: $(ls lib | wc -l | tr -d ' ') archives in $OUT/lib"
