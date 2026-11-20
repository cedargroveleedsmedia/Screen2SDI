#pragma once

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Screen2SDI = "<exe>" --tray
bool IsRunAtStartupEnabled();
bool SetRunAtStartup(bool enable);
