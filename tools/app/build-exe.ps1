# Build the 206Dash workbench into a single .exe (PyInstaller).
#
#   powershell -ExecutionPolicy Bypass -File tools\app\build-exe.ps1
#
# WHY THIS EXISTS: the user asked "can't it be packaged into an executable?".
#   The app is a stdlib-only Python server + three static files, so packaging is
#   boring on purpose -- no dependency hell. What DOES need care is that the exe
#   must still find the repository (that is where the editors / preview pages /
#   PlatformIO live), so:
#     * the three app files are embedded (--add-data) so the UI opens even with
#       no repository in sight;
#     * the exe locates the repo itself (see find_repo() in server.py), and
#       --repo lets you point it explicitly;
#     * console build (not --noconsole) so the log stays visible, and the UI has
#       a "退出应用" button that stops the server cleanly.
#
# ASCII only (PowerShell 5.1 reads BOM-less .ps1 as GBK).
param(
  [string]$Name = '206Dash-Workbench',
  [string]$OutDir,
  [switch]$OneFile
)

$ErrorActionPreference = 'Continue'
$here = $PSScriptRoot                       # ...\tools\app
$repo = Split-Path (Split-Path $here -Parent) -Parent
if (-not $OutDir) { $OutDir = Join-Path $here 'dist' }

# the python that carries PyInstaller (the project venv; falls back to PATH)
$py = 'C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }

Write-Host "repo   : $repo"
Write-Host "python : $py"
Write-Host ("mode   : " + $(if ($OneFile) { 'onefile (single .exe, ~1-2s startup)' } else { 'onedir (folder, faster startup)' }))

# PyInstaller writes its work next to the spec; keep both inside tools\app\build
$work = Join-Path $here 'build'
$spec = Join-Path $here 'build'

$args = @(
  '-m', 'PyInstaller',
  '--noconfirm', '--clean',
  '--name', $Name,
  '--distpath', $OutDir,
  '--workpath', $work,
  '--specpath', $spec,
  # embed the UI so the app opens without a repo
  '--add-data', "$here\index.html;tools/app",
  '--add-data', "$here\app.css;tools/app",
  '--add-data', "$here\app.js;tools/app",
  '--add-data', "$here\README.md;tools/app",
  # console build: the log is the point (and --noconsole would hide it)
  '--console'
)
if ($OneFile) { $args += '--onefile' }
$args += (Join-Path $here 'server.py')

Write-Host "running: $py $($args -join ' ')"
& $py @args 2>&1 | Select-Object -Last 12

$exe = if ($OneFile) { Join-Path $OutDir "$Name.exe" } else { Join-Path $OutDir "$Name\$Name.exe" }
if (Test-Path $exe) {
  $f = Get-Item $exe
  Write-Host ""
  Write-Host "OK -> $($f.FullName)"
  Write-Host ("     {0:N1} MB   {1}" -f ($f.Length / 1MB), $f.LastWriteTime)
  Write-Host ""
  Write-Host "run it:"
  Write-Host "  `"$($f.FullName)`"                       # auto-detects the repo, opens the browser"
  Write-Host "  `"$($f.FullName)`" --app-window          # chromeless window"
  Write-Host "  `"$($f.FullName)`" --repo <repo path>    # point it at the repo explicitly"
  Write-Host "  `"$($f.FullName)`" --port 8899 --no-open"
} else {
  Write-Host "FAILED: no exe at $exe"
  exit 1
}
