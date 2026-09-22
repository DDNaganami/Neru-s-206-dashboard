<#
  lt-commit.ps1 —— 笔记本侧提交包装:自动打上"来源机器"标记

  为什么要它(2026-09-21 owner 定的规矩):
    两台机器的 git 身份**完全一样**(都是 DDNaganami <scarm@126.com>),
    所以光看 `git log` 分不出哪条是笔记本改的、哪条是桌机改的。
    桌机那边的 DSH 要做代码审核,必须能一眼(并且**机器可读**)地挑出
    笔记本的改动。

  ★ 为什么不是"改 git config trailer.* 就行了":
    `git config trailer.<token>.key/value` **只对走编辑器的提交
    (`git commit -e`)和显式 `git commit --trailer` 生效**,
    对最常见的 `git commit -m "..."` **完全不生效** —— 实测过,
    设了配置再 `-m` 提交,提交信息里一个 trailer 都没有。
    所以标记必须由本脚本落地,别指望配置。

  用法(在仓库里):
    # 提交已暂存的改动(标记自动加)
    powershell -ExecutionPolicy Bypass -File tools\sync\lt-commit.ps1 -Message "fix(obd): 启用 UART1"

    # 连暂存一起做:所有已跟踪改动
    powershell -ExecutionPolicy Bypass -File tools\sync\lt-commit.ps1 -Message "..." -All

    # 只指定文件
    powershell -ExecutionPolicy Bypass -File tools\sync\lt-commit.ps1 -Message "..." -Path src\main.cpp

    # 干跑:只打印最终提交信息,不提交
    powershell -ExecutionPolicy Bypass -File tools\sync\lt-commit.ps1 -Message "..." -WhatIf

  产生的提交信息形如:

    fix(obd): 启用 UART1

    <正文(可选, -Body)>

    Machine: MikamoNeru
    Role: laptop-agent

  桌机侧怎么挑出来(两种都行):
    git log --grep='^Machine: MikamoNeru'          # 只列机器标记
    git log --format='%h %s%n%b' | Select-String '^Machine:'
    git log --invert-grep --grep='^Machine: '      # 反选:只看桌机自己的提交

  退出码: 0 成功 / 2 参数错 / 3 不在 git 仓库 / 4 没东西可提交 / 5 git 失败
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Message,
  [string]$Body = '',
  [string[]]$Path,
  [switch]$All,
  [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

function Die($code, $msg) { Write-Host "错误: $msg" -ForegroundColor Red; exit $code }

# ---- 定位仓库根(脚本放在 tools\sync\ 下,仓库根是上两级)----
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not (Test-Path (Join-Path $repoRoot '.git'))) { Die 3 "$repoRoot 不是 git 仓库根" }

# ---- 机器名:钉死成规范大小写 MikamoNeru ----
# ★ 不用 $env:COMPUTERNAME —— 它给的是全大写 MIKAMONERU。
#   标记值大小写不一致会让桌机的 `--grep='^Machine: MikamoNeru'` 漏掉,
#   所以这里写死成主机名的规范写法。
$machine = 'MikamoNeru'
if ($machine -match '[^\x00-\x7F]') { Die 2 "机器名含非 ASCII,会破坏机器可读性: $machine" }

# ---- 1. 需要时先暂存 ----
# ★ git 会把 "LF will be replaced by CRLF" 这类**警告**写到 stderr,而本脚本
#   $ErrorActionPreference='Stop' —— 直接 `2>&1` 管道会让警告变成终止错误。
#   所以用 Run-GitQuiet 吞掉 stderr,只看退出码。
function Run-GitQuiet([string[]]$GitArgs) {
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & git -C $repoRoot @GitArgs 2>$null | Out-Null
    return $LASTEXITCODE
  } finally { $ErrorActionPreference = $prev }
}

