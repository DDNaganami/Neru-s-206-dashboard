<#
  drive-log.ps1 —— 行驶采集:VAN 串口 + OBD 串口【同时】记录,时间戳同源

  为什么要有它(2026-09-22):
    车速字段(IDEN 0x824 / data[2])的标度**缺地面真值** —— "1 计数 = 1 km/h" 是
    最自然读法 + 物理自洽,但从没和真实车速对照过(见 ACCEPTANCE.md 的 ② 与 ★①)。
    转速那路不用再验(已三次实车互证),**只差车速斜率**。

    ★ 关键设计:**两个串口都接同一台笔记本**,所以两边的时间戳天然同源,
      不需要事后对齐(手机 App 那条路要手工对齐墙上时间,误差大)。

    VAN 侧  : 我们自己的板子(COM3, 115200) —— 记 IDEN 0x824 与 0x4FC 的**整个负载**
    OBD 侧  : 蓝牙 ELM327(SPP 配对后枚举成 COM 口, 38400) —— 真值源
              010D 车速 + 010C 转速 + 0105 水温 + 010F 进气温度

  产出两份 CSV(都在脚本旁边):
    van_drive.csv : t_s, wall_time, iden, len, cmd, ack, d0..d6, spd, rpm, seq
                    其中 spd = 0x824.data[2](车速,标度 2.56,已定标)、rpm = data[0..1](16 位大端)
                    ★ d0..d6 是**原样的 data[0..6]**,没有位移 —— 别把 d0 当命令字节
    obd_drive.csv : t_s, wall_time, pid, ok, raw, value
                    value 对 010D 是 km/h、对 010C 是 rpm、对 0105/010F 是 ℃ —— **地面真值**

  ★ 为什么改成记"整个负载"(2026-09-22 晚):
    上一版只挑 data[0..2] 三列,于是**事后想问的问题全都问不了** ——
    最典型的就是「进气温度(010F)在 VAN 上有没有副本」,而上一版
    `data[3]` / `data[4..5]` 根本没落盘。板子本来就把整帧打全了
    (`VAN 824 1C 1E 27 ...   # cmd=.. ack=..`,行尾注释那两位不算负载),
    所以**不用重新刷固件**,只把采集放宽即可 —— 采集脚本永远比改固件便宜,数据先囤着。

  用法:
    # 双路(推荐):VAN + 蓝牙 OBD —— 板上 CH340 那个 UART 口是 COM3,
    #   蓝牙 327 配对后通常枚举成 COM6(2026-09-22 实测就是 COM6;别指到 COM5 那个空口)
    powershell -ExecutionPolicy Bypass -File drive-log.ps1 -VanPort COM3 -ObdPort COM6 -Minutes 15

    # 只记 VAN(蓝牙还没配对时)
    powershell -ExecutionPolicy Bypass -File drive-log.ps1 -VanPort COM3 -Minutes 15

    # 自检(不碰串口)
    powershell -ExecutionPolicy Bypass -File drive-log.ps1 -SelfTest

  跑法纪律(为了事后能对齐):
    1) 出发前、到达后**各按一次双闪** —— 双闪在 VAN 上会同时改
       4FC.data[5]=0x0C 与 4FC.data[0]=0xE0(已解出),是现成的**已知时间标记**
    2) 中途**停车两次**、每次停 5 秒以上 —— 0 km/h 是最有价值的锚点
    3) 尽量覆盖:起步 → 20~30 → 50~60 → 80~90 → 减速停
    4) **倒一段车**(可选但白捡) —— 能顺手回答"倒车时 data[2] 是什么"
       (ACCEPTANCE.md 明确留着这条没验)

  退出码:0 正常 / 2 参数错 / 3 串口打不开
