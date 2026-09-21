<#
  206dash 桌机 → 笔记本 同步(SSH 路线)
  ============================================================================
  为什么存在:原来那条 SMB / robocopy 路线废了 —— 这台桌机连笔记本的共享
  一律 "System error 5 / 拒绝访问",新建 Windows 凭据也救不回来,根因是本机
  的登录会话/凭据库问题,不值得再查下去。SSH 这条路是**验证过的**,所以整套
  改走 SSH。

  设计上只有一条硬约束:**远端命令里的引号活不过 Windows OpenSSH**
  (ssh.exe 把命令行重新拼一遍再交给远端,内层双引号会被吃掉)。所以这里
  所有远端命令都先编成 base64,用 powershell -EncodedCommand 发过去 ——
  传输层全是 ASCII,中文路径和引号都不会被改写。

  数据文件只发**大小不一样**的那些(见 Get-SyncDecision),这是土办法的
  rsync:Windows 上没有 rsync,而整包重传 30~60 MB 不值得。

  ★ 2026-09-21 owner 的决定:除了那 5 个散件,**DSH 的会话记录(对话记录)也跟着
    每次同步走**。桌机上取 %USERPROFILE%\.dsh\sessions 下**最新写入**的那个
    session.v3.jsonl.zstd,发两份到 -LaptopData:
      session-<会话目录名>.jsonl.zstd  原样一份(以后能被 DSH 打开)
      对话记录.jsonl                   桌机 Python 3.14 解压出来的可读版(给人看/搜)
    ★ 本脚本只往 -LaptopData 放,**不**去写笔记本自己的 .dsh 会话树(那是 DSH 的地盘)。
      要"在笔记本的 DSH 里直接看到这次对话"是**手工一步** —— 只复制、不移动、
      不覆盖更新的(规格见 tools/sync/README.md「笔记本上的 DSH」/ docs/laptop-setup.md §8)。
#>
[CmdletBinding()]
param(
  # SSH 私钥(桌机 → 笔记本专用)。默认值里的 $env:USERPROFILE 是**运行时**展开的
  [string]$SshKey     = "$env:USERPROFILE\.ssh\dsh_laptop",
  [string]$Laptop     = '张九思@26.253.1.139',
  # ★ 2026-09-21:笔记本的 checkout 已经**搬到和桌机完全同一个绝对路径**了
  #   (原来是 C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard)。
  #   为什么对齐路径:文档/脚本里的命令两边通用,而且 DSH 的会话目录名是按工作区
  #   路径编码的 —— 两边路径一致,以后在笔记本上恢复会话才不会错位。
  [string]$LaptopRepo = 'C:\Users\张九思\206Dash\Neru-s-206-dashboard',
  [string]$LaptopData = 'C:\206dash-data',
  [int]$SshTimeout    = 10,

  # DSH 会话记录(对话记录)的老家:每个会话一个子目录,子目录里一个
  # session.v3.jsonl.zstd。递归找、取最后写入的那个(见 Resolve-SessionTranscript)。
  [string]$SessionRoot = "$env:USERPROFILE\.dsh\sessions",

  # 要搬的散件。为什么是这几个:它们都是 git 装不下的东西(抓包 CSV / 字库 bin /
  # 用户主题 json / 打包好的传输 zip),而源文件在桌机上是只读的。
  [string[]]$Asset = @(
    'C:\Users\Public\206dash\van_capture_dm.csv',
    'C:\Users\Public\206dash\drive5min.csv',
    'C:\Users\Public\206dash\image-v3.bin',
    'C:\Users\Public\206dash\theme-user.json'
  ),
  # 这个有就带、没有就跳过(不是每次都在)
  [string]$ZipAsset = 'C:\Users\Public\206dash\206dash-transfer.zip',

  [switch]$Test,
  [switch]$Register,
  [switch]$Unregister,
  [switch]$SelfTest,
  [switch]$WhatIf
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# 退出码。0 成功 / 2 参数 / 3 通道 / 4 仓库 / 5 自检 / 6 数据没传完
# 分这么细是为了让计划任务的"上次结果"一眼看出断在哪一步。
# ---------------------------------------------------------------------------
$EX_OK      = 0
$EX_ARGS    = 2
$EX_CHANNEL = 3
$EX_REPO    = 4
$EX_SELFTEST= 5
$EX_DATA    = 6

$TaskName       = '206dash-sync-ssh'
$EveryMinutes   = 30

# 会话记录(对话记录)超过这个大小就多打一行警告 —— 但**仍然照发**(见主流程 3/5 那段):
# 正常一份就在 1 MB 上下(2026-09-21 实测 0.84 MB),20 MB 说明这个会话大得离谱
# (或者哪里在刷日志),值得人看一眼;可它是真数据,不是错误,所以只提醒、不跳过、不拦同步。
# ★ 这条线量的是**原始 .zstd**;解压出来的可读版还要再大 3~4 倍(实测 0.84 MB → 3.07 MB)。
$TranscriptWarnBytes = 20MB

# ---------------------------------------------------------------------------
# ★ 两边都按 UTF-8 说话(2026-09-21,加"对话记录.jsonl"时踩出来的坑)
# ---------------------------------------------------------------------------
# 远端命令的输出是**字节流**,解成什么由两头的"控制台编码"决定,和 base64 那层无关:
#   笔记本:没设的话按 OEM 代码页(zh-CN = 936/GBK)吐字节;
#   桌机  :PowerShell 5.1 按 [Console]::OutputEncoding 解这些字节。
# 两边不一致时,**中文文件名会变成乱码** —— 实测:笔记本列目录回
# "对话记录.jsonl|1220758",桌机解出来是 "锟斤拷.jsonl|1220758" 那种乱码,
# 于是 $remoteMap.ContainsKey('对话记录.jsonl') 永远是假:
#   ① 每次都判"笔记本上还没有"→ 重发(浪费);
#   ② 发完复核又认不出这个文件名 → 记成"传完大小不对" → **假的失败、退出 6**。
# 所以:本地这一句 + 每个远端脚本正文开头那一句,两边一起钉死 UTF-8。
# ASCII 在所有编码里都一样,所以那几个散件的老行为一点都不受影响(实测过)。
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Write-Head([string]$t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Write-Ok([string]$t)   { Write-Host "  [OK]   $t" -ForegroundColor Green }
function Write-Bad([string]$t)  { Write-Host "  [FAIL] $t" -ForegroundColor Red }
function Write-Warn2([string]$t){ Write-Host "  [WARN] $t" -ForegroundColor Yellow }
function Write-Info([string]$t) { Write-Host "  $t" }

# 所有输出都走 Write-Host,不往管道里吐对象 —— 这样 ssh/scp 的 stdout 永远不会
# 混进本脚本的返回值里(计划任务和 -SelfTest 都靠这个)。

# ===========================================================================
# 远端命令的构造:全部编成 base64
# ===========================================================================
# 为什么必须这样:实测过,`ssh host 'if (x) { Write-Output "$($_.Name)" }'`
# 到了笔记本上双引号已经没了,变成 $($_.Name)|$($_.Length),PowerShell 报
# "表达式只能作为管道的第一个元素"。OpenSSH for Windows 会把命令行重新解析
# 再拼接,内层引号不保证活下来。base64 之后传输层只剩 [A-Za-z0-9+/=],稳。

function ConvertTo-EncodedCommand {
  <#  本地把一个 PowerShell 脚本正文编成 -EncodedCommand 用的字符串。
      UTF-16LE(就是 .NET 的 Unicode)+ base64 —— 这是 PowerShell 规定的格式。

      ★ 中文路径为什么这样就能过:base64 本身只含 [A-Za-z0-9+/=],
        出了 ssh 这层才在**笔记本上**被解回 UTF-16LE。中文一个字节都不会被改写
        (实测:C:\Users\张九思\... 原样到达)。所以"路径必须全是 ASCII"是**不需要**
        的约束 —— 这条曾经把本机的同步整个卡死,因为这台机器的 8.3 短名是关的,
        拿不到 ZHANGJ~1 就只能原样传中文路径。

      -RejectNonAscii 只在**故意**想验证"拦截逻辑还在"的自检里用;
      正常构造远端命令一律允许 Unicode。 #>
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$Script,
    [switch]$RejectNonAscii
  )

  if ($RejectNonAscii) {
    $nonAscii = @($Script.ToCharArray() | Where-Object { [int]$_ -gt 126 })
    if ($nonAscii.Count -gt 0) {
      throw ("远端命令里有非 ASCII 字符(U+{0:X4})。-RejectNonAscii 生效中。" -f [int]$nonAscii[0])
    }
  }
  $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script))
  return 'powershell -NoProfile -NonInteractive -EncodedCommand ' + $b64
}

function New-RemotePsCommand {
  <#  把一段 PowerShell 正文包成一条完整的远端命令行。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Script)
  return (ConvertTo-EncodedCommand -Script $Script)
}

function Get-RemoteListScript {
  <#  远端"列目录 + 报大小"的脚本。输出 <文件名>|<字节数>,每行一个。
      没这个目录就输出一行 MISSING,让本地能区分"目录不在"和"目录是空的"。

      ★ 这里必须是**单引号** here-string(@"..."@ 不行):
        双引号版本会在**本地**就把 $_.Name 展开掉 —— 实测被展开成
        "powershell.exe|292864"(当次 powershell 进程的名字和长度),
        远端永远拿不到插值,只会打印这一行垃圾。自检里有断言守这条。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Dir)
  $d = $Dir.Replace("'", "''")
  return @'
