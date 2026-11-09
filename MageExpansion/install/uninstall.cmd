@echo off
rem Mage Skills Expansion: removes Data\Win32\Packed\Reborn\MageExpansion.dv2. The game must be closed.
rem Saves are not touched. A save made with a learned new skill keeps it (see docs/RESEARCH.md, section 5).
setlocal
set PACKED=C:\Program Files (x86)\Steam\steamapps\common\divinity2_dev_cut\Data\Win32\Packed
tasklist /fi "imagename eq Divinity2.exe" | find /i "Divinity2.exe" >nul && ( echo Close the game first. & exit /b 1 )
if not exist "%PACKED%\Reborn\MageExpansion.dv2" ( echo Not installed. & exit /b 0 )
del /q "%PACKED%\Reborn\MageExpansion.dv2" || ( echo Delete failed. & exit /b 1 )
if exist "%PACKED%\Reborn\MageData.dv2" del /q "%PACKED%\Reborn\MageData.dv2"
if exist "%PACKED%\..\..\..\bin\MageExpansion\FrostNova.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\FrostNova.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\ChainLightning.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\ChainLightning.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\IceBolt.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\IceBolt.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\ArcanePush.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\ArcanePush.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\ArcaneShield.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\ArcaneShield.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\HolySmite.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\HolySmite.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\Blessing.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\Blessing.wav"
if exist "%PACKED%\..\..\..\bin\MageExpansion\Drain.wav" del /q "%PACKED%\..\..\..\bin\MageExpansion\Drain.wav"
echo Removed %PACKED%\Reborn\MageExpansion.dv2
exit /b 0
