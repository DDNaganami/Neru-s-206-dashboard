<#
  桌机 → 笔记本 自动同步(零依赖:只用 Windows 自带的 PowerShell + robocopy)

  为什么要有它(2026-09-21):
  这个项目真正的"家当"不是代码 —— 代码在 GitHub 上,`git push` 就完事;
  真正的家当是那些**大块头、不能进 git** 的东西:
    车上抓回来的 van_capture_dm.csv / drive5min.csv、烧进去的 image-v3.bin /
    theme-user.json、以及"拿到就能用"的两个 .ps1 小工具。
  这些东西每次都要手工 U 盘拷,一忘就是"笔记本上的数据是三天前的"。

  ★ 只做**单向**:桌机 → 笔记本。
  为什么不做双向:双向同步在两边都改过同一个文件时会**互相覆盖**,而且谁也说不清
  哪份是新的 —— 这比不自动同步还糟。桌机是唯一的数据源(抓帧、生成 image-v3.bin
  都在这里),笔记本是消费端。笔记本上产生的数据要走**人工**拷回来,不走这条路。

  ★ 代码和文档**不是**这个脚本的活:它们走 git(仓库已经推到 GitHub)。
  这个脚本只负责"git 装不下 / 不该装"的那些文件。

  怎么传:SMB over Radmin VPN,一条 `robocopy /MIR` 打到 UNC 路径。
  Radmin VPN 是虚拟局域网,地址形如 26.x.x.x —— 但它**必须先连上**,
  没连上时一切表现都像"对方关机"。所以下面第一件事永远是先探通不通。

  本机实测(2026-09-21,owner 的两台机器,脚本的探路就是照这个校准的):
    台式机(源) 26.177.134.224
    笔记本(目标)26.253.1.139   ← 写成 -LaptopHost 的默认值
    ping 通(2 ms);TCP 445 通(15 ms)、139 开 ⇒ SMB 这条路可用,不用上 Syncthing
    \\26.253.1.139\c$\Users\Public\206dash-sync 打不开 —— 探路查明**不是权限被拒**,
    而是笔记本上**还没有这个文件夹**。所以目标机上必须先有那个文件夹(或共享)。
  ★ 换机器(或 Radmin 重新分配了地址)**必须改 -LaptopHost 或改这里的默认值**。

  两种共享写法(挑一个):
    1) 命名共享(推荐,不要求两台机器账号一致):
         笔记本上:建好 C:\206dash-sync,再 `net share 206dash-sync=C:\206dash-sync /GRANT:"用户名",FULL`
         然后:  -ShareName 206dash-sync   →  \\26.253.1.139\206dash-sync
    2) 管理共享(不用建共享,但要求两台机器**同名同密码**的本地账号):
         -LaptopHost 26.253.1.139      →  \\26.253.1.139\c$\Users\Public\206dash-sync
       这条路还要先在笔记本上把 C:\Users\Public\206dash-sync 建出来(实测缺的就是它)。

  账号对不上时给 -Credential:
    -Credential (Get-Credential)  # 现场弹框问,脚本用 net use 建会话
  ★ 密码**只**在这一次运行里用,不落盘:脚本自己不存,计划任务也不存
    (所以账号不一致时,计划任务这条路要先在笔记本上把账号统一,原因见 README)。

  用法(所有命令都在本机跑,一般不需要管理员):
    # 0) 先自检:不碰网络,只验解析/退出码映射这些"错了会很坑"的逻辑
    powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -SelfTest

    # 1) 先探路:Radmin 通了吗 / 445 通吗 / 共享能写吗(打一张判决表 + 一句"怎么办")
    powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -Test
    powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -Test -ShareName 206dash-sync

    # 2) 真同步
    powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -ShareName 206dash-sync

    # 3) 挂计划任务(登录时 + 每 30 分钟),之后就不用管了
    powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -ShareName 206dash-sync -Register
    powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -Unregister   # 不想要了

  退出码:0 成功 / 2 参数不对 / 3 探路失败 / 4 robocopy 失败 / 5 自检失败 / 6 数据成功但快照失败
  (robocopy 自己的退出码是**位标志**,0/1/2/3 都算成功、≥8 才是失败 —— 脚本会翻译)

  ★ 两遍的顺序(2026-09-21 修 bug 后定死的,别再换回去):
    ① 攒暂存 → ② robocopy /MIR 把**数据/工具**先送到笔记本 → ③ 才打仓库快照 zip 并补传
  为什么:数据(抓包 csv、烧机 bin)是**不可再生**的,快照 zip 只是锦上添花。
  原来两样一起攒、一起传,而快照那步炸了(真事:PS 5.1 里找不到
  [System.IO.Compression.ZipArchiveMode] 类型),整个同步就中断在打包上,数据一个字节都没出去。
  现在快照失败只算**警告**,脚本最后用退出码 6 收场(计划任务日志看得出来)。详见 Invoke-Sync 上面的注释。

  注意:-MIR 会把目标目录里"桌机上没有"的文件**删掉**,这是故意的(要保持和桌机一致),
  所以笔记本那个同步文件夹是**脚本的地盘**,别往里放自己的东西。
  ★ 桌机这边的源文件**永远不会被删**,robocopy 只读它们。
#>
[CmdletBinding()]
param(
  # 笔记本在 Radmin 虚拟网里的地址。默认值 = owner 这台笔记本的实测地址;
  # ★ 换机器、或 Radmin 重新分配地址后,这里要改(或用 -LaptopHost 临时覆盖)
  [string]$LaptopHost = '26.253.1.139',
  # 命名共享名(推荐 206dash-sync,对应笔记本上的 C:\206dash-sync)。
  # 不给则退回管理共享 c$(还要求两台机器同名同密码的本地账号)
  [string]$ShareName = '',
  [System.Management.Automation.PSCredential]$Credential = $null,
  # 只走管理共享(c$)时,Public 下面那个文件夹名
  [string]$SyncRoot = '206dash-sync',
  # 只探路,不传文件
  [switch]$Test,
  # 离线自检:不碰网络
  [switch]$SelfTest,
  # 挂计划任务(登录时 + 每 N 分钟)
  [switch]$Register,
  # 删掉计划任务
  [switch]$Unregister,
  [string]$TaskName = '206dash-sync-to-laptop',
  [int]$EveryMinutes = 30,
  # 不同步"仓库快照 zip"(快照要现场打包,慢;想快就跑 -NoSnapshot)
  [switch]$NoSnapshot,
  # 仓库和 Public 数据目录(一般不用改)
  [string]$RepoRoot = '',
  [string]$PublicDir = 'C:\Users\Public\206dash',
  # 暂存目录:先在本机攒齐一份"要同步的样子",再一次性 /MIR 过去
  [string]$StageDir = 'C:\ProgramData\206dash-sync-stage',
  # 探路时等 ICMP 多久(毫秒)。故意短:没连 Radmin 时要马上给结论,不能挂着
  [int]$PingTimeoutMs = 1200,
  # 探路时等 TCP 445 多久(毫秒)。同上,故意的短
  [int]$TcpTimeoutMs = 1500,
  # ★ 内部开关:正常同步、计划任务**永远不传**它。给 -SelfTest 起一个"干净的"
  #   powershell.exe -NoProfile 子进程用,在里面把"攒暂存 → 两遍 robocopy → 打快照 zip"
  #   整条路拿本地临时目录跑一遍。为什么非要子进程,见下面 -ZipProbe 那一段。
  [switch]$ZipProbe,
  # -ZipProbe 的场地(都在 ProgramData 下,纯 ASCII 路径)
  [string]$ZipProbeDir = 'C:\ProgramData\206dash-selftest',
  # -ZipProbe 拿它当"笔记本上那个共享"(本地目录;探针全程不碰网络)
  [string]$ZipProbeDest = 'C:\ProgramData\206dash-selftest-dest',
  # -ZipProbe 的剧本:ok = 全都该成功;fail-snapshot = 故意让打 zip 失败,
  # 用来验证"快照炸了,数据也已经先落地、退出码 6"这条顺序
  [ValidateSet('ok', 'fail-snapshot')]
  [string]$ZipProbeMode = 'ok'
)

$ErrorActionPreference = 'Stop'
# 中文输出必须显式设成 UTF-8,否则在 GBK 控制台里标题变乱码(capture-van-nopy.ps1 同款处理)
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# 退出码(集中放,方便对着上面文件头的表看)
$EX_OK   = 0
$EX_ARG  = 2
$EX_TEST = 3
$EX_COPY = 4
$EX_SELF = 5
# 数据/工具都传上去了,只有仓库快照 zip 没打成。故意不是 0:计划任务的[上次结果]、
# 以及任何看日志的人,都该看得出"这次不完美"。
$EX_SNAP = 6

function Write-Step([string]$msg) { Write-Host $msg -ForegroundColor Cyan }
function Write-Ok([string]$msg)   { Write-Host $msg -ForegroundColor Green }
function Write-Warn2([string]$msg) { Write-Host $msg -ForegroundColor Yellow }
function Write-Err2([string]$msg)  { Write-Host $msg -ForegroundColor Red }

