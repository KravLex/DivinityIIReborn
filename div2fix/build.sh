#!/bin/sh
# Build on Linux: clang-cl + lld-link + llvm-dlltool + llvm-rc (LLVM 18). No Windows SDK, no C runtime.
set -e
cd "$(dirname "$0")"
mkdir -p build
llvm-dlltool -m i386 -k -d src/kernel32.def -l build/kernel32.lib
llvm-dlltool -m i386 -k -d src/user32.def   -l build/user32.lib
for f in div2fix diag detour gfx mindread camera icons intro uitext window shadow version; do clang-cl-18 --target=i686-pc-windows-msvc /nologo /c /O2 /GS- /Gs999999 /W4 /Zl /Oi- -fno-builtin /Fobuild/$f.obj src/$f.c; done
llvm-rc-18 /nologo /fo build/div2fix.res res/div2fix.rc
lld-link /nologo /dll /machine:x86 /nodefaultlib /entry:DllMain /subsystem:windows \
  /def:src/dinput8.def /out:build/dinput8.dll build/div2fix.obj build/diag.obj build/detour.obj build/gfx.obj build/mindread.obj build/camera.obj build/icons.obj build/intro.obj build/uitext.obj build/window.obj build/shadow.obj build/version.obj build/div2fix.res build/kernel32.lib build/user32.lib /safeseh:no
