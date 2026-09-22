<#
  车上抓 VAN 的一键脚本(2026-09-18)

  为什么要有它:车上手忙脚乱,而这条命令依赖三件很容易漏的事 ——
    · PYTHONPATH:`.pio-pylibs` 里才是 pyserial 3.5,**系统 python 里没有**
      (实测 `ModuleNotFoundError: No module named 'serial'`);
    · 输出编码:PowerShell 5.1 的 `>` 重定向写出的是 **UTF-16LE**,回来没法分析;
    · 端口 / 时长 / 输出路径每次都要敲对。
  这些全部写死在这里,车上只需要一条命令。

  用法:
    powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-van.ps1
    powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-van.ps1 -Seconds 300
    powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-van.ps1 -UntilFrames 200

  逃生门:若小结里 `dropped` 一直在涨(串口 115200 顶不住、阻塞拖住主循环),
  把固件的 `dash_log_begin(115200)` 改成 921600 重刷,再用 `-Baud 921600` 抓。
#>
param(
  [string]$Port = 'COM4',
  [int]$Seconds = 600,
  [string]$Out = 'C:\Users\Public\206dash\van_capture_1.txt',
  [int]$UntilFrames = 0,
  [int]$Baud = 115200          # 必须与固件 dash_log_begin() 一致(现在是 115200)
)

$ErrorActionPreference = 'Stop'
$env:PYTHONPATH = 'C:\Users\张九思\206Dash\.pio-pylibs'   # ★ pyserial 只在这里
$env:PYTHONIOENCODING = 'utf-8'
# 子进程吐的是 UTF-8,而这个控制台默认按 GBK(936) 解 —— 不换的话中文全是乱码。
# 记下原值、收尾换回去(代码页是**控制台**的属性,不还原就会一直留在 65001)。
$oldCP = $null
try {
  $oldCP = [Console]::OutputEncoding
  [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch { }

Write-Host '=== 上车后先确认这 4 条(错一条就是白跑一趟) ===' -ForegroundColor Cyan
Write-Host ' 1) 笔记本用电池:充电器必须拔掉(USB 地 + 车 12V 地 = 地环流,见 PURCHASE.md 第六节)'
Write-Host ' 2) 模块 3V3/GND 接板子;模块 GND 再单独一根到车地(OBD 4/5 脚或仪表侧搭铁)'
Write-Host ' 3) 120Ω 终端电阻已拆;GPIO16 接模块 RX;模块 TX 接 3V3(★ 不能悬空);RS 接 GND(6 脚模块无此脚=板内已接 GND)'
Write-Host ' 4) VAN_H/VAN_L 接仪表连接器 5/10 脚:先量对地 2~3V 且两根不相等,是 0V/12V 就停手'
Write-Host ''
Write-Host "开始抓帧:$Port @ $Baud / $Seconds 秒 → $Out" -ForegroundColor Cyan
Write-Host '（实时进度在下面这行滚动;结束时会给一张"抓到了什么"的小结）'

$pyArgs = @("$PSScriptRoot\capture.py", $Port, "$Seconds", 'quiet',
            '--out', $Out, '--baud', "$Baud")
if ($UntilFrames -gt 0) { $pyArgs += @('--until-frames', "$UntilFrames") }
& python @pyArgs
$code = $LASTEXITCODE

if (Test-Path $Out) {
  $f = Get-Item $Out
  Write-Host ''
  Write-Host ('原始日志:{0} ({1:N0} 字节)' -f $f.FullName, $f.Length) -ForegroundColor Green
}
if ($oldCP) { try { [Console]::OutputEncoding = $oldCP } catch { } }
exit $code
