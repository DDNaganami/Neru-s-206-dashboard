<#
  串口中继：**读 A → 写 B**，把 VAN 回放行从一边搬到另一边。

  ★★ 先看这一条（2026-09-24 实测踩到的坑，比脚本本身重要）：
    设备（和抓帧盒）自己打出来的帧行**并不是**可以直接回放的行 —— 它长这样：

        VAN 824 18 F8 27 10 00 00 00   # cmd=1 ack=0
                                       ^^^^^^^^^^^^^^^^ 这一段是 VanLogSink 加的注释

    而 `parseVanReplayLine()`（lib/dashcore/van_replay.cpp）在数据字节扫完之后
    遇到 `#` 会直接 `return false` ⇒ 设备回显 `VAN? VAN 824 18 F8 …`，数据层
    **一帧都收不到**（`SRC speed=` 一直是 sim）。第一次中继实测就是这个结果，
    所以本脚本提供 **`-ReplayLinesOnly`**：只挑 `VAN <十六进制>` 开头的行，
    并且把 `#` 之后整段**去掉**再转发。
    ⇒ 换句话说：**"抓帧日志"与"回放输入"差一个后缀**，这一步必须有人做。

  今天要它做的两件事（2026-09-24）：
    ① **两块板协同**（明天杜邦线/裸 S3 恢复后）：裸 S3 抓帧盒那个口收到的行 →
       原样转发进 2.8C 板的回放口（那一口 = `Serial0` = UART0 = 板载 CH343P =
       PC 上那个 COM 口）。
    ② **没有抓帧盒时的等价做法**：把仓库里现成的回放样本**当源**（同一份行格式），
       于是"解帧 → 数据源 → 表盘"这条链照样整条跑通 —— 2026-09-24 就是用它
       在**最终板（2.8C）**上验掉了"没有 VAN 收发器也能上屏"这条路。

  ★ 为什么是 .ps1 而不是 .py（仓库里 capture.py / replay.py 都是 .py）：
    `.py` 那两个要 **pyserial**，而本机系统 python 里**没有**它
    （只在 `C:\Users\张九思\206Dash\.pio-pylibs`，见 capture-van.ps1 的做法）。
    旁边的 `capture-boot-nopy.ps1` / `capture-van-nopy.ps1` 就是为这件事取的
    "nopy" 名字 ⇒ 本脚本沿用同一条口径：**只用 .NET 自带的 System.IO.Ports**，
    零第三方依赖、拿到任何一台 Windows 上都能直接跑。
    ★ 文件本身存成 **UTF-8 with BOM** —— 这是本仓库所有 .ps1 的一致做法
      （Windows PowerShell 5.1 读无 BOM 的 UTF-8 会按 GBK 解，中文注释直接把
        语法解析搞崩：实测报 `Unexpected token`）。改这个文件时别把 BOM 弄丢。

  ★ DTR/RTS 纪律（与 capture-*.ps1 / capture.py 完全一致）：
    两个口都在 **Open() 之前**把 `DtrEnable = RtsEnable = $false` 写死 ——
    .NET 的 SerialPort 默认就是 false，这里显式写一遍是为了"读代码的人不用去查默认值"，
    也为了将来谁想加复位功能时能一眼看到这条线在哪里。
    不做任何复位、不切换 BOOT 脚 ⇒ 中继不会打断板子上正在跑的固件
    （实测：中继期间 2.8C 的 `rgb: frames=+108/s` 一行没断）。

  用法（**两条可直接复制**）
  --------------------------
    # ① 文件源（今天就能用）：仓库现成的回放样本 → 2.8C 的回放口
    .\tools\serial-capture\relay.ps1 -From .\tools\serial-capture\sample-log.txt -To COM6 -ReplayLinesOnly -Loop -LineDelayMs 1500 -Seconds 45

    # ② 串口源（裸 S3 抓帧盒恢复后）：A 口 → B 口
    .\tools\serial-capture\relay.ps1 -From COM7 -To COM6 -ReplayLinesOnly

  参数
  ----
    -From         COMx **或**一个文件路径（自动判别：匹配 ^COM\d+$ 就是串口）
    -To           COMx
    -Baud         两个口的波特率，默认 115200（设备侧就是 115200 8N1）
    -ReplayLinesOnly  只转发 `VAN <十六进制>` 开头的行，并把 `#` 之后那段注释去掉
                  （见文件头那条 ★★）。**不打开就是逐字节原样转发** ——
                  那种模式下设备的 `VAN?` 回显会刷屏，那是**正常**的，
                  因为原样转发里混着日志行与注释。
                  ★ 2026-09-24 第一次中继实测：不加这一条，30 行全被回显 `VAN?`、
                    `SRC speed=` 一直是 sim；加上之后才真的进数据层。
    -Loop         文件源：读完回到开头继续（**文件源默认就是 true**）
    -LineDelayMs  文件源每行之间的间隔，默认 20 ms。
                  ★ 设备端有 **3 秒新鲜度窗口**（data_service 的 kStaleMs=3000）：
                    想让屏上的数**跟着样本一档一档地动**，把这个值开到 1000~2000
                    （比窗口小、又比 `SRC` 那 5 秒一行慢）；喂得比它还快就只是"一直在刷"。
    -Seconds      跑多少秒自动退出；0（默认）= 一直跑到 Ctrl+C
    -NoEcho       不把 B 口回来的字节打到控制台。**默认会打** ——
                  屏上动没动，最直接的证据就是设备那几行 `SRC …`。
    -BackLog      把 B 口回来的**原样字节**追加写进这个文件（UTF-8 无 BOM）。
                  ★ 中继跑起来之后控制台是滚动的，事后要引用"设备当时说了什么"
                    就得靠它 —— 2026-09-24 那份端到端证据就是这么留下的。
    -QuietStats   不打每 5 秒那行进度，只在退出时打一次汇总

  退出
  ----
    到点（`-Seconds`）自己退；或 Ctrl+C —— `finally` 里会把两个口关掉并打印统计。
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$From,
  [Parameter(Mandatory = $true)][string]$To,
  [int]$Baud = 115200,
  [switch]$ReplayLinesOnly,
  [switch]$Loop,
  [int]$LineDelayMs = 20,
  [int]$Seconds = 0,
  [switch]$NoEcho,
  [string]$BackLog = '',
  [switch]$QuietStats
)

