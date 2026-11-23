<#
    verify-swd.ps1
    -------------------------------------------------------------------------
    Verifies that the external J-Link really reaches the nRF54LM20A over SWD.
    This is the authoritative check for "is my SWD wiring correct?" - it is
    much stronger than a multimeter continuity test.

    Background (design side, from Netlist_PCB3 2026-10-04):
      H1 is a 2x2 / 2.54mm header:
        H1.1 = SWD_CLK   H1.2 = VCC_3V3
        H1.3 = SWD_DIO   H1.4 = GND
      BOTH SWD nets are ISOLATED on the PCB - there is no copper between H1
      and the XIAO. The link exists only through the user's hand-soldered
      fly-wires onto the module's dedicated SWCLK / SWDIO pads.
      => Nothing on the board can prove those wires; only a live connect can.

    What the script checks, in order:
      1. J-Link probe present + driver healthy            (see install-jlink-driver.ps1)
      2. JLink.exe can talk to the probe, and which VTref it senses
         -> VTref ~3.3V proves the H1.2 (3V3) wire reaches the probe
         -> VTref 0V    proves that wire is missing/open
      3. SWD connect to NRF54LM20A_M33
         -> "Found SW-DP" + "Cortex-M33" => SWCLK/SWDIO/GND wiring correct
         -> "Cannot connect to target"   => wiring / power suspect

    Exit codes:
      0 = SWD link verified (wiring correct)
      2 = probe / driver problem  (run install-jlink-driver.ps1 first)
      3 = SWD connect failed      (suspect SWCLK/SWDIO/GND or target power)
      4 = J-Link software not found

    Usage:
      powershell -NoProfile -ExecutionPolicy Bypass -File .\verify-swd.ps1
      optional:  -Speed 1000     (lower clock helps with long/poor wires)
                 -KeepLog       (keep the raw JLink output for inspection)
#>

[CmdletBinding()]
param(
    [string] $SeggerRoot = 'C:\Program Files\SEGGER',
    [int]    $Speed      = 4000,
    [int]    $TimeoutSec = 60,
    [string] $DeviceOverride = '',
    [switch] $KeepLog
)

$ErrorActionPreference = 'Continue'

function Say-Head($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Say-Ok($t)   { Write-Host "[ OK ] $t"    -ForegroundColor Green }
function Say-Warn($t) { Write-Host "[WARN] $t"    -ForegroundColor Yellow }
function Say-Err($t)  { Write-Host "[FAIL] $t"    -ForegroundColor Red }

# ------------------------------------------------------------- find JLink.exe
Say-Head '1. Locate J-Link software'

$jlinkExe = $null
if (Test-Path -LiteralPath $SeggerRoot) {
    $dirs = @(Get-ChildItem -LiteralPath $SeggerRoot -Directory -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -like 'JLink_V*' })
    $best = $null; $bestVer = -1
    foreach ($d in $dirs) {
        $m = [regex]::Match($d.Name, '(\d+)')
        $v = 0
        if ($m.Success) { $v = [int]$m.Groups[1].Value }
        if ($v -gt $bestVer) { $bestVer = $v; $best = $d }
    }
    if ($best) {
        $cand = Join-Path $best.FullName 'JLink.exe'
        if (Test-Path -LiteralPath $cand) { $jlinkExe = $cand }
    }
}

if (-not $jlinkExe) {
    Say-Err "JLink.exe not found under $SeggerRoot"
    exit 4
}
Say-Ok "JLink.exe : $jlinkExe"

# --------------------------------------------------------- 1) probe presence
Say-Head '2. J-Link probe USB state'

$probes = @(Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_1366' })

if ($probes.Count -eq 0) {
    Say-Err 'No J-Link probe enumerated on USB.'
    Say-Err 'Plug the probe in, then re-run.'
    exit 2
}

$healthy = @()
foreach ($d in $probes) {
    $line = "  {0,-7} {1}" -f $d.Status, $d.InstanceId
    if ($d.Status -ne 'OK') {
        $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue |
             Where-Object { $_.KeyName -eq 'DEVPKEY_Device_ProblemCode' }
        if ($p) { $line += ('   problemCode=' + $p.Data) }
    }
    Write-Host $line
    if ($d.Status -eq 'OK') { $healthy += $d }
}

if ($healthy.Count -eq 0) {
    Say-Err 'Probe is present but NOT healthy (driver not bound).'
    Say-Warn 'Run  install-jlink-driver.ps1  first, then re-run this script.'
    exit 2
}
Say-Ok 'Probe driver is healthy.'

# ------------------------------------------------------------ 2) JLink session
function Invoke-JLink {
    param([string] $ScriptText, [int] $TimeoutSeconds)

    $tag = [guid]::NewGuid().ToString('N')
    $scriptFile = Join-Path $env:TEMP ("jlverify_$tag.jlink")
    $outFile    = Join-Path $env:TEMP ("jlverify_$tag.out")
    $errFile    = Join-Path $env:TEMP ("jlverify_$tag.err")

    Set-Content -LiteralPath $scriptFile -Value $ScriptText -Encoding Ascii

    $proc = Start-Process -FilePath $jlinkExe `
                -ArgumentList @('-CommanderScript', $scriptFile, '-ExitOnError', '1') `
                -NoNewWindow -PassThru `
                -RedirectStandardOutput $outFile `
                -RedirectStandardError  $errFile

    $finished = $proc.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) { try { $proc.Kill() } catch { } }

    $text = ''
    if (Test-Path -LiteralPath $outFile) { $text += (Get-Content -LiteralPath $outFile -Raw) }
    if (Test-Path -LiteralPath $errFile) { $text += "`n" + (Get-Content -LiteralPath $errFile -Raw) }

    if ($KeepLog) {
        Write-Host ("  [log kept] " + $scriptFile)
        Write-Host ("  [log kept] " + $outFile)
    } else {
        Remove-Item -LiteralPath $scriptFile, $outFile, $errFile -Force -ErrorAction SilentlyContinue
    }

    return [pscustomobject]@{ Text = $text; TimedOut = (-not $finished) }
}