Write-Step '=== 206dash 桌机 → 笔记本 同步 ==='

# ---------------------------------------------------------------------------
# robocopy 退出码 → 人话
# robocopy 用**位标志**表示结果:0x01 有复制、0x02 有额外文件、0x04 有不匹配、
# 0x08 有失败、0x10 严重错误。所以 1 和 3 是**成功**,而 8 才是真失败 ——
# 第一次看这个映射的人几乎都会把 1 当成出错,把 8 当成"复制了 8 个"。
# ---------------------------------------------------------------------------
function Convert-RobocopyCode([int]$Code) {
  if ($Code -eq 0)  { return @{ Ok = $true;  Level = 'ok';   Text = '没有变化:目标已经和源一致(0 = 无事可做)' } }
  if ($Code -eq 1)  { return @{ Ok = $true;  Level = 'ok';   Text = '成功:有文件被复制(1 = 复制成功,不是错误)' } }
  if ($Code -eq 2)  { return @{ Ok = $true;  Level = 'ok';   Text = '成功:目标里有多余的文件/目录,已被清掉(2 = 有额外项)' } }
  if ($Code -eq 3)  { return @{ Ok = $true;  Level = 'ok';   Text = '成功:复制了文件 + 清了多余项(3 = 1|2,最常见)' } }
  if ($Code -ge 16) { return @{ Ok = $false; Level = 'bad';  Text = "失败:robocopy 用法/参数错误或没有权限($Code;含 0x10 位=严重错误)" } }
  if ($Code -ge 8)  { return @{ Ok = $false; Level = 'bad';  Text = "失败:有文件没能复制($Code;含 0x08 位=有失败项,去看上面的错误行)" } }
  $and = 15
  if (($Code -band $and) -eq 4) { return @{ Ok = $true; Level = 'warn'; Text = "成功但有异常:有文件不匹配(改天再看也行;$Code=0x04)" } }
  return @{ Ok = $false; Level = 'bad'; Text = "未知退出码 $Code" }
}

# ---------------------------------------------------------------------------
# 从 robocopy 日志里抓最后那张汇总表(= "复制了几个文件、多少字节")
# 为什么要解析:owner 要的是"这次传了多少",而 /NJS 一把汇总表关掉就再也拿不到了;
# 所以宁可用 /NFL /NDL 把逐行文件名压掉,也要留下 Total 那几行。
# 表头固定是英文 Total Copied Skipped Mismatch FAILED Extras,与系统语言无关。
# ---------------------------------------------------------------------------
function Parse-RobocopySummary([string]$Text) {
  $r = @{ Dirs = @(0, 0, 0, 0, 0, 0); Files = @(0, 0, 0, 0, 0, 0); Bytes = @(0, 0, 0, 0, 0, 0); Found = $false }
  if ([string]::IsNullOrEmpty($Text)) { return $r }
  foreach ($row in @('Dirs', 'Files', 'Bytes')) {
    $m = [regex]::Match($Text, '(?m)^\s*' + $row + '\s*:\s*([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)')
    if ($m.Success) {
      $vals = @()
      for ($i = 1; $i -le 6; $i++) { $vals += [int64](($m.Groups[$i].Value) -replace '[^\d]', '') }
      $r[$row] = $vals
      $r.Found = $true
    }
  }
  return $r
}

function Format-Bytes([int64]$n) {
  if ($n -ge 1073741824) { return ('{0:N2} GB' -f ($n / 1073741824)) }
  if ($n -ge 1048576)    { return ('{0:N1} MB' -f ($n / 1048576)) }
  if ($n -ge 1024)       { return ('{0:N1} KB' -f ($n / 1024)) }
  return "$n B"
}

# 把文件塞进暂存目录。
# 为什么要有暂存这一步:robocopy /MIR 不接受"只挑几个文件"的清单
# (/IF 只吃一个模式,/IF a b 直接报 Invalid Parameter),而分几次 /MIR 打到同一个
# 目标目录时,后一次会把前一次刚放上去的文件当成"多余项"删掉。
# 所以先在本机按"最终该长什么样"攒好一份,再用**一条** /MIR 打过去:
# 只有一条命令 → 只有一个退出码 → 只有一张汇总表,好懂也好排错。
function Copy-Staged([string]$Src, [string]$DstFile) {
  if (-not (Test-Path -LiteralPath $Src -PathType Leaf)) { throw "源文件不在:$Src" }
  $dir = Split-Path -Parent $DstFile
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  Copy-Item -LiteralPath $Src -Destination $DstFile -Force
  (Get-Item -LiteralPath $DstFile).Length
}

# ---------------------------------------------------------------------------
# zip 支持:PS 5.1 的类型表**不是**天生就认识 System.IO.Compression.* 的,
# 而且这几个名字分在两个程序集里:
#   System.IO.Compression.dll        → ZipArchive / ZipArchiveMode / CompressionLevel
#   System.IO.Compression.FileSystem → ZipFile / ZipFileExtensions
# 实测(2026-09-21 排这个 bug,本机干净 powershell.exe -NoProfile 里跑的):
#   只 Add-Type FileSystem 时 → FileSystem loaded=True、Compression loaded=False,
#     [System.IO.Compression.ZipFile]        → 能解析
#     [System.IO.Compression.ZipArchiveMode] → Unable to find type   ← 真同步就断在这儿
# 原因:zip 那行是 `ZipFile::Open($path, [ZipArchiveMode]::Create)`,参数先求值。
# 所以**两个都显式装上**;装不上就抛一句人话,由调用方降级成"警告 + 退出码 6",
# 而不是让整个同步中断(打 zip 是可选项,搬数据才是正事)。
# ---------------------------------------------------------------------------
function Initialize-ZipSupport {
  foreach ($asm in @('System.IO.Compression', 'System.IO.Compression.FileSystem')) {
    try { [void](Add-Type -AssemblyName $asm -ErrorAction Stop) }
    catch {
      throw "装不上程序集 $asm(打快照 zip 要用它;Windows PowerShell 5.1 自带这两个,不该缺):$($_.Exception.Message)"
    }
  }
}

# 光看"这个会话里程序集在不在"没有意义 —— 自检之所以漏掉这个 bug,就是因为老会话里
# 早就被别的脚本加载过。这个函数只用来**把事实打出来**(自检里对照"本会话 vs 干净子进程")。
function Test-AssemblyLoaded([string]$name) {
  foreach ($a in [System.AppDomain]::CurrentDomain.GetAssemblies()) {
    if ($a.GetName().Name -eq $name) { return $true }
  }
  return $false
}

# ---------------------------------------------------------------------------
# 仓库快照 zip:把源码(不含 .git/.pio)打成一个 zip。
# 为什么要有它:笔记本上可能没装 git/SSH,或者只是想让别人"解压就能看最新代码"。
# 为什么手工走目录栈、而不是 [IO.Compression.ZipFile]::CreateFromDirectory:
# 后者不支持排除目录,只能"全打包",对这一仓库来说等于把 186MB 的 .pio 也塞进去。
# ★ 这里继续用 .NET 类型、没改写成 Compress-Archive:Compress-Archive 不能按目录排除,
#   要用它就得先把整棵源码树复制一份到暂存目录(多一份 IO、多一处会失败的地方),
#   而这次的病因只是"少装了一个程序集"。现在两个都装、并且由 -ZipProbe 在干净子进程里
#   盯着这条代码路径,所以老路子更划算。
# ---------------------------------------------------------------------------
$SNAP_EXCLUDE = @('.git', '.pio', '.pio-core', '.tools', '__pycache__')
function Test-SnapshotExcluded([string]$Name) {
  return ($SNAP_EXCLUDE -contains $Name)
}

function New-RepoSnapshot([string]$Repo, [string]$ZipPath) {
  Initialize-ZipSupport   # ★ 必须在**用到任何 [System.IO.Compression.*] 类型之前**
  if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
  try {
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
      $count = 0
      $stack = New-Object System.Collections.Stack
      $stack.Push($Repo)
      while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        foreach ($e in [System.IO.Directory]::EnumerateDirectories($dir)) {
          $leaf = Split-Path -Leaf $e
          if (Test-SnapshotExcluded $leaf) { continue }   # 186MB 的 .pio 就是这么被挡在外面的
          $stack.Push($e)
        }
        foreach ($f in [System.IO.Directory]::EnumerateFiles($dir)) {
          $rel = $f.Substring($Repo.Length).TrimStart('\')
          [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f, $rel, [System.IO.Compression.CompressionLevel]::Optimal)
          $count++
        }
      }
    } finally {
      $zip.Dispose()   # 出错也得关:文件被占着的话,下面连删都删不掉
    }
    return @{ Count = $count; Bytes = (Get-Item -LiteralPath $ZipPath).Length }
  } catch {
    # 半截 zip 不能留:留着会被补传那遍当成"这次的快照"送过去,笔记本上就多个坏包
    if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue }
    throw "打仓库快照 zip 失败:$($_.Exception.Message)"
  }
}

