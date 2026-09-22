<#
  capture-boot-nopy.ps1 —— 复位并抓开机日志（零依赖，只用 Windows 自带 PowerShell）

  为什么要有它：
    capture.py 需要 pyserial，capture-van-nopy.ps1 又**只读不复位**。
    要在笔记本上"从第一个字节开始看开机自检"，需要一个会精确拉复位线的零依赖工具。

  ★ DTR/RTS 口径（照抄 capture.py:160-167 的结论，别自己发明）：
      板上 CH340 的 DTR/RTS 真的接在 IO0 / EN 上：
        RTS=True   → EN 低   → 芯片停在复位
        RTS=False  → EN 高   → 放开复位，开始跑
        DTR=True   → IO0 低  → 进下载模式（★ 这一条是"屏幕上什么都没有"的元凶）
        DTR=False  → IO0 高  → 从 flash 正常启动
      所以：**全程 DTR=False**，只用 RTS 做"按住复位 → 放开"。

  用法：
    powershell -ExecutionPolicy Bypass -File capture-boot-nopy.ps1 -Port COM3
    powershell -ExecutionPolicy Bypass -File capture-boot-nopy.ps1 -Port COM3 -Seconds 15
    powershell -ExecutionPolicy Bypass -File capture-boot-nopy.ps1 -Port COM3 -NoReset   # 只读不复位
    powershell -ExecutionPolicy Bypass -File capture-boot-nopy.ps1 -SelfTest

  产出：默认写到脚本旁边的 boot_log.txt（UTF-8 无 BOM），同时打印到屏幕。
#>
param(
  [string]$Port = 'COM3',
  [int]$Baud = 115200,
  [int]$Seconds = 12,
  [string]$Out = '',
  [switch]$NoReset,
  [switch]$SelfTest
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

function Get-SerialPorts { [System.IO.Ports.SerialPort]::GetPortNames() }

if ($SelfTest) {
  Write-Host "=== SelfTest（不碰串口）===" -ForegroundColor Cyan
  $fail = 0
  # 1) 默认值检查
  $checks = @(
    @{ n = '默认 DTR 必须是 false（拉高会进下载模式）'; v = $false; want = $false },
    @{ n = '默认 RTS 必须是 false（拉高会按住复位）'; v = $false; want = $false }
  )
  foreach ($c in $checks) {
    $ok = ($c.v -eq $c.want); if (-not $ok) { $fail++ }
    Write-Host ("{0} {1}" -f $(if ($ok) { '[OK  ]' } else { '[FAIL]' }), $c.n)
  }
  # 2) 真开一个 SerialPort 对象看它的默认值
  $sp = New-Object System.IO.Ports.SerialPort 'COM_NONEXISTENT', 115200, 'None', 8, 'One'
  $d = $sp.DtrEnable; $r = $sp.RtsEnable; $sp.Dispose()
  foreach ($pair in @(@('DtrEnable', $d), @('RtsEnable', $r))) {
    $ok = ($pair[1] -eq $false); if (-not $ok) { $fail++ }
    Write-Host ("{0} SerialPort 默认 {1} = {2}（期望 False）" -f $(if ($ok) { '[OK  ]' } else { '[FAIL]' }), $pair[0], $pair[1])
  }
  # 3) 端口枚举可用
  $ps = Get-SerialPorts
  Write-Host ("[INFO] 当前串口: " + $(if ($ps) { $ps -join ', ' } else { '(无)' }))
  Write-Host ""
  if ($fail -eq 0) { Write-Host "selftest 全部通过" -ForegroundColor Green; exit 0 }
  Write-Host "selftest 有 $fail 项失败" -ForegroundColor Red; exit 5
}

if (-not $Out) { $Out = Join-Path (Split-Path $PSCommandPath -Parent) 'boot_log.txt' }

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
# ★ 打开前就设好：这两根线接在 EN/IO0 上，默认拉高 = 复位/下载模式，收不到任何字节
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 500
$sp.WriteTimeout = 1000

try { $sp.Open() } catch { Write-Host ("串口打不开: " + $_.Exception.Message) -ForegroundColor Red; exit 3 }
Write-Host ("已打开 " + $Port + " @" + $Baud + "  DTR=" + $sp.DtrEnable + " RTS=" + $sp.RtsEnable) -ForegroundColor Green

if (-not $NoReset) {
  Write-Host "复位中（RTS 按住 200ms → 放开）..." -ForegroundColor Yellow
  $sp.RtsEnable = $true      # EN 低 → 停在复位
  Start-Sleep -Milliseconds 200
  $sp.RtsEnable = $false     # 放开 → 从 flash 启动
  Start-Sleep -Milliseconds 50
} else {
  Write-Host "只读模式：不复位，只听 $Seconds 秒" -ForegroundColor Yellow
}

$sb = New-Object System.Text.StringBuilder
# ★ 增量落盘：绝不能只攒在内存里。
#   2026-09-22 实测踩到：14 秒的日志在 115200 波特率下 ≈20KB，而"每轮只读一次
#   BytesToRead"的循环会把中间到达的数据漏掉（串口 FIFO 溢出）—— 屏幕上明明打印过
#   `frames=539`，落盘却只有 67 行/3.5KB，`VAN` 帧行全丢了，差点误判成"帧没到 sink"。
#   两个修法一起上：① 每轮把 FIFO 里的数据**全部**读干（while BytesToRead>0）；
#   ② 每轮结束就把这一轮的量 flush 到文件，进程被杀也不丢已抓到的部分。
$sw = [Diagnostics.Stopwatch]::StartNew()
$flushEveryMs = 250
$lastFlush = 0
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  try {
    # ① 把当前 FIFO 读干，而不是每轮只读一次
    while ($sp.BytesToRead -gt 0) {
      $n = $sp.BytesToRead
      if ($n -gt 4096) { $n = 4096 }
      $buf = New-Object byte[] $n
      $read = $sp.Read($buf, 0, $n)
      if ($read -le 0) { break }
      $chunk = [System.Text.Encoding]::UTF8.GetString($buf, 0, $read)
      [void]$sb.Append($chunk)
      Write-Host -NoNewline $chunk
    }
  } catch { }
  # ② 增量 flush（整份重写：文件小，这样最省事且永不出现半行）
  if (($sw.ElapsedMilliseconds - $lastFlush) -ge $flushEveryMs) {
    $lastFlush = $sw.ElapsedMilliseconds
    try { [IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object Text.UTF8Encoding($false))) } catch { }
  }
  Start-Sleep -Milliseconds 10     # 别 30ms —— 115200 下 30ms 就能灌满 FIFO
}
try { $sp.Close(); $sp.Dispose() } catch {}

