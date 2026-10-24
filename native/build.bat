@echo off
setlocal enabledelayedexpansion
set "SRC=%~dp0src"
set "OUT=%~dp0build"
set "PF86=%ProgramFiles(x86)%"

rem ---------- locate vcvars64.bat ----------
set "VCVARS="
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS if exist "!PF86!\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!PF86!\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS goto novcvars

echo [*] vcvars : !VCVARS!
call "!VCVARS!" >nul 2>&1
if errorlevel 1 goto novcvars
if not exist "!OUT!" mkdir "!OUT!"
if not exist "!OUT!\ui" mkdir "!OUT!\ui"
if not exist "!OUT!\themes" mkdir "!OUT!\themes"

echo [*] compile ...
cl /nologo /std:c++17 /O2 /EHsc /W1 /utf-8 /D_UNICODE /DUNICODE /DWIN32_LEAN_AND_MEAN /DMG_ARCH=MG_ARCH_WIN32 /I"!SRC!" /I"!SRC!\vendor" "!SRC!\main.cpp" "!SRC!\vendor\mongoose.c" /Fe:"!OUT!\coolleo-svc.exe" /Fo:"!OUT!\\" /link user32.lib shell32.lib shlwapi.lib gdi32.lib gdiplus.lib setupapi.lib hid.lib ws2_32.lib dwmapi.lib pdh.lib iphlpapi.lib ole32.lib oleaut32.lib d3d11.lib dxgi.lib advapi32.lib winhttp.lib
if errorlevel 1 goto failed

echo [*] copy ui ...
copy /y "%~dp0ui\index.html" "!OUT!\ui\index.html" >nul

echo.
echo [OK] !OUT!\coolleo-svc.exe
exit /b 0

:novcvars
echo [X] vcvars64.bat not found
echo     install Visual Studio 2019/2022 with the C++ build tools
echo     https://visualstudio.microsoft.com/visual-cpp-build-tools/
exit /b 1

:failed
echo [X] build failed
exit /b 1