# ---------------------------------------------------------------------------
# 参数/主机名体检
# ---------------------------------------------------------------------------
function Test-HostArg([string]$h) {
  if ([string]::IsNullOrWhiteSpace($h))        { return '没有给 -LaptopHost(笔记本在 Radmin 里的地址,形如 26.x.x.x)' }
  if ($h.StartsWith('\\'))                     { return "别把 UNC 路径整条塞进来:`"$h`" → 这里只要主机名/地址,-ShareName 才填共享名" }
  if ($h -match '[\\/\s]')                     { return "主机名里不该有空格或斜杠:`"$h`"" }
  return ''
}

function Get-UncPath([string]$host_, [string]$share, [string]$root) {
  if ([string]::IsNullOrWhiteSpace($share)) { return "\\$host_\c$\Users\Public\$root" }
  if ($share -match '[\\/\s]') { throw "共享名不该有空格或斜杠:`"$share`"" }
  return "\\$host_\$share"
}

# ---------------------------------------------------------------------------
# 探路:Radmin 通了吗 / SMB(445)通吗 / 共享能写吗
# 为什么这是最重要的一段:九成的"同步失败"根本不是脚本的错,而是 Radmin 没连上、
# 445 被防火墙拦了、或者共享权限/账号对不上。所以先给一张判决表,再决定能不能往下走。
# ---------------------------------------------------------------------------
function Test-HostAlive([string]$h, [int]$ms) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $ok = $false; $detail = ''
  try {
    $ping = New-Object System.Net.NetworkInformation.Ping
    $rep = $ping.Send($h, $ms)
    $ok = ($rep.Status -eq [System.Net.NetworkInformation.IPStatus]::Success)
    if ($ok) { $detail = "往返 $($rep.RoundtripTime) ms" } else { $detail = "状态 $($rep.Status)" }
  } catch {
    $detail = "Ping 抛错:$($_.Exception.Message)"
  }
  $sw.Stop()
  return @{ Ok = $ok; Detail = $detail; Ms = [int]$sw.ElapsedMilliseconds }
}

# 为什么不用 Test-NetConnection:它内部要走一堆 NetTCPIP 模块的活,慢(几秒起),
# 而这里只要一个"通/不通"。TcpClient.BeginConnect + WaitOne 是毫秒级、可控超时的写法。
function Test-TcpPort([string]$h, [int]$port, [int]$ms) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $client = New-Object System.Net.Sockets.TcpClient
  $ok = $false; $detail = ''
  try {
    $iar = $client.BeginConnect($h, $port, $null, $null)
    if ($iar.AsyncWaitHandle.WaitOne($ms, $false)) {
      try { $client.EndConnect($iar); $ok = $true; $detail = "TCP $port 握手成功" }
      catch { $detail = "TCP $port 被拒/超时:$($_.Exception.Message)" }
    } else {
      $detail = "TCP $port 超时(>${ms}ms):多半是防火墙或服务没开,不是脚本的问题"
    }
  } catch {
    $detail = "TCP $port 探测抛错:$($_.Exception.Message)"
  } finally {
    try { $client.Close() } catch { }
  }
  $sw.Stop()
  return @{ Ok = $ok; Detail = $detail; Ms = [int]$sw.ElapsedMilliseconds }
}

function Test-WriteAccess([string]$d) {
  $probe = Join-Path $d ('.206dash-write-test-' + [guid]::NewGuid().ToString('N') + '.tmp')
  try {
    [System.IO.File]::WriteAllText($probe, 'x', (New-Object System.Text.UTF8Encoding($false)))
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
    return @{ Ok = $true; Detail = '能建也能删' }
  } catch {
    if (Test-Path -LiteralPath $probe) { Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue }
    return @{ Ok = $false; Detail = $_.Exception.Message }
  }
}

function Test-UncReachable([string]$unc) {
  try {
    # 只列一条就够了;目的不是读目录,而是让 Windows 真去连一次,好把
    # "拒绝访问(账号/权限)" 和 "找不到网络路径(没共享)" 分开报出来。
    [void](Get-ChildItem -LiteralPath $unc -Force -ErrorAction Stop | Select-Object -First 1)
    return @{ Ok = $true; Detail = '路径存在' }
  } catch {
    return @{ Ok = $false; Detail = $_.Exception.Message }
  }
}

# 失败时**按原因**给一句能照做的话。只丢一个 robocopy 退出码等于没说 ——
# 这三种原因的下一步动作完全不同。
function Get-FailureHint($state, [bool]$tcpOk, [bool]$reach, [string]$reachErr) {
  if (-not $tcpOk) {
    return 'SMB(TCP 445)不通 ⇒ ① 笔记本上 Radmin 连了吗(两台都要在线、在同一个网络)② 笔记本防火墙放行[文件和打印机共享] ③ 笔记本真开了共享吗(那边跑 net share 看一眼)'
  }
  if (-not $reach) {
    if ($reachErr -match '拒绝|denied|Unauthorized|1326|logon|凭据|credential') {
      return "UNC 被拒 ⇒ 账号/权限问题:① 笔记本上把这个共享给当前账号[读/写] ② 或让两台机器用同名同密码的本地账号 ③ 或本次加 -Credential (Get-Credential)。目标:$($state.UncPath)"
    }
    if ($reachErr -match 'does not exist|找不到路径|Cannot find path') {
      # 实测踩到过:这不是权限问题,是那个文件夹**还没建**。说清楚,别让人白折腾账号。
      return "445 通、权限也没拦,只是笔记本上那个文件夹**不存在** ⇒ 在笔记本上建好它(路径见上面的[共享可读]),再跑一次;走命名共享的话就建共享那个文件夹"
    }
    return '445 通但 UNC 打不开 ⇒ ① 共享名写对了吗(-ShareName)② 笔记本上那个文件夹/共享存在吗 ③ 权限给到当前账号了吗'
  }
  return '共享能读到但写不进去 ⇒ 笔记本上把该共享的权限从[只读]改成[读/写]'
}

function Test-Preflight($state) {
  Write-Step ''
  Write-Step '---- 探路:Radmin 通了吗 / SMB 通吗 / 共享能写吗 ----'
  $rows = @()
  $fail = 0
  $tcpOk = $false; $reach = $false; $reachErr = ''

  # ① ICMP:只是参考。对方防火墙经常吞 ICMP 但 SMB 照样能走,所以这一项不致命
  $lan = Test-HostAlive $state.Host $PingTimeoutMs
  if ($lan.Ok) { $rows += @{ Name = '虚拟局域网连通(ping)'; Ok = $true; Fatal = $false; Detail = $lan.Detail } }
  else { $rows += @{ Name = '虚拟局域网连通(ping)'; Ok = $false; Fatal = $false; Detail = "$($lan.Detail);ICMP 可能只是被防火墙吞了,看下一行才是准的" } }

  # ② TCP 445:这条才是 SMB 到底通不通的判据
  $tcp = Test-TcpPort $state.Host 445 $TcpTimeoutMs
  $tcpOk = $tcp.Ok
  if ($tcp.Ok) { $rows += @{ Name = 'SMB 端口 445'; Ok = $true; Fatal = $true; Detail = "$($tcp.Detail)($($tcp.Ms) ms)" } }
  else { $rows += @{ Name = 'SMB 端口 445'; Ok = $false; Fatal = $true; Detail = $tcp.Detail } }

  # ③ 共享读得到吗(用真去连一次的办法,好把"拒绝访问"和"没这个共享"分开)
  if ($tcpOk) {
    $u = Test-UncReachable $state.UncPath
    $reach = $u.Ok; $reachErr = $u.Detail
  } else {
    $reachErr = '445 不通,没测'
  }
  if ($reach) { $rows += @{ Name = '共享可读'; Ok = $true; Fatal = $true; Detail = "$($state.UncPath) 存在" } }
  else { $rows += @{ Name = '共享可读'; Ok = $false; Fatal = $true; Detail = "$($state.UncPath) → $reachErr" } }

  # ④ 写权限:只读共享是另一类常见坑(能看见、传不进去)
  if ($reach) {
    $w = Test-WriteAccess $state.UncPath
    if ($w.Ok) { $rows += @{ Name = '共享可写'; Ok = $true; Fatal = $true; Detail = $w.Detail } }
    else { $rows += @{ Name = '共享可写'; Ok = $false; Fatal = $true; Detail = $w.Detail } }
  } else {
    $rows += @{ Name = '共享可写'; Ok = $false; Fatal = $true; Detail = '上一步没过,没测' }
  }

  $rcmd = Get-Command robocopy -ErrorAction SilentlyContinue
  if ($rcmd) { $rows += @{ Name = 'robocopy 可用'; Ok = $true; Fatal = $true; Detail = $rcmd.Source } }
  else { $rows += @{ Name = 'robocopy 可用'; Ok = $false; Fatal = $true; Detail = '找不到 robocopy.exe(竟然?)' } }

  foreach ($r in $rows) {
    if ($r.Ok) { $mark = '[ OK ]'; $color = 'Green' }
    elseif ($r.Fatal) { $mark = '[FAIL]'; $color = 'Red'; $fail++ }
    else { $mark = '[WARN]'; $color = 'Yellow' }
    Write-Host ("$mark {0,-30} {1}" -f $r.Name, $r.Detail) -ForegroundColor $color
  }

  Write-Host ''
  if ($fail -eq 0) {
    Write-Ok '判决:Radmin 通了、445 通了、共享能写 —— 可以传了'
    return @{ Ok = $true; Fail = 0; TcpOk = $tcpOk }
  }
  Write-Err2 "判决:探路没过($fail 项致命)"
  Write-Warn2 (Get-FailureHint $state $tcpOk $reach $reachErr)
  Write-Warn2 '想看得更细:在资源管理器地址栏粘上面的 UNC 路径,Windows 弹什么框就是什么错'
  return @{ Ok = $false; Fail = $fail; TcpOk = $tcpOk }
}

# ---------------------------------------------------------------------------
# net use 会话:-Credential 时才需要。
# 为什么用它:两台机器账号不一致时,Windows 不会自己认;robocopy 又没法带密码。
# 密码只在这条命令里出现一次,会话用完就 DELETE,绝不落盘(文件里也绝不写密码)。
# ---------------------------------------------------------------------------
function Connect-Lan($state) {
  if ($null -eq $Credential) { return $false }
  $pw = $Credential.GetNetworkCredential().Password
  Write-Step "用 -Credential 的账号建立会话:$($state.ShareBase) (用户 $($Credential.UserName))"
  $out = & net use $state.ShareBase $pw /user:$($Credential.UserName) 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Err2 "建立会话失败:$out"
    Write-Warn2 '检查:用户名是不是[笔记本名\用户名]的形式?共享名对吗?这个账号在笔记本上有读/写权限吗?'
    return $false
  }
  $state.SessionBase = $state.ShareBase
  return $true
}

function Disconnect-Lan($state) {
  if (-not $state.SessionBase) { return }
  [void](& net use $state.SessionBase /delete /y 2>&1)
  $state.SessionBase = ''
  Write-Host '会话已断开(net use /delete)'
}

# ---------------------------------------------------------------------------
# 跑一遍 robocopy,把人话小结打出来。
# 为什么拆成函数:现在有**两遍**(先数据、后快照),两遍的日志解析和口径必须一模一样,
# 不然"这次到底传了多少"会出现两个说法。
# ---------------------------------------------------------------------------
function Invoke-RobocopyPass([string]$Src, [string]$Dst, [string[]]$Extra, [string]$LogPath) {
  # ★ 别把 '/LOG+:' + $LogPath 写在 @(...) 里面:数组字面量里逗号比 + 更紧,
  #   会被拆成 '/LOG+:' 和日志路径**两个**参数 ⇒ robocopy 当用法错误,直接退出码 16。
  #   (这个坑是 2026-09-21 修 zip 那个 bug 时,被新加的自检子进程抓出来的:
  #    原来就是这种写法,只是同步当时早就先断在打 zip 上了,没人走到这一步。)
  $rcArgs = @($Src, $Dst) + $Extra + @('/R:1', '/W:1', '/BYTES', '/NP', '/NDL', '/NFL', '/NS', '/NC')
  $rcArgs += ('/LOG+:' + $LogPath)
  Write-Host "  robocopy $Src → $Dst $($Extra -join ' ')"
  $t0 = Get-Date
  # robocopy 的退出码是位标志,不是异常;所以只看 $LASTEXITCODE
  [void](& robocopy @rcArgs 2>&1)
  $code = $LASTEXITCODE
  $logText = ''
  if (Test-Path -LiteralPath $LogPath) { $logText = [System.IO.File]::ReadAllText($LogPath, [System.Text.Encoding]::UTF8) }
  return @{
    Code    = $code
    Verdict = (Convert-RobocopyCode $code)
    Summary = (Parse-RobocopySummary $logText)
    Log     = $LogPath
    LogText = $logText
    Elapsed = ((Get-Date) - $t0).TotalSeconds
  }
}

function Write-RobocopyVerdict($pass, [string]$Dst) {
  if ($pass.Verdict.Ok) { Write-Host ("robocopy 退出码 {0} —— {1}" -f $pass.Code, $pass.Verdict.Text) -ForegroundColor Green }
  else { Write-Host ("robocopy 退出码 {0} —— {1}" -f $pass.Code, $pass.Verdict.Text) -ForegroundColor Red }
  if ($pass.Summary.Found) {
    # 表头对照:Total Copied Skipped Mismatch FAILED Extras
    Write-Host ('复制 {0} 个文件 / {1}  ·  跳过(两边一样){2} 个 / {3}  ·  不匹配 {4}  ·  失败 {5}  ·  清掉多余 {6}' -f `
      $pass.Summary.Files[1], (Format-Bytes $pass.Summary.Bytes[1]), $pass.Summary.Files[2], (Format-Bytes $pass.Summary.Bytes[2]), $pass.Summary.Files[3], $pass.Summary.Files[4], $pass.Summary.Files[5])
    Write-Host ('目录:共 {0} / 新建 {1} / 跳过 {2}' -f $pass.Summary.Dirs[0], $pass.Summary.Dirs[1], $pass.Summary.Dirs[2])
  } else {
    Write-Warn2 '没解析到汇总表,下面是 robocopy 原样输出:'
    Write-Host $pass.LogText
  }
  Write-Host ("耗时 {0:N1} 秒" -f $pass.Elapsed)
  Write-Host "目标:$Dst"
}

