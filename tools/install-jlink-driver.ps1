<#
    install-jlink-driver.ps1
    -------------------------------------------------------------------------
    Installs the SEGGER J-Link USB driver so the probe stops showing
    "Code 28 - the drivers for this device are not installed".

    Why this is needed (measured on this machine, 2026-10-15):
      * probe enumerates as  USB\VID_1366&PID_0101\000020090928  with
        Status = Error, ProblemCode = 28  (CM_PROB_FAILED_INSTALL)
      * "pnputil /enum-drivers" lists 273 driver packages, ZERO of them SEGGER
      * JLink.exe starts but "ShowEmuList" returns an empty list
      => no SEGGER driver package is registered in the Windows driver store.

    Which .inf is the right one:
      USBDriver\x64\JLink.inf          line 126: USB\VID_1366&PID_0101   <== ours
      USBDriver\x64\JLinkWinUSB.inf    covers only PID_1020..106f        (no match)
      So JLink.inf (+ JLinkCDC.inf for the VCOM port) is what we register.

    What the script does:
      1. re-launches itself elevated (UAC prompt) if needed
      2. finds the newest  C:\Program Files\SEGGER\JLink_V*
      3. registers JLink.inf / JLinkCDC.inf with pnputil /add-driver /install
      4. restarts (or disable/enable) the probe device node
      5. falls back to SEGGER dpinst_x64.exe if pnputil did not bind it
      6. prints the resulting device status + "JLink.exe ShowEmuList" proof

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File .\install-jlink-driver.ps1
      (add  -NoPause  when calling it from another script/automation)
#>

[CmdletBinding()]
param(
    [string] $SeggerRoot = 'C:\Program Files\SEGGER',
    [string] $VidPid     = 'VID_1366',
    [switch] $NoPause
)

$ErrorActionPreference = 'Continue'

function Say-Head($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Say-Ok($t)   { Write-Host "[ OK ] $t"    -ForegroundColor Green }
function Say-Warn($t) { Write-Host "[WARN] $t"    -ForegroundColor Yellow }
function Say-Err($t)  { Write-Host "[FAIL] $t"    -ForegroundColor Red }

function Test-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-ProbeDevices {
    @(Get-PnpDevice -ErrorAction SilentlyContinue |
      Where-Object { $_.InstanceId -match $VidPid })
}

# ------------------------------------------------------------------ elevation
if (-not (Test-Elevated)) {
    Say-Warn 'Not elevated - re-launching with administrator rights (expect a UAC prompt)...'

    $childArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-SeggerRoot', ('"{0}"' -f $SeggerRoot),
        '-VidPid', $VidPid,
        '-NoPause'
    )
    try {
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $childArgs | Out-Null
    } catch {
        Say-Err ("Elevation was refused or failed: " + $_.Exception.Message)
        Say-Err 'Right-click the script and choose "Run as administrator" instead.'
    }
    exit 0
}

Say-Ok 'Running elevated.'

# ---- log everything so the outcome can be inspected afterwards -------------
$script:LogFile = ''
$transcriptPath = Join-Path $env:TEMP 'install-jlink-driver.log'
try {
    Start-Transcript -Path $transcriptPath -Force | Out-Null
    $script:LogFile = $transcriptPath
    Write-Host ("  transcript: " + $transcriptPath)
} catch {
    Write-Host ('  (could not start transcript: ' + $_.Exception.Message + ')')
}

# ------------------------------------------------------- locate the toolchain
Say-Head 'Locate SEGGER J-Link installation'

if (-not (Test-Path -LiteralPath $SeggerRoot)) {
    Say-Err "SEGGER root not found: $SeggerRoot"
    Say-Err 'Install the J-Link software package first, then re-run.'
    exit 1
}

$dirs = @(Get-ChildItem -LiteralPath $SeggerRoot -Directory -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -like 'JLink_V*' })

if ($dirs.Count -eq 0) {
    Say-Err "No 'JLink_V*' folder under $SeggerRoot"
    exit 1
}

# pick the highest version number (JLink_V942 -> 942)
$best = $null
$bestVer = -1
foreach ($d in $dirs) {
    $m = [regex]::Match($d.Name, '(\d+)')
    $v = 0
    if ($m.Success) { $v = [int]$m.Groups[1].Value }
    if ($v -gt $bestVer) { $bestVer = $v; $best = $d }
}

$jlinkDir = $best.FullName
$jlinkExe = Join-Path $jlinkDir 'JLink.exe'
Say-Ok "install dir : $jlinkDir"
if (-not (Test-Path -LiteralPath $jlinkExe)) {
    Say-Warn "JLink.exe not present in that folder - continuing anyway"
}

# ------------------------------------------------------------- pick inf folder
$archBits = 64
if ($env:PROCESSOR_ARCHITECTURE -eq 'x86') { $archBits = 32 }

$infCandidates = @()
if ($archBits -eq 64) { $infCandidates = @('x64', 'Vista_7_8_x64') }
else                 { $infCandidates = @('x86', 'Vista_7_8_x86') }

