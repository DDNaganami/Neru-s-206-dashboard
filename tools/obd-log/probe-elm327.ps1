# 最小 ELM327 串口探针：绕开 obd-log.ps1 的流程，直接看原始字节
# 用途：判断"AT 命令有应答但数据请求失败"到底是适配器问题还是脚本问题
param(
  [string]$Port = 'COM4',
  [int]$Baud = 38400,
  [int]$ReadMs = 3000
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

function Try-Cmd($sp, $cmd) {
  Write-Host ("--- 发送: " + $cmd + " ---") -ForegroundColor Cyan
  try {
    # ★ DTR/RTS 必须保持 false（见 obd-log.ps1 文件头：拉高会复位板子）
    $sp.Write($cmd + "`r")
  } catch {
    Write-Host ("  写失败: " + $_.Exception.Message) -ForegroundColor Red
    return $null
  }
  Start-Sleep -Milliseconds 150
  $sb = New-Object System.Text.StringBuilder
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $ReadMs) {
    try {
      if ($sp.BytesToRead -gt 0) {
        $n = $sp.BytesToRead
        $buf = New-Object byte[] $n
        $read = $sp.Read($buf, 0, $n)
        for ($i = 0; $i -lt $read; $i++) { [void]$sb.Append([char]$buf[$i]) }
      }
    } catch {
      Write-Host ("  读失败: " + $_.Exception.Message) -ForegroundColor Red
      break
    }
    if ($sb.ToString() -match '>') { break }
    Start-Sleep -Milliseconds 50
  }
  $raw = $sb.ToString()
  if ($raw.Length -eq 0) {
    Write-Host "  (无应答)" -ForegroundColor Yellow
  } else {
    $vis = $raw -replace "`r", '<CR>' -replace "`n", '<LF>'
    Write-Host ("  原始: " + $vis)
  }
  return $raw
}

Write-Host ("打开 " + $Port + " @" + $Baud + " ...") -ForegroundColor Green
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.ReadTimeout = 1000
$sp.WriteTimeout = 3000
try { $sp.Open() } catch { Write-Host ("打开失败: " + $_.Exception.Message) -ForegroundColor Red; exit 1 }
Write-Host ("已打开。DTR=" + $sp.DtrEnable + " RTS=" + $sp.RtsEnable + " IsOpen=" + $sp.IsOpen)

$results = [ordered]@{}
foreach ($c in @('ATZ', 'ATE0', 'ATI', 'ATSP0', '0100', '010C', '010D')) {
  $r = Try-Cmd $sp $c
  $results[$c] = if ($null -eq $r) { '(写失败)' } elseif ($r.Length -eq 0) { '(无应答)' } else { ($r -replace "`r?`n", ' | ').Trim() }
  Start-Sleep -Milliseconds 300
}

Write-Host ""
Write-Host "===== 汇总 =====" -ForegroundColor Green
foreach ($k in $results.Keys) { Write-Host ("  {0,-7} -> {1}" -f $k, $results[$k]) }

try { $sp.Close(); $sp.Dispose() } catch {}
Write-Host ""
Write-Host "完成。"
