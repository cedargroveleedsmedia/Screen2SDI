# NDI2SDI

> [!WARNING]
> **Experimental.** This was built for one church's playout PC and has been tested on a single
> setup (see *Tested with* below). It is shared as-is, with **no warranty and no support**. Test
> it thoroughly before relying on it for a live service, and keep a fallback ready.

A small Windows tray app that plays up to **three** pictures out as SDI through a Blackmagic DeckLink
card. Each output takes either a **Windows display** (captured directly, lowest delay; e.g. a virtual
monitor that WorshipTools Presenter puts an output on) or an **NDI source**. It is built to run
unattended: it starts with Windows, outputs black while a source is missing, reconnects on its own,
and never shows error pop-ups.

| Tray icon | Meaning |
|---|---|
| Green | Every enabled output is live |
| Yellow | At least one output is showing **black** while it waits for its source |
| Red | At least one output is not running: DeckLink error (retrying), stopped, or not configured |

Hover the icon for one line per output, e.g. `Main: Live` / `Live: Black (waiting)`. Settings shows
the full status of each output.
Right-click for **Settings…**, **Start/Stop Output**, **Open Log Folder**, **Run at Windows Startup**
and **Exit**. Double-click opens Settings.

### Tested with

| | |
|---|---|
| OS | Windows 10 Pro 22H2 (x64) |
| Card | Blackmagic DeckLink Duo (sub-devices used independently) |
| Drivers | Desktop Video 15.1 |
| NDI | NDI 6 SDK / Runtime / Tools 6.3.2 |
| Build | Visual Studio 2022 Build Tools 17.14 |
| Sources | WorshipTools Presenter on a Virtual Display Driver monitor, and Presenter's NDI output |

Other DeckLink models, Windows 11 and other NDI senders should work but have **not** been tested.

---

## 0. Quick start (setup script)

On a 64-bit Windows 10/11 PC with a DeckLink card installed:

1. Download this repository: **Code → Download ZIP**, then extract it somewhere permanent
   (for example `C:\NDI2SDI-src`), or `git clone https://github.com/cedargroveleedsmedia/NDI2SDI.git`.
2. Double-click **`setup.bat`** (or run `setup.bat -Install -WithVirtualDisplays` from a
   terminal; the options are listed below). Accept the Administrator (UAC) prompt.
