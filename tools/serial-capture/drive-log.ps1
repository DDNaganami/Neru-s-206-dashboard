<#
  drive-log.ps1 —— 行驶采集:VAN 串口 + OBD 串口【同时】记录,时间戳同源

  为什么要有它(2026-09-22):
    车速字段(IDEN 0x824 / data[2])的标度**缺地面真值** —— "1 计数 = 1 km/h" 是
    最自然读法 + 物理自洽,但从没和真实车速对照过(见 ACCEPTANCE.md 的 ② 与 ★①)。
    转速那路不用再验(已三次实车互证),**只差车速斜率**。

    ★ 关键设计:**两个串口都接同一台笔记本**,所以两边的时间戳天然同源,
      不需要事后对齐(手机 App 那条路要手工对齐墙上时间,误差大)。

    VAN 侧  : 我们自己的板子(COM3, 115200) —— 记 IDEN 0x824 的 data[0..2]
    OBD 侧  : 蓝牙 ELM327(SPP 配对后枚举成 COM 口, 38400) —— 真值源 010D 车速 + 010C 转速

  产出两份 CSV(都在脚本旁边):
    van_drive.csv : t_s, wall_time, spd_count, data0, data1, rpm_count, seq
                    其中 spd_count = 0x824.data[2](待定标), rpm_count = data[0..1](16 位大端)
    obd_drive.csv : t_s, wall_time, pid, ok, raw, value
                    value 对 010D 是 km/h、对 010C 是 rpm —— **这就是地面真值**

  用法:
    # 双路(推荐):VAN + 蓝牙 OBD
    powershell -ExecutionPolicy Bypass -File drive-log.ps1 -VanPort COM3 -ObdPort COM5 -Minutes 15

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
  $b = [regex]::Matches($dataHex, '[0-9A-F]{2}') | ForEach-Object { [Convert]::ToInt32($_.Value, 16) }
  if ($b.Count -lt 7) { return $null }
  return [PSCustomObject]@{
    rpmCount = $b[0] * 256 + $b[1]
    spdCount = $b[2]
    data0    = $b[0]
    data1    = $b[1]
    d3       = $b[3]
    seq      = $b[6]
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
    @{ n = 'seq = 0x05 = 5';           got = $f.seq;      want = 5 }
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
    # ELM327 初始化(自动探测波特率:38400 不通换 9600 由调用方重跑)
    foreach ($c in @('ATZ', 'ATE0', 'ATL0', 'ATH0', 'ATSP0')) {
      try { $obd.Write($c + "`r") } catch {}
      Start-Sleep -Milliseconds 350
      try { $null = $obd.ReadExisting() } catch {}
    }
    Write-Host "OBD : AT 序列已发" -ForegroundColor Green
  }
}

Write-Host ""
Write-Host ("开始记录 " + [math]::Round($runSec/60.0,1) + " 分钟。★ 出发前/到达后各按一次双闪;中途停两次车。") -ForegroundColor Cyan
Write-Host ("  VAN -> " + $vanCsv)
if ($obd) { Write-Host ("  OBD -> " + $obdCsv) }
Write-Host ""

$vanLines = New-Object System.Collections.Generic.List[string]
$vanLines.Add('t_s,wall_time,spd_count,data0,data1,rpm_count,seq')
$obdLines = New-Object System.Collections.Generic.List[string]
$obdLines.Add('t_s,wall_time,pid,ok,raw,value')

$t0 = Get-Date
$sw = [Diagnostics.Stopwatch]::StartNew()
$vanBuf = ''
$obdBuf = ''
$nextObd = 0.0
$n824 = 0; $nObd = 0
$lastFlush = 0

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
    if ($line -match 'VAN 824 ((?:[0-9A-F]{2} ?)+)') {
      $p = Parse-824 $Matches[1]
      if ($p) {
        $n824++
        $vanLines.Add(("{0:N3},{1},{2},{3},{4},{5},{6}" -f $el, (Get-Date -Format 'HH:mm:ss.fff'),
          $p.spdCount, $p.data0, $p.data1, $p.rpmCount, $p.seq))
      }
    }
  }

  # ---- OBD:每 0.4s 轮一次 010D + 010C(车速优先,它才是待定标那个) ----
  if ($obd -and $el -ge $nextObd) {
    $nextObd = $el + 0.4
    foreach ($curPid in @('010D', '010C')) {
      try { $obd.Write($curPid + "`r") } catch { break }
      Start-Sleep -Milliseconds 120
      $resp = ''
      $w = [Diagnostics.Stopwatch]::StartNew()
      while ($w.ElapsedMilliseconds -lt 700) {
        try { if ($obd.BytesToRead -gt 0) { $resp += $obd.ReadExisting() } } catch {}
        if ($resp -match '>') { break }
        Start-Sleep -Milliseconds 40
      }
      $p = Parse-Obd $resp $curPid
      $nObd++
      $rawShort = ($resp -replace "`r?`n", ' ').Trim()
      if ($rawShort.Length -gt 60) { $rawShort = $rawShort.Substring(0, 60) }
      $obdLines.Add(("{0:N3},{1},{2},{3},{4},{5}" -f $el, (Get-Date -Format 'HH:mm:ss.fff'),
        $curPid, [int]$p.ok, $rawShort.Replace(',', ';'), $p.value))
    }
  }

  # ---- 每 2 秒落盘一次(拔线/断电也不丢已记的) ----
  if (($sw.ElapsedMilliseconds - $lastFlush) -ge 2000) {
    $lastFlush = $sw.ElapsedMilliseconds
    try { [IO.File]::WriteAllLines($vanCsv, $vanLines, (New-Object Text.UTF8Encoding($false))) } catch {}
    if ($obd) { try { [IO.File]::WriteAllLines($obdCsv, $obdLines, (New-Object Text.UTF8Encoding($false))) } catch {} }
    Write-Host ("  {0:N0}s  VAN 帧 {1}  OBD 采样 {2}" -f $el, $n824, $nObd)
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
if (-not $obd) {
  Write-Host ""
  Write-Host "⚠ 这次只记了 VAN。要定标还得有地面真值:" -ForegroundColor Yellow
  Write-Host "  · 把蓝牙 327 在 Windows 里配对 → 会出现一个新 COM 口 → 用 -ObdPort 指它重跑;" -ForegroundColor Yellow
  Write-Host "  · 或者用手机 App 记 010D,事后按墙上时间(两边的 wall_time 列)对齐。" -ForegroundColor Yellow
}
