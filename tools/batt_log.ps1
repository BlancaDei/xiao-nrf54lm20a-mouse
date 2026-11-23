# 续航测试记录器：监听接收器串口，把每 5 秒状态行里的电量/收包数存成 CSV。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools/batt_log.ps1 -Port COM8
param(
    [string]$Port = 'COM8',
    [string]$OutDir = (Join-Path $PSScriptRoot '..\logs')
)

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$out = Join-Path (Resolve-Path $OutDir) "endurance_$stamp.csv"
"time,bat_pct,rx_5s,btn_total,link" | Set-Content -LiteralPath $out -Encoding UTF8

$sp = $null
$link = 'unknown'

while ($true) {
    try {
        if ($null -eq $sp) {
            $sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
            $sp.ReadTimeout = 3000
            $sp.Open()
            $sp.DiscardInBuffer()
        }

        $line = $sp.ReadLine().Trim()
        if ($line -match '^link (up|idle)') {
            $link = $Matches[1]
            continue
        }
        if ($line -match '^esb: rx (\d+) in 5 s .* \| btn usb (\d+) \| bat (-?\d+)%') {
            $rx = $Matches[1]
            $btn = $Matches[2]
            $bat = $Matches[3]
            $t = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
            "$t,$bat,$rx,$btn,$link" | Add-Content -LiteralPath $out -Encoding UTF8
        }
    }
    catch {
        if ($null -ne $sp) {
            try { $sp.Close() } catch {}
            $sp = $null
        }
        Start-Sleep -Seconds 2
    }
}
