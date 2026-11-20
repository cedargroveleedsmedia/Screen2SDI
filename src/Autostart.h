#pragma once

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run\NDI2SDI = "<exe>" --tray
bool IsRunAtStartupEnabled();
bool SetRunAtStartup(bool enable);