# ---------------------------------------------------------------------------
# 这次同步该用哪个退出码(集中一处,好让 -ZipProbe 也断言同一套规则)
#   0 = 数据和快照都成功
#   4 = robocopy 失败(数据没传全,最严重)
#   6 = 数据传上去了,只有仓库快照 zip 失败(不完美,但笔记本上的数据是新的)
# ---------------------------------------------------------------------------
function Get-SyncExitCode($sync) {
  if ($null -eq $sync) { return $EX_COPY }
  if (-not $sync.Ok) { return $EX_COPY }
  if ($sync.SnapshotFailed) { return $EX_SNAP }
  return $EX_OK
}

# ---------------------------------------------------------------------------
# 同步:① 攒暂存 → ② 一条 robocopy /MIR 把**真家当**送过去 → ③ 才补仓库快照 zip
#
# ★ 这个顺序是 2026-09-21 修 bug 时特意定下来的,别再换回去:
#   数据(抓包 csv、烧机 bin、两个现成工具)是**不可再生**的,快照 zip 只是"锦上添花"
#   (给没装 git 的机器解压看代码用)。原来两样东西一起攒、一起传,而快照那步炸了
#   (真事:PS 5.1 里 [System.IO.Compression.ZipArchiveMode] 找不到类型),结果是整个
#   同步中断在打包上 —— 数据一个字节都没出去。现在:数据先走完,快照失败只打**警告**
#   并继续,最后用退出码 6 让计划任务日志看得出"这次不完美"。顺序换回去 = bug 重演。
#
# 为什么快照是"第二遍 robocopy"、而不是塞进第一遍的暂存清单:
#   /MIR 会把目标里"暂存没有的"东西删掉。快照要是没打成、而名字又在第一遍的清单里,
#   第一遍就会把笔记本上**上一份**快照删掉、又补不上新的;反过来,第二遍要是也用 /MIR,
#   它会把第一遍刚传过去的 data/ 和 tools/ 当成"多余项"清掉(脚本头上讲过这个坑)。
#   所以第二遍故意用 /E /IS:只复制、不清目标,而且每次都覆盖成现场打的那一份。
# ---------------------------------------------------------------------------
function Invoke-Sync($state) {
  $pub = $PublicDir
  $done = @()
  $missing = @()

  # 先清暂存:上一次的残留会让 /MIR 把"这次不打算传的文件"当成该有的东西留在目标里
  if (Test-Path -LiteralPath $StageDir) { Remove-Item -LiteralPath $StageDir -Recurse -Force }
  $out = Join-Path $StageDir 'out'
  New-Item -ItemType Directory -Force -Path $out | Out-Null

  # ① 大块头 + 数据:抓到的东西(真正的家当)
  $dataFiles = @('van_capture_dm.csv', 'drive5min.csv', 'image-v3.bin', 'theme-user.json')
  foreach ($f in $dataFiles) {
    $src = Join-Path $pub $f
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { $missing += $src; continue }
    $n = Copy-Staged $src (Join-Path $out "data\$f")
    $done += @{ Rel = "data/$f"; Bytes = $n }
  }

  # ② 拿到就能用的小工具(笔记本上直接 -File 指过去即可)
  $toolMap = @(
    @{ Src = (Join-Path $RepoRoot 'tools\serial-capture\capture-van-nopy.ps1'); Rel = 'tools\capture-van-nopy.ps1' },
    @{ Src = (Join-Path $RepoRoot 'tools\obd-log\obd-log.ps1');                        Rel = 'tools\obd-log.ps1' }
  )
  foreach ($t in $toolMap) {
    if (-not (Test-Path -LiteralPath $t.Src -PathType Leaf)) { $missing += $t.Src; continue }
    $n = Copy-Staged $t.Src (Join-Path $out $t.Rel)
    $done += @{ Rel = ($t.Rel -replace '\\', '/'); Bytes = $n }
  }

  # ③ 现成的传输包(有就带,没有不报错)—— 注意:仓库快照 zip **不在这里**,
  #    它在下面第 2 遍才打(顺序的原因见本函数头上的注释)
  $zip = Join-Path $pub '206dash-transfer.zip'
  if (Test-Path -LiteralPath $zip -PathType Leaf) {
    $n = Copy-Staged $zip (Join-Path $out '206dash-transfer.zip')
    $done += @{ Rel = '206dash-transfer.zip'; Bytes = $n }
  }

  if ($missing.Count -gt 0) {
    Write-Warn2 '这些源文件不在,跳过(不是错误,只是没有):'
    foreach ($m in $missing) { Write-Warn2 "  - $m" }
  }
  if ($done.Count -eq 0) { throw '一个可同步的源文件都没有,收工' }

  $total = 0
  foreach ($d in $done) { $total += $d.Bytes }
  Write-Step ''
  Write-Step "暂存清单($($done.Count) 项,$(Format-Bytes $total)):"
  foreach ($d in $done) { Write-Host ('  {0,-40} {1,12}' -f $d.Rel, (Format-Bytes $d.Bytes)) }

  # 日志落在暂存目录、而不是 %TEMP%:%TEMP% 是 C:\Users\<中文名>\AppData\...(本来也能用,
  # 但少一个中文路径就少一类花样问题;暂存目录在 ProgramData 下,是纯 ASCII)
  $log = Join-Path $StageDir 'robocopy.log'
  Write-Step ''
  Write-Step '---- 第 1 遍:数据 + 工具(robocopy /MIR;目标会被镜像成暂存的样子)----'
  Write-Step "开始传输:robocopy $out $($state.UncPath) /MIR /R:1 /W:1"
  $pass1 = Invoke-RobocopyPass -Src $out -Dst $state.UncPath -Extra @('/MIR') -LogPath $log

  Write-Step ''
  Write-Step '==== 同步小结 ===='
  Write-RobocopyVerdict $pass1 $state.UncPath
  Write-Host "清单里的文件都先攒在本机:$out(想核对就进去看)"
  if (-not $pass1.Verdict.Ok) {
    Write-Warn2 (Get-FailureHint $state $true $true '')
    Write-Warn2 "robocopy 完整日志:$($pass1.Log)"
  }

  # ④ 到这一步**数据已经在笔记本上了**,才开始打快照。
  #    下面整段无论出什么事都只降级成"警告":快照是可选项,不能反过来把数据卡住。
  $snapOk  = $true
  $snapErr = ''
  if ($NoSnapshot) {
    Write-Step ''
    Write-Step '-NoSnapshot:跳过仓库快照 zip(笔记本上那份旧的会被第 1 遍的 /MIR 清掉)'
  } else {
    Write-Step ''
    Write-Step '---- 第 2 遍:仓库快照 zip(/E /IS:只补这一个文件,不清目标)----'
    try {
      $snapStage = Join-Path $StageDir 'snap'
      New-Item -ItemType Directory -Force -Path $snapStage | Out-Null
      $snapZip = Join-Path $snapStage 'repo-snapshot.zip'
      Write-Step '打仓库快照 zip(源码,不含 .git/.pio)……'
      $sw = [System.Diagnostics.Stopwatch]::StartNew()
      $snap = New-RepoSnapshot $RepoRoot $snapZip
      $sw.Stop()
      Write-Host ("  快照:{0} 个文件,{1},{2:N1} 秒(不含 .git/.pio/.pio-core/.tools/__pycache__)" -f $snap.Count, (Format-Bytes $snap.Bytes), $sw.Elapsed.TotalSeconds)
      $pass2 = Invoke-RobocopyPass -Src $snapStage -Dst $state.UncPath -Extra @('/E', '/IS') -LogPath (Join-Path $StageDir 'robocopy-snapshot.log')
      Write-RobocopyVerdict $pass2 $state.UncPath
      if (-not $pass2.Verdict.Ok) {
        $snapOk  = $false
        $snapErr = "补传快照失败(robocopy $($pass2.Code):$($pass2.Verdict.Text));日志:$($pass2.Log)"
      }
    } catch {
      $snapOk  = $false
      $snapErr = $_.Exception.Message
    }
    if (-not $snapOk) {
      # 只是警告:数据已经在笔记本上了,这里绝不能把整次同步判成失败(退出码见 Get-SyncExitCode)
      Write-Warn2 "仓库快照 zip 没成功(只是警告 —— 数据/工具已经传上去了):$snapErr"
      Write-Warn2 '  快照是给"没装 git 的机器"看的,可有可无;数据才是正事。'
      Write-Warn2 '  想再试一次:再跑一遍真同步;只想要数据、不想等打包:加 -NoSnapshot。'
      Write-Warn2 "  退出码会是 $EX_SNAP(不是 0),这样计划任务的[上次结果]看得出来这次不完美。"
    }
  }

  return @{
    Ok             = $pass1.Verdict.Ok
    Code           = $pass1.Code
    Summary        = $pass1.Summary
    Elapsed        = $pass1.Elapsed
    SnapshotOk     = $snapOk
    SnapshotFailed = (-not $snapOk)
    SnapshotError  = $snapErr
  }
}

