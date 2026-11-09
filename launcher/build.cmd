@echo off
rem Divinity II Reborn launcher - build with the C# compiler that ships with .NET Framework 4.x (no SDK needed).
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (echo csc.exe not found: %CSC% & exit /b 1)
set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"
"%CSC%" /nologo /target:winexe /platform:x86 /optimize+ /codepage:65001 /win32manifest:"%~dp0app.manifest" /out:"%OUT%\Div2RebornLauncher.exe" /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll "%~dp0Launcher.cs" "%~dp0ReShade.cs"
exit /b %ERRORLEVEL%
