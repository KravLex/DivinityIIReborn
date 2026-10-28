#!/usr/bin/env bash
# Builds the 32-bit DXVK d3d9.dll shipped with Divinity II Reborn: DXVK v3.1.1 + patches/*.patch, MSYS2 MINGW32.
# One-time setup (MSYS2): pacman -S git mingw-w64-i686-gcc mingw-w64-i686-meson mingw-w64-i686-ninja
#                                    mingw-w64-i686-pkgconf mingw-w64-x86_64-glslang
#   git clone --recursive --branch v3.1.1 https://github.com/doitsujin/dxvk dxvk/dxvk-src
# Build:  bash dxvk/build_dxvk.sh patched   (or "vanilla" = unmodified v3.1.1, toolchain check)
# Result: dxvk/dxvk-out/<variant>/bin/d3d9.dll
# Flags follow DXVK's package-release.sh: --buildtype release --strip -Db_ndebug=if-release.
set -euo pipefail
variant="${1:-patched}"
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/dxvk-src"
out="$here/dxvk-out/$variant"
export PATH="/mingw32/bin:/mingw64/bin:/usr/bin:$PATH"   # i686 gcc/meson/ninja; glslangValidator from mingw64

cd "$src"
git checkout -q -- . && git clean -qfd -e 'build.*'    # start from the pristine tag every time
if [ "$variant" = patched ]; then
  for p in "$here"/patches/*.patch; do echo "applying $(basename "$p")"; git apply --whitespace=nowarn "$p"; done
fi
rm -rf "build.$variant" "$out"
meson setup --buildtype release --strip -Db_ndebug=if-release --prefix="$out" "build.$variant" > "$here/dxvk-$variant-setup.log" 2>&1 \
  || { tail -30 "$here/dxvk-$variant-setup.log"; exit 1; }
ninja -C "build.$variant" install > "$here/dxvk-$variant-build.log" 2>&1 \
  || { grep -m 20 -E 'error|Error' "$here/dxvk-$variant-build.log"; tail -20 "$here/dxvk-$variant-build.log"; exit 1; }
# symbols of the (otherwise identical) unstripped build, for tools/benchmark/prof_symbolize.py
nm -C --defined-only "build.$variant/src/d3d9/d3d9.dll" | grep -i ' t ' > "$out/d3d9.sym"
objdump -p "build.$variant/src/d3d9/d3d9.dll" | grep -m1 ImageBase >> "$out/d3d9.sym"
ls -l "$out/bin/d3d9.dll"
objdump -p "$out/bin/d3d9.dll" | grep -E 'DLL Name' | sort -u