# ---------------------------------------------------------------------------
# 计划任务:登录时跑一次 + 每 N 分钟跑一次
# 为什么要计划任务而不是"守着窗口":同步的价值全在"不用想它"。
# 账号不一致(-Credential)时**不挂**任务:任务不能存密码,存了就等于把密码写进系统,
# 正确做法是先把两台机器的账号统一(见 README),再用这个开关。
# ---------------------------------------------------------------------------
function Register-SyncTask($state) {
  $self = $PSCommandPath
  if ([string]::IsNullOrWhiteSpace($self)) { $self = $MyInvocation.MyCommand.Path }
  if ([string]::IsNullOrWhiteSpace($self)) { throw '拿不到脚本自身路径,没法挂计划任务' }
  if ($null -ne $Credential) {
    throw '带 -Credential 时不该挂计划任务:任务没法安全地存密码。先把两台机器的用户名密码统一(见 README),再 -Register'
  }
  $callArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ($self -replace '"', '""'), '-LaptopHost', $LaptopHost)
  if ($ShareName -ne '') { $callArgs += @('-ShareName', $ShareName) }
  if ($SyncRoot -ne '206dash-sync') { $callArgs += @('-SyncRoot', $SyncRoot) }
  if ($NoSnapshot) { $callArgs += '-NoSnapshot' }
  $argStr = ($callArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '

  $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $argStr
  $t1 = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
  $t2 = New-ScheduledTaskTrigger -Once -At (Get-Date).Date.AddMinutes(1) `
          -RepetitionInterval (New-TimeSpan -Minutes $EveryMinutes)
  $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -MultipleInstances IgnoreNew `
                -ExecutionTimeLimit (New-TimeSpan -Hours 2) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
  $principal = New-ScheduledTaskPrincipal -UserId ($env:USERDOMAIN + '\' + $env:USERNAME) -LogonType Interactive -RunLevel Limited

  try {
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger @($t1, $t2) `
      -Settings $settings -Principal $principal -Force `
      -Description '206dash:桌机 → 笔记本 单向同步(tools/sync/sync-to-laptop.ps1)' | Out-Null
  } catch {
    Write-Err2 "挂计划任务失败:$($_.Exception.Message)"
    Write-Warn2 '提示:这条要写系统任务库。当前用户挂自己的任务通常不用管理员;被策略拦了就开一个管理员 PowerShell 再跑这条命令。'
    return 1
  }
  Write-Ok "计划任务已挂:$TaskName"
  Write-Host "  触发:登录时 + 每 $EveryMinutes 分钟(两者共用同一个任务)"
  Write-Host "  运行:powershell.exe $argStr"
  Write-Host "  看状态:Get-ScheduledTask -TaskName $TaskName | Get-ScheduledTaskInfo"
  Write-Host "  立刻跑一次:Start-ScheduledTask -TaskName $TaskName"
  Write-Host '  不想要了:powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -Unregister'
  return 0
}

function Unregister-SyncTask() {
  $t = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
  if (-not $t) { Write-Warn2 "计划任务不存在(已经是干净的了):$TaskName"; return 0 }
  try {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Ok "计划任务已删除:$TaskName"
    Write-Host '  (这台机器上已经没有自动同步了;文件都还在,想手工跑就不加 -Register)'
    return 0
  } catch {
    Write-Err2 "删计划任务失败:$($_.Exception.Message)"
    return 1
  }
}

# ===========================================================================
# -ZipProbe:内部开关,给 -SelfTest 起一个**干净的 Windows PowerShell 5.1 子进程**用。
#
# ★ 为什么非得是新进程:这个脚本真栽过一回 —— 真同步断在"打仓库快照 zip",
#   报 Unable to find type [System.IO.Compression.ZipArchiveMode];而当时的 -SelfTest
#   是**绿的**,因为自检跑在"早就被别的脚本加载过 System.IO.Compression*"的会话里,
#   类型找得到。用户真同步多半是计划任务里 powershell.exe 新起的进程 —— 只有
#   `powershell.exe -NoProfile` 子进程才是"第一次跑"的样子。所以自检必须自己再起一个。
# ★ 探针全程**不碰网络**:把目标目录直接给 Invoke-Sync 一个本地文件夹
#   (Invoke-Sync 只认 $state.UncPath,给它本地路径它照样干活),
#   于是"攒暂存 → 第 1 遍 robocopy /MIR → 打 zip → 第 2 遍 /E /IS"整条路都能离线跑。
# 两种剧本(给 -SelfTest 用):
#   ok             数据和快照都该成功(退出码 0),目标里该有 data/、tools/、repo-snapshot.zip
#   fail-snapshot  故意把一个源文件独占锁住,让打 zip 必炸 —— 验证顺序:
#                  快照炸了,数据/工具照样落地,退出码 6,目标里不留半截 zip
# 输出约定:每个事实一行 "PROBE xxx=yyy",全部符合预期才打 "PROBE-OK" 并 exit 0。
# ===========================================================================
if ($ZipProbe) {
  $probeFail = ''
  $probeLock = $null
  try {
    if (Test-Path -LiteralPath $ZipProbeDir) { Remove-Item -LiteralPath $ZipProbeDir -Recurse -Force }
    if (Test-Path -LiteralPath $ZipProbeDest) { Remove-Item -LiteralPath $ZipProbeDest -Recurse -Force }
    $fixPub   = Join-Path $ZipProbeDir 'public'   # 假装 C:\Users\Public\206dash
    $fixRepo  = Join-Path $ZipProbeDir 'repo'     # 假装仓库根
    $fixStage = Join-Path $ZipProbeDir 'stage'    # 假装 C:\ProgramData\206dash-sync-stage
    foreach ($d in @(
        $fixPub, $fixRepo, $fixStage, $ZipProbeDest,
        (Join-Path $fixRepo '.git'), (Join-Path $fixRepo '.pio'),
        (Join-Path $fixRepo 'src\deep\__pycache__'),
        (Join-Path $fixRepo 'tools\serial-capture'), (Join-Path $fixRepo 'tools\obd-log')
      )) { New-Item -ItemType Directory -Force -Path $d | Out-Null }

    $enc = New-Object System.Text.UTF8Encoding($false)
    # 假装的那几样"家当"(内容随便,探针只验"搬过去了")
    [System.IO.File]::WriteAllText((Join-Path $fixPub 'van_capture_dm.csv'), "t,id,data`n0,1,00FF`n", $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixPub 'drive5min.csv'), "t,id,data`n0,1,11FF`n", $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixPub 'image-v3.bin'), 'BIN-PROBE', $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixPub 'theme-user.json'), '{"probe":true}', $enc)
    # 假装的仓库:该进快照的进,该被排除的也造出来
    [System.IO.File]::WriteAllText((Join-Path $fixRepo 'platformio.ini'), "[env:probe]`n", $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixRepo 'src\main.cpp'), "int main(){return 0;}`n", $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixRepo 'src\deep\b.txt'), "b`n", $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixRepo 'src\deep\__pycache__\x.pyc'), 'pyc', $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixRepo '.git\config'), 'git', $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixRepo '.pio\build.bin'), 'pio', $enc)
    # 两个工具:Invoke-Sync 按仓库里的固定路径找它们
    [System.IO.File]::WriteAllText((Join-Path $fixRepo 'tools\serial-capture\capture-van-nopy.ps1'), "# probe tool`n", $enc)
    [System.IO.File]::WriteAllText((Join-Path $fixRepo 'tools\obd-log\obd-log.ps1'), "# probe tool`n", $enc)

    if ($ZipProbeMode -eq 'fail-snapshot') {
      # 独占锁住快照树里的一个文件:CreateEntryFromFile 打不开它 ⇒ 打 zip 必失败。
      # 这就是"快照炸了"的现场制造法(不用给正式代码加任何后门开关)。
      $probeLock = [System.IO.File]::Open((Join-Path $fixRepo 'src\deep\b.txt'),
        [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    }

    # ★ 关键:整条同步走的就是正式函数 Invoke-Sync / New-RepoSnapshot,
    #   只是把 源 / 暂存 / 目标 全换成上面这些临时目录(所以没网也能跑)。
    $PublicDir = $fixPub
    $RepoRoot  = $fixRepo
    $StageDir  = $fixStage
    $state = @{ Host = 'probe'; UncPath = $ZipProbeDest; ShareBase = ''; SessionBase = '' }

    Write-Host ("PROBE pid={0} ps={1}" -f $PID, $PSVersionTable.PSVersion.ToString())
    Write-Host ("PROBE assemblies-before Compression={0} FileSystem={1}" -f `
      (Test-AssemblyLoaded 'System.IO.Compression'), (Test-AssemblyLoaded 'System.IO.Compression.FileSystem'))

    $r = Invoke-Sync $state

    $dataOk = (Test-Path -LiteralPath (Join-Path $ZipProbeDest 'data\van_capture_dm.csv')) -and `
              (Test-Path -LiteralPath (Join-Path $ZipProbeDest 'data\drive5min.csv'))
    $toolOk = (Test-Path -LiteralPath (Join-Path $ZipProbeDest 'tools\capture-van-nopy.ps1')) -and `
              (Test-Path -LiteralPath (Join-Path $ZipProbeDest 'tools\obd-log.ps1'))
    $zipDest = Join-Path $ZipProbeDest 'repo-snapshot.zip'
    $zipOk   = Test-Path -LiteralPath $zipDest
    $raw = @(); $norm = @(); $entryOk = $false
    # 快照里该有的东西 = fixture 里没被排除的那些(5 个文件)
    $expect = @(
      'platformio.ini', 'src/main.cpp', 'src/deep/b.txt',
      'tools/serial-capture/capture-van-nopy.ps1', 'tools/obd-log/obd-log.ps1'
    )
    if ($zipOk) {
      # 在同一个干净进程里再把 zip 打开读一遍:证明它真是个能用的包,不是半截
      $z = [System.IO.Compression.ZipFile]::OpenRead($zipDest)
      try { $raw = @($z.Entries | ForEach-Object { $_.FullName }) } finally { $z.Dispose() }
      $norm = @($raw | ForEach-Object { $_ -replace '\\', '/' })
      $bad  = @($norm | Where-Object { $_ -match '(^|/)(\.git|\.pio|\.pio-core|\.tools|__pycache__)(/|$)' })
      $entryOk = ($bad.Count -eq 0) -and ($norm.Count -eq $expect.Count)
      if ($entryOk) { foreach ($x in $expect) { if ($norm -notcontains $x) { $entryOk = $false } } }
    }
    $zipLeftInStage = Test-Path -LiteralPath (Join-Path $fixStage 'snap\repo-snapshot.zip')

    Write-Host ("PROBE robocopy-code={0} verdict-ok={1}" -f $r.Code, $r.Ok)
    Write-Host ("PROBE data-copied={0} tools-copied={1}" -f $dataOk, $toolOk)
    Write-Host ("PROBE zip-in-dest={0} entries={1} expected={2}" -f $zipOk, $norm.Count, $expect.Count)
    Write-Host ("PROBE zip-entry-names={0}" -f (($raw | Sort-Object) -join ','))
    Write-Host ("PROBE zip-excludes-ok={0}" -f $entryOk)
    Write-Host ("PROBE zip-left-in-stage={0}(fail-snapshot 剧本里必须是 False:半截包不能留)" -f $zipLeftInStage)
    Write-Host ("PROBE snapshot-failed={0}" -f $r.SnapshotFailed)
    Write-Host ("PROBE snapshot-error={0}" -f $r.SnapshotError)
    Write-Host ("PROBE exit-code={0}" -f (Get-SyncExitCode $r))

    # ok 剧本再跑**第二遍**:同步是每 30 分钟跑一次的,第二遍也必须能把快照刷过去。
    # 这一遍目标里已经有上一份 zip 和数据了 —— 正好检验第 2 遍用的 /E /IS(/IS=连"两边一样"
    # 也复制,不然时钟偏差会让新快照传不过去),以及"暂存清空重建 + /MIR 重新镜像"不出岔子。
    $rerunOk = $true
    if ($ZipProbeMode -ne 'fail-snapshot') {
      $r2 = Invoke-Sync $state
      $zip2 = Test-Path -LiteralPath $zipDest
      $rerunOk = ($r2.Ok -and $zip2 -and (-not $r2.SnapshotFailed) -and ((Get-SyncExitCode $r2) -eq $EX_OK))
      Write-Host ("PROBE rerun robocopy-code={0} verdict-ok={1} zip-in-dest={2} snapshot-failed={3} exit-code={4}" -f `
        $r2.Code, $r2.Ok, $zip2, $r2.SnapshotFailed, (Get-SyncExitCode $r2))
    } else {
      Write-Host 'PROBE rerun=skipped(fail-snapshot 剧本不跑第二遍)'
    }

    if ($ZipProbeMode -eq 'fail-snapshot') {
      # 剧本 B 的期望:数据/工具**必须**先在目标里(这就是顺序),快照必须失败,退出码必须是 6
      if (-not ($dataOk -and $toolOk)) { $probeFail = '快照炸了的时候数据/工具没落地 —— 顺序错了(数据必须先传)' }
      elseif (-not $r.SnapshotFailed) { $probeFail = '故意锁住文件,快照居然算成功了?那这个探针没测到东西' }
      elseif ($zipOk)                  { $probeFail = '快照失败了,目标里却出现了 repo-snapshot.zip' }
      elseif ($zipLeftInStage)         { $probeFail = '暂存里留着半截 zip(失败时该删掉,别让它被传过去)' }
      elseif ((Get-SyncExitCode $r) -ne $EX_SNAP) { $probeFail = "快照失败时退出码该是 $EX_SNAP" }
    } else {
      if (-not $r.Ok)                      { $probeFail = "第 1 遍 robocopy 没过(退出码 $($r.Code))" }
      elseif (-not ($dataOk -and $toolOk)) { $probeFail = '数据/工具没到目标目录' }
      elseif (-not $r.SnapshotOk)          { $probeFail = "快照失败:$($r.SnapshotError)" }
      elseif (-not $zipOk)                 { $probeFail = '目标目录里没有 repo-snapshot.zip' }
      elseif (-not $entryOk)               { $probeFail = "zip 内容不对(条目:$($norm -join ','))" }
      elseif ((Get-SyncExitCode $r) -ne $EX_OK) { $probeFail = "全成功时退出码该是 $EX_OK" }
      elseif (-not $rerunOk)               { $probeFail = '第二遍同步(目标里已经有上一份快照)没跑通' }
    }
  } catch {
    $probeFail = "探针自己抛异常:$($_.Exception.Message)"
  } finally {
    if ($null -ne $probeLock) { try { $probeLock.Dispose() } catch { } }
  }

  if ($probeFail -ne '') { Write-Host "PROBE-FAIL $probeFail" -ForegroundColor Red; exit 1 }
  Write-Host 'PROBE-OK' -ForegroundColor Green
  exit 0
}