$ProgressPreference = 'SilentlyContinue'
# 输出按 UTF-8 吐字节 —— 桌机那边也钉死 UTF-8,中文文件名(对话记录.jsonl)
# 才不会在"列目录"里变成乱码(为什么关系到大小比较,见文件头那段)。ASCII 不受影响。
[Console]::OutputEncoding = [Text.Encoding]::UTF8
if (Test-Path -LiteralPath '__DIR__') {
  Get-ChildItem -LiteralPath '__DIR__' -File -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Output ($_.Name + '|' + $_.Length) }
} else { Write-Output 'MISSING' }
'@.Replace('__DIR__', $d)
}

function Get-RemoteRepoProbeScript {
  <#  远端仓库体检:先看有没有本地改动(gate),再报 HEAD。
      gate 和取 HEAD 放在同一次往返里,省一次 SSH。同样是单引号 here-string。

      ★ gate 只看**已跟踪**文件的改动(--untracked-files=no)。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Repo)
  $r = $Repo.Replace("'", "''")
  return @'
$ProgressPreference = 'SilentlyContinue'
# 输出 UTF-8(桌机那边同样钉死);中文路径出现在 DIRTY: / UNTRACKED 行里也不会乱码
[Console]::OutputEncoding = [Text.Encoding]::UTF8
Set-Location -LiteralPath '__REPO__'

# ---- gate 判据(2026-09-21 改)----------------------------------------------
# 为什么**不看未跟踪文件**(--untracked-files=no):
#   1) 未跟踪文件根本挡不住快进合并。就算某个未跟踪文件的名字和这次要写进来的
#      已跟踪文件撞上,git 自己会中止合并并原样留着那个文件 —— 它**不会**覆盖
#      任何东西。也就是说"未跟踪文件"这条风险 git 已经替我们挡了,gate 再拦一遍
#      只是把正常同步变成永远失败。
#   2) 这不是理论:2026-09-21 那次真同步,笔记本 checkout 里躺着 26 个早期
#      sshd / Radmin 调试残留(A.sshd.log、diagnose-sshd*.ps1、fix-route-metric.ps1 …),
#      全是未跟踪文件。旧 gate 用含未跟踪的 `git status --porcelain`,于是每次都
#      拒绝快进、退出 6 —— 明明整条同步都成功了。所以判据收窄。
# 已跟踪改动 / 暂存改动 / 删除文件**照旧一律拒绝**(gate 的本意没变):
# 那些是别人的活儿,我们不 stash / 不 reset / 不 clean。
$dirty = @(git status --porcelain --untracked-files=no)
Write-Output ('PORCELAIN_COUNT=' + $dirty.Count)
$dirty | ForEach-Object { Write-Output ('DIRTY:' + $_) }

# 未跟踪的只**报个数**给人看(gate 不看它们,但别让人以为它们不存在)。
# 用 StartsWith 而不用 -like '??*':后者的 ? 是通配符,会连 ' M file' 一起算进来。
$untracked = @(git status --porcelain --untracked-files=normal | Where-Object { $_.StartsWith('??') })
Write-Output ('UNTRACKED_COUNT=' + $untracked.Count)

Write-Output ('HEAD=' + (git rev-parse HEAD).Trim())
Write-Output ('BRANCH=' + (git rev-parse --abbrev-ref HEAD).Trim())
'@.Replace('__REPO__', $r)
}

function Get-RemoteRepoMergeScript {
  <#  fetch + merge --ff-only。只快进,不产生 merge commit,也绝不碰本地改动。

      ★ fetch 的退出码必须**单独**报回来(FETCH_EXIT)。为什么:fetch 一失败,
        `merge --ff-only origin/main` 就是拿**过期的** origin/main 去比,典型输出
        是 "Already up to date." + 退出码 0 —— 只看 MERGE_EXIT 会把它当成成功。
        2026-09-21 真跑就踩到了:笔记本连不上 github.com:443,脚本却打印
        "[OK] 笔记本 fetch + ff-only 成功",把真问题藏了起来。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Repo)
  $r = $Repo.Replace("'", "''")
  return @'
$ProgressPreference = 'SilentlyContinue'
# 输出 UTF-8(桌机那边同样钉死),git 的中文路径/提示不会乱码
[Console]::OutputEncoding = [Text.Encoding]::UTF8
Set-Location -LiteralPath '__REPO__'
git fetch --prune origin 2>&1 | ForEach-Object { Write-Output ('FETCH:' + $_) }
Write-Output ('FETCH_EXIT=' + $LASTEXITCODE)
git merge --ff-only origin/main 2>&1 | ForEach-Object { Write-Output ('MERGE:' + $_) }
Write-Output ('MERGE_EXIT=' + $LASTEXITCODE)
Write-Output ('HEAD=' + (git rev-parse HEAD).Trim())
'@.Replace('__REPO__', $r)
}

# ===========================================================================
# 大小比较(土 rsync 的决策核心)
# ===========================================================================
function ConvertFrom-RemoteListing {
  <#  把远端那段 "名字|大小" 的文本解析成哈希表。MISSING 行 = 空表。
      解析不了的行直接跳过(远端 PowerShell 偶尔会吐警告)。 #>
  [CmdletBinding()]
  param([AllowNull()][string[]]$Lines)
  $map = @{}
  foreach ($ln in @($Lines)) {
    if ([string]::IsNullOrWhiteSpace($ln)) { continue }
    if ($ln.Trim() -eq 'MISSING') { continue }
    $i = $ln.LastIndexOf('|')
    if ($i -le 0) { continue }
    $name = $ln.Substring(0, $i).Trim()
    $sizeText = $ln.Substring($i + 1).Trim()
    $size = 0L
    if (-not [long]::TryParse($sizeText, [ref]$size)) { continue }
    $map[$name] = $size
  }
  return $map
}

function Get-SyncDecision {
  <#  单个文件的判决。这是整个脚本唯一"有脑子"的地方,所以 -SelfTest 单独测它。
      - 本地没有          → Missing(文件不在,跳过;比如 206dash-transfer.zip)
      - 远端没有          → Send
      - 两边大小不同      → Send
      - 两边大小相同      → Skip(这就是土 rsync:比大小,不比时间戳也不比哈希)
      显式传 -RemoteSize $null 表示"远端没这个文件"。 #>
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$Name,
    # -LocalMissing 时本地根本读不到大小,所以这个参数不设成强制的
    [long]$LocalSize = 0,
    # 故意用 [object]:-RemoteSize $null 要能表示"远端没这个文件"。
    # 写成 [Nullable[long]] 反而会在绑定时把 $null 判成转换失败。
    [AllowNull()][object]$RemoteSize,
    [switch]$LocalMissing
  )
  if ($LocalMissing) {
    return [pscustomobject]@{ Action = 'Missing'; Reason = '桌机上没有这个文件'; Delta = 0L }
  }
  if ($null -eq $RemoteSize) {
    return [pscustomobject]@{ Action = 'Send'; Reason = '笔记本上还没有'; Delta = $LocalSize }
  }
  if ([long]$RemoteSize -ne $LocalSize) {
    return [pscustomobject]@{
      Action = 'Send'
      Reason = ('大小不同 桌机 {0} / 笔记本 {1}' -f $LocalSize, [long]$RemoteSize)
      Delta  = $LocalSize - [long]$RemoteSize
    }
  }
  return [pscustomobject]@{ Action = 'Skip'; Reason = '大小一致'; Delta = 0L }
}

function Get-RepoVerdict {
  <#  仓库这步的判决。**纯函数** —— 只吃四个事实,不打印、不碰网络,
      所以 -SelfTest 能把每条分支都点一遍(这台的教训见下面)。

      返回值:
        'refused' gate 拒绝,根本没过 fetch/merge(gate 优先:没碰就是没碰)
        'nofetch' 笔记本自己 git fetch origin 失败 —— origin/main 还是旧的,
                  这时 `merge --ff-only` 常常回一句 "Already up to date." + 退出码 0,
                  **不能**当成功(2026-09-21 真跑:笔记本连不上 github.com:443,
                  旧代码却打了 "[OK] fetch + ff-only 成功")。
        'failed'  merge 报错,或者两边 HEAD 对不上 —— 包括"哈希恰好相同但
                  merge 报错"这种,哈希相同不等于这次真的同步过。
        'ok'      fetch 成功 + merge --ff-only 成功 + 两边 HEAD 一致。 #>
  [CmdletBinding()]
  param(
    [AllowEmptyString()][string]$DesktopHead = '',
    [AllowEmptyString()][string]$LaptopHead  = '',
    [int]$FetchExit = 0,
    [int]$MergeExit = 0,
    [int]$DirtyCount = 0
  )
  if ($DirtyCount -gt 0) { return 'refused' }
  if ($FetchExit -ne 0)  { return 'nofetch' }
  if ($MergeExit -ne 0)  { return 'failed' }
  if ($DesktopHead -ne $LaptopHead) { return 'failed' }
  return 'ok'
}

