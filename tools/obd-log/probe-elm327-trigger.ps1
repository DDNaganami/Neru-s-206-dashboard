# 判定 ELM327 复位的触发点：ATSP0 之后还活着吗？还是一碰车辆数据就死？
# 每轮都重新开关串口 —— 掉了线也能继续测，不会因为一个失败就中断整轮
param([string]$Port = 'COM4', [int]$Baud = 38400, [int]$ReadMs = 3500)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

function Run-Seq([string[]]$cmds) {
  $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
  $sp.DtrEnable = $false; $sp.RtsEnable = $false
  $sp.ReadTimeout = 1000; $sp.WriteTimeout = 3000
  $log = @()
  try { $sp.Open() } catch { return @("串口打不开: " + $_.Exception.Message) }
  foreach ($c in $cmds) {
    $line = "    {0,-6} -> " -f $c
    try { $sp.Write($c + "`r") } catch { $log += ($line + "[写失败] " + $_.Exception.Message); break }
    Start-Sleep -Milliseconds 150
    $sb = New-Object System.Text.StringBuilder
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ReadMs) {
      try {
        if ($sp.BytesToRead -gt 0) {
          $n = $sp.BytesToRead; $buf = New-Object byte[] $n
          $r = $sp.Read($buf, 0, $n); for ($i=0; $i -lt $r; $i++) { [void]$sb.Append([char]$buf[$i]) }
        }
      } catch { $log += ($line + "[读失败] " + $_.Exception.Message); $sb = $null; break }
      if ($sb -and $sb.ToString() -match '>') { break }
      Start-Sleep -Milliseconds 60
    }
    if ($null -eq $sb) { break }
    $t = ($sb.ToString() -replace "`r?`n", ' ').Trim()
    if ($t.Length -eq 0) { $t = '(无应答)' } elseif ($t.Length -gt 70) { $t = $t.Substring(0,70) + '…' }
    $log += ($line + $t)
  }
  try { $sp.Close(); $sp.Dispose() } catch {}
  return $log
}

$tests = [ordered]@{
  'A: 只发 ATZ'                      = @('ATZ')
  'B: ATZ 后纯 AT 命令'              = @('ATZ', 'ATI', 'ATRV')
  'C: ATSP0 之后还发 AT 命令'        = @('ATZ', 'ATE0', 'ATSP0', 'ATRV', 'ATI')
  'D: ATSP0 之后发车辆数据请求'      = @('ATZ', 'ATE0', 'ATSP0', '0100')
  'E: 不发 ATSP0 直接问数据'         = @('ATZ', 'ATE0', '0100')
  'F: 指定协议 3 (ISO9141-2) 再问'   = @('ATZ', 'ATE0', 'ATSP3', '0100')
}

foreach ($name in $tests.Keys) {
  Write-Host ("--- " + $name + " ---") -ForegroundColor Cyan
  Run-Seq $tests[$name] | ForEach-Object { Write-Host $_ }
  Start-Sleep -Milliseconds 800
}
Write-Host ""
Write-Host "判定方法:" -ForegroundColor Yellow
Write-Host "  B 全通 + C 全通  => 适配器健康,只有'车辆数据请求'触发复位"
Write-Host "  B 全通 + C 卡死  => ATSP0(协议搜索)本身就把它搞死了"
Write-Host "  E 也卡死         => 连协议搜索都没到就死,更靠底层"
