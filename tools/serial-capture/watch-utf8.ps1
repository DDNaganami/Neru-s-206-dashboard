# watch-utf8.ps1 -- capture one board's serial output to a file, decoded as UTF-8.
#
# WHY THIS EXISTS (learned the hard way, 2026-09-28):
#   The obvious ways to read a serial port in PowerShell decode bytes with the
#   console codepage (GBK on a Chinese Windows), so every Chinese log line comes
#   out as '??????'. The judgement lines we actually need are Chinese, so this
#   script reads the RAW stream and writes the bytes as-is.
#
#   It does NOT touch DTR/RTS (both held low), so opening the port does not reset
#   the board -- that matters when you are capturing a running system.
#
# ** 2026-09-29 -- TWO HARD-WON FIXES, both from the OTA bring-up:
#   1. OPEN WITH RETRY. Right after a board reset (especially "press u to enter
#      OTA mode") the USB-Serial-JTAG RE-ENUMERATES and Open() fails for a while.
#      The old "fail and exit" behaviour produced a 0-byte file that looked like
#      "the board printed nothing" -- it sent us chasing the wrong bug twice.
#   2. REOPEN AND KEEP WRITING. A reset kills the open handle mid-capture, so the
#      boot lines (the evidence!) were lost while only the ROM banner made it
#      into the file. Now: close -> retry up to ~60s -> keep writing to the SAME
#      file and insert a marker line where the gap was.
#
#   3. "THE FILE IS EMPTY" IS NOT ALWAYS "THE BOARD IS DEAD" (2026-10-10): the log goes
#      over NATIVE USB-CDC on these boards, and after an esptool session that ended with a
#      plain `--after hard-reset` the chip can stay parked in the ROM stub: the app never
#      reaches loop(), the log ring is never drained, and the port opens but stays silent.
#      Reset with `--before usb-reset --after hard-reset` (or re-flash) BEFORE hunting a bug.
#
# *** THIS FILE MUST STAY PURE ASCII. Windows PowerShell 5.1 reads a BOM-less
#   .ps1 with the OEM/ANSI codepage (GBK here), so Chinese comment bytes get
#   mis-decoded -- and a stray byte can become a '}' or a quote and break the
#   parser. This file was broken exactly that way on 2026-09-29 ("Unexpected
#   token '}'"), while the same text is perfectly fine in a .md or .cpp file.
#
# Usage (Windows PowerShell 5.1):
#   powershell -ExecutionPolicy Bypass -File tools\serial-capture\watch-utf8.ps1 `
#       -Port COM8 -Seconds 60 -Out .\car-on.log
#
# Read the result with (note -Encoding UTF8 -- the file is raw board output):
#   Get-Content .\car-on.log -Encoding UTF8 | Select-String 'state=|SRC |ota:'
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

# --- fix 1: open with retry (USB re-enumeration after a reset) --------------
$opened = $false
for ($i = 0; $i -lt 40; $i++) {
  try { $p.Open(); $opened = $true; break } catch { Start-Sleep -Milliseconds 500 }
}
if (-not $opened) { "OPEN-FAILED on $Port after 20s"; exit 2 }

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$fs = [System.IO.File]::Create($Out)
$buf = New-Object byte[] 4096
$sw = [Diagnostics.Stopwatch]::StartNew()

while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  try {
    $n = $p.BaseStream.Read($buf, 0, $buf.Length)
    if ($n -gt 0) { $fs.Write($buf, 0, $n); $fs.Flush() }
  } catch {
    # *** fix 2b (2026-09-29): A READ TIMEOUT IS NOT A DISCONNECT. ***
    #   ReadTimeout is 300ms, so a board that is merely quiet (e.g. sitting in OTA
    #   mode, which prints only a 2s heartbeat) throws TimeoutException on every
    #   read. Treating that as "port lost" made this script reopen the port in a
    #   loop and DROP the lines it was meant to capture -- during the OTA test it
    #   printed dozens of bogus "port reopened" markers. Timeout => just continue.
    if ($_.Exception.GetType().Name -match 'Timeout' -or
        ($_.Exception.InnerException -and $_.Exception.InnerException.GetType().Name -match 'Timeout')) {
      continue
    }
    # --- fix 2: the board really reset -> reopen and continue in the same file ----
    try { $p.Close() } catch { }
    $re = $false
    # ~60s window: entering OTA mode takes longer than a plain reset.
    for ($k = 0; $k -lt 120 -and $sw.Elapsed.TotalSeconds -lt $Seconds; $k++) {
      Start-Sleep -Milliseconds 500
      try { $p.Open(); $re = $true; break } catch { }
    }
    $mark = [Text.Encoding]::UTF8.GetBytes("`r`n--- port reopened (board reset / USB re-enumeration) ---`r`n")
    $fs.Write($mark, 0, $mark.Length); $fs.Flush()
    if (-not $re) { "PORT-LOST on $Port (could not reopen within 60s)"; break }
  }
}
$fs.Close(); $p.Close()
"captured $Seconds s from $Port -> $Out ($((Get-Item $Out).Length) bytes)"