$ErrorActionPreference = 'Stop'

function Open-Serial([string]$name, [int]$baud) {
  $sp = New-Object System.IO.Ports.SerialPort $name, $baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
  # ★ 必须在 Open() 之前（见文件头"DTR/RTS 纪律"）
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.ReadTimeout = 50
  $sp.WriteTimeout = 500
  # 设备日志是 UTF-8；不设的话 .NET 默认编码会把中文全解成 '?'
  $sp.Encoding = New-Object System.Text.UTF8Encoding($false)
  $sp.Open()
  return $sp
}

# 把"设备自己打的那一行"变成"能回放的那一行"：
#   · 先砍掉 `#` 之后（含 #）—— 那是 VanLogSink 加的 `cmd=/ack=` 注释，
#     回放解析器遇到它会直接判错（见文件头 ★★）。
#   · 再要求它真的是 `VAN <十六进制>` 形状。★ 这里必须要求"VAN 后面紧跟十六进制位"：
#     仓库的日志里还有一行 `van phy: gpio 就绪 …`，它同样以 "van " 开头
#     （PowerShell 的 -match 默认不分大小写），只看前缀会把它也转过去 —— 实测就是这样
#     多出来一串 `VAN? van phy: …`。
function Convert-ToReplayLine([string]$line) {
  $l = $line.TrimEnd("`r")
  $h = $l.IndexOf('#')
  if ($h -ge 0) { $l = $l.Substring(0, $h) }
  $l = $l.Trim()
  if ($l -notmatch '^[Vv][Aa][Nn]\s+[0-9A-Fa-f]') { return $null }
  return $l
}

