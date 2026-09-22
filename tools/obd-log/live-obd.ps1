<#
  live-obd.ps1 —— 锁协议 + 实时轮询 OBD(零依赖,只用 Windows 自带 PowerShell)

  为什么要有它(2026-09-22):
    实测这只有线 327(自报 ELM327 v2.1,克隆板)有个通病:
      · ATSP0(自动协议搜索)一旦真去问车辆数据,适配器就掉线/复位;
      · 但只要**锁定协议**(如 ATSP3 = ISO 9141-2),它就稳,而且会老实回答
        "BUS INIT: . . .UNABLE TO CONNECT" —— 这是有意义的答案,不是崩溃。
    所以现场排查要能锁协议、能换协议、能看原始回话。

  用法:
    powershell -ExecutionPolicy Bypass -File live-obd.ps1
    powershell -ExecutionPolicy Bypass -File live-obd.ps1 -Port COM4 -Seconds 60
    powershell -ExecutionPolicy Bypass -File live-obd.ps1 -Protocols 3,5      # 只试这两个
    powershell -ExecutionPolicy Bypass -File live-obd.ps1 -Pids '010C,0105'   # 只看转速/水温

  产出:CSV(UTF-8 无 BOM,逐行 flush,拔线也不丢已记的行)
    列:t_s,proto,pid,ok,raw,value

  退出码:0 跑完 / 2 参数错 / 3 串口打不开