# ===========================================================================
# DSH 会话记录(对话记录)
# ===========================================================================
# 2026-09-21 owner 的决定:每次同步都把桌机上**最新那个会话**带到笔记本上。
# 这三个函数都是纯本地/纯函数(自检里用临时目录测,不碰网络、也不碰真的 .dsh)。
function Resolve-SessionTranscript {
  <#  找"当前最新的会话记录":$Root 下面**递归**找所有 session.v3.jsonl.zstd,
      按**最后写入时间**取最新的那个 —— DSH 是边跑边往这个文件里追加的,
      所以"最后写入"就等于"现在正在用的那个会话"。

      ★ 找不到就返回 $null,**不是**错误:调用方打一行就跳过这一步,绝不因此让
        整条同步失败(这台机器上没跑过 DSH,也不该影响抓包 CSV / 字库那些文件)。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Root)

  if (-not (Test-Path -LiteralPath $Root)) { return $null }
  $f = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter 'session.v3.jsonl.zstd' -ErrorAction SilentlyContinue |
         Sort-Object -Property LastWriteTime -Descending)
  if ($f.Count -eq 0) { return $null }
  $newest = $f[0]
  return [pscustomobject]@{
    Path        = $newest.FullName
    SessionName = (Split-Path -Path $newest.DirectoryName -Leaf)
    Size        = [long]$newest.Length
    Written     = $newest.LastWriteTime
  }
}

function Get-TranscriptSendNames {
  <#  会话记录到了笔记本上叫什么名字(两个都落在 -LaptopData 里):
        Raw       session-<会话目录名>.jsonl.zstd   原样一份,以后 DSH 能直接打开
        Readable  对话记录.jsonl                    解压出来的可读版,给人看/搜

      ★ 会话目录名先洗一遍:DSH 的会话目录名本来就是 GUID / session-GUID(纯 ASCII),
        这里只是**兜底** —— 万一哪天是别的名字,把非 [A-Za-z0-9._-] 的字符换成 _,
        免得中文/怪字符出现在 scp 的目标名里(scp 那边只有目录是 ASCII 才最稳)。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$SessionName)
  $safe = ($SessionName -replace '[^A-Za-z0-9._-]', '_')
  return [pscustomobject]@{
    Raw      = ('session-{0}.jsonl.zstd' -f $safe)
    Readable = '对话记录.jsonl'
  }
}

function Get-TranscriptSizeNote {
  <#  会话记录大小体检:超过 $TranscriptWarnBytes(20 MB)就返回一行警告文本,
      否则返回 $null。**只提醒,不跳过** —— 判据和理由见 $TranscriptWarnBytes。 #>
  [CmdletBinding()]
  param([long]$Size)
  if ($Size -le $TranscriptWarnBytes) { return $null }
  return ('会话记录 {0} MB —— 超过 {1} MB 这条警戒线(正常一份就在 1 MB 上下),仍然照发,只是提醒你瞄一眼' -f `
            [math]::Round($Size / 1MB, 2), [math]::Round($TranscriptWarnBytes / 1MB, 0))
}

# ===========================================================================
# SSH / SCP 薄封装
# ===========================================================================
function Get-SshArgs {
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$RemoteCommand)
  # BatchMode=yes:任何情况下都不弹密码提示(计划任务里弹提示 = 永远卡住)
  return @(
    '-o', 'BatchMode=yes',
    '-o', ("ConnectTimeout={0}" -f $SshTimeout),
    '-i', $SshKey,
    $Laptop,
    (New-RemotePsCommand -Script $RemoteCommand)
  )
}

function Invoke-Native {
  <#  叫一个外部程序并连 stderr 一起收回来。

      ★ 为什么非要包这一层:$ErrorActionPreference='Stop' 之下,
        PowerShell 5.1 会把**外部程序写到 stderr 的普通输出**当成终止性错误。
        实测 `git push` 明明返回 0、只说了句 "Everything up-to-date"(走 stderr),
        整个脚本就死在那里退出 1 了。scp 的进度条同理。
        所以这里临时把 EAP 放回 Continue,只认退出码。 #>
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string[]]$Arguments
  )
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $raw = & $Exe @Arguments 2>&1
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $prev
  }
  return [pscustomobject]@{
    ExitCode = $code
    Lines    = @($raw | ForEach-Object { $_.ToString() })
  }
}

function Invoke-RemoteScript {
  <#  跑一段远端 PowerShell 正文,返回 stdout 行。失败抛异常。
      ssh 参数统一由 Get-SshArgs 造 —— 自检断言的就是那一份,不会和实际用的走偏。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Script)
  $r = Invoke-Native -Exe 'ssh' -Arguments (Get-SshArgs -RemoteCommand $Script)
  if ($r.ExitCode -ne 0) {
    throw ("远端命令失败(exit {0}):{1}" -f $r.ExitCode, ($r.Lines -join ' / '))
  }
  return $r.Lines
}

function Test-SshChannel {
  <#  预检:能不能连上、远端 PowerShell 活不活。探针本身也走 base64 ——
      这样探通了就说明后面所有远端命令都能发。 #>
  [CmdletBinding()]
  param()
  try {
    $lines = Invoke-RemoteScript -Script 'Write-Output ("echo ok|" + $env:COMPUTERNAME)'
    $joined = ($lines -join "`n")
    if ($joined -match 'echo ok\|') {
      $host_ = ($joined -split '\|')[-1].Trim()
      return [pscustomobject]@{ Ok = $true; HostName = $host_; Detail = $joined.Trim() }
    }
    return [pscustomobject]@{ Ok = $false; HostName = ''; Detail = ('探针输出看不懂:' + $joined) }
  } catch {
    return [pscustomobject]@{ Ok = $false; HostName = ''; Detail = $_.Exception.Message }
  }
}