if ($All) {
  [void](Run-GitQuiet @('add', '-A'))
} elseif ($Path) {
  # ★ 同时支持 `-Path a,b`(PowerShell 数组)和 `-Path "a,b"`(一整串)两种写法:
  #   后者是常见误用 —— 会被当成一个不存在的文件名而报错,容易白折腾。
  $wantPaths = @()
  foreach ($item in $Path) {
    $wantPaths += ($item -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
  }
  foreach ($p in $wantPaths) {
    $full = Join-Path $repoRoot $p
    if (-not (Test-Path $full)) { Die 2 "路径不存在: $p" }
    $rc = Run-GitQuiet @('add', '--', $p)
    if ($rc -ne 0) { Die 5 "git add 失败(退出码 $rc): $p" }
  }
}

# ---- 2. 确认真的有东西可提交 ----
$staged = & git -C $repoRoot diff --cached --name-only 2>&1
if (-not $staged) {
  Die 4 "没有已暂存的改动。用 -All 或 -Path <文件> 指定,或先自己 git add。"
}

# ---- 3. 组装提交信息 ----
$full = $Message.TrimEnd()
if ($Body) { $full += "`n`n" + $Body.TrimEnd() }

# trailer 用 --trailer 显式加(不依赖 trailer.* 配置,那条对 -m 不生效)
$trailers = @(
  @('Machine', $machine),
  @('Role', 'laptop-agent')
)

# ---- 4. 干跑:用 interpret-trailers 预演最终信息 ----
$tmp = [IO.Path]::GetTempFileName()
try {
  [IO.File]::WriteAllText($tmp, $full + "`n", (New-Object Text.UTF8Encoding($false)))
  # ★ 变量名不能叫 $args —— 那是 PowerShell 的保留自动变量,赋值会失败/行为未定义
  # ★ --trailer 后面的值**必须自己带引号**:PS 5.1 转发原生命令参数时,会把不带引号的
  #   "Role: laptop-agent" 按空格拆成两个参数 → trailer 被悄悄丢掉(实测只加进了 Machine)。
  $itArgs = @('interpret-trailers')
  foreach ($t in $trailers) { $itArgs += @('--trailer', "`"$($t[0]): $($t[1])`"") }
  $itArgs += '--if-exists'
  $itArgs += 'addIfDifferent'
  $itArgs += $tmp
  $final = & git -C $repoRoot @itArgs 2>&1 | Out-String

  if ($WhatIf) {
    Write-Host '===== 将提交以下内容(干跑,未提交)=====' -ForegroundColor Cyan
    Write-Host ("暂存文件($((@($staged)).Count) 个):")
    @($staged) | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host '提交信息:'
    Write-Host $final
    exit 0
  }

  # ---- 5. 真提交:把组装好的信息写文件,-F 提交(避免命令行/编码问题) ----
  $msgFile = [IO.Path]::GetTempFileName()
  [IO.File]::WriteAllText($msgFile, $final, (New-Object Text.UTF8Encoding($false)))
  $out = & git -C $repoRoot commit -F $msgFile 2>&1 | Out-String
  $code = $LASTEXITCODE
  Remove-Item $msgFile -Force -ErrorAction SilentlyContinue
  Write-Host $out
  if ($code -ne 0) { Die 5 "git commit 失败(退出码 $code)" }

  # ---- 6. 回读验证:确认 trailer 真的进去了 ----
  $verify = & git -C $repoRoot log -1 --format='%H%n%B' 2>&1 | Out-String
  if ($verify -match "(?m)^Machine:\s*$([regex]::Escape($machine))\s*$") {
    Write-Host "✅ 已提交并带来源标记 Machine: $machine" -ForegroundColor Green
  } else {
    Write-Host "⚠ 提交成功,但没读到 Machine 标记 —— 请人工核一下:" -ForegroundColor Yellow
    Write-Host $verify
    exit 5
  }
} finally {
  Remove-Item $tmp -Force -ErrorAction SilentlyContinue
}