#>
param(
  [string]$Port = 'COM4',
  [int]$Baud = 38400,
  [int]$Seconds = 45,
  [int[]]$Protocols = @(3, 5, 6, 7),
  [string[]]$Pids = @('0100', '010C', '0105', '010D', '010F'),
  [string]$Out = '',
  [switch]$SelfTest
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$protoName = @{
  0 = 'Auto'; 1 = 'J1850 PWM'; 2 = 'J1850 VPW'; 3 = 'ISO 9141-2'
  4 = 'ISO 14230-4 KWP (5 baud)'; 5 = 'ISO 14230-4 KWP (fast)'
  6 = 'ISO 15765-4 CAN (11b/500k)'; 7 = 'ISO 15765-4 CAN (29b/500k)'
}

# ---------- 解析(与 obd-log.ps1 同口径,便于互相对照) ----------
function Parse-Resp([string]$raw, [string]$PidHex) {
  # 返回 @{ ok=bool; value=string; note=string }
  if (-not $raw) { return @{ ok = $false; value = ''; note = '无应答' } }
  $t = $raw -replace "`r", ' ' -replace "`n", ' '
  # 去掉命令回显(以我们要发的命令开头的那一段)
  $t = $t -replace [regex]::Escape($PidHex), ' '
  $t = ($t -replace '\s+', ' ').Trim()
  foreach ($kw in 'UNABLE TO CONNECT', 'BUS INIT', 'NO DATA', 'SEARCHING', 'CAN ERROR', 'ERR', '\?', 'STOPPED') {
    if ($t -match $kw) { return @{ ok = $false; value = ''; note = $kw.Trim('\?') } }
  }
  # 找 "41 <pid2> ..." 的数据段:去掉空格后按偶数字节扫
  $hex = ($t -replace '[^0-9A-Fa-f]', '').ToUpper()
  $pid2 = $PidHex.Substring(2, 2).ToUpper()
  $idx = $hex.IndexOf('41' + $pid2)
  if ($idx -lt 0) { return @{ ok = $false; value = ''; note = '没有 41 ' + $pid2 } }
  $rest = $hex.Substring($idx + 4)
  if ($rest.Length -lt 2) { return @{ ok = $false; value = ''; note = '数据太短' } }
  $bytes = @()
  for ($i = 0; $i + 1 -lt $rest.Length; $i += 2) { $bytes += [Convert]::ToInt32($rest.Substring($i, 2), 16) }
  switch ($pid2) {
    '0C' { if ($bytes.Count -ge 2) { return @{ ok = $true; value = [string](($bytes[0] * 256 + $bytes[1]) / 4); note = 'rpm' } } }
    '0D' { if ($bytes.Count -ge 1) { return @{ ok = $true; value = [string]$bytes[0]; note = 'km/h' } } }
    '05' { if ($bytes.Count -ge 1) { return @{ ok = $true; value = [string]($bytes[0] - 40); note = 'C' } } }
    '0F' { if ($bytes.Count -ge 1) { return @{ ok = $true; value = [string]($bytes[0] - 40); note = 'C' } } }
    default { return @{ ok = $true; value = $hex.Substring($idx); note = 'raw' } }
  }
  return @{ ok = $true; value = $hex.Substring($idx); note = 'raw' }
}

if ($SelfTest) {
  $cases = @(
    @{ r = 'ATZ'; p = '0100'; want = $false },
    @{ r = 'BUS INIT: . . .UNABLE TO CONNECT'; p = '0100'; want = $false },
    @{ r = 'NO DATA'; p = '0100'; want = $false },
    @{ r = '41 00 BE 3E B8 13'; p = '0100'; want = $true },
    @{ r = '41 0C 1A F8'; p = '010C'; want = $true },
    @{ r = '410C1AF8'; p = '010C'; want = $true },
    @{ r = '41 0D 3C'; p = '010D'; want = $true },
    @{ r = '41 05 7B'; p = '0105'; want = $true },
    @{ r = ''; p = '0100'; want = $false }
  )
  $fail = 0
  foreach ($c in $cases) {
    $res = Parse-Resp $c.r $c.p
    $got = [bool]$res.ok
    $mark = if ($got -eq $c.want) { '[OK  ]' } else { $fail++; '[FAIL]' }
    Write-Host ("{0} '{1}' / {2} => ok={3} value={4} note={5}" -f $mark, $c.r, $c.p, $got, $res.value, $res.note)
  }
  # 数值换算抽查
  $rpm = Parse-Resp '41 0C 1A F8' '010C'
  $mark = if ($rpm.value -eq '1726') { '[OK  ]' } else { $fail++; '[FAIL]' }
  Write-Host ("{0} 转速换算 1AF8 => {1} (期望 1726)" -f $mark, $rpm.value)
  $spd = Parse-Resp '41 0D 3C' '010D'
  $mark = if ($spd.value -eq '60') { '[OK  ]' } else { $fail++; '[FAIL]' }
  Write-Host ("{0} 车速换算 3C => {1} (期望 60)" -f $mark, $spd.value)
  Write-Host ""
  if ($fail -eq 0) { Write-Host "selftest 全部通过" -ForegroundColor Green; exit 0 }
  else { Write-Host "selftest 有 $fail 项失败" -ForegroundColor Red; exit 5 }
}

if (-not $Out) { $Out = Join-Path (Split-Path $PSCommandPath -Parent) 'obd_live.csv' }

function Write-Line($sp, $cmd, [int]$readMs = 2500) {
  try { $sp.Write($cmd + "`r") } catch { return @{ text = ''; err = '[写失败] ' + $_.Exception.Message } }
  Start-Sleep -Milliseconds 200      # ★ 克隆板复位后要 >150ms 才认命令,别压太紧
  $sb = New-Object System.Text.StringBuilder
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $readMs) {
    try {
      if ($sp.BytesToRead -gt 0) {
        $n = $sp.BytesToRead; $buf = New-Object byte[] $n
        $r = $sp.Read($buf, 0, $n)
        for ($i = 0; $i -lt $r; $i++) { [void]$sb.Append([char]$buf[$i]) }
      }
    } catch { return @{ text = ''; err = '[读失败] ' + $_.Exception.Message } }
    if ($sb.ToString() -match '>') { break }
    Start-Sleep -Milliseconds 60
  }
  return @{ text = $sb.ToString(); err = '' }
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$csv = New-Object System.Collections.Generic.List[string]
$csv.Add('t_s,proto,pid,ok,raw,value')

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable = $false    # ★ 必须 false:拉高会复位板子(见 obd-log.ps1 文件头)
$sp.RtsEnable = $false
$sp.ReadTimeout = 1000; $sp.WriteTimeout = 3000
try { $sp.Open() } catch { Write-Host ("串口打不开: " + $_.Exception.Message) -ForegroundColor Red; exit 3 }

Write-Host ("打开 " + $Port + " @" + $Baud) -ForegroundColor Green
$r = Write-Line $sp 'ATZ' 3000
Write-Host ("  ATZ -> " + (($r.text -replace "`r?`n", ' ').Trim()))
Start-Sleep -Milliseconds 500
Write-Line $sp 'ATE0' | Out-Null
Write-Line $sp 'ATL0' | Out-Null
Write-Line $sp 'ATH0'  | Out-Null
Write-Host ("  适配器电压: " + ((Write-Line $sp 'ATRV').text -replace "`r?`n", ' ').Trim())

foreach ($proto in $Protocols) {
  if ($sw.Elapsed.TotalSeconds -ge $Seconds) { break }
  $pn = if ($protoName.ContainsKey($proto)) { $protoName[$proto] } else { "协议 $proto" }
  Write-Host ""
  Write-Host ("===== 锁定 ATSP" + $proto + " (" + $pn + ") =====") -ForegroundColor Cyan
  $sp2 = $sp
  if (-not $sp2.IsOpen) {
    Write-Host "  串口已断,重开..." -ForegroundColor Yellow
    try { $sp2 = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
          $sp2.DtrEnable = $false; $sp2.RtsEnable = $false; $sp2.ReadTimeout = 1000; $sp2.WriteTimeout = 3000
          $sp2.Open(); Start-Sleep -Milliseconds 1500 } catch { Write-Host ("  重开失败: " + $_.Exception.Message) -ForegroundColor Red; continue }
  }
  $r = Write-Line $sp2 ("ATSP" + $proto) 2000
  if ($r.err) { Write-Host ("  ATSP$proto " + $r.err) -ForegroundColor Red; $sp = $sp2; continue }
  Write-Host ("  ATSP" + $proto + " -> " + (($r.text -replace "`r?`n", ' ').Trim()))
  $sp = $sp2

  foreach ($curPid in $Pids) {
    if ($sw.Elapsed.TotalSeconds -ge $Seconds) { break }
    $r = Write-Line $sp $curPid 2500
    $raw = ($r.text -replace "`r?`n", ' ').Trim()
    if ($r.err) {
      Write-Host ("    " + $curPid + " " + $r.err) -ForegroundColor Red
      $csv.Add(("{0:N1},{1},{2},0,{3},{4}" -f $sw.Elapsed.TotalSeconds, $proto, $curPid, $r.err.Replace(',', ';'), ''))
      break   # 串口断了,换下个协议(会重开)
    }
    $p = Parse-Resp $r.text $curPid
    $flag = if ($p.ok) { '有数据' } else { $p.note }
    $color = if ($p.ok) { 'Green' } else { 'DarkGray' }
    Write-Host ("    {0,-5} {1,-12} {2}" -f $curPid, $flag, ($p.value)) -ForegroundColor $color
    if ($raw.Length -gt 80) { $raw = $raw.Substring(0, 80) + '…' }
    $csv.Add(("{0:N1},{1},{2},{3},{4},{5}" -f $sw.Elapsed.TotalSeconds, $proto, $curPid, [int]$p.ok, $raw.Replace(',', ';'), $p.value))
    $csv | Set-Content -Path $Out -Encoding UTF8
    Start-Sleep -Milliseconds 120
  }
}

try { if ($sp.IsOpen) { $sp.Close() }; $sp.Dispose() } catch {}
$csv | Set-Content -Path $Out -Encoding UTF8
Write-Host ""
Write-Host ("CSV -> " + $Out) -ForegroundColor Green
$okRows = @($csv | Where-Object { $_ -match ',\d+,\d+,1,' }).Count
Write-Host ("有数据的行数: " + $okRows)
if ($okRows -eq 0) {
  Write-Host ""
  Write-Host "全程没有读到任何车辆数据。" -ForegroundColor Yellow
  Write-Host "  - 若回的是 UNABLE TO CONNECT => K 线那头 ECU 没应答(钥匙位置/接口没插紧/该协议不对)" -ForegroundColor Yellow
  Write-Host "  - 若一个协议都没有任何回话     => 适配器在协议搜索里掉线了(换 ATSP3 单独试)" -ForegroundColor Yellow
}
