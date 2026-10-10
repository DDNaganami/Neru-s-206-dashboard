# soak-report.ps1 -- pull the interesting lines out of a LONG soak capture.
#
# WHY: a fault that only shows up after hours ("tach panel grew streaks while the board
# sat idle") is diagnosed from what the board was doing AT THAT MOMENT -- and by then the
# log is megabytes of routine lines. This script cuts the window down and highlights the
# lines that can carry a cause:
#   panelguard:  -> the TCA9554 output register disagreed with our shadow (RST/CS clobbered)
#   boot: reason -> the board restarted (WDT / brownout / USB reset) and why
#   panel:       -> panel (re)initialisation events
#   rgb:         -> flush path: timeout= / fullrb>0 / forced>0KB / swap_wait_max=/phase_max=
#   loop:        -> stalls (stall=, max=) that could starve the display
#   obd-ble:     -> BLE state changes (connect attempts are radio+power bursts)
#   trust: / alert: -> what the UI was doing
#
# The capture file is written by watch-utf8.ps1 with FileShare.Read, so this can read it
# WHILE the capture is still running.
#
# ASCII ONLY (Windows PowerShell 5.1 reads a BOM-less .ps1 with the OEM codepage).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\serial-capture\soak-report.ps1 -Log C:\206dash-scratch\soak-slave-1744.log
#   ... -Minutes 20        # only the last 20 minutes (estimated from how the file grew)
param(
  [Parameter(Mandatory=$true)][string]$Log,
  [int]$Minutes = 0,
  [int]$MaxLines = 60
)
if (-not (Test-Path $Log)) { "no such log: $Log"; exit 2 }

function Read-Shared([string]$path) {
  $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open,
          [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  $sr = New-Object System.IO.StreamReader($fs, [Text.Encoding]::UTF8)
  $txt = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $txt
}

$item = Get-Item $Log
$txt  = Read-Shared $Log
$all  = $txt -split "`n"
"file    : $($item.Name)  $($item.Length) bytes  (mtime $($item.LastWriteTime.ToString('HH:mm:ss')))"
"lines   : $($all.Count)"

# --- window: the log grows at a roughly constant rate, so a byte fraction is a good
#     enough estimate of "the last N minutes" (the point is to find the moment, not to
#     be exact to the second).
$sel = $all
if ($Minutes -gt 0) {
  $spanMin = ($item.LastWriteTime - $item.CreationTime).TotalMinutes
  if ($spanMin -gt 0.5) {
    $frac = [Math]::Min(1.0, $Minutes / $spanMin)
    $keep = [int]($all.Count * $frac)
    if ($keep -lt 50) { $keep = [Math]::Min(50, $all.Count) }
    $sel = $all[($all.Count - $keep)..($all.Count - 1)]
    "window  : last ~$Minutes min (file spans ~$([Math]::Round($spanMin,1)) min) -> $($sel.Count) lines"
  } else { "window  : file too young to estimate; using the whole file" }
}

$pats = [ordered]@{
  'panelguard'   = 'panelguard:'
  'boot/reset'   = '^boot: reason|^panel: '
  'rgb anomaly'  = 'timeout=[1-9]|fullrb=[1-9]|stall=[1-9]'
  'loop stall'   = '^loop: .*\bstall=[1-9]'
  'ble'          = '^obd-ble: (state=connecting|state=connected)|onConnect|onDisconnect'
  'ui'           = '^trust:|^alert:'
}
""
foreach ($k in $pats.Keys) {
  $hits = @($sel | Where-Object { $_ -match $pats[$k] })
  "== {0,-12} {1} hit(s)" -f $k, $hits.Count
  if ($hits.Count) {
    $show = if ($hits.Count -gt $MaxLines) { $hits[($hits.Count - $MaxLines)..($hits.Count - 1)] } else { $hits }
    $show | ForEach-Object { "   " + $_.Trim() }
  }
}
""
"== context: first and last rgb line in the window"
($sel | Where-Object { $_ -match '^rgb: vsync' } | Select-Object -First 1) | ForEach-Object { "   first: " + ($_.Trim() -replace 'rgb: ..*','') }
($sel | Where-Object { $_ -match '^rgb: vsync' } | Select-Object -Last  1) | ForEach-Object { "   last : " + $_.Trim() }