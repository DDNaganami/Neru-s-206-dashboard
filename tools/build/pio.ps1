<#
  pio.ps1 —— 笔记本上跑 PlatformIO 的**唯一入口**（把踩过的环境坑全固化成一处）

  为什么要有它（2026-09-27 一天之内踩了四个坑，每个都能浪费半小时）：

  1. **必须用 PlatformIO 自己的 venv python**
     `C:\.platformio\penv\Scripts\python.exe`
     ★ 这台机器上还有另一个 python（`...\pythoncore-3.14-64\python.exe`），
       它**装了 platformio 6.2.0 但没装 esptool/intelhex** ⇒ 编 `esp32s3` 时看不出来
       （不打包 bin），一到要生成 `bootloader.bin` 就报
       `ModuleNotFoundError: No module named 'intelhex'`。
       ★ 而 `tool-esptoolpy/esptool.py` 只有 `#!/usr/bin/env python`，
         它会去 PATH 上找 python ⇒ **把 penv 的 Scripts 放进 PATH 最前**也一样重要。

  2. **中文用户名会把宿主机工具链打死**（GCC/ld/as）
     症状**看着像链接失败、其实是连中间 .o 都写不出来**：
       `Fatal error: can't create C:\Users\<中文>\AppData\Local\Temp\ccXXXX.o`
       `ld.exe: cannot find .../crt2.o / -lstdc++ / -lmingw32 ...`（一连串）
     修法：**只给编译器一个 ASCII 的 TMP**（GCC 优先读 `TMP`，其次 `TEMP`）：
       `$env:TMP='C:\temp'`，并且**不要再设 `$env:TEMP`** ——
       TEMP 一改，Python 的用户 site-packages 就 import 不到（见坑 3）。
     ★ 工具链本身也搬到了 `C:\mingw64`（winget 装的那份在带中文的 WinGet 路径下）。

  3. **别改用户级的 `TEMP`**。曾经把它指到 `C:\temp` 求 ASCII，结果
     `intelhex` 直接 import 不到（用户 site-packages 的解析跟着变）。
     只改 `TMP` 就够，GCC 认它。

  4. **构建目录要 ASCII**：`C:\206dash-build`
     （仓库在 `C:\Users\张九思\...` 里，路径带中文）。

  5. ★★ **pioarduino 那几档还得从 ASCII 镜像编**（2026-09-27 实测）
     pioarduino 平台自己的构建脚本（`platforms/espressif32@src-*/builder/frameworks/
     arduino.py` → `pioarduino-build.py`）在**中文路径**下解不出 `FRAMEWORK_DIR`：
       `TypeError: argument should be a str or an os.PathLike object where
        __fspath__ returns a str, not <class 'NoneType'>`
     ⇒ 用 pioarduino 的档（`esp32s3-rgb*`）要从**纯 ASCII 的镜像**编：
       `robocopy C:\Users\张九思\206Dash\Neru-s-206-dashboard C:\206dash-repo /MIR /XD .git .pio`
       然后在 `C:\206dash-repo` 里编（实测：编译 SUCCESS，128 秒）。
     （官方 `espressif32` 的档 —— `esp32s3` / `esp32dev` —— 在原路径能编，不用镜像。）

  用法（在仓库里的任意 PowerShell 里；**pio 的参数要整串用引号包起来**）：
    .\tools\build\pio.ps1 -Cmd 'test -e native'
    .\tools\build\pio.ps1 -Cmd 'run -e esp32s3-rgb'
    .\tools\build\pio.ps1 -Cmd 'run -e esp32s3 -t upload --upload-port COM7'

  ★★ **两块 2.8C 真正跑的是哪一档**（编错会被代码里的闸门拦下 —— 那正是闸门的用处）：
    · 主板（右屏）= `esp32s3-rgb-master-now`
    · 从板（左屏）= `esp32s3-rgb-slave-now`
    这两个是**无线档**（ESP-NOW，`LINK_PHY_UART=0`）+ 自己的分区表
    `partitions-s3-now.csv`。而 `esp32s3` 是**有线链路**档 ⇒ 它与 `VAN_RX_PIN=44`
    不能共存（`lib/dashcore/van_phy_gpio.cpp` 里那道 `#error` 就是拦这个）。

  ★ 为什么是 `-Cmd '整串'` 而不是直接 `pio.ps1 run -e esp32s3`：
    PowerShell 会把 `-e` 当成**本脚本自己的参数名**（报 "parameter name 'e' is
    ambiguous"，匹配到 `-ErrorAction`），而 `--` 分隔符在 `-File` 调用下也不吃。
    整串传参绕开这一切，也不用管谁在解析引号。

  退出码：透传 pio 的退出码（0 成功）。
#>
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Cmd,
  [switch]$DryRun     # 只打印环境与将要执行的命令，不真跑
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ---- 1. python：优先 PlatformIO venv ----
$py = 'C:\.platformio\penv\Scripts\python.exe'
if (-not (Test-Path $py)) {
  Write-Host "★ 找不到 PlatformIO 的 venv python: $py" -ForegroundColor Red
  Write-Host "  （没有它就没有 intelhex/esptool，打包 bin 会失败）" -ForegroundColor Yellow
  exit 2
}

# ---- 2. PATH：penv 的 Scripts（给 esptool.py 的 shebang 用）+ mingw64（宿主机编译器） ----
$penvScripts = Split-Path $py -Parent
$machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$user = [Environment]::GetEnvironmentVariable('Path', 'User')
$env:Path = "$penvScripts;C:\mingw64\bin;$machine;$user"

# ---- 3. 临时目录：只改 TMP（GCC 认它），**不动 TEMP**（Python 要用真的那个） ----
if (-not (Test-Path 'C:\temp')) { New-Item -ItemType Directory -Force -Path 'C:\temp' | Out-Null }
$env:TMP = 'C:\temp'
Remove-Item Env:TEMP -ErrorAction SilentlyContinue

# ---- 4. 构建目录：纯 ASCII ----
if (-not $env:PLATFORMIO_BUILD_DIR) { $env:PLATFORMIO_BUILD_DIR = 'C:\206dash-build' }

Write-Host ("pio.py   : " + $py) -ForegroundColor DarkGray
Write-Host ("TMP      : " + $env:TMP + "   (TEMP 故意不设)") -ForegroundColor DarkGray
Write-Host ("build dir: " + $env:PLATFORMIO_BUILD_DIR) -ForegroundColor DarkGray
Write-Host ("cmd      : pio " + $Cmd) -ForegroundColor DarkGray
Write-Host ""
if ($DryRun) { exit 0 }

# ---- 5. 在仓库根跑（脚本在 tools\build\ 下，上两级就是仓库根） ----
# ★ `-Cmd '整串'` 按空格切开再传（pio 的参数里没有带空格的值，除了路径 —— 真遇到
#   带空格的路径，那一次就手动 set 环境自己跑，别把这个脚本搞复杂）。
$pioArgv = @($Cmd -split '\s+' | Where-Object { $_ })
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $repoRoot
try {
  & $py -m platformio @pioArgv
  $code = $LASTEXITCODE
} finally { Pop-Location }
exit $code
