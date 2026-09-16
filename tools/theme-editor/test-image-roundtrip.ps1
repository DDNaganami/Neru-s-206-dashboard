<#
.SYNOPSIS
  JS 打包器 ↔ 固件解析器 的往返一致性测试。

.DESCRIPTION
  image.bin 这个二进制格式由两套代码共同维护:
    · 生成方 tools/theme-editor/image-blob-build.js (浏览器界面 / Node)
    · 读取方 lib/themetool/image_blob.cpp           (ESP32 固件)
  两边只要有一处对不上(字段偏移、字节序、名字长度、stride 算法),
  症状是"图不显示"甚至"读越界",而编译期完全看不出来。

  这个脚本就是防这个的:用 JS 打一个测试镜像,再让**固件自己的解析器**
  读它,逐字段、逐字节对账。

  三件事:
    1. Node 侧打包器的单元测试(image-blob-build.js 的断言)
    2. 生成测试镜像 + manifest(可读的对账清单)
    3. 跑 native 测试,其中 test_image_roundtrip 用固件解析器核对镜像

  日常开发只要在提交前跑一次:
     pwsh tools/theme-editor/test-image-roundtrip.ps1

.NOTES
  ★★ 这个文件必须存成**带 BOM 的 UTF-8** ★★
  Windows PowerShell 5.1 读没有 BOM 的 .ps1 会按 GBK 解释,脚本里的中文
  会变成乱码,进而**破坏注释块与字符串**,报一堆莫名其妙的
  `Missing argument in parameter list` / `The string is missing the terminator`。
  用编辑器/工具改完这个文件后,补 BOM:
    $p='...\test-image-roundtrip.ps1'
    $b=[System.IO.File]::ReadAllBytes($p)
    if (-not ($b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)) {
      [System.IO.File]::WriteAllBytes($p, [byte[]](0xEF,0xBB,0xBF) + $b)
    }
  (改一次踩一次,别嫌烦。)

  必须在**纯 ASCII 路径**的副本里跑(中文路径会让 xtensa 工具链失败)。
  默认用 C:\Users\Public\206dash\Neru-s-206-dashboard;
  改了路径就传 -WorkDir。
#>
[CmdletBinding()]
param(
  [string]$WorkDir = "C:\Users\Public\206dash\Neru-s-206-dashboard",
  [string]$SourceDir,
  [switch]$SkipSync
)

$ErrorActionPreference = "Stop"

# ---- 路径 ----
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $SourceDir) {
  # tools/theme-editor → 仓库根
  $SourceDir = (Resolve-Path (Join-Path $scriptDir "..\..")).Path
}

Write-Host "源目录:   $SourceDir"
Write-Host "构建目录: $WorkDir"

# ---- 同步到纯 ASCII 副本 ----
if (-not $SkipSync) {
  Write-Host "`n[1/5] 同步到构建目录 ..."
  robocopy $SourceDir $WorkDir /E /XD .pio .git .vscode /XF *.code-workspace /NFL /NDL /NJH /NJS /NP | Out-Null
  # robocopy 的退出码是位掩码:0=没变化,1=有复制,3=有复制+有新增,>=8 才是错
  if ($LASTEXITCODE -ge 8) { throw "robocopy 失败 (exit $LASTEXITCODE)" }
  Write-Host "      OK"
} else {
  Write-Host "`n[1/5] 跳过同步(-SkipSync)"
}

$toolDir = Join-Path $WorkDir "tools\theme-editor"
if (-not (Test-Path (Join-Path $toolDir "image-blob-build.js"))) {
  throw "构建目录里找不到 tools\theme-editor\image-blob-build.js:$toolDir"
}

# ---- 1. Node 侧的打包器单测 ----
Write-Host "`n[2/5] 打包器单元测试 (Node) ..."
Push-Location $toolDir
try {
  & node "test-image-blob-build.js"
  if ($LASTEXITCODE -ne 0) { throw "打包器单元测试失败 (exit $LASTEXITCODE)" }
} finally { Pop-Location }
Write-Host "      OK"

# ---- 2. 生成测试镜像 ----
# 图案刻意选得"带位置信息"(颜色随 x/y 变化),这样行列错位/stride 算错
# 都会被逐字节对比抓到,而不是"看着像张图"就蒙过去。
Write-Host "`n[3/5] 生成测试镜像 ..."
$outDir = Join-Path $WorkDir ".pio\image-test"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$binPath = Join-Path $outDir "image.bin"
$manPath = "$binPath.manifest"
$specPath = Join-Path $outDir "spec.json"