# 从缓冲里取出**完整行**（以 \n 结尾的那些），剩下的半行留在缓冲里。
function Take-CompleteLines([System.Text.StringBuilder]$buf) {
  $txt = $buf.ToString()
  $idx = $txt.LastIndexOf("`n")
  if ($idx -lt 0) { return @() }
  $head = $txt.Substring(0, $idx)
  $rest = $txt.Substring($idx + 1)
  [void]$buf.Clear(); [void]$buf.Append($rest)
  return ,@($head -split "`n")
}

$isPortSource = $From -match '^COM\d+$'
if (-not $isPortSource -and -not (Test-Path -LiteralPath $From)) {
  throw "-From '$From' 既不是 COMx 也不是一个存在的文件路径"
}

$script:stats = [ordered]@{ lines_fwd = 0; bytes_fwd = 0; bytes_back = 0; lines_back = 0; chunks_back = 0 }

function Show-Stats([string]$tag) {
  if ($QuietStats -and $tag -ne 'exit') { return }
  Write-Host ("[{0}] 转发 {1} 行 / {2} 字节  ←  {3} 字节 / {4} 行 来自 {5}" -f `
      $tag, $script:stats.lines_fwd, $script:stats.bytes_fwd, `
      $script:stats.bytes_back, $script:stats.lines_back, $To)
}

$src = $null; $dst = $null
$lines = @(); $lineIdx = 0
$srcBuf = New-Object System.Text.StringBuilder
$backBuf = New-Object System.Text.StringBuilder
$backWriter = $null
if ($BackLog -ne '') {
  # 追加写、UTF-8 无 BOM —— 与"设备日志是 UTF-8"对齐
  $backWriter = New-Object System.IO.StreamWriter($BackLog, $true, (New-Object System.Text.UTF8Encoding($false)))
  $backWriter.AutoFlush = $true
}

function Send-ToDevice([string]$payload) {
  if ([string]::IsNullOrEmpty($payload)) { return }
  if ($payload -notmatch "`n$") { $payload += "`n" }
  $dst.Write($payload)
  $dst.BaseStream.Flush()
  $script:stats.bytes_fwd += $payload.Length
  $script:stats.lines_fwd++
}

