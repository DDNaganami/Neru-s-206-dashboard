<#
  pio.ps1 -- the ONLY entry point for running PlatformIO on the laptop
             (every environment trap this machine has is baked in here)

  WHY THIS SCRIPT EXISTS -- 7 traps, each one cost half an hour when it was hit:

  1. MUST use PlatformIO's own venv python:
       C:\.platformio\penv\Scripts\python.exe
     There is another python on this machine (...\pythoncore-3.14-64\python.exe)
     that has platformio 6.2.0 but NOT esptool/intelhex.  Building `esp32s3` looks
     fine (no bin packing), but the moment bootloader.bin is needed it fails with
     `ModuleNotFoundError: No module named 'intelhex'`.
     Also: tool-esptoolpy/esptool.py has a bare `#!/usr/bin/env python` shebang and
     resolves python from PATH -> penv\Scripts must come FIRST on PATH.

  2. A non-ASCII username kills the host toolchain (GCC/ld/as).
     The symptom LOOKS like a link failure but is really "cannot write the .o":
       Fatal error: can't create C:\Users\<cjk>\AppData\Local\Temp\ccXXXX.o
       ld.exe: cannot find .../crt2.o / -lstdc++ / -lmingw32 ...
     Fix: give the compiler an ASCII-only TMP (GCC reads TMP before TEMP):
       TMP=C:\temp   and do NOT set TEMP (see trap 3).
     The toolchain itself also lives at C:\mingw64.

  3. NEVER change the user-level TEMP.  Pointing it at C:\temp for ASCII broke
     `import intelhex` (user site-packages resolution follows TEMP).
     Setting TMP alone is enough -- GCC honours it.

  4. The build directory must be ASCII: C:\206dash-build
     (the repo lives under C:\Users\<cjk>\...).

  5. The pioarduino environments must be built from an ASCII MIRROR.
     pioarduino's own build script cannot resolve FRAMEWORK_DIR under a CJK path:
       TypeError: argument should be a str or an os.PathLike object where
        __fspath__ returns a str, not <class 'NoneType'>
     So for the esp32s3-rgb* environments:
       robocopy <repo> C:\206dash-repo /MIR /XD .git .pio
     and build inside C:\206dash-repo.  (The official espressif32 envs --
     esp32s3 / esp32dev -- build fine in the original path.)

  6. PYTHONIOENCODING=utf-8 is NOT optional -- without it an UPLOAD CRASHES
     HALFWAY AND BRICKS THE SCREEN (measured 2026-09-28; cost: one board black
     for ~15 minutes plus one 10-minute timeout).
     Symptom: `pio run -t upload` connects, runs the stub, starts writing
     bootloader.bin -- and then:
       UnicodeEncodeError: 'gbk' codec can't encode character '\u2591'
     Root cause is neither the board nor USB: this machine's ANSI code page is
     936 (GBK) and esptool draws its write progress bar with U+2591 (the light
     shade block).  Encoding stdout as GBK throws, the upload thread dies in the
     middle of writing the bootloader, so the boot region is erased but not
     rewritten => the board is dark and the serial port is completely silent (the
     chip is alive and USB still enumerates, there is just no firmware to run).
     NOTE: `[Console]::OutputEncoding = UTF8` below does NOT help -- that only
     changes the .NET side; the python subprocess follows PYTHONIOENCODING and
     the machine ANSI code page.  Those are two different things here.
     Setting it before spawning the child (step 4.5 below) is the fix.
     RECOVERY after being bitten: no shorting, no manual download mode --
     just flash again (the chip is fine; a reflash rewrites 0x00000000).

  7. THIS FILE IS DELIBERATELY PURE ASCII, and that is now a rule.
     Measured 2026-09-28: after an edit tool rewrote this file as BOM-less
     UTF-8, Windows PowerShell 5.1 (the one this harness invokes as
     `powershell -File`) parsed it as ANSI/GBK; the block comment was no longer
     recognised AS a comment and the whole body was EXECUTED as code:
       Missing ')' in function parameter list
       The term '<cjk text>' is not recognized as a cmdlet ...
     So: a BOM-less UTF-8 .ps1 is not a cosmetic problem, the script simply does
     not run.  Keeping this file ASCII makes the encoding question moot.
     The long-form Chinese notes that used to live here are in docs/BUILD-ENV.md.
     If you ever DO add non-ASCII text back, save with a UTF-8 BOM and verify:
       [IO.File]::ReadAllBytes($p)[0..2]   # must be EF BB BF

  USAGE (from any PowerShell; ALWAYS quote the whole -Cmd argument):
     .\tools\build\pio.ps1 -Cmd 'test -e native'
     .\tools\build\pio.ps1 -Cmd 'run -e esp32s3-rgb'
     .\tools\build\pio.ps1 -Cmd 'run -e esp32s3 -t upload --upload-port COM7'

  WHICH ENVIRONMENT THE TWO 2.8C BOARDS ACTUALLY RUN:
     master (right screen) = esp32s3-rgb-master-now
     slave  (left  screen) = esp32s3-rgb-slave-now
  Both are wireless (ESP-NOW, LINK_PHY_UART=0) with their own partition table
  partitions-s3-now.csv.  `esp32s3` is the WIRED-link env and cannot coexist with
  VAN_RX_PIN=44 (the #error in lib/dashcore/van_phy_gpio.cpp guards exactly that).

  WHY `-Cmd '<one string>'` instead of passing pio arguments directly:
  PowerShell would treat `-e` as a parameter of THIS script ("parameter name 'e'
  is ambiguous", matching -ErrorAction), and `--` does not help under -File.
  One string sidesteps all of it.

  Exit code: pio's exit code is passed through (0 = success).
#>
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Cmd,
  [switch]$DryRun     # only print env + the command that would run
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ---- 1. python: prefer the PlatformIO venv ----
$py = 'C:\.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $py)) {
  Write-Host "ERROR: PlatformIO venv python not found: $py" -ForegroundColor Red
  Write-Host "       (without it there is no intelhex/esptool; bin packing fails)" -ForegroundColor Yellow
  exit 2
}

# ---- 2. PATH: penv Scripts (for esptool.py's shebang) + mingw64 (host compiler) ----
$penvScripts = Split-Path $py -Parent
$machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$user = [Environment]::GetEnvironmentVariable('Path', 'User')
$env:Path = "$penvScripts;C:\mingw64\bin;$machine;$user"

# ---- 3. temp dir: set TMP only (GCC honours it), leave TEMP alone (python needs it) ----
if (-not (Test-Path 'C:\temp')) { New-Item -ItemType Directory -Force -Path 'C:\temp' | Out-Null }
$env:TMP = 'C:\temp'
Remove-Item Env:TEMP -ErrorAction SilentlyContinue

# ---- 4. build dir must be ASCII ----
if (-not $env:PLATFORMIO_BUILD_DIR) { $env:PLATFORMIO_BUILD_DIR = 'C:\206dash-build' }

# ---- 4.5 python stdout MUST be utf-8 (trap 6 above) ----
# Without this, esptool's progress bar (U+2591) raises UnicodeEncodeError under the
# machine's GBK code page, the upload thread dies mid-bootloader, and the board goes
# dark with a silent serial port.  Must be PYTHONIOENCODING -- the
# [Console]::OutputEncoding line above only affects the .NET side.
$env:PYTHONIOENCODING = 'utf-8'

Write-Host ("pio.py   : " + $py) -ForegroundColor DarkGray
Write-Host ("TMP      : " + $env:TMP + "   (TEMP deliberately unset)") -ForegroundColor DarkGray
Write-Host ("build dir: " + $env:PLATFORMIO_BUILD_DIR) -ForegroundColor DarkGray
Write-Host ("cmd      : pio " + $Cmd) -ForegroundColor DarkGray
Write-Host ""
if ($DryRun) { exit 0 }

# ---- 5. run from the repo root (this script lives in tools\build\, so two levels up) ----
# Split the single -Cmd string on whitespace.  No pio argument contains a space
# except paths; if a path with spaces ever shows up, set the environment by hand
# for that one run rather than complicating this script.
$pioArgv = @($Cmd -split '\s+' | Where-Object { $_ })
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $repoRoot
try {
  & $py -m platformio @pioArgv
  $code = $LASTEXITCODE
} finally { Pop-Location }
exit $code
