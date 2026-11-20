@echo off
rem Builds Screen2SDI (Release, x64) with Visual Studio 2022 or the VS 2022 Build Tools.
rem Output: build\bin\Release\Screen2SDI.exe
rem   build.bat         include NDI if the NDI SDK is installed, otherwise display capture only
rem   build.bat nondi   always build without NDI (display capture only)
setlocal
set "NONDI=OFF"
if /i "%~1"=="nondi" set "NONDI=ON"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio 2022 / Build Tools not found.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo The C++ build tools workload is not installed.
  exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DSCREEN2SDI_WITHOUT_NDI=%NONDI% || exit /b 1
cmake --build build --config Release || exit /b 1
echo.
echo Built: %~dp0build\bin\Release\Screen2SDI.exe
