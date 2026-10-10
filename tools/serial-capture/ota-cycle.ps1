# ota-cycle.ps1 -- ONE-SHOT on-board OTA cycle:
#   enter OTA mode -> win the WiFi fight -> upload firmware -> prove the slot rotated
#   -> put the desktop's normal network back.
#
# WHY IT MUST BE ONE SINGLE SCRIPT (read this before "simplifying" it):
#   The board's OTA mode is a WiFi SoftAP with NO INTERNET. Driving it step by step from
#   an agent session means the agent loses its own model connection the moment the
#   desktop switches to the board's AP -- that actually happened once ("DeepSeek API
#   request failed"). The whole round trip therefore runs inside ONE process, and the
#   normal network is restored before anybody needs it again.
#
# WHY THE WIFI PART IS SO PARANOID (every item below cost a real attempt, 2026-10-10):
#   1. `netsh wlan connect` is only a REQUEST. Windows keeps its own opinion: with other
#      profiles in "auto" mode it stays on -- or returns to -- the office/home network,
#      so the adapter reported SSID=<some other AP> and never got a 192.168.4.x address.
#      => every other profile is switched to "manual" for the duration and put back
#      EXACTLY as it was afterwards (modes are read from the profile store, not guessed).
#   2. "Joined" is not "reachable": Windows can report the right SSID while the adapter
#      still has no IPv4 address. Then /status times out and the POST never reaches
#      /update -- app1 stays 0xFF (erased) -- while the client only says "Unable to
#      connect to the remote server". => wait for a real 192.168.4.x address.
#   3. Do not parse netsh prose. Its labels are localized (Chinese Windows here) and CJK
#      literals inside a .ps1 get mangled by the GBK console codepage -- the first
#      version of this file silently listed ZERO profiles and then blamed the board.
#      => the profile store under Wlansvc\Profiles\Interfaces is read directly instead.
#   4. PowerShell resolves ALIASES before functions: a helper named Curl is dead code,
#      because `curl` is an alias for Invoke-WebRequest in Windows PowerShell 5.1.
#   5. Output produced inside a function becomes that function's RETURN VALUE, so Say
#      swallowed its own messages and made Join-Net return a truthy array -- a FAILED
#      join looked like a success. => Say writes to the host; Join-Net returns one bool.
#
# THE PROOF OF A SUCCESSFUL UPLOAD is the boot banner after the board restarts: the
#   "running slot / target slot" line swaps over (app0 <-> app1), or best of all the
#   normal boot line `boot: slot=app1 @0xa70000`. No HTTP bookkeeping is involved, and a
#   LOST HTTP RESPONSE DOES NOT MEAN A FAILED UPLOAD -- the board restarts as soon as the
#   write completes, which can kill the reply in flight.
#
# THIS FILE MUST STAY PURE ASCII. Windows PowerShell 5.1 reads a BOM-less .ps1 with the
#   OEM codepage (GBK here), so multi-byte comment bytes get mis-decoded and a stray byte
#   can become a quote or '}' and break the parser (that happened 2026-09-29, and again
#   2026-10-10 in this very file).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\serial-capture\ota-cycle.ps1 -Port COM8
#   ... -Bin C:\path\to\firmware.bin -Ap 206dash-SLAVE -HomeAp MikamoNeru
param(
  [string]$Port     = 'COM8',
  [string]$Bin      = '',
  [string]$Ap       = '206dash-SLAVE',
  [string]$HomeAp   = 'MikamoNeru',
  [string]$BoardIp  = '192.168.4.1',
  [string]$Origin   = 'http://127.0.0.1:8787',
  [int]$UploadTimeoutSec = 300,
  [string]$Work     = 'C:\206dash-scratch'
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cap  = Join-Path $PSScriptRoot 'watch-utf8.ps1'
$curlExe = Join-Path $env:SystemRoot 'System32\curl.exe'
if (-not (Test-Path $curlExe)) { $curlExe = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source }

function Say($m) { Write-Host "  $m" }
function WlanSsid {
  $l = netsh wlan show interfaces 2>&1 | Select-String '^\s+SSID\s+:' | Select-Object -First 1
  if ($l) { return ($l.Line -split ':',2)[1].Trim() } else { return '' }
}
function WlanIpv4 {
  $a = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
       Where-Object { $_.InterfaceAlias -match 'WLAN|Wi-Fi|WiFi' -and $_.IPAddress -notmatch '^169\.254\.' } |
       Select-Object -First 1
  if ($a) { return $a.IPAddress } else { return '' }
}
# name -> connectionMode, straight from the profile store: no prose parsing, no language
# assumptions, and the exact value needed to restore the machine afterwards.
function Get-ProfileTable {
  $tab = @{}
  $root = 'C:\ProgramData\Microsoft\Wlansvc\Profiles\Interfaces'
  foreach ($d in (Get-ChildItem $root -Directory -ErrorAction SilentlyContinue)) {
    foreach ($f in (Get-ChildItem $d.FullName -Filter '*.xml' -ErrorAction SilentlyContinue)) {
      $x = [IO.File]::ReadAllText($f.FullName, [Text.Encoding]::UTF8)
      $n = [regex]::Match($x, '<name>(.*?)</name>').Groups[1].Value
      $c = [regex]::Match($x, '<connectionMode>(.*?)</connectionMode>').Groups[1].Value
      if ($n) { $tab[$n] = $c }
    }
  }
  return $tab
}
function Set-ConnMode($name, $mode) { netsh wlan set profileparameter name="$name" connectionmode=$mode 2>&1 | Out-Null }
function Show-VisibleNetworks {
  $n = 0
  foreach ($l in (netsh wlan show networks 2>&1)) {
    if ($l -match '^\s*SSID\s') { Say "    " + $l.Trim(); $n++ }
  }
  if ($n -eq 0) { Say "    (the adapter reports no SSID at all -- scan cache empty or radio busy)" }
}
function ApVisible([string]$name) {
  foreach ($l in (netsh wlan show networks 2>&1)) { if ($l -match ':\s*' + [regex]::Escape($name) + '\s*$') { return $true } }
  return $false
}
function Run-Curl([string[]]$ArgList) { if (-not $curlExe) { return '' }; & $curlExe @ArgList 2>&1 | Out-String }
function Join-Net($name, [int]$WaitSec = 45, [string]$IpPrefix = '') {
  for ($try = 1; $try -le 2; $try++) {
    netsh wlan disconnect 2>&1 | Out-Null
    # THE IMPORTANT PART: `netsh wlan show networks` reports Windows' CACHED scan, so a
    # brand-new AP is simply absent until Windows runs its own periodic scan (a few
    # seconds after a disconnect). Connecting against a stale cache answers "connection
    # request was completed successfully" and then never associates -- which is how this
    # script spent an hour blaming the board. => poll until the AP is really listed.
    $vis = $false
    for ($v = 0; $v -lt 9 -and -not $vis; $v++) {
      Start-Sleep -Seconds 10
      $vis = ApVisible $name
      if (-not $vis) { Say "  waiting for a fresh scan to list '$name' ... ($(($v+1)*10)s)" }
    }
    if (-not $vis) { Say "  '$name' never appeared in the scan"; return $false }
    $raw = (netsh wlan connect name="$name" ssid="$name" 2>&1 | Out-String).Trim()
    Say "connect request (try $try) -> $raw"
    for ($i = 0; $i -lt $WaitSec; $i++) {
      Start-Sleep -Seconds 1
      $ssid = WlanSsid; $ip = WlanIpv4
      $okIp = ($ip -ne '')
      if ($IpPrefix -ne '') { $okIp = ($ip -like "$IpPrefix*") }
      $matched = ($ssid -eq $name)
      if ($IpPrefix -ne '') { $matched = $true }   # an address on the expected subnet IS the
                                                   # criterion -- the SSID field can read empty
                                                   # for a second or two mid-association
      if ($matched -and $okIp) { Say "on '$name' ip=$ip (try $try, $($i+1)s)"; return $true }
      if (($i % 10) -eq 9) { Say "  ...$($i+1)s: ssid='$ssid' ip='$ip'" }
    }
    Say "try $try gave up (ssid='$(WlanSsid)' ip='$(WlanIpv4)')"
  }
  return $false
}
function SendU {
  $sp = New-Object System.IO.Ports.SerialPort $Port,115200,'None',8,'One'
  $sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 300
  $ok = $false
  for ($i = 0; $i -lt 20 -and -not $ok; $i++) {
    try {
      $sp.Open(); Start-Sleep -Milliseconds 500; $sp.DiscardInBuffer()
      $sp.Write("u`r`n"); Start-Sleep -Milliseconds 300; $sp.Close(); $ok = $true
    } catch { Start-Sleep -Milliseconds 500; try { $sp.Close() } catch { } }
  }
  if (-not $ok) { Say "could not open $Port to send 'u'" }
  return $ok
}
function SerialCapture([int]$Seconds, [string]$Out) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File $cap -Port $Port -Seconds $Seconds -Out $Out | Out-Null
}
function BoardStatus($tag) {
  $t = (Run-Curl @('-sS','-i','-m','15','-H',"Origin: $Origin","http://$BoardIp/status")).Trim()
  if ($t) {
    Say "$tag /status:"
    $t -split "`r?`n" | Where-Object { $_ -match 'HTTP/|Access-Control|fw|role|running|target|ota_' } | ForEach-Object { Say "    " + $_.Trim() }
  } else { Say "$tag /status: EMPTY (nothing answered at $BoardIp)" }
  return $t
}