# ===========================================================================
# 计划任务
# ===========================================================================
function Register-SyncTask {
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$ScriptPath)
  $arg = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}"' -f $ScriptPath
  $action    = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg
  $principal = New-ScheduledTaskPrincipal -UserId ($env:USERDOMAIN + '\' + $env:USERNAME) `
                 -LogonType Interactive -RunLevel Limited
  $t1 = New-ScheduledTaskTrigger -AtLogOn
  $t2 = New-ScheduledTaskTrigger -Once -At (Get-Date).Date.AddMinutes(2) `
          -RepetitionInterval (New-TimeSpan -Minutes $EveryMinutes)
  $t2.RepetitionDuration = (New-TimeSpan -Days 3650)
  $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                -StartWhenAvailable -MultipleInstances IgnoreNew
  try {
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger @($t1, $t2) `
      -Principal $principal -Settings $settings -Force `
      -Description '206dash 桌机→笔记本 SSH 同步(登录时 + 每 30 分钟)' | Out-Null
    Write-Ok "计划任务已注册:$TaskName(登录时 + 每 $EveryMinutes 分钟)"
    Write-Info "它跑的是:$arg"
    return $EX_OK
  } catch {
    Write-Bad "注册计划任务失败:$($_.Exception.Message)"
    Write-Info '提示:这条要写系统任务库。开一个**管理员** PowerShell 再跑一次。'
    return 1
  }
}

function Unregister-SyncTask {
  [CmdletBinding()]
  param()
  $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
  if (-not $t) { Write-Warn2 "计划任务不存在(已经是干净的):$TaskName"; return $EX_OK }
  try {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Ok "计划任务已删除:$TaskName"
    return $EX_OK
  } catch {
    Write-Bad "删计划任务失败:$($_.Exception.Message)"
    Write-Info '这条需要管理员权限(任务文件在 C:\Windows\System32\Tasks)。'
    return 1
  }
}

# ===========================================================================
# -SelfTest:不碰网络
# ===========================================================================
# 只测"没有网也能判对错"的东西。远端命令的引号问题**必须**在这里断言,因为
# 它是这套脚本唯一踩过的坑:断言 base64 解回来和原文逐字节相等 —— 等价于
# "远端拿到的字符串就是我们写的那个",不需要真的连笔记本。
function Invoke-SelfTest {
  [CmdletBinding()]
  param()
  # 计数器用哈希表:嵌套函数里改 $script:xxx 在 Set-StrictMode 下会取不到值,
  # 而哈希表是按引用走的,内外都是同一个对象。
  $tally = @{ Pass = 0; Fail = 0 }
  function Assert([string]$name, [bool]$cond, [string]$detail = '') {
    if ($cond) { $tally.Pass++; Write-Host ("  [PASS] " + $name) -ForegroundColor Green }
    else       { $tally.Fail++; Write-Host ("  [FAIL] " + $name + $(if ($detail) { " —— $detail" } else { '' })) -ForegroundColor Red }
  }

  Write-Head 'SelfTest 1/6:参数解析'
  Assert '默认 SshKey 指向 ~\.ssh\dsh_laptop' ($SshKey -eq (Join-Path $env:USERPROFILE '.ssh\dsh_laptop')) "实际:$SshKey"
  Assert '默认 Laptop 正确'      ($Laptop -eq '张九思@26.253.1.139')              "实际:$Laptop"
  # ★ 2026-09-21:笔记本 checkout 已搬到和桌机**同一个绝对路径**(理由见参数上的注释)
  Assert '默认 LaptopRepo 正确(两边同路径)' ($LaptopRepo -eq 'C:\Users\张九思\206Dash\Neru-s-206-dashboard') "实际:$LaptopRepo"
  Assert '  LaptopRepo 不再是旧的 PlatformIO 路径' (-not $LaptopRepo.Contains('PlatformIO'))
  Assert '默认 LaptopData 正确'  ($LaptopData -eq 'C:\206dash-data')              "实际:$LaptopData"
  Assert '默认 SshTimeout=10'    ($SshTimeout -eq 10)                             "实际:$SshTimeout"
  Assert '默认 SessionRoot 指向 ~\.dsh\sessions' ($SessionRoot -eq (Join-Path $env:USERPROFILE '.dsh\sessions')) "实际:$SessionRoot"
  Assert '会话记录警戒线 = 20 MB' ($TranscriptWarnBytes -eq 20MB)                 "实际:$TranscriptWarnBytes"
  Assert '默认搬 4 个散件'       ($Asset.Count -eq 4)                             "实际:$($Asset.Count)"
  Assert '4 个散件名字都对'      ((@($Asset | ForEach-Object { Split-Path $_ -Leaf }) -join ',') -eq 'van_capture_dm.csv,drive5min.csv,image-v3.bin,theme-user.json')
  Assert 'zip 是单独一个可选件'  ($ZipAsset -like '*206dash-transfer.zip')
  Assert '目标不是已废的 C:\206dash-sync' ($LaptopData -ne 'C:\206dash-sync')
  Assert '任务名是 206dash-sync-ssh' ($TaskName -eq '206dash-sync-ssh')

  Write-Head 'SelfTest 2/6:大小比较逻辑'
  $d = Get-SyncDecision -Name 'a.csv' -LocalSize 100 -RemoteSize $null
  Assert '远端没有 → Send'            ($d.Action -eq 'Send')
  $d = Get-SyncDecision -Name 'a.csv' -LocalSize 100 -RemoteSize 100L
  Assert '大小相同 → Skip'            ($d.Action -eq 'Skip')
  $d = Get-SyncDecision -Name 'a.csv' -LocalSize 100 -RemoteSize 99L
  Assert '远端小 1 字节 → Send'       ($d.Action -eq 'Send')
  $d = Get-SyncDecision -Name 'a.csv' -LocalSize 100 -RemoteSize 101L
  Assert '远端大 1 字节 → Send'       ($d.Action -eq 'Send')
  $d = Get-SyncDecision -Name 'a.csv' -LocalSize 0 -RemoteSize 0L
  Assert '两边都是 0 → Skip'          ($d.Action -eq 'Skip')
  $d = Get-SyncDecision -Name 'a.csv' -LocalSize 3091431 -RemoteSize 34303713L
  Assert '真实场景(3.0MB vs 34.3MB)→ Send' ($d.Action -eq 'Send')
  Assert '  delta 算对'               ($d.Delta -eq (3091431L - 34303713L)) "实际:$($d.Delta)"
  $d = Get-SyncDecision -Name 'z.zip' -LocalMissing
  Assert '本地没有 → Missing(不是 Send)' ($d.Action -eq 'Missing')

  $listing = @('van_capture_dm.csv|3091431', 'drive5min.csv|34303713', 'MISSING', '', 'garbage-line')
  $map = ConvertFrom-RemoteListing -Lines $listing
  Assert '列目录解析出 2 个文件'      ($map.Count -eq 2)                  "实际:$($map.Count)"
  Assert '  大小解析正确'             ($map['drive5min.csv'] -eq 34303713L)
  Assert '  MISSING/垃圾行被跳过'     ($map.Count -eq 2 -and -not $map.ContainsKey(''))
  $empty = ConvertFrom-RemoteListing -Lines @('MISSING')
  Assert '目录不存在 → 空表(全部要发)' ($empty.Count -eq 0)
  # 中文文件名必须原样当键 —— 对话记录.jsonl 就是靠这个被认出来的
  # (★ 光这一条不够:两头编码不一致时远端回来的就是乱码,见 3/6 的 UTF-8 三条)
  $cnMap = ConvertFrom-RemoteListing -Lines @('对话记录.jsonl|1220758', 'session-5124ca8a-ec3f-4fdc-b00d-f349800a99f0.jsonl.zstd|329494')
  Assert '中文文件名能当列目录的键'   ($cnMap.ContainsKey('对话记录.jsonl')) "实际:$((@($cnMap.Keys)) -join ',')"
  Assert '  它的字节数也解析对了'     ($cnMap['对话记录.jsonl'] -eq 1220758L)

  Write-Head 'SelfTest 3/6:远端命令的引号/编码'
  # 这是核心断言:解回来的字符串必须和原文一字不差。
  $samples = @(
    (Get-RemoteListScript -Dir 'C:\206dash-data'),
    (Get-RemoteListScript -Dir 'C:\Users\Public\206dash'),
    (Get-RemoteRepoProbeScript -Repo 'C:\repo'),
    (Get-RemoteRepoMergeScript -Repo 'C:\repo'),
    'Write-Output ("echo ok|" + $env:COMPUTERNAME)'
  )
  $names = @('列目录','列目录(Public)','仓库体检','仓库快进','通道探针')
  for ($i = 0; $i -lt $samples.Count; $i++) {
    $full = New-RemotePsCommand -Script $samples[$i]
    $okPrefix = $full.StartsWith('powershell -NoProfile -NonInteractive -EncodedCommand ')
    Assert ("$($names[$i]):用了 -EncodedCommand") $okPrefix
    $b64 = $full.Substring($full.LastIndexOf(' ') + 1)
    $back = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($b64))
    Assert ("$($names[$i]):解回来和原文逐字节相等") ($back -ceq $samples[$i])
  }
  # $_.Name / $_.Length 的插值必须**原样**留到远端去展开 —— 上一版在这里
  # 就被本地展开成了 "powershell.exe|292864"。断言检查的是脚本正文里的字面量。
  $listScript = Get-RemoteListScript -Dir 'C:\206dash-data'
  Assert '列目录脚本里保留了 $_.Name 插值'     ($listScript.Contains('$_.Name'))
  Assert '列目录脚本里保留了 $_.Length 插值'   ($listScript.Contains('$_.Length'))
  Assert '列目录脚本里没有被本地提前展开'      (-not $listScript.Contains('powershell.exe'))
  Assert '列目录脚本里保留了 | 分隔符'          ($listScript.Contains("+ '|' +"))
  # ★ UTF-8:两头都得钉死,否则中文文件名(对话记录.jsonl)在列目录里变乱码,
  #   大小比较认不出它 → 每次重发 + 发完复核假失败退 6。本地那句在脚本头部。
  Assert '本地已按 UTF-8 解远端输出'            ([Console]::OutputEncoding.CodePage -eq 65001) "实际:$([Console]::OutputEncoding.CodePage)"
  Assert '列目录脚本里钉死了 UTF-8 输出'        ($listScript.Contains('[Console]::OutputEncoding = [Text.Encoding]::UTF8'))
  Assert '仓库体检脚本里钉死了 UTF-8 输出'      ((Get-RemoteRepoProbeScript -Repo 'C:\repo').Contains('[Console]::OutputEncoding = [Text.Encoding]::UTF8'))
  Assert '仓库快进脚本里钉死了 UTF-8 输出'      ((Get-RemoteRepoMergeScript -Repo 'C:\repo').Contains('[Console]::OutputEncoding = [Text.Encoding]::UTF8'))
  Assert '整条命令里没有裸引号会活不过传输'     ($full -match '^(powershell -NoProfile -NonInteractive -EncodedCommand [A-Za-z0-9+/=]+)$')
  # ssh 参数
  $sa = Get-SshArgs -RemoteCommand 'Write-Output 1'
  Assert 'ssh 参数带 BatchMode=yes(绝不弹密码)' ($sa -contains 'BatchMode=yes')
  Assert 'ssh 参数带 ConnectTimeout'            (($sa -join ' ') -match 'ConnectTimeout=10')
  Assert 'ssh 参数带 -i 私钥'                   ($sa -contains '-i' -and $sa -contains $SshKey)
  Assert 'ssh 目标是 张九思@26.253.1.139'       ($sa -contains '张九思@26.253.1.139')

  Write-Head 'SelfTest 4/6:gate 判据 + 仓库判决(2026-09-21)'
  # gate 原来用**含未跟踪文件**的 `git status --porcelain`:笔记本 checkout 里躺着
  # 26 个早期 sshd / Radmin 调试残留(A.sshd.log、diagnose-sshd*.ps1 …),于是每次
  # 都快进失败、退出 6,明明同步是成功的。下面几条把新判据钉死。
  $gateScript = Get-RemoteRepoProbeScript -Repo $LaptopRepo
  Assert 'gate 用 --untracked-files=no(未跟踪不进判据)' ($gateScript.Contains('git status --porcelain --untracked-files=no'))
  Assert 'gate 里没有含未跟踪的裸 porcelain'            (-not ($gateScript -match 'git status --porcelain\s*\)'))
  Assert '判据仍是 PORCELAIN_COUNT(只是收窄了)'         ($gateScript.Contains("('PORCELAIN_COUNT=' + " + '$dirty.Count'))
  Assert '已跟踪改动的明细照旧打出来(DIRTY:)'           ($gateScript.Contains("('DIRTY:' + "))
  Assert '未跟踪的只单独报个数(UNTRACKED_COUNT=)'        ($gateScript.Contains("('UNTRACKED_COUNT=' + "))
  Assert '数未跟踪用 StartsWith(不用会把 M 也数进去)'   ($gateScript.Contains("StartsWith('??')"))
  Assert '注释里留着"为什么"+日期(2026-09-21)'          ($gateScript.Contains('2026-09-21'))
  # fetch 失败时 merge --ff-only 会拿**过期的** origin/main 比出 "Already up to date."
  # + 退出码 0,所以两个退出码必须分开报,不能只看 MERGE_EXIT。
  $mergeScript = Get-RemoteRepoMergeScript -Repo $LaptopRepo
  Assert '快进脚本单独报 FETCH_EXIT(fetch 失败不算成功)' ($mergeScript.Contains("('FETCH_EXIT=' + "))
  Assert '快进脚本仍然报 MERGE_EXIT'                     ($mergeScript.Contains("('MERGE_EXIT=' + "))
  Assert '快进脚本仍然报 HEAD'                           ($mergeScript.Contains("('HEAD=' + "))
  # 判决矩阵:一条条点。★ 这里就是第一版翻车的地方 —— 状态机写错时,两边 HEAD
  # 明明都是 383c07c,脚本照样判"失败"退出 6。所以判决必须是纯函数 + 逐条断言。
  $v = Get-RepoVerdict -DesktopHead 'aaa' -LaptopHead 'aaa' -FetchExit 0 -MergeExit 0 -DirtyCount 0
  Assert '判决:全成功 → ok'                    ($v -eq 'ok')                     "实际:$v"
  $v = Get-RepoVerdict -DesktopHead 'aaa' -LaptopHead 'aaa' -FetchExit 128 -MergeExit 0 -DirtyCount 0
  Assert '判决:fetch 失败 → nofetch(不许当成功)' ($v -eq 'nofetch')               "实际:$v"
  $v = Get-RepoVerdict -DesktopHead 'aaa' -LaptopHead 'aaa' -FetchExit 0 -MergeExit 1 -DirtyCount 0
  Assert '判决:merge 报错 → failed(哈希相同也不行)' ($v -eq 'failed')             "实际:$v"
  $v = Get-RepoVerdict -DesktopHead 'bbb' -LaptopHead 'aaa' -FetchExit 0 -MergeExit 0 -DirtyCount 0
  Assert '判决:HEAD 不一致 → failed'           ($v -eq 'failed')                 "实际:$v"
  $v = Get-RepoVerdict -DesktopHead 'aaa' -LaptopHead 'aaa' -FetchExit 0 -MergeExit 0 -DirtyCount 3
  Assert '判决:有已跟踪改动 → refused'         ($v -eq 'refused')                "实际:$v"
  $v = Get-RepoVerdict -DesktopHead 'aaa' -LaptopHead 'bbb' -FetchExit 128 -MergeExit 0 -DirtyCount 2
  Assert '判决:refused 优先(没碰就是没碰)'     ($v -eq 'refused')                "实际:$v"
  # 中文路径那条不影响 gate:同一份正文仍然要能逐字节穿过 base64(见 5/5)。

  Write-Head 'SelfTest 5/6:中文路径原样穿过(本机 8.3 短名是关的)'
  # 这台机器 NtfsDisable8dot3NameCreation 开着,拿不到 ZHANGJ~1。
  # 所以必须证明:中文路径在 base64 里一字不改地活到远端。这是整条同步的前提。
  $cnScript = Get-RemoteRepoProbeScript -Repo $LaptopRepo
  Assert '生成的仓库体检脚本里带着中文路径' ($cnScript.Contains('张九思'))
  $cnCmd = New-RemotePsCommand -Script $cnScript
  $cnB64 = $cnCmd.Substring($cnCmd.LastIndexOf(' ') + 1)
  $cnBack = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($cnB64))
  Assert '中文路径在 base64 里逐字节无损'     ($cnBack -ceq $cnScript)
  Assert '  中文原样在,没有被换成 ? 或乱码'   ($cnBack.Contains('张九思'))
  Assert '整条命令仍然是纯 ASCII(传输安全)'   ($cnCmd -match '^[\x20-\x7E]+$')
  # 拦截开关本身还要能拦住(留给需要的人,默认不启用)
  $threw = $false
  try { ConvertTo-EncodedCommand -Script "Set-Location 'C:\Users\张九思'" -RejectNonAscii | Out-Null } catch { $threw = $true }
  Assert '-RejectNonAscii 生效时确实会拦'     $threw
  # 中文任务/路径不该影响的是:远端正文里不能出现"本地进程名被展开"的痕迹
  Assert '仓库体检脚本没有被本地提前展开'     (-not $cnScript.Contains('powershell.exe'))

  Write-Head 'SelfTest 6/6:会话记录(对话记录)—— 临时目录 + 纯函数,不碰网络也不碰真的 .dsh'
  # 造一棵假的会话树:三个会话目录 + 嵌套一层 + 一个"名字像但不是"的干扰文件。
  # ★ 名字排序和时间排序**故意不一致**(aaaa 最早其实是中间,zzzz 名字最大反而最老),
  #   这样"按最后写入时间取最新"这条才真的被测到,而不是被名字顺序蒙对。
  $tRoot = Join-Path ([IO.Path]::GetTempPath()) ('206dash-selftest-' + [guid]::NewGuid().ToString('N'))
  $tEmpty = Join-Path ([IO.Path]::GetTempPath()) ('206dash-selftest-empty-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $tRoot -Force | Out-Null
  New-Item -ItemType Directory -Path $tEmpty -Force | Out-Null
  try {
    $sA = Join-Path $tRoot 'aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa'
    $sM = Join-Path $tRoot 'mmmmmmmm-2222-4222-8222-mmmmmmmmmmmm'
    $sZ = Join-Path $tRoot 'zzzzzzzz-3333-4333-8333-zzzzzzzzzzzz'
    foreach ($p in @($sA, $sM, $sZ, (Join-Path $sM 'deep'))) { New-Item -ItemType Directory -Path $p -Force | Out-Null }
    Set-Content -LiteralPath (Join-Path $sA 'session.v3.jsonl.zstd') -Value 'aaaa'     -NoNewline -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $sM 'deep\session.v3.jsonl.zstd') -Value 'cc' -NoNewline -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $sZ 'session.v3.jsonl.zstd') -Value 'bbbbbb'  -NoNewline -Encoding ASCII
    # 干扰项:名字像,但**不是** session.v3.jsonl.zstd,而且写得最新 —— 必须不被选中
    Set-Content -LiteralPath (Join-Path $sZ 'session.v3.jsonl') -Value 'decoy' -NoNewline -Encoding ASCII
    (Get-Item -LiteralPath (Join-Path $sA 'session.v3.jsonl.zstd')).LastWriteTime      = (Get-Date).AddMinutes(-20)
    (Get-Item -LiteralPath (Join-Path $sM 'deep\session.v3.jsonl.zstd')).LastWriteTime = (Get-Date).AddMinutes(-2)
    (Get-Item -LiteralPath (Join-Path $sZ 'session.v3.jsonl.zstd')).LastWriteTime      = (Get-Date).AddMinutes(-30)
    (Get-Item -LiteralPath (Join-Path $sZ 'session.v3.jsonl')).LastWriteTime           = (Get-Date)

    $expect = Join-Path $sM 'deep\session.v3.jsonl.zstd'
    $t = Resolve-SessionTranscript -Root $tRoot
    Assert '找得到会话记录'                       ($null -ne $t)
    Assert '  递归进子目录 + 取最后写入的那个'    ($null -ne $t -and $t.Path -eq $expect) "实际:$($t.Path)"
    Assert '  是按写入时间挑的(不是按目录名)'    ($null -ne $t -and $t.Path -notlike "*$([IO.Path]::DirectorySeparatorChar)aaaa*" -and $t.Path -notlike '*zzzzzzzz*') "实际:$($t.Path)"
    Assert '  会话名 = 文件所在目录名'            ($null -ne $t -and $t.SessionName -eq 'deep') "实际:$($t.SessionName)"
    Assert '  大小读得到(2 字节)'                ($null -ne $t -and $t.Size -eq 2) "实际:$($t.Size)"
    Assert '  只认 session.v3.jsonl.zstd(干扰项不算)' ($null -ne $t -and $t.Path.EndsWith('session.v3.jsonl.zstd'))
    # 找不到 = $null(打一行就跳过),这是"绝不因为会话记录让整条同步失败"的根据
    Assert '空目录 → $null(跳过,不报错)'         ($null -eq (Resolve-SessionTranscript -Root $tEmpty))
    Assert '目录不存在 → $null(跳过,不报错)'     ($null -eq (Resolve-SessionTranscript -Root (Join-Path $tRoot 'no-such-dir')))
  } finally {
    Remove-Item -LiteralPath $tRoot  -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tEmpty -Recurse -Force -ErrorAction SilentlyContinue
  }

  # 两个目标名:原样一份(DSH 以后能打开)+ 可读一份(给人看/搜)
  $tn = Get-TranscriptSendNames -SessionName '5124ca8a-ec3f-4fdc-b00d-f349800a99f0'
  Assert '原始文件叫 session-<会话目录名>.jsonl.zstd' ($tn.Raw -eq 'session-5124ca8a-ec3f-4fdc-b00d-f349800a99f0.jsonl.zstd') "实际:$($tn.Raw)"
  Assert '可读版固定叫 对话记录.jsonl'                ($tn.Readable -eq '对话记录.jsonl') "实际:$($tn.Readable)"
  $tn2 = Get-TranscriptSendNames -SessionName 'weird name/x'
  Assert '会话名里的怪字符洗成 _(scp 目标名要稳)'     ($tn2.Raw -match '^session-[A-Za-z0-9._-]+\.jsonl\.zstd$') "实际:$($tn2.Raw)"

  # 超 20 MB:只多打一行警告,仍然照发(绝不静默跳过)
  Assert '1 MB 不吭声'                ($null -eq (Get-TranscriptSizeNote -Size 1MB))
  Assert '正好 20 MB 不吭声(线是"超过")' ($null -eq (Get-TranscriptSizeNote -Size $TranscriptWarnBytes))
  $note = Get-TranscriptSizeNote -Size ($TranscriptWarnBytes + 1)
  Assert '超过 20 MB → 有一行警告'      ($null -ne $note)
  Assert '  警告里写明 20 MB 这条线'     ($null -ne $note -and $note -match '20 MB')
  Assert '  警告里说明"仍然照发"'        ($null -ne $note -and $note -match '仍然照发')
  # 阈值和"为什么"必须留在注释里(owner 明确要求:说清楚这条线是干嘛的)
  $selfText = Get-Content -LiteralPath $scriptPath -Raw
  Assert '注释里留着 20 MB 阈值 + 理由'  ($selfText.Contains('$TranscriptWarnBytes = 20MB') -and $selfText.Contains('1 MB 上下') -and $selfText.Contains('解压出来的可读版还要再大'))
  Assert '注释里说明会话记录每次都会变'  ($selfText.Contains('通常每次都会变'))

  Write-Host ''
  if ($tally.Fail -eq 0) {
    Write-Host ("SelfTest 通过:$($tally.Pass) 项全绿(未使用任何网络)") -ForegroundColor Green
    Write-Info '注意:自检不验证笔记本那边 —— 通不通要真跑一次(不加参数)'
    return $EX_OK
  }
  Write-Host ("SelfTest 失败:$($tally.Fail) 项红了 / 共 $($tally.Pass + $tally.Fail) 项") -ForegroundColor Red
  return $EX_SELFTEST
}

# ===========================================================================
# 主流程
# ===========================================================================
$scriptPath = $PSCommandPath
if (-not $scriptPath) { $scriptPath = $MyInvocation.MyCommand.Path }

Write-Host '=== 206dash 桌机 → 笔记本 同步(SSH) ===' -ForegroundColor Cyan

if ($SelfTest)   { exit (Invoke-SelfTest) }
if ($Unregister) { exit (Unregister-SyncTask) }
if ($Register)   { exit (Register-SyncTask -ScriptPath $scriptPath) }

# ---- 参数体检 ----
if (-not (Test-Path -LiteralPath $SshKey)) {
  Write-Bad "找不到 SSH 私钥:$SshKey"
  Write-Info '笔记本上生成/放好密钥,或显式传 -SshKey <路径>。'
  exit $EX_ARGS
}
if ($SshTimeout -lt 1) { Write-Bad '-SshTimeout 至少是 1'; exit $EX_ARGS }

# 中文路径不再需要 8.3 短名:远端命令走 base64,UTF-16LE 里的中文原样到达
# (本机的 8.3 短名是关的,拿不到 ZHANGJ~1 —— 早先那条"必须 ASCII"的约束
#  就是在这里把整个同步卡死的)。
$repoShort = $LaptopRepo
$dataShort = $LaptopData

if ($WhatIf) {
  Write-Head 'WhatIf:只说要做什么,不动任何东西'
  Write-Info "通道    : ssh -i $SshKey $Laptop <探针>"
  Write-Info "仓库    : 桌机 git push;笔记本 git fetch + merge --ff-only origin/main"
  Write-Info "数据    : 只发大小不一样的 → $LaptopData"
  foreach ($a in ($Asset + $ZipAsset)) { Write-Info "          $a" }
  # 会话记录也是"只发大小不一样的",两个目标名都在 -LaptopData 里(不碰笔记本的 .dsh)
  $whatIfT = Resolve-SessionTranscript -Root $SessionRoot
  if ($null -eq $whatIfT) {
    Write-Info "          (会话记录: $SessionRoot 下没找到 session.v3.jsonl.zstd → 这步会跳过)"
  } else {
    $whatIfN = Get-TranscriptSendNames -SessionName $whatIfT.SessionName
    Write-Info ("          {0}" -f $whatIfT.Path)
    Write-Info ("            → {0} + {1}" -f $whatIfN.Raw, $whatIfN.Readable)
  }
  Write-Info '计划任务: 不加 -Register 就不会有任何计划任务'
  exit $EX_OK
}

# ---- 第 1 步:通道 ----
Write-Head '1/5 通道(Radmin → SSH)'
$probe = Test-SshChannel
if (-not $probe.Ok) {
  Write-Bad "SSH 通道不通:$($probe.Detail)"
  Write-Host ''
  Write-Host '  三件事按顺序查(这一条就能定位):' -ForegroundColor Yellow
  Write-Host '    1) Radmin 通不通   —— 两台机器都进同一个 Radmin 网络;先 ping 26.253.1.139'
  Write-Host '    2) 笔记本 sshd 在不在 —— 笔记本上:Get-Service sshd 应该是 Running'
  Write-Host "    3) 密钥对不对      —— 私钥路径:$SshKey(文件在不在?笔记本上 authorized_keys 里有没有对应的公钥?)"
  exit $EX_CHANNEL
}
Write-Ok "通道通:笔记本 = $($probe.HostName)（BatchMode,没有弹过密码）"
if ($Test) {
  Write-Head '2/2 仓库与数据(只读体检)'
  try {
    $rLines = Invoke-RemoteScript -Script (Get-RemoteRepoProbeScript -Repo $repoShort)
  } catch {
    Write-Bad "仓库体检失败:$($_.Exception.Message)"; exit $EX_REPO
  }
  $count = ($rLines | Where-Object { $_ -like 'PORCELAIN_COUNT=*' }) -replace 'PORCELAIN_COUNT=', ''
  $uCount = ($rLines | Where-Object { $_ -like 'UNTRACKED_COUNT=*' }) -replace 'UNTRACKED_COUNT=', ''
  $rHead = ($rLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
  $rBr   = ($rLines | Where-Object { $_ -like 'BRANCH=*' }) -replace 'BRANCH=', ''
  $dLines = @($rLines | Where-Object { $_ -like 'DIRTY:*' })
  Write-Info "笔记本 HEAD : $rHead ($rBr)"
  Write-Info "已跟踪改动  : $count 项(gate 只看这个)"
  Write-Info "未跟踪文件  : $uCount 个(不拦快进,gate 不看;要撞车 git 自己会中止)"
  Write-Info "仓库可快进  : $(if ([int]$count -eq 0) { '是' } else { '否 —— 有已跟踪改动,同步会拒绝动它' })"
  Write-Info '数据目标目录:'
  $listing = Invoke-RemoteScript -Script (Get-RemoteListScript -Dir $dataShort)
  if ($listing -contains 'MISSING') { Write-Info "  $LaptopData 不存在(真同步时会建)" }
  else { foreach ($l in $listing) { Write-Info "  $l" } }
  # 会话记录只读地看一眼(桌机本地找,不碰网络)
  $tProbe = Resolve-SessionTranscript -Root $SessionRoot
  if ($null -eq $tProbe) {
    Write-Info "会话记录    : $SessionRoot 下没有 session.v3.jsonl.zstd(真同步时会跳过这一步)"
  } else {
    $tProbeN = Get-TranscriptSendNames -SessionName $tProbe.SessionName
    Write-Info ("会话记录    : {0}" -f $tProbe.Path)
    Write-Info ("              会话 {0} / {1} MB / 写于 {2} → 会发成 {3} + {4}" -f `
      $tProbe.SessionName, [math]::Round($tProbe.Size / 1MB, 2), $tProbe.Written.ToString('yyyy-MM-dd HH:mm:ss'), $tProbeN.Raw, $tProbeN.Readable)
  }
  Write-Host ''
  Write-Host '探路结束:通道没问题。去掉 -Test 就会真同步。' -ForegroundColor Green
  exit $EX_OK
}