# ===========================================================================
# -SelfTest:不碰网络、不碰共享,只验那些"错了会很坑"的纯逻辑
# 为什么这些值得自检:退出码映射错 → 把失败当成功(数据没传却以为传了);
# 汇总表解析错 → 小结里的数字全是假的;快照排除错 → 有人把 186MB 的 .pio 传过去。
# ★ 自检里唯一"动手真干"的是第 ⑨ 项:它起两个干净的 powershell.exe 子进程,
#   把 zip/同步那条路真跑一遍 —— 因为"在本会话里验"根本挡不住这一类 bug(见上)。
# ===========================================================================
if ($SelfTest) {
  Write-Step ''
  Write-Step '==== -SelfTest(离线:不 ping 外网、不连共享、不传文件)===='
  $checks = @()
  # 注意:$want 可能是数组(@(12,4) 这种),而 PowerShell 的 -eq 遇到数组会做"数组比较"、
  # 永远为假 —— 所以是数组时只取第一个元素(自检里第一列就是要断言的总数)。
  function Add-Check([string]$name, $got, $want) {
    $w = $want
    if ($want -is [array]) { $w = $want[0] }
    $script:checks += @{ Name = $name; Got = $got; Want = $w; Ok = ($got -eq $w) }
  }

  # ① 退出码翻译
  Add-Check 'robocopy 0 = 成功(无事可做)'   (Convert-RobocopyCode 0).Ok  $true
  Add-Check 'robocopy 1 = 成功(有复制)'     (Convert-RobocopyCode 1).Ok  $true
  Add-Check 'robocopy 2 = 成功(清多余)'     (Convert-RobocopyCode 2).Ok  $true
  Add-Check 'robocopy 3 = 成功(1|2)'        (Convert-RobocopyCode 3).Ok  $true
  Add-Check 'robocopy 4 = 成功但有异常'     (Convert-RobocopyCode 4).Ok  $true
  Add-Check 'robocopy 8 = 失败'             (Convert-RobocopyCode 8).Ok  $false
  Add-Check 'robocopy 9 = 失败(1|8)'        (Convert-RobocopyCode 9).Ok  $false
  Add-Check 'robocopy 16 = 失败(严重)'      (Convert-RobocopyCode 16).Ok $false

  # ② 汇总表解析(照抄真机跑出来的样子,数字故意各不相同,串列就露馅)
  $fake = @'
               Total    Copied   Skipped  Mismatch    FAILED    Extras
    Dirs :         3         1         2         0         0         0
   Files :        12         4         7         1         0         2
   Bytes :  34000000  12345678  21654322         0         0       123
'@
  $p = Parse-RobocopySummary $fake
  Add-Check '解析:找到汇总表'        $p.Found     $true
  Add-Check '解析:Files 总数'        $p.Files[0]  12
  Add-Check '解析:Files 已复制'      $p.Files[1]  4
  Add-Check '解析:Files 已跳过'      $p.Files[2]  7
  Add-Check '解析:Files 不匹配'      $p.Files[3]  1
  Add-Check '解析:Files 失败'        $p.Files[4]  0
  Add-Check '解析:Files 多余'        $p.Files[5]  2
  Add-Check '解析:Bytes 总数'        $p.Bytes[0]  @([int64]34000000)
  Add-Check '解析:Bytes 已复制'      $p.Bytes[1]  @([int64]12345678)
  Add-Check '解析:Bytes 已跳过'      $p.Bytes[2]  @([int64]21654322)
  Add-Check '解析:空日志不炸'        (Parse-RobocopySummary '').Found $false

  # ③ 快照排除:这几个目录**绝不能**进快照(186MB、机器相关)
  foreach ($n in @('.git', '.pio', '.pio-core', '.tools', '__pycache__')) {
    Add-Check "快照排除 $n" (Test-SnapshotExcluded $n) $true
  }
  foreach ($n in @('src', 'lib', 'tools', 'include', 'test')) {
    Add-Check "快照保留 $n" (Test-SnapshotExcluded $n) $false
  }

  # ④ UNC 路径拼法:两种共享写法
  Add-Check '管理共享(不给 -ShareName)' (Get-UncPath '26.253.1.139' '' '206dash-sync') '\\26.253.1.139\c$\Users\Public\206dash-sync'
  Add-Check '命名共享(-ShareName)'       (Get-UncPath '26.253.1.139' '206dash-sync' '206dash-sync') '\\26.253.1.139\206dash-sync'

  # ⑤ 主机名体检:最常见的两种错填
  Add-Check '空主机名被拦'        ((Test-HostArg '').Length -gt 0) $true
  Add-Check '整条 UNC 被拦'       ((Test-HostArg '\\26.253.1.139\c$').Length -gt 0) $true
  Add-Check '带空格被拦'          ((Test-HostArg '26.253.1.139 x').Length -gt 0) $true
  Add-Check '正常 IP 放行'        (Test-HostArg '26.253.1.139') ''
  Add-Check '主机名放行'          (Test-HostArg 'laptop') ''
  Add-Check '默认主机名是实测那台' $LaptopHost '26.253.1.139'

  # ⑥ 字节格式化(小结里要给人看)
  Add-Check '512 B'   (Format-Bytes 512) '512 B'
  Add-Check '2.0 KB'  (Format-Bytes 2048) '2.0 KB'
  Add-Check '1.0 MB'  (Format-Bytes 1048576) '1.0 MB'

  # ⑦ 探路超时必须真的短:没连 Radmin 时不能挂在这儿
  Add-Check 'ping 超时 <= 2000ms' ($PingTimeoutMs -le 2000) $true
  Add-Check 'TCP 超时 <= 2000ms'  ($TcpTimeoutMs -le 2000) $true

  # ⑧ 唯一碰网络栈的地方:回环,不外发
  $loop = Test-HostAlive '127.0.0.1' $PingTimeoutMs
  Add-Check '回环 ping 通' $loop.Ok $true
  Add-Check '回环 ping 1 秒内返回' ($loop.Ms -lt 1000) $true
  $self445 = Test-TcpPort '127.0.0.1' 445 $TcpTimeoutMs
  Add-Check 'TCP 探测能给出结论(不挂死)' ($self445.Ms -lt 2000) $true
  Write-Host ("       (本机 445:Ok={0} {1};{2} ms —— 只作参考,不是断言本机开了共享)" -f $self445.Ok, $self445.Detail, $self445.Ms)

  # ⑨ ★ 这一项是 2026-09-21 补的,专门补上"自检漏掉 zip bug"那个洞:
  #    起两个**干净的** powershell.exe -NoProfile 子进程,在里面真跑一整遍
  #    "攒暂存 → 第 1 遍 robocopy /MIR → 打 zip → 第 2 遍 /E /IS"。
  #    为什么非得起进程:老会话里程序集早就被别的脚本加载过,类型一找一个准,
  #    所以"在本会话里验"永远复现不出用户真同步时那个 Unable to find type。
  #    两个剧本:ok(都成功)和 fail-snapshot(故意让 zip 炸,验"数据照样先落地 + 退出码 6")。
  $psExe = Join-Path $PSHOME 'powershell.exe'
  if (-not (Test-Path -LiteralPath $psExe)) {
    $cmd = Get-Command 'powershell.exe' -ErrorAction SilentlyContinue
    if ($cmd) { $psExe = $cmd.Source }
  }
  Add-Check '找得到 powershell.exe(要起干净子进程)' ((Test-Path -LiteralPath $psExe)) $true
  Write-Host ("       (本会话里:Compression={0} FileSystem={1};子进程里这两个必须是 False,否则不算干净)" -f `
    (Test-AssemblyLoaded 'System.IO.Compression'), (Test-AssemblyLoaded 'System.IO.Compression.FileSystem'))
  $probeDir  = 'C:\ProgramData\206dash-selftest'
  $probeDest = 'C:\ProgramData\206dash-selftest-dest'
  foreach ($mode in @('ok', 'fail-snapshot')) {
    $raw = ''; $pcode = -1
    if (Test-Path -LiteralPath $psExe) {
      $pr = & {
        $ErrorActionPreference = 'Continue'   # 子进程的 stderr 不该把自检自己打断
        $o = (& $psExe -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath `
                -ZipProbe -ZipProbeMode $mode -ZipProbeDir $probeDir -ZipProbeDest $probeDest 2>&1 | Out-String)
        [pscustomobject]@{ Out = $o; Code = $LASTEXITCODE }
      }
      $raw = $pr.Out; $pcode = $pr.Code
    }
    Add-Check "干净子进程($mode):退出码 0"       $pcode 0
    Add-Check "干净子进程($mode):打出 PROBE-OK" ($raw -match '(?m)^PROBE-OK\s*$') $true
    Write-Host "      --- 子进程原样输出:powershell.exe -NoProfile -File sync-to-laptop.ps1 -ZipProbe -ZipProbeMode $mode ---"
    foreach ($ln in ($raw -split "`r?`n")) {
      if ($ln.Trim() -ne '') { Write-Host ('      | ' + $ln.TrimEnd()) }
    }
    Write-Host "      (场地:$probeDir;假装成笔记本那个共享的本地目录:$probeDest)"
  }

  $bad = 0
  foreach ($c in $checks) {
    if (-not $c.Ok) { $bad++ }
    $mark = '[ OK ]'; $color = 'Green'
    if (-not $c.Ok) { $mark = '[FAIL]'; $color = 'Red' }
    Write-Host ("$mark {0,-34} got={1} want={2}" -f $c.Name, $c.Got, $c.Want) -ForegroundColor $color
  }
  Write-Host ''
  if ($bad -gt 0) {
    Write-Err2 "$bad / $($checks.Count) 项不符 —— 先别同步,这些逻辑错了会把失败当成功"
    exit $EX_SELF
  }
  Write-Ok "自检全部通过($($checks.Count) 项);回环 ping 用掉 $($loop.Ms) ms"
  Write-Host '注意:自检不验证笔记本那边 —— 通不通要跑 -Test(会 ping 默认地址)'
  exit $EX_OK
}

