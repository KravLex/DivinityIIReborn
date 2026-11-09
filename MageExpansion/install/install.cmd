@echo off
rem Mage Skills Expansion: copies build\MageExpansion.dv2 into Data\Win32\Packed\Reborn\ (Div2Fix overlay, [Overlay]
rem Enable=1). Adds one file, changes nothing else. Undo: uninstall.cmd. The game must be closed.
setlocal
cd /d "%~dp0"
set PACKED=C:\Program Files (x86)\Steam\steamapps\common\divinity2_dev_cut\Data\Win32\Packed
tasklist /fi "imagename eq Divinity2.exe" | find /i "Divinity2.exe" >nul && ( echo Close the game first. & exit /b 1 )
if not exist ..\build\MageExpansion.dv2 ( echo Build first: py -3 -I tools\build_mage.py "game dir" build & exit /b 1 )
if not exist "%PACKED%\Reborn" mkdir "%PACKED%\Reborn"
copy /y ..\build\MageExpansion.dv2 "%PACKED%\Reborn\MageExpansion.dv2" >nul || ( echo Copy failed. & exit /b 1 )
copy /y ..\build\MageData.dv2 "%PACKED%\Reborn\MageData.dv2" >nul || ( echo Copy failed. & exit /b 1 )
set BIN=%PACKED%\..\..\..\bin
if not exist "%BIN%\MageExpansion" mkdir "%BIN%\MageExpansion"
copy /y ..\build\FrostNova.wav "%BIN%\MageExpansion\FrostNova.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\ChainLightning.wav "%BIN%\MageExpansion\ChainLightning.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\IceBolt.wav "%BIN%\MageExpansion\IceBolt.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\ArcanePush.wav "%BIN%\MageExpansion\ArcanePush.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\ArcaneShield.wav "%BIN%\MageExpansion\ArcaneShield.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\HolySmite.wav "%BIN%\MageExpansion\HolySmite.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\Blessing.wav "%BIN%\MageExpansion\Blessing.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\Drain.wav "%BIN%\MageExpansion\Drain.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\BattleCry_M.wav "%BIN%\MageExpansion\BattleCry_M.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
copy /y ..\build\BattleCry_F.wav "%BIN%\MageExpansion\BattleCry_F.wav" >nul || ( echo Copy of the sound failed. & exit /b 1 )
echo Installed %PACKED%\Reborn\MageExpansion.dv2 and bin\MageExpansion\FrostNova.wav, ChainLightning.wav, IceBolt.wav, ArcanePush.wav, ArcaneShield.wav, HolySmite.wav, Blessing.wav, Drain.wav
exit /b 0
