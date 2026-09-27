# watch-utf8.ps1 -- capture one board's serial output to a file, decoded as UTF-8.
#
# WHY THIS EXISTS (learned the hard way, 2026-09-28):
#   The obvious ways to read a serial port in PowerShell decode bytes with the
#   console codepage (GBK on a Chinese Windows), so every Chinese log line comes
#   out as '??????'. Tonight's first captures were unreadable exactly that way, and
#   the judgement lines we actually needed ("启动闸门开了", "从板侧就绪", ...) are
#   Chinese. Reading the RAW stream and writing bytes as-is keeps them intact.
#
#   It also does NOT touch DTR/RTS (both held low), so opening the port does not
#   reset the board -- that matters when you are capturing a running system.
#
# Usage (Windows PowerShell 5.1; ASCII-only file on purpose):
#   powershell -ExecutionPolicy Bypass -File tools\serial-capture\watch-utf8.ps1 `
#       -Port COM8 -Seconds 60 -Out .\car-on.log
#
# Read the result with (note -Encoding UTF8 -- the file is raw board output):
#   Get-Content .\car-on.log -Encoding UTF8 | Select-String '启动闸门|state=|SRC '
param(
  [Parameter(Mandatory=$true)][string]$Port,
  [int]$Seconds = 30,
  [string]$Out = '.\serial.log',
  [int]$Baud = 115200
)

$p = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.ReadTimeout = 300
try { $p.Open() } catch { "OPEN-FAILED on $Port : $($_.Exception.Message)"; exit 2 }

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$fs = [System.IO.File]::Create($Out)
$buf = New-Object byte[] 4096
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  try {
    $n = $p.BaseStream.Read($buf, 0, $buf.Length)
    if ($n -gt 0) { $fs.Write($buf, 0, $n); $fs.Flush() }
  } catch { }
}
$fs.Close(); $p.Close()
"captured $Seconds s from $Port -> $Out ($((Get-Item $Out).Length) bytes)"
