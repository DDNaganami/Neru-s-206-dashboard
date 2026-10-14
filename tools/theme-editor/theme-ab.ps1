# ab.ps1 -- theme range A/B on the host (pcpreview frames + per-pixel diff)
#   3 arms: A=defaults(60~130/0~80)   B=ranges(20~100/0~150)   C=control(bg_color only)
#
#   WHY NOT `pio run -t exec`: the preview program does NOT exit after its 150 frames
#   (README says "Ctrl+C 后打开 preview/preview.html"), so -t exec blocks forever and
#   only the first arm would ever run.  => build once, then run program.exe directly
#   under a timeout per arm, with THEME_FILE set differently for each arm.
#
#   Guards: every arm must (a) leave a FRESH frame mtime and (b) have written 150+150 frames.
#   ASCII ONLY (tools/build/pio.ps1 trap 7: a BOM-less UTF-8 .ps1 is parsed as GBK by
#   Windows PowerShell 5.1 -- which is what this harness runs -- mangling CJK paths).
$ErrorActionPreference = 'Continue'
"PSVersion: $($PSVersionTable.PSVersion)"

$repo   = Join-Path $env:USERPROFILE '206Dash\Neru-s-206-dashboard'
$pylibs = Join-Path $env:USERPROFILE '206Dash\.pio-pylibs'
$copy   = 'C:\206dash-scratch\now-vanraw'
$py     = 'C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe'
$exe    = Join-Path $copy '.pio\build\pcpreview\program.exe'
foreach ($p in @($repo, $copy, $py, $pylibs)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

"== robocopy sync =="
robocopy $repo $copy /E /XD .git .pio /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed: $LASTEXITCODE" }

$env:PYTHONPATH          = $pylibs
$env:PLATFORMIO_CORE_DIR = 'C:\206dash-scratch\pio-core-mix'
$env:PATH                = 'C:\206dash-scratch\zigbin;' + $env:PATH
$env:PYTHONUTF8          = '1'
$env:PYTHONIOENCODING    = 'utf-8'
Set-Location $copy

# a previously aborted run leaves program.exe alive holding the output file
Get-Process -Name program -ErrorAction SilentlyContinue | ForEach-Object {
  "killing stale program.exe pid=$($_.Id)"; try { $_.Kill() } catch {}
}
Start-Sleep -Milliseconds 500

"== build once (pcpreview) =="
$blog = 'C:\206dash-scratch\ab-build.log'
cmd /c "`"$py`" -m platformio run -e pcpreview > `"$blog`" 2>&1"
$bcode = $LASTEXITCODE
if (-not (Select-String -Path $blog -Pattern '\[SUCCESS\]' -Quiet)) {
  throw "build failed (exit=$bcode): $((Get-Content $blog -Tail 5) -join ' | ')"
}
"build ok: $((Get-Item $exe).LastWriteTime.ToString('HH:mm:ss'))  log=$((Get-Item $blog).Length)B"

$frames = Join-Path $copy 'preview\frames'
function Run-Arm([string]$name, [string]$themeFile) {
  if ($themeFile) { $env:THEME_FILE = $themeFile } else { Remove-Item Env:THEME_FILE -ErrorAction SilentlyContinue }
  Remove-Item (Join-Path $frames '*.bmp') -Force -ErrorAction SilentlyContinue
  $t0 = Get-Date
  $p = Start-Process -FilePath $exe -WorkingDirectory $copy -NoNewWindow -PassThru
  Start-Sleep -Seconds 42
  if (-not $p.HasExited) { $p.Kill(); Start-Sleep -Milliseconds 500 }
  $got = Get-ChildItem (Join-Path $frames '*.bmp')
  if ($got.Count -lt 300) { throw "ARM $name produced only $($got.Count) frames (expected 300)" }
  $f = Get-Item (Join-Path $frames 'l_0030.bmp')
  if ($f.LastWriteTime -lt $t0) { throw "ARM $name frames are stale" }
  $d = "C:\206dash-scratch\ab\$name"
  New-Item -ItemType Directory -Force $d | Out-Null
  Copy-Item (Join-Path $frames '*.bmp') "$d\" -Force
  $src = if ($themeFile) { $themeFile } else { '(defaults)' }
  "ARM $name ok: frames=$($got.Count) fresh=$($f.LastWriteTime.ToString('HH:mm:ss')) theme=$src"
}

Run-Arm 'A' $null
Run-Arm 'B' 'C:\206dash-scratch\theme-rangeB.json'
Run-Arm 'C' 'C:\206dash-scratch\theme-colorC.json'

"== A vs B (only the four range keys differ) =="
& node 'C:\206dash-scratch\ab-diff.js' 'C:\206dash-scratch\ab\A' 'C:\206dash-scratch\ab\B' 2>&1 | Select-Object -First 10
"== A vs C (only one colour key differs -- control) =="
& node 'C:\206dash-scratch\ab-diff.js' 'C:\206dash-scratch\ab\A' 'C:\206dash-scratch\ab\C' 2>&1 | Select-Object -First 6
