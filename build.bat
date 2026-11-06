@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ build tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist src\app.ico powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 || exit /b 1
if not exist build mkdir build

rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl /nologo /utf-8 /O1 /MT /EHsc /std:c++17 /W3 /GL /DUNICODE /D_UNICODE /Fobuild\ /Febuild\HoverPreview.exe src\main.cpp build\app.res /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF || exit /b 1

echo.
echo Built build\HoverPreview.exe