Say-Head '3. SWD connect test'

$deviceNames = @()
if ($DeviceOverride) { $deviceNames += $DeviceOverride }
$deviceNames += @('NRF54LM20A_M33', 'nRF54LM20A_M33')

$success = $false
$lastText = ''
$usedDevice = ''

foreach ($dev in $deviceNames) {
    Write-Host ''
    Write-Host "--- trying device name: $dev (SWD @ ${Speed}kHz) ---" -ForegroundColor DarkGray

    $script = @(
        'si SWD',
        "speed $Speed",
        "device $dev",
        'connect',
        'exit'
    ) -join "`n"

    $r = Invoke-JLink -ScriptText $script -TimeoutSeconds $TimeoutSec
    $text = $r.Text
    $lastText = $text
    $usedDevice = $dev

    # raw output (trim noise)
    Write-Host $text.Trim()

    if ($r.TimedOut) {
        Say-Warn 'JLink.exe timed out - treating as failure.'
        continue
    }

    if ($text -match 'No J-Link found|Cannot find any J-Link|no emulators') {
        Say-Err 'JLink.exe cannot see any probe (driver/session problem).'
        exit 2
    }

    if ($text -match '(?i)unknown device') {
        Say-Warn "J-Link does not know device name '$dev' - trying next spelling."
        continue
    }

    if ($text -match 'Found SW-DP|Found SWDP|Cortex-M33') {
        $success = $true
        break
    }
}

# --------------------------------------------------------------- interpretation
Say-Head '4. Verdict'

$vref = 'unknown'
$m = [regex]::Match($lastText, 'VTref=([0-9]+\.[0-9]+)V')
if ($m.Success) { $vref = $m.Groups[1].Value + ' V' }
Write-Host ("  target reference voltage reported by the probe (VTref) : " + $vref)

$fw = ''
$mf = [regex]::Match($lastText, 'Firmware:\s*(.+)')
if ($mf.Success) { $fw = $mf.Groups[1].Value.Trim() }
if ($fw) { Write-Host ("  probe firmware                                        : " + $fw) }

if ($vref -ne 'unknown') {
    $vf = [double]($vref -replace ' V', '')
    if ($vf -lt 1.5) {
        Say-Warn 'VTref is low/near 0V  ->  the H1.2 (3V3) wire to the probe is missing or the target is unpowered.'
    } else {
        Say-Ok  'VTref is close to 3.3V -> the 3V3 reference wire is good.'
    }
}

if ($success) {
    Write-Host ''
    Say-Ok "SWD LINK VERIFIED  (device '$usedDevice', " + $Speed + ' kHz)'
    Say-Ok 'The SWCLK / SWDIO / GND wiring is electrically correct.'
    Write-Host ''
    Write-Host 'Next step - flash the firmware:' -ForegroundColor Cyan
    Write-Host '  west flash --runner jlink -d C:/Users/asus/.cache/xiao-fw/build'
    exit 0
}

Write-Host ''
Say-Err 'SWD CONNECT FAILED - the wiring is suspect.'
Write-Host ''
Write-Host 'Checklist, in order of likelihood:' -ForegroundColor Cyan
Write-Host '  a) SWCLK and SWDIO swapped.  H1.1 must reach the module SWCLK pad,'
Write-Host '     H1.3 must reach the module SWDIO pad.  (swapping these is the'
Write-Host '     single most common mistake and it is NOT destructive.)'
Write-Host '  b) GND not common between probe and target  (H1.4 -> module GND).'
Write-Host '  c) Target not powered.  The bare XIAO needs USB or a battery for VTref.'
Write-Host '  d) Fly-wire solder joint not actually connected (re-check continuity).'
Write-Host '  e) Try a slower clock:  -Speed 1000'
Write-Host ''

if (-not $KeepLog -and $lastText) {
    $dump = Join-Path $env:TEMP 'verify-swd-last.txt'
    Set-Content -LiteralPath $dump -Value $lastText -Encoding UTF8
    Write-Host ("Raw JLink output saved to: " + $dump)
}

exit 3
