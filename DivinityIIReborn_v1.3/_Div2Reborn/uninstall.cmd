@echo off
chcp 65001 >nul
rem Divinity II Reborn 1.3 - удаление. Удаляет только файлы мода; оригинальные файлы игры и сохранения не трогает.
set "G=%~dp0.."
if not exist "%G%\bin\Divinity2.exe" (echo Папка _Div2Reborn должна лежать в папке игры, рядом с bin и Data. & pause & exit /b 1)
tasklist /FI "IMAGENAME eq Divinity2.exe" | find /I "Divinity2.exe" >nul && (echo Закройте игру и запустите удаление снова. & pause & exit /b 1)
echo === Удаление Divinity II Reborn ===
echo.
echo Будут удалены из bin: dinput8.dll, Div2Fix.ini, Div2Fix_mindread.txt, d3d9.dll, dxvk.conf и журналы мода.
choice /C YN /M "Продолжить? Y = да, N = нет"
if errorlevel 2 exit /b 0
set "B=%G%\bin"
for %%f in (dinput8.dll Div2Fix.ini Div2Fix_mindread.txt Div2Fix.log d3d9.dll dxvk.conf Divinity2_d3d9.log Divinity2.dxvk-cache) do if exist "%B%\%%f" del /f /q "%B%\%%f" && echo   удалён bin\%%f
del /f /q "%B%\Div2Fix_perf_*.txt" "%B%\Div2Fix_perf_*.csv" 2>nul
echo.
set "P=%G%\Data\Win32\Packed"
set N=0
for %%f in (Patch.dv2 Patch_2.dv2 Patch_3.dv2 Patch_4.dv2 DKS_Patch.dv2 DKS_Patch_2.dv2 DKS_Patch_3.dv2 DKS_Patch_4.dv2 FOV_Patch.dv2 FOV_Patch_2.dv2 FOV_Patch_3.dv2 FOV_Patch_4.dv2) do if exist "%P%\%%f" set /a N+=1
if %N%==0 goto done
echo Найдено файлов пака HD-текстур: %N% (Data\Win32\Packed\Patch*, DKS_Patch*, FOV_Patch*).
echo Если эти файлы ставил другой мод - ответьте N.
choice /C YN /M "Удалить HD-текстуры? Y = да, N = нет"
if errorlevel 2 goto done
for %%f in (Patch.dv2 Patch_2.dv2 Patch_3.dv2 Patch_4.dv2 DKS_Patch.dv2 DKS_Patch_2.dv2 DKS_Patch_3.dv2 DKS_Patch_4.dv2 FOV_Patch.dv2 FOV_Patch_2.dv2 FOV_Patch_3.dv2 FOV_Patch_4.dv2) do if exist "%P%\%%f" del /f /q "%P%\%%f" && echo   удалён Data\Win32\Packed\%%f
:done
echo.
echo Готово. ReShade и Div2Patch ставятся отдельно и удаляются своими средствами.
echo Папку _Div2Reborn можно удалить вручную.
pause
