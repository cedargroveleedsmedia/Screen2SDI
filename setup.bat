@echo off
rem Double-click to set up Screen2SDI. Passes any arguments on, e.g.:
rem   setup.bat -Install -WithVirtualDisplays
rem See setup.ps1 / README.md. EXPERIMENTAL.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