# ===========================================================================
# -Unregister:只删任务,别的一概不动
# ===========================================================================
if ($Unregister) {
  exit (Unregister-SyncTask)
}

# ===========================================================================
# 参数体检:两种模式下都要有 -LaptopHost,先统一查掉
# ===========================================================================
$hostErr = Test-HostArg $LaptopHost
if ($hostErr -ne '') {
  Write-Err2 "参数不对:$hostErr"
  Write-Host '例:powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -LaptopHost 26.253.1.139 -Test'
  exit $EX_ARG
}
if ($EveryMinutes -lt 1) { Write-Err2 '-EveryMinutes 至少 1 分钟'; exit $EX_ARG }

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
  # $PSScriptRoot = ...\Neru-s-206-dashboard\tools\sync → 往上两级就是仓库根
  $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot 'platformio.ini'))) {
  Write-Warn2 "警告:$RepoRoot 看着不像这个仓库(没有 platformio.ini)。快照里的东西可能不对。"
}

$uncPath = Get-UncPath $LaptopHost $ShareName $SyncRoot
if ($ShareName -ne '') { $shareBase = "\\$LaptopHost\$ShareName" } else { $shareBase = "\\$LaptopHost\c$" }
$state = @{
  Host        = $LaptopHost
  UncPath     = $uncPath
  ShareBase   = $shareBase
  SessionBase = ''
}

