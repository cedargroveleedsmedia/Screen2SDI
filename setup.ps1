<#
.SYNOPSIS
  Screen2SDI Windows setup: checks/installs prerequisites, builds, and (optionally) installs the app.

.DESCRIPTION
  EXPERIMENTAL. Read README.md first.

  What it does, in order:
    1. Checks Windows x64 and winget. Re-launches itself as Administrator if needed.
    2. Visual Studio 2022 Build Tools (C++ workload + CMake): installs with winget, or adds the
       missing components to an existing Visual Studio 2022.
    3. Blackmagic Desktop Video: detects it. It cannot be downloaded automatically (Blackmagic
       requires a registration form), so the script opens the download page and stops.
    4. NDI 6 SDK (OPTIONAL): NDI is NOT required. Without it the app is built for display capture
       only (virtual or physical monitor -> SDI). If the SDK is missing the script asks whether to
       continue without NDI, or opens the download page (it needs a form on ndi.video).
    5. NDI 6 Runtime (only if building with NDI): detects it; if missing, offers to run the runtime
       installer (from the SDK's Redist folder, or downloaded from ndi.link).
    6. Virtual Display Driver (only with -WithVirtualDisplays): installs it with winget.
    7. Builds with build.bat and runs the unit tests.
    8. With -Install: copies Screen2SDI.exe to "C:\Program Files\Screen2SDI", adds a Windows Firewall
       rule (Private networks) for NDI discovery, and starts the app as the signed-in user.

  Run it again after installing anything it asked you to install by hand. Every step that is
  already done is skipped.

.PARAMETER Install
  Copy the built exe to C:\Program Files\Screen2SDI, add the firewall rule and start it.

.PARAMETER NoStart
  With -Install: install but do NOT start the app (e.g. while another program still uses the
  DeckLink). Start it later from "C:\Program Files\Screen2SDI\Screen2SDI.exe".

.PARAMETER WithVirtualDisplays
  Also install the Virtual Display Driver (virtual monitors for display capture).

.PARAMETER VirtualMonitors
  Set the number of virtual monitors (1-16) and restart the Virtual Display Driver. All virtual
  monitors blink while it restarts. (Also possible later from Screen2SDI's Settings window.)

.PARAMETER NoNdi
  Build without NDI (display capture only) and skip the NDI SDK/runtime checks entirely.

.PARAMETER SkipPrereqs
  Do not check or install prerequisites; just build (and install if -Install).

.PARAMETER SkipBuild
  Do not build. With -Install, installs the exe that was built last time.

.EXAMPLE
  setup.bat
  setup.bat -Install -WithVirtualDisplays
  setup.bat -Install -WithVirtualDisplays -NoNdi      (virtual monitor -> SDI only, no NDI at all)
  powershell -ExecutionPolicy Bypass -File setup.ps1 -Install
#>
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$NoStart,
    [switch]$WithVirtualDisplays,
    [ValidateRange(0, 16)][int]$VirtualMonitors = 0,
    [switch]$NoNdi,
    [switch]$SkipPrereqs,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$InstallDir = Join-Path $env:ProgramFiles 'Screen2SDI'
$BuiltExe = Join-Path $Root 'build\bin\Release\Screen2SDI.exe'
$TestsExe = Join-Path $Root 'build\bin\Release\Screen2SDI_tests.exe'
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$VsSetup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
$VsComponents = @(
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    'Microsoft.VisualStudio.Component.VC.CMake.Project',
    'Microsoft.VisualStudio.Component.Windows11SDK.22621'
)
$DeckLinkTypeLib = 'Registry::HKEY_CLASSES_ROOT\TypeLib\{D864517A-EDD5-466D-867D-C819F1C052BB}'

function Step($msg) { Write-Host ''; Write-Host "==> $msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "    OK   $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "    WARN $msg" -ForegroundColor Yellow }
function Bad($msg)  { Write-Host "    MISSING $msg" -ForegroundColor Red }
function Ask($msg)  { $a = Read-Host "    $msg [Y/n]"; return ($a -eq '' -or $a -match '^[Yy]') }
function Pause-Exit($code) {
    Write-Host ''
    if ($Host.Name -eq 'ConsoleHost') { Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}
function Get-MachineEnv($name) {
    $v = [Environment]::GetEnvironmentVariable($name, 'Machine')
    if (-not $v) { $v = [Environment]::GetEnvironmentVariable($name, 'Process') }
    return $v
}

Write-Host '------------------------------------------------------------------' -ForegroundColor Yellow
Write-Host ' Screen2SDI setup  -  EXPERIMENTAL software, no warranty. See README.md' -ForegroundColor Yellow
Write-Host '------------------------------------------------------------------' -ForegroundColor Yellow

# ---------------------------------------------------------------- 1. basics + elevation
Step 'Checking Windows'
if (-not [Environment]::Is64BitOperatingSystem) { Bad 'Screen2SDI needs 64-bit Windows 10 or 11.'; Pause-Exit 1 }
$os = Get-CimInstance Win32_OperatingSystem
Ok "$($os.Caption) build $($os.BuildNumber)"
if ([int]$os.BuildNumber -lt 17763) { Warn 'Windows 10 1809 or newer is recommended.' }

$needAdmin = (-not $SkipPrereqs) -or $Install
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($needAdmin -and -not $isAdmin) {
    Step 'Restarting as Administrator (needed to install software)'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"")
    foreach ($p in $PSBoundParameters.Keys) {
        $v = $PSBoundParameters[$p]
        if ($v -is [System.Management.Automation.SwitchParameter]) { if ($v.IsPresent) { $argList += "-$p" } }
        else { $argList += "-$p"; $argList += "$v" }
    }
    try {
        Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs | Out-Null
    } catch {
        Bad 'Administrator rights were refused. Re-run and accept the UAC prompt, or use -SkipPrereqs to only build.'
        Pause-Exit 1
    }
    exit 0
}

$missing = @()

if (-not $SkipPrereqs) {
    Step 'Checking winget (App Installer)'
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Bad 'winget not found. Install "App Installer" from the Microsoft Store, then run setup again.'
        Start-Process 'ms-windows-store://pdp/?ProductId=9NBLGGH4NNS1'
        Pause-Exit 1
    }
    Ok 'winget found'

    # ------------------------------------------------------------ 2. VS 2022 Build Tools
    Step 'Visual Studio 2022 C++ build tools + CMake'
    $vsAll = $null
    if (Test-Path $VsWhere) {
        $vsAll = & $VsWhere -version '[17.0,18.0)' -products * -property installationPath | Select-Object -First 1
    }
    $vsOk = $null
    if (Test-Path $VsWhere) {
        $req = @(); foreach ($c in $VsComponents[0..1]) { $req += '-requires'; $req += $c }
        $vsOk = & $VsWhere -version '[17.0,18.0)' -products * @req -property installationPath | Select-Object -First 1
    }
    if ($vsOk) {
        Ok "Found: $vsOk"
    } elseif ($vsAll) {
        Warn "Visual Studio 2022 found at $vsAll but the C++ tools or CMake component is missing. Adding them (this can take 10+ minutes)..."
        $a = @('modify', '--installPath', "`"$vsAll`"", '--quiet', '--norestart', '--wait')
        foreach ($c in $VsComponents) { $a += '--add'; $a += $c }
        $p = Start-Process -FilePath $VsSetup -ArgumentList $a -Wait -PassThru
        if ($p.ExitCode -notin 0, 3010) { Bad "Visual Studio Installer exited with $($p.ExitCode)."; $missing += 'VS C++ tools' }
        else { Ok 'Components added' }
    } else {
        Warn 'Not found. Installing Visual Studio 2022 Build Tools with winget (large download, 10-30 minutes)...'
        $override = '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        foreach ($c in $VsComponents) { $override += " --add $c" }
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override $override
        if ($LASTEXITCODE -ne 0) { Bad "winget exited with $LASTEXITCODE."; $missing += 'VS Build Tools' }
        else { Ok 'Installed' }
    }

    # ------------------------------------------------------------ 3. Desktop Video
    Step 'Blackmagic Desktop Video (DeckLink driver)'
    if (Test-Path $DeckLinkTypeLib) {
        $dv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
              Where-Object { $_.DisplayName -eq 'Blackmagic Desktop Video' } | Select-Object -First 1
        if ($dv) { Ok "Desktop Video $($dv.DisplayVersion)" } else { Ok 'DeckLink API is registered' }
    } else {
        Bad 'Desktop Video is not installed (needed to build AND run).'
        Write-Host '    Download "Desktop Video" for Windows (latest), install it, restart the PC, then run setup again.'
        Write-Host '    https://www.blackmagicdesign.com/support/family/capture-and-playback'
        Start-Process 'https://www.blackmagicdesign.com/support/family/capture-and-playback'
        $missing += 'Desktop Video'
    }

    # ------------------------------------------------------------ 4. NDI SDK (optional)
    Step 'NDI 6 SDK (OPTIONAL - only for NDI sources; display capture works without it)'
    $sdk = $null
    if ($NoNdi) {
        Ok 'Skipped (-NoNdi): building for display capture only'
    } else {
        foreach ($cand in @((Get-MachineEnv 'NDI_SDK_DIR'), "$env:ProgramFiles\NDI\NDI 6 SDK", "$env:ProgramFiles\NDI\NDI 5 SDK")) {
            if ($cand -and (Test-Path (Join-Path $cand 'Include\Processing.NDI.Lib.h'))) { $sdk = $cand; break }
        }
        if ($sdk) {
            Ok "$sdk (NDI sources will be available)"
        } else {
            Warn 'NDI SDK not installed. That is fine if you only need monitor -> SDI.'
            if (Ask 'Continue WITHOUT NDI (display capture only)?') {
                $NoNdi = $true
                Ok 'Building for display capture only'
            } else {
                Write-Host '    Download the "NDI SDK" for Windows (free, asks for an e-mail), install it, then run setup again.'
                Write-Host '    https://ndi.video/for-developers/ndi-sdk/'
                Start-Process 'https://ndi.video/for-developers/ndi-sdk/'
                $missing += 'NDI SDK'
            }
        }
    }

    # ------------------------------------------------------------ 5. NDI runtime
    Step 'NDI 6 Runtime (only for NDI sources; NDI Tools also includes it)'
    $rt = Get-MachineEnv 'NDI_RUNTIME_DIR_V6'
    if ($NoNdi) {
        Ok 'Skipped (display capture only)'
    } elseif ($rt -and (Test-Path (Join-Path $rt 'Processing.NDI.Lib.x64.dll'))) {
        Ok $rt
    } elseif ($sdk) {
        Warn 'NDI runtime not found. Display capture outputs work without it; NDI sources do not.'
        if (Ask 'Run the NDI 6 Runtime installer now?') {
            $redist = $null
            if ($sdk) { $redist = Get-ChildItem (Join-Path $sdk 'Redist') -Filter '*Runtime*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 }
            if ($redist) {
                $exe = $redist.FullName
            } else {
                $exe = Join-Path $env:TEMP 'NDI6Runtime.exe'
                Write-Host '    Downloading from http://ndi.link/NDIRedistV6 ...'
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -Uri 'http://ndi.link/NDIRedistV6' -OutFile $exe -UseBasicParsing
            }
            Write-Host "    Starting $exe (accept the NDI licence in the installer)"
            Start-Process -FilePath $exe -Wait
            $rt = Get-MachineEnv 'NDI_RUNTIME_DIR_V6'
            if ($rt) { Ok "Installed: $rt" } else { Warn 'Runtime still not detected. A sign-out/sign-in may be needed.' }
        }
    }

    # ------------------------------------------------------------ 6. Virtual Display Driver
    Step 'Virtual Display Driver (optional virtual monitors)'
    $vdd = Get-PnpDevice -FriendlyName 'Virtual Display Driver' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vdd) {
        Ok "Installed ($($vdd.InstanceId), $($vdd.Status))"
    } elseif ($WithVirtualDisplays) {
        winget install --id VirtualDrivers.Virtual-Display-Driver -e --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) { Warn "winget exited with $LASTEXITCODE. Install it by hand: https://github.com/VirtualDrivers/Virtual-Display-Driver" }
        else { Ok 'Installed. Set the monitor count in C:\VirtualDisplayDriver\vdd_settings.xml (see README, step 3).' }
    } else {
        Write-Host '    Not installed. Only needed to capture a screen that has no physical monitor.'
        Write-Host '    Re-run with -WithVirtualDisplays to install it.'
    }

    if ($VirtualMonitors -gt 0) {
        Step "Setting the number of virtual monitors to $VirtualMonitors"
        $xmlPath = $null
        $reg = Get-ItemProperty 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver' -ErrorAction SilentlyContinue
        if ($reg -and $reg.VDDPATH -and (Test-Path (Join-Path $reg.VDDPATH 'vdd_settings.xml'))) { $xmlPath = Join-Path $reg.VDDPATH 'vdd_settings.xml' }
        elseif (Test-Path 'C:\VirtualDisplayDriver\vdd_settings.xml') { $xmlPath = 'C:\VirtualDisplayDriver\vdd_settings.xml' }
        if (-not $xmlPath) {
            Warn 'vdd_settings.xml not found (is the Virtual Display Driver installed?). Skipped.'
        } else {
            $text = [IO.File]::ReadAllText($xmlPath)
            $rx = [regex]'(?s)(<monitors>.*?<count>\s*)(\d+)(\s*</count>)'
            $m = $rx.Match($text)
            if (-not $m.Success) {
                Warn "No <monitors><count> in $xmlPath. Skipped."
            } elseif ([int]$m.Groups[2].Value -eq $VirtualMonitors) {
                Ok "Already $VirtualMonitors"
            } else {
                $bak = "$xmlPath.bak-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
                Copy-Item $xmlPath $bak
                $new = $text.Substring(0, $m.Groups[2].Index) + $VirtualMonitors + $text.Substring($m.Groups[2].Index + $m.Groups[2].Length)
                [IO.File]::WriteAllText($xmlPath, $new, (New-Object System.Text.UTF8Encoding($false)))
                Ok "$($m.Groups[2].Value) -> $VirtualMonitors in $xmlPath (backup: $bak)"
                $dev = Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\MttVDD' } | Select-Object -First 1
                if ($dev) {
                    Write-Host '    Restarting the Virtual Display Driver (virtual monitors blink)...'
                    $ErrorActionPreference = 'Continue'
                    pnputil /restart-device "$($dev.InstanceId)" | Out-Null
                    $rc = $LASTEXITCODE
                    $ErrorActionPreference = 'Stop'
                    if ($rc -ne 0) {
                        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
                        Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
                    }
                    Ok 'Driver restarted'
                } else {
                    Warn 'Driver device not found; the new count applies after a restart of the PC.'
                }
            }
        }
    }

    if ($missing.Count -gt 0) {
        Write-Host ''
        Write-Host "Still missing: $($missing -join ', ')" -ForegroundColor Red
        Write-Host 'Install them (the pages were opened for you), then run setup again. Nothing was built.' -ForegroundColor Red
        Pause-Exit 2
    }
}

# ---------------------------------------------------------------- 7. build + tests
if (-not $SkipBuild) {
    if ($NoNdi) { Step 'Building (Release, x64, display capture only - no NDI)' } else { Step 'Building (Release, x64)' }
    $running = Get-Process Screen2SDI -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $BuiltExe }
    if ($running) {
        Warn 'Screen2SDI is running from the build folder; the exe is locked. Exit it from the tray first.'
        Pause-Exit 1
    }
    $buildArg = ''
    if ($NoNdi) { $buildArg = ' nondi' }
    & cmd.exe /c "`"$Root\build.bat`"$buildArg"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $BuiltExe)) { Bad "Build failed (exit $LASTEXITCODE). See the output above."; Pause-Exit 1 }
    Ok $BuiltExe

    Step 'Running unit tests'
    $ErrorActionPreference = 'Continue'   # PS 5.1 would turn native stderr into terminating errors
    $out = & $TestsExe 2>&1 | Out-String
    $ErrorActionPreference = 'Stop'
    Write-Host $out.Trim()
    if ($LASTEXITCODE -ne 0) { Bad 'Unit tests failed.'; Pause-Exit 1 }
    Ok 'Tests passed'
}

# ---------------------------------------------------------------- 8. install
if ($Install) {
    Step "Installing to $InstallDir"
    if (-not (Test-Path $BuiltExe)) { Bad "No built exe at $BuiltExe. Run without -SkipBuild."; Pause-Exit 1 }
    $target = Join-Path $InstallDir 'Screen2SDI.exe'
    # NDI2SDI is this app's old name (up to 1.1). It must not run alongside the new one.
    $wasRunning = Get-Process Screen2SDI, NDI2SDI -ErrorAction SilentlyContinue
    if ($wasRunning) {
        Warn "$((($wasRunning | Select-Object -ExpandProperty ProcessName -Unique) -join ' and ')) is running. It will be closed so the new version can take over (SDI output stops briefly)."
        if (-not (Ask 'Continue?')) { Pause-Exit 1 }
        $wasRunning | Stop-Process -Force
        Start-Sleep -Milliseconds 800
    }
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    if (Test-Path $target) {
        $bak = "$target.bak"
        Copy-Item $target $bak -Force
        Ok "Previous version kept as $bak"
    }
    Copy-Item $BuiltExe $target -Force
    Ok $target

    Step 'Windows Firewall rule (Private networks, for NDI discovery)'
    if (-not (Get-NetFirewallRule -DisplayName 'Screen2SDI' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName 'Screen2SDI' -Direction Inbound -Program $target -Action Allow -Profile Private | Out-Null
        Ok 'Rule added'
    } else { Ok 'Rule already exists' }

    if ($NoStart) {
        Step 'Not starting Screen2SDI (-NoStart)'
        Ok "Installed only. Start it when ready: $target"
    } else {
        Step 'Starting Screen2SDI as the signed-in user'
        # explorer.exe starts it un-elevated in the user's session, so the tray icon and the
        # "Run at Windows startup" entry (HKCU) belong to the person at the desk, not the admin.
        Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$target`""
        Ok 'Started. Settings opens on the first run; see README section 2, step 4.'
    }

    $oldDir = Join-Path $env:ProgramFiles 'NDI2SDI'
    if (Test-Path $oldDir) {
        Write-Host ''
        Write-Host "    Note: the old version (NDI2SDI) is still in $oldDir." -ForegroundColor Yellow
        Write-Host '    Screen2SDI copied its settings and took over its startup entry. Once the new version' -ForegroundColor Yellow
        Write-Host '    works, you can delete that folder (and the "NDI2SDI" firewall rule, if any).' -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
if (-not $Install) { Write-Host "Run '$BuiltExe' to try it, or re-run setup with -Install." }
Pause-Exit 0