3. The script checks each prerequisite and installs what it can:

   | Prerequisite | What the script does |
   |---|---|
   | Visual Studio 2022 Build Tools (C++ + CMake) | Installs it with winget, or adds missing parts to an existing VS 2022. Large download (10–30 min). |
   | Blackmagic **Desktop Video** | Detects it. If missing, opens the download page and stops: Blackmagic requires a form, so install it yourself, **restart**, and run setup again. |
   | **NDI 6 SDK** | Detects it. If missing, opens ndi.video and stops (free, asks for an e-mail). Install it and run setup again. |
   | **NDI 6 Runtime** | Detects it; if missing, offers to run the official runtime installer (you accept NDI's licence there). Not needed if you only use display capture. |
   | Virtual Display Driver | Only with `-WithVirtualDisplays`: installs it with winget. Then set the monitor count (section 2, step 3). |

4. It then builds with `build.bat`, runs the unit tests, and with `-Install` copies the exe to
   `C:\Program Files\NDI2SDI`, adds a firewall rule for NDI (Private networks only) and starts it.
5. Continue with **section 2, steps 3–6** (virtual monitors, Settings, auto sign-in, power plan).

Re-running the script is safe: finished steps are skipped. If a step fails, the manual
instructions in sections 1 and 2 below cover the same ground.

| Option | Effect |
|---|---|
| *(none)* | Check/install prerequisites, build and test. Nothing is installed to Program Files. |
| `-Install` | Also install to `C:\Program Files\NDI2SDI` (keeps the previous exe as `.bak`), add the firewall rule, and start the app. |
| `-WithVirtualDisplays` | Also install the Virtual Display Driver. |
| `-SkipPrereqs` | Only build (no admin rights needed unless `-Install`). |
| `-SkipBuild` | Don't build; with `-Install`, install the last build. |

If PowerShell refuses to run scripts, use `setup.bat`, or
`powershell -ExecutionPolicy Bypass -File setup.ps1`. If Windows says the files are from the
internet, right-click the ZIP → *Properties* → **Unblock** before extracting.

---

## 1. Building

### Prerequisites (on the build PC)

| What | Notes |
|---|---|
| Visual Studio 2022 **or** VS 2022 Build Tools with the *Desktop development with C++* workload (includes *C++ CMake tools* and a Windows SDK) | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"` |
| NDI 6 SDK | https://ndi.video/for-developers/ndi-sdk/ (installs to `C:\Program Files\NDI\NDI 6 SDK` and sets `NDI_SDK_DIR`). Only the headers are used. |
| Blackmagic Desktop Video (drivers) | The DeckLink API headers are generated at compile time from the type library that Desktop Video registers (`#import` in `src/DeckLink.cpp`), so the separate DeckLink SDK download is not needed. |

### Build

Double-click `build.bat`, or from a terminal:

```bat
cd C:\path\to\NDI2SDI
build.bat
```

That configures CMake (Visual Studio 2022 generator, x64), builds Release and prints the result:

```
build\bin\Release\NDI2SDI.exe        the app (the only file you need to deploy)
build\bin\Release\NDI2SDI_tests.exe  unit tests for pixel conversion / JSON (should print "All tests passed")
```

Manual equivalent (from a *x64 Native Tools* prompt):

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

You can also open the folder in Visual Studio 2022 (*File → Open → Folder*) and build there.
If the NDI SDK is somewhere unusual, add `-DNDI_SDK_DIR="D:\path\to\NDI 6 SDK"`.

The build uses `/W4 /WX` (warnings are errors) and a static C runtime, so the exe has no
Visual C++ redistributable dependency.

> **Rebuilding while the app runs fails** (the exe is locked). Exit it from the tray first.

---

## 2. Installing on the playout PC

1. **Drivers/runtime** (once):
   - Blackmagic **Desktop Video** (https://www.blackmagicdesign.com/support/family/capture-and-playback).
     Restart after installing, then check the card in *Desktop Video Setup*.
   - The **NDI runtime**. Either NDI Tools (https://ndi.video/tools/), or the stand-alone
     `NDI 6 Runtime.exe` from the SDK's `Redist` folder (also at http://ndi.link/NDIRedistV6).
     NDI2SDI loads `Processing.NDI.Lib.x64.dll` from `%NDI_RUNTIME_DIR_V6%` at run time, as the NDI
     SDK recommends, so **no NDI DLL is copied next to the exe**.
2. **Copy** `NDI2SDI.exe` to a permanent folder, e.g. `C:\Program Files\NDI2SDI\NDI2SDI.exe`.
   That is the only file.
3. **Virtual monitors** (for display capture without physical screens): install the
   [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) and set the
   number of monitors in `C:\VirtualDisplayDriver\vdd_settings.xml` (`<count>3</count>`, with a
   `1920x1080` / `60` resolution listed first), then restart the driver
   (`pnputil /restart-device "ROOT\DISPLAY\0001"` as admin, or Device Manager → Display adapters →
   Virtual Display Driver → Disable/Enable). In Presenter, put each output full screen on its own
   virtual monitor. NDI2SDI lists them as **Virtual 1/2/3** (left to right).
4. **Run it once**. Settings opens on first run. For each of the three rows:
   - **On**, and a **Name** (e.g. `Main`, `Live`; shown in the tooltip, status and log).
   - **Source**: `Display` + the monitor to capture, or `NDI` + the source name (pick it, or type the
     exact name, e.g. `PRESENTER-PC (Main Audience Output)`, if it is offline).
     **Refresh lists** re-reads NDI sources and displays.
   - **SDI out** and **Format**, e.g. `Projector` and `1080p59.94`. Each output needs its own connector.
   - Leave **Start output automatically** and **Run at Windows startup** ticked.
   - Click **Save**. Changed outputs restart immediately; unchanged ones keep running.
5. If Windows Firewall asks about NDI2SDI, **allow it on Private networks**. NDI discovery
   needs this when the source is on another PC.
6. Make the PC boot straight to the desktop so the tray app starts with no clicks:
   - **Automatic sign-in**: the app runs at user logon (HKCU Run key). Use Sysinternals
     *Autologon* or `netplwiz`.
   - Power plan: **never sleep**; turn off USB/PCIe power saving if possible.
   - Windows Update: set *active hours* to cover services so it never restarts mid-service.
7. Optional: `NDI2SDI.exe --diag` writes the DeckLink devices, formats, Windows displays and visible
   NDI sources to the log without touching the output (safe while the app runs). Use it to check names after hardware changes.

**Moving the exe:** the startup entry stores the exe's full path. After moving the exe, open
Settings and click Save (with *Run at Windows startup* ticked) to update it.

### Files

| Path | Contents |
|---|---|
| `%APPDATA%\NDI2SDI\config.json` | Settings: per output name, source (display device path or NDI name), device + persistent ID, format; buffer, auto-start. A 1.0 config is migrated to Output 1 automatically. |
| `C:\VirtualDisplayDriver\vdd_settings.xml` | Virtual monitor count and resolutions (Virtual Display Driver) |
| `%APPDATA%\NDI2SDI\logs\NDI2SDI_YYYY-MM-DD.log` | Daily logs, last 7 days kept |
| `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\NDI2SDI` | `"<exe>" --tray` when *Run at Windows startup* is on |

### Command line

| Argument | Effect |
|---|---|
| *(none)* | Start in the tray; open Settings if there is no config yet. If already running, opens the running copy's Settings. |
| `--tray` | Start hidden in the tray (used by the startup entry). If already running, does nothing. |
| `--diag` | Log devices/formats/NDI sources and exit (never enables output). |

---

## 3. How it works

- **The DeckLink is the master clock.** Playback is scheduled with a small preroll (Settings →
  *Output buffer*, 2–5 frames, default 2). Every `ScheduledFrameCompleted` callback refills that same
  frame and schedules it at the end of the queue. 2 frames is the minimum (one on air, one being
  filled). Raise it only if the log shows late/dropped frames on a busy PC.
- **Outputs**: each of the three outputs runs on its own thread with its own DeckLink sub-device, so
  one output failing or restarting never affects the others.
- **Display capture**: DXGI Desktop Duplication grabs the chosen monitor on a worker thread as soon as
  Windows composes a new frame (mouse pointer not included). Each DeckLink frame takes the newest
  capture; while the screen is unchanged the previous conversion is reused, so static slides cost
  almost no CPU. Monitors are remembered by device path, which survives reboots and display
  renumbering. If the monitor disappears the output goes black and keeps looking for it; during a
  UAC prompt or the lock screen the last picture is held. Virtual monitors run at 60 Hz, so at
  59.94 one frame is repeated about every 17 s (invisible on slides); 1080p60 avoids that if the
  downstream gear accepts it.
- **NDI frame-sync**: each DeckLink frame pulls the right NDI frame with
  `NDIlib_framesync_capture_video` (time-base corrected), which absorbs clock drift between the
  sender and the card. There is no NDI receive loop pushing frames.
- **Pixels**: NDI is asked for `UYVY_BGRA`. UYVY is copied row by row (respecting stride) to
  `bmdFormat8BitYUV`. BGRA/BGRX/RGBA/RGBX is converted to UYVY (BT.709, limited range), with any
  alpha composited over black. If the size differs from the output format, the image is scaled
  bilinearly and letterboxed/pillarboxed to keep its aspect ratio. Interlaced formats get
  interleaved frames from frame-sync.
- **No source / source lost**: black is output, the receiver keeps looking for the source *by name*
  forever and is rebuilt every 30 s while disconnected (this covers network changes). The SDI signal
  itself never stops.
- **DeckLink problems** (device busy, missing, playback stalls): the tray goes red, the reason is
  logged, and output restarts automatically with backoff (1 s → 10 s). A watchdog restarts output
  if the frame callbacks stop for 3 s.
- **Crash safety**: an unhandled crash is logged and the app relaunches itself in the tray (if it
  had been running for over 30 s, to avoid a restart loop).
- **Audio**: none (video only). Simplest and most stable.
- **Shutdown** (Exit, logoff, Windows shutdown): stops playback, disables output, releases COM
  objects and destroys NDI objects.
- Once per minute (first report 15 s after output starts) the log records output frames, live vs
  black frames, late/dropped frames, resyncs, buffer depth and NDI receive stats, plus a
  **Latency** line per output: the age of each new frame (NDI: from the sender's submit; display: from
  Windows presenting it), plus the time it waits in the DeckLink queue. Typical: display ~25-30 ms,
  NDI ~40-60 ms. This covers everything NDI2SDI controls. It does not include
  anything the sender does *before* submitting the frame, the card's own frame of output delay, or
  the processing delay of the projector/TV. (The age is only valid when the sender's clock matches,
  e.g. same PC.)

### Source layout

```
CMakeLists.txt, build.bat
setup.bat, setup.ps1   prerequisite check/install, build, test, optional install (section 0)
res/      app.rc (dialog, icons, version), app.manifest, *.ico, resource.h
tools/    make_icons.ps1 (regenerates the status icons)
tests/    test_main.cpp (pixel conversion + JSON tests)
src/
  main.cpp            entry, single-instance mutex, --tray/--diag, crash relaunch
  TrayApp.*           hidden window, tray icon + menu, session end, Explorer restarts
  SettingsDialog.*    modeless Settings window
  Pipeline.*          one per output: start/stop/retry/watchdog, status, per-minute stats
  NdiLib.*            dynamic NDI runtime loading, shared source finder
  NdiReceiver.*       connect by name, frame-sync, reconnect forever, renders into DeckLink frames
  Displays.*          active monitors with stable device paths, "Virtual N" labels
  DisplayCapture.*    DXGI Desktop Duplication worker + triple buffer, renders into DeckLink frames
  DeckLink.*          device/format enumeration, scheduled playback (all DeckLink COM code)
  FrameConvert.*      UYVY copy, BGRA→UYVY, bilinear scale + letterbox
  Config.*, Json.*    %APPDATA% JSON settings (atomic save), minimal JSON reader/writer
  Autostart.*         HKCU Run key
  Log.*               daily rotating log files
```

---

## 4. Troubleshooting

| Symptom | Likely cause |
|---|---|
| Red, *"… is in use by another application"* | Another program (ProPresenter, Media Express, an old capture tool…) holds that DeckLink sub-device. Close it, or pick another output. NDI2SDI keeps retrying. |
| Red, *"DeckLink device '…' not found"* | The sub-device was renamed in Desktop Video Setup, or the card's profile changed. Open Settings and pick it again. |
| Yellow forever | The source name doesn't match exactly, Presenter's NDI output is off, or the firewall blocks NDI. Open Settings → Refresh to see what is visible. |
| Source on another subnet never shows | Use NDI Access Manager (NDI Tools) to add the sender's IP. |
| *"Not saved: … both use …"* in Settings | Two enabled outputs point at the same SDI connector. Give each its own. |
| Display output black, *"display … not found"* | The virtual monitor is gone (driver disabled, count lowered) or was replaced. Settings → Refresh lists, re-pick it. |
| Display output black, *"access denied"* | Windows is on the secure desktop (UAC prompt, lock screen, Ctrl+Alt+Del). Capture resumes by itself. |
| SDI is behind the local screen (NDI source) | Switch that output to **Display** capture (step 3), which skips NDI entirely. Otherwise: |
| SDI is behind the local screen | Check the log's `Latency:` line (NDI2SDI's own share, typically ~60 ms at 1080p59.94 with a 2-frame buffer). For the rest, open **NDI Studio Monitor** on the same source next to Presenter's screen: if it lags just as much, the delay is in Presenter's NDI output. Otherwise it's downstream (converter, scaler, projector/TV processing). Use the display's game/low-latency mode. |

---

## 5. Test checklist

Watch the SDI output on a monitor and the tray icon. Each step should also leave a line in the
log (*Open Log Folder*).

| # | Test | Expected |
|---|---|---|
| 1 | **Cold boot with Presenter not running** | Desktop appears; within a few seconds SDI shows **black**, icon **yellow**, tooltip *"Waiting for … (black)"*. Log: `DeckLink: output started`, `Status: waiting`. |
| 2 | **Start Presenter** (NDI output on) | Live within a few seconds; icon **green**. Log: `NDI: connected`, `NDI: receiving 1920x1080 …`, `Status: live`. |
| 3 | **Close Presenter** | SDI goes **black** (signal stays up); icon **yellow**. Log: `lost connection`. **Reopen Presenter** → live again automatically. |
| 4 | **Unplug the network** (remote source only) | Black + yellow; plug back in → live again (within ~30 s at worst). |
| 5 | **Change the format in Settings** (e.g. 1080p29.97 → 1080i59.94) → Save | Output switches within a second or two, no app restart. Tooltip shows the new format. Log: `Applying config`, `output started … 1080i59.94`. |
| 6 | **Reboot** | Signs in automatically, icon appears in the tray, output starts with **no clicks** (yellow → green once Presenter is up). |
| 7 | Launch NDI2SDI.exe a second time | No second copy; the running copy's Settings window comes to the front. |
| 8 | Right-click → Stop Output / Start Output | Red *"Output stopped"* / back to yellow or green. |
| 9 | Right-click → Exit | Icon disappears; log ends with `Shutdown complete`. |
| 10 | Put a moving picture on a captured virtual monitor | SDI follows it smoothly; log `Latency: display frame age …` ~15-20 ms. |
| 11 | Set two outputs to the same SDI out → Save | Not saved; status shows which outputs clash. |
| 12 | Turn one output off → Save | Only that output stops; the others keep running without a glitch. |
| 13 | Wait 1+ minute while live | Log has a `Stats/min:` line with `late=0 dropped=0` (occasional late frames under heavy load are tolerated). |

---

## 6. Status, disclaimer and trademarks

- **Experimental**: provided "as is", without warranty of any kind. Use at your own risk. Issues and
  pull requests are welcome, but there is no promise of support or fixes.
- **No open-source licence is granted.** The code is published so others can read it and learn from
  it; all rights are reserved. Please ask if you would like to reuse it.
- This repository contains **no** NDI SDK or Blackmagic SDK files. You download those yourself
  under their own licences; the setup script only opens their official pages or runs their official
  installers.
- NDI® is a registered trademark of Vizrt NDI AB (https://ndi.video). Blackmagic Design, DeckLink
  and Desktop Video are trademarks of Blackmagic Design Pty. Ltd. WorshipTools is a trademark of its
  owner. This project is not affiliated with or endorsed by any of them.