Say "wifi now: ssid='$(WlanSsid)' ip='$(WlanIpv4)'"
$modes = Get-ProfileTable
$profiles = @($modes.Keys)
Say "profiles (name=mode): $(($profiles | Sort-Object | ForEach-Object { "$_=$($modes[$_])" }) -join ', ')"

# ---- resolve the image -----------------------------------------------------
if (-not $Bin) {
  # The build tree is the ASCII mirror: PlatformIO and the toolchain choke on the
  # non-ASCII user-profile path, so the repo is edited here and BUILT there (docs/OTA.md).
  foreach ($c in @((Join-Path $repo '.pio\build\esp32s3-rgb-slave-ota\firmware.bin'),
                   'C:\206dash-scratch\now-vanraw\.pio\build\esp32s3-rgb-slave-ota\firmware.bin')) {
    if (Test-Path $c) { $Bin = $c; break }
  }
}
if (-not $Bin -or -not (Test-Path $Bin)) { Say "no image found (pass -Bin <firmware.bin>)"; exit 2 }
# curl.exe is an ANSI program: hand it an ASCII-only path, and never firmware.factory.bin
# (that one is the whole-flash image meant for USB).
$up = Join-Path $Work 'ota-upload.bin'
New-Item -ItemType Directory -Force $Work | Out-Null
Copy-Item $Bin $up -Force
$len = (Get-Item $up).Length
Say ("image: {0} ({1:N0} bytes, {2:N2} MB)" -f (Split-Path -Leaf $Bin), $len, ($len/1MB))
Say ("sha256: {0}" -f (Get-FileHash $up -Algorithm SHA256).Hash)

