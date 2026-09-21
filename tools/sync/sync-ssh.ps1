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
#>
[CmdletBinding()]
param(
  # SSH 私钥(桌机 → 笔记本专用)。默认值里的 $env:USERPROFILE 是**运行时**展开的
  [string]$SshKey     = "$env:USERPROFILE\.ssh\dsh_laptop",
  [string]$Laptop     = '张九思@26.253.1.139',
  [string]$LaptopRepo = 'C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard',
  [string]$LaptopData = 'C:\206dash-data',
  [int]$SshTimeout    = 10,

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
if (Test-Path -LiteralPath '__DIR__') {
  Get-ChildItem -LiteralPath '__DIR__' -File -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Output ($_.Name + '|' + $_.Length) }
} else { Write-Output 'MISSING' }
'@.Replace('__DIR__', $d)
}

function Get-RemoteRepoProbeScript {
  <#  远端仓库体检:先看有没有本地改动(gate),再报 HEAD。
      gate 和取 HEAD 放在同一次往返里,省一次 SSH。同样是单引号 here-string。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Repo)
  $r = $Repo.Replace("'", "''")
  return @'
$ProgressPreference = 'SilentlyContinue'
Set-Location -LiteralPath '__REPO__'
$dirty = @(git status --porcelain)
Write-Output ('PORCELAIN_COUNT=' + $dirty.Count)
$dirty | ForEach-Object { Write-Output ('DIRTY:' + $_) }
Write-Output ('HEAD=' + (git rev-parse HEAD).Trim())
Write-Output ('BRANCH=' + (git rev-parse --abbrev-ref HEAD).Trim())
'@.Replace('__REPO__', $r)
}

