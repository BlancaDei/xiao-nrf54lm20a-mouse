# 用 NVIDIA FrameView SDK 的 PresentMon 记录帧时间。
# 必须以管理员权限运行（ETW 需要）。输出 CSV 同级会生成 .log。
param(
    [Parameter(Mandatory = $true)][string]$OutFile,
    [int]$Seconds = 300,
    [string]$ProcessName = 'FPSAimTrainer-Win64-Shipping.exe',
    [string]$SessionName = 'AB_CAP'
)

$pm = 'C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\PresentMon_x64.exe'
& $pm --process_name $ProcessName --output_file $OutFile --timed $Seconds `
      --no_console_stats --session_name $SessionName *> ($OutFile + '.log')