# ---- 1) enter OTA mode -----------------------------------------------------
Say "== step 1: send 'u' over $Port (reboot into OTA mode)"
if (-not (SendU)) { Say "cannot reach the board -- aborting"; exit 3 }
Start-Sleep -Seconds 6
SerialCapture 10 (Join-Path $Work 'ota-1.log')
Get-Content (Join-Path $Work 'ota-1.log') -Encoding UTF8 |
  Where-Object { $_ -match 'fw: build|boot: slot|ota: |panel: ' } | Select-Object -First 7 | ForEach-Object { Say $_.Trim() }

# ---- 2) win the WiFi fight, then join the AP -------------------------------
Say "== step 2: silence every other profile, then join '$Ap'"
foreach ($n in $profiles) { if ($n -ne $Ap) { Set-ConnMode $n 'manual' } }
Start-Sleep -Seconds 2
netsh wlan disconnect 2>&1 | Out-Null
Say "waiting for the AP to appear in a FRESH scan (Windows scans by itself after a disconnect)"
$vis = $false
for ($v = 0; $v -lt 9 -and -not $vis; $v++) {
  Start-Sleep -Seconds 10
  $vis = ApVisible $Ap
  if (-not $vis) { Say "  not listed yet ($(($v+1)*10)s)" }
}
Say "AP '$Ap' visible: $vis"
if (-not $vis) { Say "networks the adapter can see:"; Show-VisibleNetworks }
if (-not (Join-Net $Ap 45 '192.168.4.')) {
  Say "JOIN FAILED (no 192.168.4.x address). interface state:"
  (netsh wlan show interfaces 2>&1) | Where-Object { $_ -match ':\s*\S' } | Select-Object -First 12 | ForEach-Object { Say "    " + $_.Trim() }
  Say "restoring every profile and aborting"
  foreach ($n in $profiles) { Set-ConnMode $n $modes[$n] }
  Join-Net $HomeAp 45 | Out-Null
  exit 3
}
$before = BoardStatus 'before'