$spec = @'
{
  "images": [
    { "pattern": "ramp",   "w": 8,  "h": 4, "role": "background",      "order": 0, "name": "grad" },
    { "pattern": "checker","w": 8,  "h": 8, "role": "face_idle",       "order": 0, "name": "idleL" },
    { "pattern": "solid",  "w": 4,  "h": 4, "role": "face_cruise",     "order": 1, "name": "cruiseL" },
    { "pattern": "checker","w": 16, "h": 2, "role": "face_idle",       "order": 2, "name": "idleL2" },
    { "pattern": "ramp",   "w": 6,  "h": 6, "role": "face_overspeed_r", "order": 0, "name": "overR", "stride_pad": 2 },
    { "pattern": "checker","w": 12, "h": 6, "role": "face_idle_r",     "order": 0, "name": "idleR" },
    { "pattern": "solid",  "w": 8,  "h": 8, "role": "face_sport_r",    "order": 0, "name": "sportR" }
  ]
}
'@
# 用 UTF8 无 BOM 写(Set-Content -Encoding utf8 在 Windows PowerShell 下会加 BOM,
# Node 的 JSON.parse 读不了 BOM 开头的文件)
[System.IO.File]::WriteAllText($specPath, $spec, (New-Object System.Text.UTF8Encoding($false)))

Push-Location $toolDir
try {
  & node "build-image-bin.js" --spec $specPath --out $binPath --manifest $manPath
  if ($LASTEXITCODE -ne 0) { throw "打包失败 (exit $LASTEXITCODE)" }
} finally { Pop-Location }

$binSize = (Get-Item $binPath).Length
Write-Host "      $binPath ($binSize 字节)"

# ---- 3. 跑固件解析器的往返核对 ----
Write-Host "`n[4/5] 固件解析器往返核对 (pio test -e native) ..."

$env:PATH = "C:\Users\Public\206dash\.tools\zigbin;$env:PATH"
$env:PYTHONPATH = "C:\Users\张九思\206Dash\.pio-pylibs"
$env:PLATFORMIO_CORE_DIR = "C:\Users\Public\206dash\.pio-core"
$env:IMAGE_BLOB = $binPath
$env:IMAGE_BLOB_MANIFEST = $manPath

Push-Location $WorkDir
$testLog = Join-Path $outDir "test.log"
try {
  # ★ 不能写成 `... 2>&1 | Tee-Object -FilePath $log`:
  #   PlatformIO 把测试结果写在 stderr 上,而 PS 5.1 在本脚本
  #   $ErrorActionPreference='Stop' 的设定下,会把"原生命令往 stderr 写了一行"
  #   当成**终止性错误**抛出 —— 结果日志文件是空的,人看到的只有
  #   `Program received signal CTRL_BREAK_EVENT`,完全误导。
  #   正确做法:放宽为 Continue,并用文件重定向把两个流一起收下来。
  $prevEap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  & python -m platformio test -e native *> $testLog
  $pioExit = $LASTEXITCODE
  $ErrorActionPreference = $prevEap
} finally { Pop-Location }

if (-not (Test-Path $testLog)) { throw "没有拿到测试输出" }
$log = Get-Content $testLog
if (-not $log) { throw "测试输出是空的(见上面的错误信息)" }

# 关键结果行
$round = $log | Select-String -Pattern 'test_js_blob_roundtrip|test_roundtrip_guard'
$round | ForEach-Object { Write-Host "      $($_.Line.Trim())" }

# ★ 注意:这里要 Where-Object,不能写成 `$round -notmatch ...`。
#   -notmatch 对数组是"只要有任意一个不匹配就为真",两个测试里
#   guard 那条必然不匹配,于是"明明通过了却报失败"。
$passed = $round | Where-Object { $_.Line -match 'test_js_blob_roundtrip\s+\[PASSED\]' }
if (-not $passed) {
  Write-Host "`n往返测试没有通过,失败详情:" -ForegroundColor Red
  # 把 FAILED 那一行**原样**打出来:断言消息里有 C 侧写好的具体原因
  # (第几行、哪个字段对不上、期望值与实际值),比一句概括有用得多。
  $log | Select-String -Pattern 'FAILED|ERROR|Expected|不符|拒收|manifest' |
    ForEach-Object { Write-Host "  $($_.Line.Trim())" }
  Write-Host "`n  完整日志: $testLog"
  throw "固件解析器不接受 JS 生成的镜像(或字段不一致)"
}

$summary = $log | Select-String -Pattern 'test cases:'
Write-Host "      $($summary.Line.Trim())"

if ($pioExit -ne 0) {
  $log | Select-String -Pattern 'FAILED' | ForEach-Object { Write-Host "      $($_.Line.Trim())" }
  throw "native 测试有失败项 (exit $pioExit)"
}

# ---- 4. 收尾 ----
Write-Host "`n[5/5] 结果"
Write-Host "  ✓ 打包器单元测试通过" -ForegroundColor Green
Write-Host "  ✓ 固件解析器逐字节接受 JS 生成的镜像" -ForegroundColor Green
Write-Host ""
Write-Host "  测试镜像: $binPath"
Write-Host "  对账清单: $manPath"
Write-Host ""
Write-Host "  真机刷写(图片分区,0x254000 = 1MB):" -ForegroundColor Cyan
Write-Host "    python -m esptool --chip esp32 --port COM3 --baud 921600 write_flash 0x254000 image.bin"
Write-Host ""
Write-Host "  图形界面(拖图片、导 bin): tools\theme-editor\image-editor.html"
