@echo off
setlocal
set "KITS=%ProgramFiles(x86)%\Windows Kits\10\UnionMetadata"
set "FACADES=%ProgramFiles(x86)%\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8\Facades"
if not exist "%FACADES%\System.Runtime.InteropServices.WindowsRuntime.dll" set "FACADES=%ProgramFiles(x86)%\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.7.2\Facades"
set "RUNTIME=%SystemRoot%\Microsoft.NET\assembly\GAC_MSIL\System.Runtime.WindowsRuntime\v4.0_4.0.0.0__b77a5c561934e089\System.Runtime.WindowsRuntime.dll"
set "CSC=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
for /f "delims=" %%V in ('dir /b /ad /o-n "%KITS%\10.*" 2^>nul') do if not defined SDK_WINMD if exist "%KITS%\%%V\Windows.winmd" set "SDK_WINMD=%KITS%\%%V\Windows.winmd"
if not defined SDK_WINMD (
  echo Windows SDK metadata was not found.
  exit /b 1
)
if not exist "%FACADES%\System.Runtime.InteropServices.WindowsRuntime.dll" (
  echo Install the .NET Framework 4.8 or 4.7.2 Developer Pack.
  exit /b 1
)
pushd "%~dp0"
"%CSC%" /nologo /target:exe /out:"%~dp0RingBridge.exe" /reference:"%SDK_WINMD%" /reference:"%RUNTIME%" /reference:"%FACADES%\System.Runtime.dll" /reference:"%FACADES%\System.Runtime.InteropServices.WindowsRuntime.dll" RingBridge.cs
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