# ---- 第 2 步:仓库(先 push,再在笔记本上快进) ----
Write-Head '2/5 仓库(桌机 push → 笔记本 ff-only)'
$desktopRepo = Split-Path -Parent (Split-Path -Parent $scriptPath)
$desktopHead = ''
Push-Location $desktopRepo
try {
  $pushRes = Invoke-Native -Exe 'git' -Arguments @('push')
  foreach ($l in $pushRes.Lines) {
    # git 把 "Everything up-to-date" 写在 stderr 上,那是正常输出,不是错误
    if ($l.Trim()) { Write-Info "push: $l" }
  }
  if ($pushRes.ExitCode -ne 0) { Write-Warn2 "git push 退出码 $($pushRes.ExitCode)(继续:笔记本只快进到 origin/main)" }
  $desktopHead = (Invoke-Native -Exe 'git' -Arguments @('rev-parse', 'HEAD')).Lines[0].Trim()
} finally { Pop-Location }
Write-Ok "桌机 HEAD  : $desktopHead"

# gate:笔记本上有**已跟踪**文件的改动就**不动**它。不 stash / 不 reset / 不 clean ——
# 那些会把别人没提交的工作弄丢,而这是台在用的开发机。
# ★ 未跟踪文件**不进** gate 判据(2026-09-21 定的):它们挡不住快进,真要撞车
#   git 自己会中止合并、不会覆盖。为什么收窄、当时踩了什么,见
#   Get-RemoteRepoProbeScript 里那段注释。
$rLines = Invoke-RemoteScript -Script (Get-RemoteRepoProbeScript -Repo $repoShort)
$dirtyCount = [int](($rLines | Where-Object { $_ -like 'PORCELAIN_COUNT=*' }) -replace 'PORCELAIN_COUNT=', '')
$untrackedCount = [int](($rLines | Where-Object { $_ -like 'UNTRACKED_COUNT=*' }) -replace 'UNTRACKED_COUNT=', '')
$laptopHeadBefore = ($rLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
$dirtyLines = @($rLines | Where-Object { $_ -like 'DIRTY:*' } | ForEach-Object { $_.Substring(6) })
Write-Info "笔记本 HEAD(动之前): $laptopHeadBefore"
if ($untrackedCount -gt 0) {
  Write-Info "笔记本另有 $untrackedCount 个未跟踪文件 —— 按 2026-09-21 的决定不拦(快进不会覆盖它们)"
}

if ($dirtyCount -gt 0) {
  $repoState = 'refused'
  Write-Bad "笔记本仓库有 $dirtyCount 项**已跟踪**文件的改动 —— 拒绝碰它(没有 stash / reset / clean)"
  foreach ($l in ($dirtyLines | Select-Object -First 10)) { Write-Info "  $l" }
  if ($dirtyCount -gt 10) { Write-Info "  ...还有 $($dirtyCount - 10) 项" }
  Write-Info '处理办法:在笔记本上把这些改动处理掉(提交 / 挪走 / 删掉)再同步。'
  Write-Info "仓库这步**跳过**了(未跟踪文件不算,数据文件也不受影响,继续往下走)。"
} else {
  $mLines = Invoke-RemoteScript -Script (Get-RemoteRepoMergeScript -Repo $repoShort)
  foreach ($l in ($mLines | Where-Object { $_ -like 'FETCH:*' -or $_ -like 'MERGE:*' })) { Write-Info $l }
  $fExit = [int](($mLines | Where-Object { $_ -like 'FETCH_EXIT=*' }) -replace 'FETCH_EXIT=', '')
  $mExit = [int](($mLines | Where-Object { $_ -like 'MERGE_EXIT=*' }) -replace 'MERGE_EXIT=', '')
  $laptopHeadAfter = ($mLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''

  # 判决交给 Get-RepoVerdict(纯函数,自检里逐条测过)。★ 别在这儿手写状态机 ——
  # 第一版就是在这一步把"默认值 failed"和"merge 真报错"混成一个值,结果两边 HEAD
  # 明明都是 383c07c 也判成失败、退出 6。
  $repoState = Get-RepoVerdict -DesktopHead $desktopHead -LaptopHead $laptopHeadAfter `
                 -FetchExit $fExit -MergeExit $mExit -DirtyCount $dirtyCount

  switch ($repoState) {
    'ok' {
      Write-Ok "笔记本 fetch + ff-only 成功,两边 HEAD 一致:$laptopHeadAfter"
    }
    'nofetch' {
      # 笔记本自己 fetch 失败 = origin/main 还是旧的。那句 "Already up to date."
      # 是**跟旧数据比**出来的,不能当成功。
      Write-Bad "笔记本自己 git fetch origin 失败(退出码 $fExit)—— origin/main 还是旧的,看上面的 FETCH: 行"
      Write-Info "笔记本 HEAD : $laptopHeadAfter"
    }
    default {
      if ($mExit -ne 0) { Write-Warn2 "git merge --ff-only 退出码 $mExit(可能是有未跟踪文件正好要被覆盖 —— git 自己中止了)" }
      if ($desktopHead -ne $laptopHeadAfter) { Write-Warn2 "两边 HEAD 不一致:桌机 $desktopHead / 笔记本 $laptopHeadAfter" }
      else { Write-Warn2 '两边 HEAD 哈希恰好相同,但这一步没成功 —— 不能当成“已同步”' }
    }
  }
}
if ($repoState -ne 'ok') { Write-Warn2 '仓库这一步没成功(数据照常同步,最后退出码会体现)' }

# ---- 第 3 步:问笔记本要现有大小 ----
Write-Head '3/5 数据(只发大小不一样的)'
$createDir = "if (-not (Test-Path -LiteralPath '$dataShort')) { New-Item -ItemType Directory -Force -Path '$dataShort' | Out-Null; Write-Output 'CREATED' } else { Write-Output 'EXISTS' }"
$cOut = Invoke-RemoteScript -Script $createDir
if ($cOut -contains 'CREATED') { Write-Ok "已在笔记本上建好 $LaptopData" }
else { Write-Info "$LaptopData 已存在" }

$remoteLines = Invoke-RemoteScript -Script (Get-RemoteListScript -Dir $dataShort)
$remoteMap = ConvertFrom-RemoteListing -Lines $remoteLines
Write-Info ("笔记本上现有 {0} 个文件" -f $remoteMap.Count)

# ---- 第 4 步:逐个判决 + 传 ----
$toSend = @()
foreach ($a in ($Asset + $ZipAsset)) {
  $leaf = Split-Path $a -Leaf
  $exists = Test-Path -LiteralPath $a
  $localSize = 0L
  if ($exists) { $localSize = (Get-Item -LiteralPath $a).Length }
  $remoteSize = $null
  if ($remoteMap.ContainsKey($leaf)) { $remoteSize = [long]$remoteMap[$leaf] }
  $dec = Get-SyncDecision -Name $leaf -LocalSize $localSize -RemoteSize $remoteSize -LocalMissing:(-not $exists)
  $toSend += [pscustomobject]@{ Path = $a; Leaf = $leaf; LocalSize = $localSize; RemoteSize = $remoteSize; Action = $dec.Action; Reason = $dec.Reason }
}

# ---- 会话记录(DSH 对话记录):也走同一条"比大小 → 发/跳"的路 ------------------
# 2026-09-21 owner 的决定:每次同步都把桌机上最新那个会话带到笔记本上,两个文件
# 都落在 -LaptopData。★ 脚本**不**碰笔记本自己的 .dsh 会话树 —— 要让会话出现在
# 笔记本 DSH 的列表里,是**手工一步**(只复制、不覆盖更新的;见 README「笔记本上的 DSH」)。
#
# ★ 这一条**通常每次都会变**:DSH 一直在往会话文件里追加,大小几乎每次都不一样,
#   所以大小比较基本每次都判"发"。这是**预期**行为,不是 bug —— 就这一个文件,
#   正常 1 MB 上下(2026-09-21 实测 .zstd 0.84 MB / 可读版 3.07 MB),重发一遍的代价可以忽略。
$transcriptTemp = $null
$transcript = Resolve-SessionTranscript -Root $SessionRoot
if ($null -eq $transcript) {
  Write-Warn2 "会话记录:在 $SessionRoot 下没找到 session.v3.jsonl.zstd —— 这一步跳过(数据文件照常,不算失败)"
} else {
  $tNames = Get-TranscriptSendNames -SessionName $transcript.SessionName
  Write-Info ("会话记录:{0}" -f $transcript.Path)
  Write-Info ("          会话 {0} / {1} MB / 写于 {2} → {3} + {4}" -f `
    $transcript.SessionName, [math]::Round($transcript.Size / 1MB, 2), $transcript.Written.ToString('yyyy-MM-dd HH:mm:ss'), $tNames.Raw, $tNames.Readable)
  $tNote = Get-TranscriptSizeNote -Size $transcript.Size
  if ($tNote) { Write-Warn2 $tNote }

  # ★ 为什么先拍个快照再发:会话文件是**活的** —— DSH 正在往里追加。要是边发边涨,
  #   传完复核的大小就永远对不上,会被记成"传输失败"→ 退出码 6,明明文件已经到了。
  #   所以先 Copy-Item 到临时目录冻结一份,发的是冻结那份、复核的也是它。
  #   (临时目录在 %TEMP% 下;正常跑完就在下面删掉。)
  $transcriptTemp = Join-Path ([IO.Path]::GetTempPath()) ('206dash-sync-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $transcriptTemp -Force | Out-Null
  $snapRaw = Join-Path $transcriptTemp $tNames.Raw
  Copy-Item -LiteralPath $transcript.Path -Destination $snapRaw -Force

  # 可读版:桌机的 Python 3.14 自带 compression.zstd(实测有,多帧也一次解完)。
  # Python 不在 / 模块不在 / 解压报错 —— 三种都**不**连累别的:原始那份照发,
  # 这里只打一行警告就跳过(owner 明确要的是"跳过要有一行清楚的话")。
  $readablePath = Join-Path $transcriptTemp $tNames.Readable
  $pyExe = $null
  foreach ($cand in @('python', 'python3', 'py')) {
    if (Get-Command $cand -CommandType Application -ErrorAction SilentlyContinue) { $pyExe = $cand; break }
  }
  if (-not $pyExe) {
    Write-Warn2 ("对话记录.jsonl 这次不做:这台机器上没有 python —— 只发原始那份({0})" -f $tNames.Raw)
  } else {
    # Python 正文里只用单引号、一个双引号都不出现 —— PS 5.1 给外部程序拼参数时
    # 只会在外面套一层双引号,正文里再冒出双引号就会被它改坏(和 ssh 那个坑同源)。
    # ★ try/catch 是必须的:实测 `& <不存在的程序>` 抛的是**终止性**错误,
    #   连 Invoke-Native 里那句 EAP=Continue 都拦不住 —— 不包起来的话
    #   "没装 Python"会把整条同步炸掉,而这里要的只是"跳过可读版 + 一行警告"。
    $pyCode = 'from compression.zstd import decompress;import sys;open(sys.argv[2],''wb'').write(decompress(open(sys.argv[1],''rb'').read()))'
    # -X utf8:让 Python 的 stdout/stderr 也走 UTF-8,和上面钉死的两边编码一致
    # (出错时那行警告才不会变成乱码;文件本身是二进制写,不受影响)
    $pyRes = $null
    try {
      $pyRes = Invoke-Native -Exe $pyExe -Arguments @('-X', 'utf8', '-c', $pyCode, $snapRaw, $readablePath)
    } catch {
      $pyRes = [pscustomobject]@{ ExitCode = -1; Lines = @($_.Exception.Message) }
    }
    if ($pyRes.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $readablePath)) {
      Write-Warn2 ("对话记录.jsonl 这次没做出来({0} 退出码 {1},可能抄到半条或模块不在)—— 只发原始那份;{2}" -f `
        $pyExe, $pyRes.ExitCode, (($pyRes.Lines | Select-Object -First 2) -join ' / '))
    } else {
      Write-Ok ("对话记录.jsonl 已解压出来({0} 字节)" -f (Get-Item -LiteralPath $readablePath).Length)
    }
  }

  # 两个目标名都按同一套判决走:笔记本上没有就发,大小不同就发,一样就跳。
  $tCand = @([pscustomobject]@{ Path = $snapRaw; Leaf = $tNames.Raw })
  if (Test-Path -LiteralPath $readablePath) { $tCand += [pscustomobject]@{ Path = $readablePath; Leaf = $tNames.Readable } }
  foreach ($c in $tCand) {
    $tLocalSize = [long](Get-Item -LiteralPath $c.Path).Length
    $tRemoteSize = $null
    if ($remoteMap.ContainsKey($c.Leaf)) { $tRemoteSize = [long]$remoteMap[$c.Leaf] }
    $tDec = Get-SyncDecision -Name $c.Leaf -LocalSize $tLocalSize -RemoteSize $tRemoteSize
    $toSend += [pscustomobject]@{ Path = $c.Path; Leaf = $c.Leaf; LocalSize = $tLocalSize; RemoteSize = $tRemoteSize; Action = $tDec.Action; Reason = $tDec.Reason }
  }
}

$sent = 0; $sentBytes = 0L; $skipped = 0; $missing = 0; $failed = 0
foreach ($item in $toSend) {
  switch ($item.Action) {
    'Missing' {
      $missing++
      Write-Warn2 ("{0,-22} 跳过 —— {1}" -f $item.Leaf, $item.Reason)
    }
    'Skip' {
      $skipped++
      Write-Info ("{0,-22} 跳过 —— {1}({2} 字节)" -f $item.Leaf, $item.Reason, $item.LocalSize)
    }
    'Send' {
      $mb = [math]::Round($item.LocalSize / 1MB, 2)
      Write-Info ("{0,-22} 发送 —— {1}（{2} 字节 / {3} MB）" -f $item.Leaf, $item.Reason, $item.LocalSize, $mb)
      # scp 是标准件,Windows 自带;rsync 在 Windows 上没有 ——
      # 所以"只发差异"这件事只能自己用大小比较来做(见 Get-SyncDecision)。
      # 远端目标要写成 用户@主机:C:\路径 的形式,而且不能加引号 ——
      # 实测给路径套引号反而传不过去。
      $scpRes = Invoke-Native -Exe 'scp' -Arguments @(
        '-o', 'BatchMode=yes',
        '-o', ("ConnectTimeout={0}" -f $SshTimeout),
        '-i', $SshKey,
        '--', $item.Path,
        ("{0}:{1}" -f $Laptop, $LaptopData)
      )
      foreach ($l in $scpRes.Lines) { if ($l.Trim()) { Write-Info "    scp: $l" } }
      if ($scpRes.ExitCode -ne 0) {
        $failed++
        Write-Bad ("{0} 传输失败(scp 退出码 {1})" -f $item.Leaf, $scpRes.ExitCode)
        continue
      }
      # 传完复核大小 —— 这是唯一可信的"真到了"证据
      $verifyLines = Invoke-RemoteScript -Script (Get-RemoteListScript -Dir $dataShort)
      $verifyMap = ConvertFrom-RemoteListing -Lines $verifyLines
      if ($verifyMap.ContainsKey($item.Leaf) -and [long]$verifyMap[$item.Leaf] -eq $item.LocalSize) {
        $sent++; $sentBytes += $item.LocalSize
        Write-Ok ("{0} 已送达并核对大小一致（{1} 字节）" -f $item.Leaf, $item.LocalSize)
      } else {
        $failed++
        $got = if ($verifyMap.ContainsKey($item.Leaf)) { [long]$verifyMap[$item.Leaf] } else { '不存在' }
        Write-Bad ("{0} 传完大小不对:桌机 {1} / 笔记本 {2}" -f $item.Leaf, $item.LocalSize, $got)
      }
    }
  }
}

# 会话记录的临时快照(冻结的那一份)用完就删 —— 它只活在这一次同步里。
# (要是脚本在中间异常退出,这份会留在 %TEMP% 里;不影响下次同步,下次是新的目录名。)
if ($transcriptTemp -and (Test-Path -LiteralPath $transcriptTemp)) {
  Remove-Item -LiteralPath $transcriptTemp -Recurse -Force -ErrorAction SilentlyContinue
}

# ---- 第 5 步:小结 ----
Write-Head '4/5 两边 HEAD 复核'
Push-Location $desktopRepo
try { $desktopHead = (Invoke-Native -Exe 'git' -Arguments @('rev-parse', 'HEAD')).Lines[0].Trim() } finally { Pop-Location }
$finalLines = Invoke-RemoteScript -Script (Get-RemoteRepoProbeScript -Repo $repoShort)
$laptopHeadFinal = ($finalLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
Write-Info "桌机 HEAD   : $desktopHead"
Write-Info "笔记本 HEAD : $laptopHeadFinal"
# ★ 哈希相同 ≠ 这一步做过:gate 拒绝时两边的 HEAD 本来就可能一样(笔记本早就停在
#   同一个提交上)。所以先看 $repoState,不然后面会打出假的"已同步"。
if ($desktopHead -eq $laptopHeadFinal) {
  if ($repoState -eq 'ok') { Write-Ok '仓库两边一致(第 2 步的 fetch + ff-only 确实跑过)' }
  elseif ($repoState -eq 'refused') { Write-Warn2 '两边 HEAD 哈希恰好相同,但第 2 步被 gate 拒绝过 —— 这**不算**已同步(本次没碰笔记本的仓库)' }
  else { Write-Warn2 '两边 HEAD 哈希恰好相同,但第 2 步的快进报了错 —— 这**不算**已同步' }
} else {
  Write-Bad '仓库两边不一致(见上面仓库那一步的报错)'
  if ($repoState -eq 'ok') { $repoState = 'failed' }
}

Write-Head '5/5 笔记本上现在的数据(证明东西真在)'
$finalList = Invoke-RemoteScript -Script (Get-RemoteListScript -Dir $dataShort)
foreach ($l in $finalList) { Write-Info ("  {0}" -f $l) }

Write-Head '小结'
Write-Info "通道    : 通(笔记本 = $($probe.HostName))"
# 仓库这步**照实说**:成功 / 被 gate 拒绝(跳过) / 快进报错,三种分开讲,
# 不拿"两边哈希一样"冒充"已同步"。
switch ($repoState) {
  'ok'      { Write-Ok    "仓库    : 已快进,两边 HEAD 一致(桌机 $desktopHead / 笔记本 $laptopHeadFinal)" }
  'refused' { Write-Warn2 "仓库    : **跳过**(gate 拒绝:笔记本有 $dirtyCount 项已跟踪改动,脚本不碰它)" }
  'nofetch' { Write-Bad   "仓库    : **没成功**(笔记本自己 fetch origin 失败,origin/main 是旧的;桌机 $desktopHead / 笔记本 $laptopHeadFinal)" }
  default   { Write-Bad   "仓库    : **没成功**(fetch / merge --ff-only 报错;桌机 $desktopHead / 笔记本 $laptopHeadFinal)" }
}
Write-Info ("数据    : 发送 {0} 个({1} 字节 / {2} MB);跳过 {3} 个;桌机上没有 {4} 个;失败 {5} 个" -f `
  $sent, $sentBytes, [math]::Round($sentBytes / 1MB, 2), $skipped, $missing, $failed)
if ($failed -gt 0) {
  Write-Bad '有文件没传成功 —— 再跑一次(已经传过去的会被大小比较跳过,不会重传)'
  exit $EX_DATA
}
if ($repoState -ne 'ok') {
  if ($repoState -eq 'refused') {
    Write-Bad '数据传完了,但仓库这一步**被跳过**了:笔记本 checkout 有已跟踪改动,gate 拒绝动它'
    Write-Info '数据是新的;仓库要等那些改动(提交 / 挪走 / 删掉)处理掉再同步。'
  } elseif ($repoState -eq 'nofetch') {
    Write-Bad '数据传完了,但仓库没同步:笔记本自己 git fetch origin 失败(笔记本那边连不上它的远端)'
    Write-Info '桌机这边是好的(push 走 ssh.github.com:443)。查笔记本的网络 / remote.origin.url(它的 VPN 应该是常开的)。'
    Write-Info '★ "用 git bundle 从桌机中继 git 对象"这条**已评估过、决定不做**(VPN 常开 ⇒ 多此一举;理由见 tools/sync/README.md「已评估但不做」)。'
  } else {
    Write-Bad '数据传完了,但仓库那一步没成功(看上面的 FETCH: / MERGE: 行)'
  }
  exit $EX_DATA
}
Write-Ok '全部成功'
exit $EX_OK