#>
param(
  [string]$VanPort = 'COM3',
  [int]$VanBaud = 115200,
  [string]$ObdPort = '',
  [int]$ObdBaud = 38400,
  # ★ 协议:默认 ATSP0(自动)。2026-09-22 实测这台 206 用 ATSP0 能出数据;
  #   试过 ATSP3(ISO 9141-2)反而把 K 线卡在 BUS INIT 不动 ⇒ 别锁 3。
  #   若哪天自动搜索不稳,可依次试 -ObdProto ATSP5(KWP fast)/ATSP4(KWP 5baud)。
  [string]$ObdProto = 'ATSP0',
  [double]$Minutes = 15,    # 行驶时长(分钟);显式给了 -Seconds 时以 -Seconds 为准
  [double]$Seconds = 0,     # 时长(秒) —— 秒级自测用;>0 时**优先于** -Minutes(见 runSec)
  [string]$OutDir = '',
  [switch]$SelfTest
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ★ 时长口径(2026-09-22 修了两次,两次都是真踩到):
#   第一版只有 [int]$Minutes ⇒ `-Minutes 0.33` 被**取整成 0**,循环一次都不跑、空 CSV。
#   第二版写成 Max($Minutes*60, $Seconds) ⇒ **$Minutes 的默认值 15 一直参与**,
#     于是 `-Seconds 20` 被判成 Max(900,20)=900 ⇒ 跑满 15 分钟(实测卡到 179 秒才发现)。
#   正确口径:**显式给了 -Seconds 就只用 -Seconds**;没给才用 -Minutes。
$runSec = if ($PSBoundParameters.ContainsKey('Seconds') -and $Seconds -gt 0) {
  [double]$Seconds
} else {
  [double]$Minutes * 60.0
}
if ($runSec -le 0) { Write-Host "错误: 时长必须 > 0" -ForegroundColor Red; exit 2 }

# ---------- 0x824 帧解析(与 van_source.h 的实测口径一致) ----------
# data[0..1] = 转速 x8(16 位大端,×0.125 = rpm)
# data[2]    = 车速(单字节,待定标)
# data[6]    = 帧序号
function Parse-824([string]$dataHex) {
  # ★ 2026-09-22 晚:不再只取前 3 个字节、也不再要求 >=7 —— 整帧全收。
  #   ★★ 索引口径**已按实测数据核对**(不是照抄注释):
  #     板子那行日志是 `VAN 824 <data[0]> <data[1]> ... <data[len-1]>`,
  #     **不带 cmd 字节**(cmd/ack 打在行尾注释里),所以 hex 的第 i 个就是 data[i]:
  #       data[0..1] = 转速 x8 (16 位大端)   ← 旧 CSV 的 data0 列=0x1C 就是它的高字节
  #       data[2]    = 车速(单字节, x2.56)
  #       data[3]    = 未知(与车速反相相关)
  #       data[4..5] = 里程累计量(16 位大端,单调不减)
  #       data[6]    = 帧序号
  #   越界的列统一给 -1,免得不同长度的帧把列数撑歪。
  $b = @([regex]::Matches($dataHex, '[0-9A-F]{2}') | ForEach-Object { [Convert]::ToInt32($_.Value, 16) })
  if ($b.Count -lt 3) { return $null }
  $at = { param($i) if ($i -lt $b.Count) { $b[$i] } else { -1 } }
  return [PSCustomObject]@{
    datalen  = $b.Count
    d0       = & $at 0
    d1       = & $at 1
    d2       = & $at 2
    d3       = & $at 3
    d4       = & $at 4
    d5       = & $at 5
    d6       = & $at 6
    rpmCount = (& $at 0) * 256 + (& $at 1)
    spdCount = & $at 2
    seq      = & $at 6
  }
}

# ---------- OBD 响应解析(与 obd-log.ps1 同口径) ----------
# ★ 参数名不能叫 $pid —— 那是 PowerShell 的只读自动变量(当前进程 ID)。
#   同一个坑在 live-obd.ps1 上踩过一次,这里又犯了一次,故写进注释。
function Parse-Obd([string]$raw, [string]$PidHex) {
  if (-not $raw) { return @{ ok = $false; value = ''; note = '无应答' } }
  $t = ($raw -replace "`r", ' ' -replace "`n", ' ')
  $t = $t -replace [regex]::Escape($PidHex), ' '
  $t = ($t -replace '\s+', ' ').Trim()
  foreach ($kw in 'UNABLE TO CONNECT', 'BUS INIT', 'NO DATA', 'SEARCHING', 'CAN ERROR', 'ERR', '\?', 'STOPPED') {
    if ($t -match $kw) { return @{ ok = $false; value = ''; note = $kw.Trim('\?') } }
  }
  $hex = ($t -replace '[^0-9A-Fa-f]', '').ToUpper()
  $pid2 = $PidHex.Substring(2, 2).ToUpper()
  $i = $hex.IndexOf('41' + $pid2)
  if ($i -lt 0) { return @{ ok = $false; value = ''; note = '没有 41 ' + $pid2 } }
  $rest = $hex.Substring($i + 4)
  $bytes = @()
  for ($k = 0; $k + 1 -lt $rest.Length; $k += 2) { $bytes += [Convert]::ToInt32($rest.Substring($k, 2), 16) }
  switch ($pid2) {
    '0D' { if ($bytes.Count -ge 1) { return @{ ok = $true; value = [string]$bytes[0]; note = 'km/h' } } }
    '0C' { if ($bytes.Count -ge 2) { return @{ ok = $true; value = [string](($bytes[0] * 256 + $bytes[1]) / 4); note = 'rpm' } } }
    # ★ 0105(水温)/010F(进气)都是单字节 A,℃ = A - 40。
    #   实测口径:010F -> '41 0F 6F' = 0x6F-40 = 71℃(冬菇头,偏高属正常)。
    '05' { if ($bytes.Count -ge 1) { return @{ ok = $true; value = [string]($bytes[0] - 40); note = 'C-coolant' } } }
    '0F' { if ($bytes.Count -ge 1) { return @{ ok = $true; value = [string]($bytes[0] - 40); note = 'C-intake' } } }
    default { return @{ ok = $true; value = $hex.Substring($i); note = 'raw' } }
  }
  return @{ ok = $false; value = ''; note = '太短' }
}

if ($SelfTest) {
  Write-Host "=== SelfTest(不碰串口)===" -ForegroundColor Cyan
  $fail = 0
  # 824 解析:造一帧,转速 0x1AF8=6904 -> /8 = 863rpm;车速 0x27=39
  $f = Parse-824 '1A F8 27 00 00 00 05'
  $chk = @(
    @{ n = 'rpmCount = 0x1AF8 = 6904'; got = $f.rpmCount; want = 6904 },
    @{ n = 'spdCount = 0x27 = 39';     got = $f.spdCount; want = 39 },
    @{ n = 'd3 = 0x00(未知列,现在要落盘)'; got = $f.d3; want = 0 },
    @{ n = 'datalen = 7';              got = $f.datalen; want = 7 },
    @{ n = 'seq = 0x05 = 5';           got = $f.seq;      want = 5 },
    # ★ 短帧不能崩、也不能把列撑歪:3 字节帧 -> d3..d6 = -1
    @{ n = '短帧 d3 = -1(越界哨兵)'; got = (Parse-824 '1C 1E 27').d3; want = -1 },
    @{ n = '短帧 datalen = 3';        got = (Parse-824 '1C 1E 27').datalen; want = 3 },
    # ★ 实测口径:旧 CSV 的 data0=0x1C 是**转速高字节**,不是命令字节
    @{ n = 'data[0] 是转速高字节(0x1C)'; got = $f.d0; want = 0x1A }
  )
  foreach ($c in $chk) {
    $ok = ($c.got -eq $c.want); if (-not $ok) { $fail++ }
    Write-Host ("{0} {1} (got {2})" -f $(if ($ok) { '[OK  ]' } else { '[FAIL]' }), $c.n, $c.got)
  }
  # OBD 解析
  $o = @(
    @{ raw = '41 0D 3C';                    pid = '010D'; want = $true;  wv = '60' },
    @{ raw = '410D3C';                      pid = '010D'; want = $true;  wv = '60' },
    @{ raw = '41 0C 1A F8';                 pid = '010C'; want = $true;  wv = '1726' },
    @{ raw = '41 05 80';                    pid = '0105'; want = $true;  wv = '88' },
    @{ raw = '41 0F 6F';                    pid = '010F'; want = $true;  wv = '71' },
    @{ raw = 'NO DATA';                     pid = '010D'; want = $false; wv = '' },
    @{ raw = 'UNABLE TO CONNECT';           pid = '010D'; want = $false; wv = '' },
    @{ raw = '';                            pid = '010D'; want = $false; wv = '' }
  )
  foreach ($c in $o) {
    $r = Parse-Obd $c.raw $c.pid
    $ok = ([bool]$r.ok -eq $c.want) -and ($r.value -eq $c.wv)
    if (-not $ok) { $fail++ }
    Write-Host ("{0} OBD '{1}' / {2} => ok={3} value='{4}'" -f $(if ($ok) { '[OK  ]' } else { '[FAIL]' }), $c.raw, $c.pid, $r.ok, $r.value)
  }
  Write-Host ""
  if ($fail -eq 0) { Write-Host "selftest 全部通过" -ForegroundColor Green; exit 0 }
  Write-Host "selftest 有 $fail 项失败" -ForegroundColor Red; exit 5
}

if (-not $OutDir) { $OutDir = Split-Path $PSCommandPath -Parent }
$vanCsv = Join-Path $OutDir 'van_drive.csv'
$obdCsv = Join-Path $OutDir 'obd_drive.csv'
foreach ($f in @($vanCsv, $obdCsv)) { Remove-Item $f -Force -ErrorAction SilentlyContinue }

# ---------- 开 VAN 口 ----------
$van = New-Object System.IO.Ports.SerialPort $VanPort, $VanBaud, 'None', 8, 'One'
$van.DtrEnable = $false      # ★ 板上 RTS/DTR 接 EN/IO0,拉高会复位/进下载模式
$van.RtsEnable = $false
$van.ReadTimeout = 500
try { $van.Open() } catch { Write-Host ("VAN 口打不开: " + $_.Exception.Message) -ForegroundColor Red; exit 3 }
Write-Host ("VAN : " + $VanPort + " @" + $VanBaud) -ForegroundColor Green

# ---------- 开 OBD 口(可选) ----------
$obd = $null
if ($ObdPort) {
  $obd = New-Object System.IO.Ports.SerialPort $ObdPort, $ObdBaud, 'None', 8, 'One'
  $obd.DtrEnable = $false
  $obd.RtsEnable = $false
  $obd.ReadTimeout = 800
  try { $obd.Open() } catch { Write-Host ("OBD 口打不开,改只记 VAN: " + $_.Exception.Message) -ForegroundColor Yellow; $obd = $null }
  if ($obd) {
    Write-Host ("OBD : " + $ObdPort + " @" + $ObdBaud) -ForegroundColor Green
    # ★ ELM327 初始化(2026-09-22 实测修):
    #   原版发完 AT 序列就立刻开始 0.4s 轮询 ⇒ **适配器还在扫协议**
    #   (K 线上要 1~2 秒),于是整轮采样全被记成 SEARCHING/STOPPED。
    #   现在:把每条 AT 的回应读出来(不再盲发),发完再**静等协议锁定**。
    foreach ($c in @('ATZ', 'ATE0', 'ATL0', 'ATH0', $ObdProto)) {
      try { $obd.Write($c + "`r") } catch {}
      $rr = ''; $w0 = [Diagnostics.Stopwatch]::StartNew()
      while ($w0.ElapsedMilliseconds -lt 1200) {
        try { if ($obd.BytesToRead -gt 0) { $rr += $obd.ReadExisting() } } catch {}
        if ($rr -match '>') { break }
        Start-Sleep -Milliseconds 60
      }
      Write-Host ("  {0,-7} -> {1}" -f $c, (($rr -replace "`r?`n", ' ').Trim()))
      Start-Sleep -Milliseconds 250
    }
    Write-Host "  等协议锁定(2.5s)..." -ForegroundColor Yellow
    Start-Sleep -Milliseconds 2500
    # 用一次 0100 确认真的通了(失败也只是警告,照样记录 —— 总比盲跑强)
    try { $obd.Write("0100`r") } catch {}
    $rv = ''; $w0 = [Diagnostics.Stopwatch]::StartNew()
    while ($w0.ElapsedMilliseconds -lt 8000) {
      try { if ($obd.BytesToRead -gt 0) { $rv += $obd.ReadExisting() } } catch {}
      if ($rv -match '>') { break }
      Start-Sleep -Milliseconds 80
    }
    $rvClean = ($rv -replace "`r?`n", ' ').Trim()
    # ★ 判据只用 "41 00"(真的有位图数据)。别用 BUS INIT —— 它是 BUS INIT: OK 的
    #   **前缀**,而适配器扫描期间会先吐 'BUS INIT: ...'(省略号,还没连上),
    #   于是造成**假阳性**(实测踩到:显示"已连上"却返回 SEARCHING...)。
    if ($rvClean -match '41 00') {
      Write-Host ("OBD : 已连上 ECU ✓  " + $rvClean.Substring(0, [Math]::Min(60, $rvClean.Length))) -ForegroundColor Green
    } else {
      Write-Host ("OBD : ⚠ 0100 未确认(" + $rvClean + ") —— 出车前先看这一行,别白跑") -ForegroundColor Red
    }
  }
}

Write-Host ""
Write-Host ("开始记录 " + [math]::Round($runSec/60.0,1) + " 分钟。★ 出发前/到达后各按一次双闪;中途停两次车。") -ForegroundColor Cyan
Write-Host ("  VAN -> " + $vanCsv)
if ($obd) { Write-Host ("  OBD -> " + $obdCsv) }
Write-Host ""

$vanLines = New-Object System.Collections.Generic.List[string]
# ★ 列在 2026-09-22 晚放宽(见文件头说明):整帧 + 标识/长度/cmd/ack,824 与 4FC 都进同一张表。
#   解析时**必须按 iden 过滤**(iden=824 的行 spd/rpm 才有意义)——
#   忘了过滤、把 4FC 的 d2 当车速读,是最容易犯的错。
$vanLines.Add('t_s,wall_time,iden,len,cmd,ack,d0,d1,d2,d3,d4,d5,d6,spd,rpm,seq')
$obdLines = New-Object System.Collections.Generic.List[string]
$obdLines.Add('t_s,wall_time,pid,ok,raw,value')

$t0 = Get-Date
$sw = [Diagnostics.Stopwatch]::StartNew()
$vanBuf = ''
$obdBuf = ''
$n824 = 0; $nObd = 0; $lastVanOk = 0.0; $vanStalled = $false
$lastFlush = 0
# OBD 轮询状态机(见循环里那段注释):一次只挂一个 PID,响应到/超时才轮到下一个
$obdPids = @('010D', '010C', '0105', '010F')
$pidIdx = 0; $curPid = ''; $resp = ''; $pidSentAt = 0.0; $nObdBad = 0

while ($sw.Elapsed.TotalSeconds -lt $runSec) {
  $el = $sw.Elapsed.TotalSeconds

  # ---- VAN:读干 FIFO,按行切 ----
  try {
    while ($van.BytesToRead -gt 0) {
      $n = $van.BytesToRead; if ($n -gt 4096) { $n = 4096 }
      $buf = New-Object byte[] $n
      $r = $van.Read($buf, 0, $n)
      if ($r -le 0) { break }
      $vanBuf += [System.Text.Encoding]::UTF8.GetString($buf, 0, $r)
    }
  } catch {}
  while ($vanBuf.Contains("`n")) {
    $i = $vanBuf.IndexOf("`n")
    $line = $vanBuf.Substring(0, $i).Trim(); $vanBuf = $vanBuf.Substring($i + 1)
    # ★ 宽着收:824(车速/转速)**和** 4FC(灯/门,现成的已知时间标记)都要;
    #   行尾那两行注释 `# cmd=.. ack=..` 单独摘出来(它们**不是**负载的一部分)。
    if ($line -match '^VAN ([0-9A-F]{3}) ((?:[0-9A-F]{2} ?)+)') {
      $idenHex = $Matches[1]
      $p = Parse-824 $Matches[2]
      if ($p) {
        $cmd = -1; $ack = -1
        if ($line -match 'cmd=(\d+)\s+ack=(\d+)') { $cmd = [int]$Matches[1]; $ack = [int]$Matches[2] }
        $n824++
        $lastVanOk = $el      # ★ VAN 活着的时间戳(见下面的失联告警)
        # 非 824 的帧:把随帧统计列写成空,免得后面误当车速读(列数不变,Csv 依然好用)
        $isSpd = ($idenHex -eq '824')
        $spdS = if ($isSpd) { [string]$p.spdCount } else { '' }
        $rpmS = if ($isSpd) { [string]$p.rpmCount } else { '' }
        $seqS = if ($isSpd) { [string]$p.seq }      else { '' }
        $vanLines.Add(("{0:N3},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15}" -f `
            $el, (Get-Date -Format 'HH:mm:ss.fff'), $idenHex, $p.datalen, $cmd, $ack,
          $p.d0, $p.d1, $p.d2, $p.d3, $p.d4, $p.d5, $p.d6, $spdS, $rpmS, $seqS))
      }
    }
  }

  # ---- ★ VAN 失联告警(2026-09-22 加) ----
  #   起因:实测板子中途被复位过一次(rst:0x1 POWERON),那之后 VAN 一个字节都没有,
  #   而 OBD 那半照常采样 —— **回来才发现 VAN 全空 = 白跑一趟**。
  #   这里一旦发现 VAN 连续 10 秒没有帧就**大声告警**(屏幕红字 + CSV 里插标记行),
  #   跑的时候就能看见,不用等回来。
  if ($el -ge 15 -and ($el - $lastVanOk) -ge 10) {
    if (-not $vanStalled) {
      $vanStalled = $true
      Write-Host ("!!! VAN 已失联 " + [int]($el - $lastVanOk) + " 秒 —— 板子可能被复位/掉线,检查 USB 与供电 !!!") -ForegroundColor Red
      $vanLines.Add(("#VAN_STALL_FROM,{0:N3},{1}" -f $el, (Get-Date -Format 'HH:mm:ss.fff')))
    }
  } elseif ($vanStalled -and ($el - $lastVanOk) -lt 3) {
    $vanStalled = $false
    Write-Host "VAN 已恢复" -ForegroundColor Green
    $vanLines.Add(("#VAN_RECOVER,{0:N3},{1}" -f $el, (Get-Date -Format 'HH:mm:ss.fff')))
  }

  # ---- OBD:4 个 PID 轮询(010D/010C/0105/010F),**与 VAN 交替**而不是阻塞 ----
  #   ★ 2026-09-22 晚重构。原版是「发一问 → Start-Sleep 120ms → 死等最多 2500ms」,
  #     这两三秒里**完全没读 VAN 口**:CH340 那点驱动缓冲在 115200 下一两秒就满,
  #     多出来的**默默丢掉**。只有 2 个 PID 时还勉强,加到 4 个 PID 后
  #     (4 × 2.5s = 最多 10s 的盲区)会把 VAN 那半彻底废掉。
  #   现在:每轮 15ms 的循环里**先读干 VAN**,再看这个状态机 ——
  #     ① 该发下一个 PID 就发(顺手把上一个 PID 的响应结算掉);
  #     ② 不等 sleep,等的是**墙上时间**;响应到了或超时才结算。
  #   超时 2500 → 1200ms:实测这个蓝牙 327 一问一答 200~400ms 就回来了,
  #   2500ms 只是上一版为「SEARCHING 之后才来」留的冗余;真超时会**计数并报**,
  #   不再静默降级(上一版就是静默降级骗我们白跑了一趟)。
  #   ★★ 结算条件必须是「收到 '>' 提示符」或超时,**不能是 BytesToRead>0** ——
  #     2026-09-22 晚实测踩到:327 会先吐半截 `SEARCHING..`,那**一个字符**就让
  #     BytesToRead 变正,于是我们在真数据到达前就结算了 ⇒ 209 条里 179 条失败。
  #     判据换成提示符以后,失败率才是个能看的数。
  if ($obd) {
    if ($curPid) {
      # --- 结算上一次:收全到 '>' 或超时 ---
      try { if ($obd.BytesToRead -gt 0) { $resp += $obd.ReadExisting() } } catch {}
      if (($resp -match '>') -or ($el - $pidSentAt -ge 1.2)) {
        $pp = Parse-Obd $resp $curPid
        $nObd++
        if (-not $pp.ok) { $nObdBad++ }
        $rawShort = ($resp -replace "`r?`n", ' ').Trim()
        if ($rawShort.Length -gt 60) { $rawShort = $rawShort.Substring(0, 60) }
        $obdLines.Add(("{0:N3},{1},{2},{3},{4},{5}" -f $el, (Get-Date -Format 'HH:mm:ss.fff'),
          $curPid, [int]$pp.ok, $rawShort.Replace(',', ';'), $pp.value))
        $curPid = ''
      }
    } else {                                                            # --- 发下一个 ---
      $curPid = $obdPids[$pidIdx]
      $pidIdx = ($pidIdx + 1) % $obdPids.Count
      $resp = ''
      $pidSentAt = $el
      try { $obd.Write($curPid + "`r") } catch { $curPid = '' }
    }
  }

  # ---- 每 2 秒落盘一次(拔线/断电也不丢已记的) ----
  if (($sw.ElapsedMilliseconds - $lastFlush) -ge 2000) {
    $lastFlush = $sw.ElapsedMilliseconds
    try { [IO.File]::WriteAllLines($vanCsv, $vanLines, (New-Object Text.UTF8Encoding($false))) } catch {}
    if ($obd) { try { [IO.File]::WriteAllLines($obdCsv, $obdLines, (New-Object Text.UTF8Encoding($false))) } catch {} }
    Write-Host ("  {0:N0}s  VAN 帧 {1}  OBD 采样 {2}{3}" -f $el, $n824, $nObd,
      $(if ($obd -and $nObd -gt 0 -and $nObdBad -gt 0) { "  其中失败 $nObdBad" } else { '' }))
  }
  Start-Sleep -Milliseconds 15
}

try { if ($van.IsOpen) { $van.Close() }; $van.Dispose() } catch {}
if ($obd) { try { if ($obd.IsOpen) { $obd.Close() }; $obd.Dispose() } catch {} }

[IO.File]::WriteAllLines($vanCsv, $vanLines, (New-Object Text.UTF8Encoding($false)))
if ($obd) { [IO.File]::WriteAllLines($obdCsv, $obdLines, (New-Object Text.UTF8Encoding($false))) }

Write-Host ""
Write-Host ("完成:VAN 824 帧 " + $n824 + " 条 / OBD 采样 " + $nObd + " 条 / 用时 " + [math]::Round($sw.Elapsed.TotalMinutes, 1) + " 分钟") -ForegroundColor Green
Write-Host ("  " + $vanCsv)
if ($obd) { Write-Host ("  " + $obdCsv) }
# ★ 覆盖率要当场看见:VAN 那半的判据是「824 帧/秒」够不够(实车 18/s 量级),
#   OBD 那半的判据是「失败率」。两条都不合格就别急着分析数据 —— 先修采集。
Write-Host ""
Write-Host ("采集质量:VAN 824 帧 " + [math]::Round($n824 / [Math]::Max(1.0, $sw.Elapsed.TotalSeconds), 1) + " 条/秒" +
            $(if ($obd) { ";OBD 失败 " + $nObdBad + "/" + $nObd + $(if ($nObd -gt 0 -and $nObdBad * 5 -gt $nObd) { "  ← 失败超过 20%,真值不可信!" } else { "" }) } else { "" })) -ForegroundColor $(if ($obd -and $nObd -gt 0 -and $nObdBad * 5 -gt $nObd) { 'Red' } else { 'Gray' })
if (-not $obd) {
  Write-Host ""
  Write-Host "⚠ 这次只记了 VAN。要定标还得有地面真值:" -ForegroundColor Yellow
  Write-Host "  · 把蓝牙 327 在 Windows 里配对 → 会出现一个新 COM 口 → 用 -ObdPort 指它重跑;" -ForegroundColor Yellow
  Write-Host "  · 或者用手机 App 记 010D,事后按墙上时间(两边的 wall_time 列)对齐。" -ForegroundColor Yellow
}
