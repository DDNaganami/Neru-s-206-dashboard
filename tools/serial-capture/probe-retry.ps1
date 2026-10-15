# probe-retry.ps1 -- wait until a COM port becomes openable, then capture raw lines to a file.
# WHY: after the 2026-10-15 crash the ports were denied at the driver level (orphaned handle),
# so a plain open fails; re-enumerating the device (unplug/replug) clears it. While waiting we
# must be ALREADY capturing, otherwise the one-shot boot banner (`boot: reason=... prev_up=...`)
# -- which carries the uptime before the reset -- is missed.
# ASCII ONLY (docs/BUILD-ENV.md trap 7/8; this harness runs Windows PowerShell 5.1).
param(
  [string]$Port = 'COM9',
  [int]$WaitSeconds = 300,
  [int]$CaptureSeconds = 150,
  [string]$Out = 'C:\206dash-scratch\retry-capture.log'
)
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$sp = $null
while ((Get-Date) -lt $deadline) {
  try {
    $sp = New-Object System.IO.Ports.SerialPort($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.ReadTimeout = 1500
    $sp.Open()
    "OPENED $Port at $((Get-Date).ToString('HH:mm:ss'))"
    break
  } catch {
    if ($sp) { try { $sp.Dispose() } catch {} ; $sp = $null }
    Start-Sleep -Milliseconds 1000
  }
}
if (-not $sp -or -not $sp.IsOpen) { "GIVE-UP: $Port never became openable within $WaitSeconds s"; exit 3 }
$enc = New-Object System.Text.UTF8Encoding($false)
$sw = New-Object System.IO.StreamWriter($Out, $false, $enc)
$end = (Get-Date).AddSeconds($CaptureSeconds)
$n = 0
while ((Get-Date) -lt $end) {
  try { $line = $sp.ReadLine(); $sw.WriteLine($line); $sw.Flush(); $n++ } catch {}
}
$sw.Close(); $sp.Close()
"CAPTURED $n lines -> $Out"