try {
  # ---------------- 打开 B 口（目的地，永远是串口）----------------
  $dst = Open-Serial $To $Baud
  Write-Host ("relay: {0} -> {1} @{2} 8N1" -f $From, $To, $Baud)
  Write-Host "relay: DTR/RTS 两个口都是 false（不做复位、不碰 BOOT 脚）"
  Write-Host ("relay: ReplayLinesOnly={0}（只挑 'VAN <hex>' 行并去掉 '#' 注释）" -f [bool]$ReplayLinesOnly)

  # ---------------- 打开源 ----------------
  if ($isPortSource) {
    $src = Open-Serial $From $Baud
    Write-Host ("relay: 源=串口 {0}（读到的行写 {1}）" -f $From, $To)
  } else {
    $lines = @(Get-Content -LiteralPath $From)
    if ($ReplayLinesOnly) {
      $lines = @($lines | ForEach-Object { Convert-ToReplayLine $_ } | Where-Object { $null -ne $_ })
    }
    if ($lines.Count -eq 0) { throw "源文件里没有可转发的行：$From" }
    if (-not $PSBoundParameters.ContainsKey('Loop')) { $Loop = $true }   # 文件源默认循环
    Write-Host ("relay: 源=文件 {0}（{1} 行，Loop={2}，每行间隔 {3} ms）" -f `
        $From, $lines.Count, [bool]$Loop, $LineDelayMs)
    if ($ReplayLinesOnly) {
      Write-Host ("relay: 采样第一行 -> '{0}'" -f $lines[0])
    }
  }

  # ---------------- 主循环 ----------------
  $sw      = [System.Diagnostics.Stopwatch]::StartNew()
  $nextOut = 0                       # 下一次写 B 的时刻（ms）
  $nextLog = 5000                    # 下一次打进度

  while ($true) {
    # (1) **先收 B 口**：设备日志必须持续被读走，否则它那边的 TX 会堵
    while ($dst.BytesToRead -gt 0) {
      $chunk = $dst.ReadExisting()
      if ($chunk.Length -gt 0) {
        $script:stats.bytes_back += $chunk.Length
        $script:stats.chunks_back++
        [void]$backBuf.Append($chunk)
        if ($null -ne $backWriter) { $backWriter.Write($chunk) }
        if (-not $NoEcho) { [Console]::Out.Write($chunk) }
      }
    }
    # 数行（只数，不改内容）
    $nl = 0
    foreach ($ch in $backBuf.ToString().ToCharArray()) { if ($ch -eq "`n") { $nl++ } }
    if ($nl -gt 0) {
      $script:stats.lines_back += $nl
      $txt = $backBuf.ToString()
      [void]$backBuf.Clear()
      [void]$backBuf.Append($txt.Substring($txt.LastIndexOf("`n") + 1))
    }

    # (2) 源 → B
    $now = [int]$sw.Elapsed.TotalMilliseconds
    if ($now -ge $nextOut) {
      if ($isPortSource) {
        while ($src.BytesToRead -gt 0) {
          $s = $src.ReadExisting()
          if ($s.Length -gt 0) { [void]$srcBuf.Append($s) }
        }
        if ($srcBuf.Length -gt 0) {
          if ($ReplayLinesOnly) {
            foreach ($l in (Take-CompleteLines $srcBuf)) {
              $out = Convert-ToReplayLine $l
              if ($null -ne $out) { Send-ToDevice $out }
            }
          } else {
            $raw = $srcBuf.ToString(); [void]$srcBuf.Clear()
            Send-ToDevice $raw
          }
        }
        $nextOut = $now        # 串口源：有多少转多少，不按节拍
      } else {
        $line = $lines[$lineIdx]
        $lineIdx++
        if ($lineIdx -ge $lines.Count) {
          if ($Loop) { $lineIdx = 0 } else { $line = $null; $lineIdx = $lines.Count }
        }
        $nextOut = $now + [Math]::Max(0, $LineDelayMs)
        if ($null -ne $line) { Send-ToDevice $line }

        if ($lineIdx -ge $lines.Count) {
          Write-Host "relay: 文件已读完且未开 -Loop —— 退出前再收 1 秒 B 口的回显"
          $t = [int]$sw.Elapsed.TotalMilliseconds
          while ([int]$sw.Elapsed.TotalMilliseconds - $t -lt 1000) {
            while ($dst.BytesToRead -gt 0) {
              $chunk = $dst.ReadExisting()
              if ($chunk.Length -gt 0) {
                $script:stats.bytes_back += $chunk.Length
                if ($null -ne $backWriter) { $backWriter.Write($chunk) }
                if (-not $NoEcho) { [Console]::Out.Write($chunk) }
              }
            }
            Start-Sleep -Milliseconds 20
          }
          break
        }
      }
    }

    # (3) 进度与到点
    if ($now -ge $nextLog) { $nextLog += 5000; Show-Stats ("{0:N1}s" -f ($now / 1000.0)) }
    if ($Seconds -gt 0 -and $now -ge ($Seconds * 1000)) { Write-Host "relay: 到 -Seconds $Seconds 秒，正常退出"; break }
    Start-Sleep -Milliseconds 2
  }
}
finally {
  foreach ($sp in @($src, $dst)) {
    if ($null -ne $sp) { try { if ($sp.IsOpen) { $sp.Close() } } catch { } }
  }
  if ($null -ne $backWriter) { try { $backWriter.Flush(); $backWriter.Close() } catch { } }
  Show-Stats 'exit'
  Write-Host "relay: 两个口都已关闭"
}