# ===========================================================================
# -Test:只探路,给判决和"怎么办"
# ===========================================================================
if ($Test) {
  try {
    if ($null -ne $Credential) {
      if (-not (Connect-Lan $state)) { Write-Warn2 '会话没建起来,下面的探路结果仅供参考' }
    }
    $pf = Test-Preflight $state
    Write-Host ''
    Write-Host "目标路径:$uncPath"
    if ($ShareName -eq '') {
      # 注意 `$ 是转义:不转义的话 PowerShell 会把 c$: 当成变量 $: ,打出来只剩一个 c
      Write-Host '现在走的是管理共享 c$:要求笔记本上 C:\Users\Public\206dash-sync 已存在,且两台机器是同名同密码的本地账号'
      Write-Host '更省事的做法:笔记本上建好文件夹并共享,然后加 -ShareName 206dash-sync'
    } else {
      Write-Host "现在走命名共享:$ShareName(笔记本上那个文件夹必须已经共享、并给当前账号读/写)"
    }
    if (-not $pf.Ok) { exit $EX_TEST }
    Write-Ok '探路通过。去掉 -Test 就是真同步:'
    $hintArgs = ''
    if ($ShareName -ne '') { $hintArgs = " -ShareName $ShareName" }
    Write-Host "  powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1$hintArgs"
    exit $EX_OK
  } finally {
    Disconnect-Lan $state
  }
}

# ===========================================================================
# -Register:探路 → 挂任务(探不通就不挂,免得每分钟失败一次刷日志)
# ===========================================================================
if ($Register) {
  try {
    if ($null -ne $Credential) {
      if (-not (Connect-Lan $state)) { exit $EX_TEST }
    }
    $pf = Test-Preflight $state
    if (-not $pf.Ok) { exit $EX_TEST }
    exit (Register-SyncTask $state)
  } finally {
    Disconnect-Lan $state
  }
}

# ===========================================================================
# 默认:探路(自动)→ 同步 → 小结
# 探路在这里不是可选项:九成的失败是 Radmin / 445 / 共享权限,先说人话,
# 而不是让 robocopy 打一屏英文错误码让人猜。
# ===========================================================================
$sync = $null
try {
  if ($null -ne $Credential) {
    if (-not (Connect-Lan $state)) { exit $EX_TEST }
  }
  $pf = Test-Preflight $state
  if (-not $pf.Ok) { exit $EX_TEST }
  $sync = Invoke-Sync $state
} catch {
  Write-Err2 "同步中断:$($_.Exception.Message)"
  Write-Warn2 '检查:暂存目录能写吗?(默认 C:\ProgramData\206dash-sync-stage)源文件还在吗?'
  exit $EX_COPY
} finally {
  Disconnect-Lan $state
}

Write-Host ''
$exitCode = Get-SyncExitCode $sync
if ($exitCode -eq $EX_OK) {
  Write-Ok '同步完成。笔记本上看到的就是桌机现在的样子。'
  exit $EX_OK
}
if ($exitCode -eq $EX_SNAP) {
  # 数据到位了,只有"锦上添花"的快照没打成(警告已经在上面打过了)。
  # 退出码仍然非 0:计划任务的[上次结果]和任何看日志的人都该看得出这次不完美。
  Write-Err2 "数据同步完成,但仓库快照 zip 失败:$($sync.SnapshotError)"
  Write-Err2 "退出码 $EX_SNAP(不是 0):笔记本上的数据是新的,只是没有新快照 —— 想补就跑一次真同步。"
  exit $EX_SNAP
}
Write-Err2 "同步没成功(robocopy $($sync.Code)),别当它成功了 —— 上面写了具体是哪一类。"
if ($sync.SnapshotFailed) { Write-Warn2 "另外仓库快照 zip 也没打成:$($sync.SnapshotError)" }
exit $EX_COPY