$infDir = $null
foreach ($c in $infCandidates) {
    $p = Join-Path $jlinkDir ('USBDriver\' + $c)
    if (Test-Path -LiteralPath $p) { $infDir = $p; break }
}

if ($null -eq $infDir) {
    Say-Err "No USBDriver\<arch> folder found under $jlinkDir"
    exit 1
}
Say-Ok "driver dir  : $infDir"

# ------------------------------------------------------ register driver packs
Say-Head 'Register driver packages (pnputil /add-driver /install)'

$pnputilOk = $true
foreach ($infName in @('JLink.inf', 'JLinkCDC.inf')) {
    $infPath = Join-Path $infDir $infName
    if (-not (Test-Path -LiteralPath $infPath)) {
        Say-Warn "missing: $infName"
        continue
    }
    Write-Host "  -> $infPath"
    $out = (& pnputil.exe /add-driver $infPath /install 2>&1 | Out-String)
    if ($out.Trim().Length -gt 0) { Write-Host ('     ' + $out.Trim()) }
    if ($LASTEXITCODE -ne 0) { $pnputilOk = $false }
}

if ($pnputilOk) { Say-Ok 'pnputil reported success.' }
else            { Say-Warn 'pnputil reported a non-zero exit code (see above).' }

# ------------------------------------------------------------- restart device
Say-Head 'Restart the probe device node'

$probes = Get-ProbeDevices
if ($probes.Count -eq 0) {
    Say-Warn 'No probe with that VID/PID is currently enumerated.'
    Say-Warn 'Plug the J-Link into USB and re-run this script.'
} else {
    foreach ($d in $probes) {
        Write-Host ("  restart: " + $d.InstanceId)
        $r1 = (& pnputil.exe /restart-device $d.InstanceId 2>&1 | Out-String)
        if ($LASTEXITCODE -ne 0) {
            # older Windows: fall back to disable/enable
            Write-Host '     pnputil /restart-device unavailable - trying disable/enable'
            try {
                Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction Stop
                Start-Sleep -Milliseconds 800
                Enable-PnpDevice  -InstanceId $d.InstanceId -Confirm:$false -ErrorAction Stop
            } catch {
                Write-Host ('     ' + $_.Exception.Message)
            }
        } elseif ($r1.Trim().Length -gt 0) {
            Write-Host ('     ' + $r1.Trim())
        }
    }
}

Start-Sleep -Seconds 2

# ---------------------------------------------------------- dpinst fallback
$probes = Get-ProbeDevices
$healthy = @($probes | Where-Object { $_.Status -eq 'OK' })

if ($healthy.Count -eq 0) {
    $dpinst = Join-Path $infDir 'dpinst_x64.exe'
    if (Test-Path -LiteralPath $dpinst) {
        Say-Head 'Fallback: SEGGER dpinst_x64.exe /S /SE'
        $out = (& $dpinst /S /SE 2>&1 | Out-String)
        if ($out.Trim().Length -gt 0) { Write-Host $out.Trim() }
        Start-Sleep -Seconds 2
        $probes = Get-ProbeDevices
        $healthy = @($probes | Where-Object { $_.Status -eq 'OK' })
    }
}

# ------------------------------------------------------------------- verdict
Say-Head 'Result'

$probes = Get-ProbeDevices
if ($probes.Count -eq 0) {
    Say-Err 'No J-Link device enumerated at all.'
    Say-Err 'Check the USB cable, then re-run.'
} else {
    foreach ($d in $probes) {
        $pc = ''
        if ($d.Status -ne 'OK') {
            $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue |
                 Where-Object { $_.KeyName -eq 'DEVPKEY_Device_ProblemCode' }
            if ($p) { $pc = ' problemCode=' + $p.Data }
        }
        Write-Host ("  {0,-7} {1}{2}" -f $d.Status, $d.InstanceId, $pc)
    }
}

if ($healthy.Count -gt 0) {
    Say-Ok 'J-Link driver is bound and the device is healthy.'

    Say-Head 'End-to-end proof: JLink.exe ShowEmuList'
    $tmp = Join-Path $env:TEMP ('jlink_probe_' + [guid]::NewGuid().ToString('N') + '.jlink')
    Set-Content -LiteralPath $tmp -Value "ShowEmuList`nexit" -Encoding Ascii
    $out = (& $jlinkExe -CommanderScript $tmp -ExitOnError 0 2>&1 | Out-String)
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    Write-Host $out.Trim()
    Say-Ok 'If the list above shows a J-Link entry, the driver is working.'
    Say-Warn 'Next: run  verify-swd.ps1  to test the actual SWD link.'
} else {
    Say-Warn 'Probe is still not OK.'
    Say-Warn 'Try: unplug the J-Link from USB, wait 3 s, plug it back in, re-run this script.'
    exit 2
}

if ($script:LogFile) { try { Stop-Transcript | Out-Null } catch { } }

if (-not $NoPause) {
    Write-Host ''
    Read-Host 'Press Enter to close'
}
exit 0
