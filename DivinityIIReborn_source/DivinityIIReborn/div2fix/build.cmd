@echo off
rem Windows build of Div2Fix (dinput8.dll) with LLVM for Windows (https://github.com/llvm/llvm-project/releases).
rem Needs clang-cl, lld-link, llvm-dlltool and llvm-rc on PATH. No Windows SDK and no C runtime are used.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
llvm-dlltool -m i386 -k -d src\kernel32.def -l build\kernel32.lib || goto :err
llvm-dlltool -m i386 -k -d src\user32.def   -l build\user32.lib   || goto :err
for %%f in (div2fix diag detour gfx mindread camera icons intro uitext window shadow version) do (
  clang-cl --target=i686-pc-windows-msvc /nologo /c /O2 /GS- /Gs999999 /W4 /Zl /Oi- -fno-builtin /Fobuild\%%f.obj src\%%f.c || goto :err
)
llvm-rc /nologo /fo build\div2fix.res res\div2fix.rc || goto :err
lld-link /nologo /dll /machine:x86 /nodefaultlib /entry:DllMain /subsystem:windows /def:src\dinput8.def /out:build\dinput8.dll ^
  build\div2fix.obj build\diag.obj build\detour.obj build\gfx.obj build\mindread.obj build\camera.obj build\icons.obj ^
  build\intro.obj build\uitext.obj build\window.obj build\shadow.obj build\version.obj build\div2fix.res ^
  build\kernel32.lib build\user32.lib /safeseh:no || goto :err
echo Built build\dinput8.dll
exit /b 0
:err
echo Build failed.
exit /b 1