function Get-RemoteRepoMergeScript {
  <#  fetch + merge --ff-only。只快进,不产生 merge commit,也绝不碰本地改动。 #>
  [CmdletBinding()]
  param([Parameter(Mandatory)][string]$Repo)
  $r = $Repo.Replace("'", "''")
  return @'
$ProgressPreference = 'SilentlyContinue'
Set-Location -LiteralPath '__REPO__'
git fetch --prune origin 2>&1 | ForEach-Object { Write-Output ('FETCH:' + $_) }
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

  Write-Head 'SelfTest 1/4:参数解析'
  Assert '默认 SshKey 指向 ~\.ssh\dsh_laptop' ($SshKey -eq (Join-Path $env:USERPROFILE '.ssh\dsh_laptop')) "实际:$SshKey"
  Assert '默认 Laptop 正确'      ($Laptop -eq '张九思@26.253.1.139')              "实际:$Laptop"
  Assert '默认 LaptopRepo 正确'  ($LaptopRepo -eq 'C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard')
  Assert '默认 LaptopData 正确'  ($LaptopData -eq 'C:\206dash-data')              "实际:$LaptopData"
  Assert '默认 SshTimeout=10'    ($SshTimeout -eq 10)                             "实际:$SshTimeout"
  Assert '默认搬 4 个散件'       ($Asset.Count -eq 4)                             "实际:$($Asset.Count)"
  Assert '4 个散件名字都对'      ((@($Asset | ForEach-Object { Split-Path $_ -Leaf }) -join ',') -eq 'van_capture_dm.csv,drive5min.csv,image-v3.bin,theme-user.json')
  Assert 'zip 是单独一个可选件'  ($ZipAsset -like '*206dash-transfer.zip')
  Assert '目标不是已废的 C:\206dash-sync' ($LaptopData -ne 'C:\206dash-sync')
  Assert '任务名是 206dash-sync-ssh' ($TaskName -eq '206dash-sync-ssh')

  Write-Head 'SelfTest 2/4:大小比较逻辑'
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

  Write-Head 'SelfTest 3/4:远端命令的引号/编码'
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
  Assert '整条命令里没有裸引号会活不过传输'     ($full -match '^(powershell -NoProfile -NonInteractive -EncodedCommand [A-Za-z0-9+/=]+)$')
  # ssh 参数
  $sa = Get-SshArgs -RemoteCommand 'Write-Output 1'
  Assert 'ssh 参数带 BatchMode=yes(绝不弹密码)' ($sa -contains 'BatchMode=yes')
  Assert 'ssh 参数带 ConnectTimeout'            (($sa -join ' ') -match 'ConnectTimeout=10')
  Assert 'ssh 参数带 -i 私钥'                   ($sa -contains '-i' -and $sa -contains $SshKey)
  Assert 'ssh 目标是 张九思@26.253.1.139'       ($sa -contains '张九思@26.253.1.139')

  Write-Head 'SelfTest 4/4:中文路径原样穿过(本机 8.3 短名是关的)'
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
  $rHead = ($rLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
  $rBr   = ($rLines | Where-Object { $_ -like 'BRANCH=*' }) -replace 'BRANCH=', ''
  $dLines = @($rLines | Where-Object { $_ -like 'DIRTY:*' })
  Write-Info "笔记本 HEAD : $rHead ($rBr)"
  Write-Info "本地改动    : $count 项"
  Write-Info "仓库可快进  : $(if ([int]$count -eq 0) { '是' } else { '否 —— 有本地改动,同步会拒绝动它' })"
  Write-Info '数据目标目录:'
  $listing = Invoke-RemoteScript -Script (Get-RemoteListScript -Dir $dataShort)
  if ($listing -contains 'MISSING') { Write-Info "  $LaptopData 不存在(真同步时会建)" }
  else { foreach ($l in $listing) { Write-Info "  $l" } }
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

# gate:笔记本上有本地改动就**不动**它。不 stash / 不 reset / 不 clean ——
# 那些会把别人没提交的工作弄丢,而这是台在用的开发机。
$rLines = Invoke-RemoteScript -Script (Get-RemoteRepoProbeScript -Repo $repoShort)
$dirtyCount = [int](($rLines | Where-Object { $_ -like 'PORCELAIN_COUNT=*' }) -replace 'PORCELAIN_COUNT=', '')
$laptopHeadBefore = ($rLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
$dirtyLines = @($rLines | Where-Object { $_ -like 'DIRTY:*' } | ForEach-Object { $_.Substring(6) })
Write-Info "笔记本 HEAD(动之前): $laptopHeadBefore"

$repoOk = $false
if ($dirtyCount -gt 0) {
  Write-Bad "笔记本仓库有 $dirtyCount 项本地改动 —— 拒绝碰它(没有 stash / reset / clean)"
  foreach ($l in ($dirtyLines | Select-Object -First 10)) { Write-Info "  $l" }
  if ($dirtyCount -gt 10) { Write-Info "  ...还有 $($dirtyCount - 10) 项" }
  Write-Info '处理办法:在笔记本上把这些改动处理掉(提交 / 挪走 / 删掉)再同步。'
  Write-Info '数据文件不受影响:它们进的是另一个目录,继续往下走。'
  $repoOk = $false
} else {
  $mLines = Invoke-RemoteScript -Script (Get-RemoteRepoMergeScript -Repo $repoShort)
  foreach ($l in ($mLines | Where-Object { $_ -like 'FETCH:*' -or $_ -like 'MERGE:*' })) { Write-Info $l }
  $mExit = ($mLines | Where-Object { $_ -like 'MERGE_EXIT=*' }) -replace 'MERGE_EXIT=', ''
  $laptopHeadAfter = ($mLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
  if ([int]$mExit -eq 0) { Write-Ok "笔记本 fetch + ff-only 成功,HEAD : $laptopHeadAfter" }
  else {
    Write-Warn2 "git merge --ff-only 退出码 $mExit(可能是没提交的改动挡住了快进)"
    Write-Info "笔记本 HEAD : $laptopHeadAfter"
  }
  if ($desktopHead -eq $laptopHeadAfter) { Write-Ok '两边 HEAD 一致' }
  else { Write-Warn2 "两边 HEAD 不一致:桌机 $desktopHead / 笔记本 $laptopHeadAfter"; $repoOk = $false }
  if ([int]$mExit -eq 0 -and $desktopHead -eq $laptopHeadAfter) { $repoOk = $true }
}
if (-not $repoOk) { Write-Warn2 '仓库这一步没完全成功(数据照常同步,最后退出码会体现)' }

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

# ---- 第 5 步:小结 ----
Write-Head '4/5 两边 HEAD 复核'
Push-Location $desktopRepo
try { $desktopHead = (Invoke-Native -Exe 'git' -Arguments @('rev-parse', 'HEAD')).Lines[0].Trim() } finally { Pop-Location }
$finalLines = Invoke-RemoteScript -Script (Get-RemoteRepoProbeScript -Repo $repoShort)
$laptopHeadFinal = ($finalLines | Where-Object { $_ -like 'HEAD=*' }) -replace 'HEAD=', ''
Write-Info "桌机 HEAD   : $desktopHead"
Write-Info "笔记本 HEAD : $laptopHeadFinal"
if ($desktopHead -eq $laptopHeadFinal) { Write-Ok '仓库两边一致' } else { Write-Bad '仓库两边不一致(见上面仓库那一步的报错)'; $repoOk = $false }

Write-Head '5/5 笔记本上现在的数据(证明东西真在)'
$finalList = Invoke-RemoteScript -Script (Get-RemoteListScript -Dir $dataShort)
foreach ($l in $finalList) { Write-Info ("  {0}" -f $l) }

Write-Head '小结'
Write-Info "通道    : 通(笔记本 = $($probe.HostName))"
Write-Info "仓库    : 桌机 $desktopHead / 笔记本 $laptopHeadFinal"
Write-Info ("数据    : 发送 {0} 个({1} 字节 / {2} MB);跳过 {3} 个;桌机上没有 {4} 个;失败 {5} 个" -f `
  $sent, $sentBytes, [math]::Round($sentBytes / 1MB, 2), $skipped, $missing, $failed)
if ($failed -gt 0) {
  Write-Bad '有文件没传成功 —— 再跑一次(已经传过去的会被大小比较跳过,不会重传)'
  exit $EX_DATA
}
if (-not $repoOk) {
  Write-Bad '数据传完了,但仓库那一步没成功(看上面的 WARN/FAIL)'
  exit $EX_DATA
}
Write-Ok '全部成功'
exit $EX_OK