# ---- 3) upload -------------------------------------------------------------
Say "== step 3: upload to http://$BoardIp/update (curl.exe, multipart/form-data)"
$resp = Join-Path $Work 'ota-resp.txt'
$sw = [Diagnostics.Stopwatch]::StartNew()
$code = Run-Curl @('-sS','-m',"$UploadTimeoutSec",'-H',"Origin: $Origin",
                   '-F',"firmware=@$up;filename=firmware.bin;type=application/octet-stream",
                   '-o',$resp,'-w','%{http_code}',"http://$BoardIp/update")
Say ("  http_code=$($code.Trim())  elapsed={0:N1}s" -f $sw.Elapsed.TotalSeconds)
if (Test-Path $resp) { Get-Content $resp | Select-Object -First 5 | ForEach-Object { Say "  resp: $_" } }

# ---- 4) prove the slot rotated (serial only, no HTTP involved) -------------
Say "== step 4: wait for the reboot, re-enter OTA, read the slot bookkeeping"
Start-Sleep -Seconds 10
SendU | Out-Null
Start-Sleep -Seconds 6
SerialCapture 12 (Join-Path $Work 'ota-2.log')
$slot = Get-Content (Join-Path $Work 'ota-2.log') -Encoding UTF8 |
        Where-Object { $_ -match 'fw: build|boot: slot|ota: ' } | Select-Object -First 8
if ($slot) { $slot | ForEach-Object { Say $_.Trim() } } else { Say "  no boot banner captured (see $Work\ota-2.log)" }

# ---- 5) /status from the second OTA entry ---------------------------------
Say "== step 5: /status after the rotation check"
if (-not (Join-Net $Ap 30 '192.168.4.')) { Say "  could not rejoin '$Ap'" } else { $after = BoardStatus 'after' }

# ---- 6) restore ------------------------------------------------------------
Say "== step 6: restore every profile and go back to '$HomeAp'"
foreach ($n in $profiles) { Set-ConnMode $n $modes[$n] }
$check = Get-ProfileTable
$wrong = @($profiles | Where-Object { $check[$_] -ne $modes[$_] })
if ($wrong.Count) { Say "WARNING: these profiles did not go back to their old mode: $($wrong -join ', ')" } else { Say "all $($profiles.Count) profiles restored" }
if (Join-Net $HomeAp 45) { Say "back on $(WlanSsid) ip=$(WlanIpv4)" } else { Say "COULD NOT RESTORE -- reconnect manually" }

Say "== summary"
Say "image : $up  ($len bytes)"
Say "before: $($before -replace "`r?`n",' | ')"
Say "after : $($after  -replace "`r?`n",' | ')"