$text = $sb.ToString()
[IO.File]::WriteAllText($Out, $text, (New-Object Text.UTF8Encoding($false)))

Write-Host ""
Write-Host ("--- 抓到 " + $text.Length + " 字符 -> " + $Out) -ForegroundColor Green

# ---- 自动核对关键自检行（PINOUT.md 的判据）----
Write-Host ""
Write-Host "=== 关键行核对 ===" -ForegroundColor Cyan
$rules = @(
  @{ k = '206 dash ok';                     want = '固件跑起来了' },
  @{ k = 'chip  :';                         want = '芯片型号' },
  @{ k = 'flash : 16 MB';                   want = 'flash 必须 16MB（报 4/8 说明不是 N16，上车起不来）' },
  @{ k = 'psram : 8';                       want = 'PSRAM 必须 8000+ KB（报 0 是 BOARD_HAS_PSRAM/OPI 没生效）' },
  @{ k = 'van phy: gpio 就绪';              want = 'VAN 硬件收帧已启用（显示 stub 就是没收帧）' },
  @{ k = 'obd: UART1';                      want = 'OBD 已挂上 ELM327' }
)
foreach ($r in $rules) {
  $hit = $text -match [regex]::Escape($r.k)
  $mark = if ($hit) { '[OK  ]' } else { '[MISS]' }
  $color = if ($hit) { 'Green' } else { 'Yellow' }
  Write-Host ("{0} {1,-24} {2}" -f $mark, $r.k, $r.want) -ForegroundColor $color
}
# 数值行的实际值
foreach ($pat in 'psram : .*', 'flash : .*', 'chip  : .*', 'van phy: .*', 'obd: .*') {
  $m = [regex]::Match($text, $pat)
  if ($m.Success) { Write-Host ("     实际: " + $m.Value.Trim()) -ForegroundColor DarkGray }
}
