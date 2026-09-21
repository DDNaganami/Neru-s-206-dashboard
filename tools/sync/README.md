# tools/sync —— 桌机 → 笔记本 同步(SSH 路线)

这一套只干一件事:**把不能进 git 的大文件,从台式机同步到笔记本**。

- 单向,桌机是唯一数据源(笔记本上改的东西不会被传回来)。
  为什么不双向:两边都改过同一个文件时会互相覆盖,而且谁也说不清哪份是新的 ——
  双向比不自动同步还危险。
- 代码和文档**不走这条路**,走 `git push`。这里只搬“git 装不下 / 不该装”的东西。

## ★ SMB / robocopy 那条路**已废弃**(2026-09-21)

现在用的只有 SSH。`sync-to-laptop.ps1` 和它的计划任务 `206dash-sync-to-laptop`
**都不再用了**,别再照着老文档去配共享。
把桌机上那个老任务**退役**掉要用**管理员** PowerShell(任务库在
`C:\Windows\System32\Tasks`),命令在下面“计划任务”一节里:
`Unregister-ScheduledTask -TaskName 206dash-sync-to-laptop -Confirm:$false`。

为什么废:这台桌机连笔记本的共享一律 `System error 5 / 拒绝访问`,
不管共享权限怎么给都一样;新加一条 Windows 凭据也救不回来
(`Test-Path \\26.253.1.139\206dash-sync` 始终 False,笔记本那边 `C:\206dash-sync` 一直是 0 个文件)。
根因是本机的**登录会话 / 凭据库**问题 —— 换别的手段查下去不划算,所以整条路放弃,改走 SSH。

> 老脚本 `sync-to-laptop.ps1` 还留在仓库里(没删,免得丢历史),但**不要再用**。
> 它的 robocopy `/MIR` 目标是 `C:\206dash-sync`;新路线**不往那个目录写任何东西**
> (`C:\206dash-sync` 现在就是个空壳,可以当它不存在)。

## 现在用哪条路:SSH(实测可用)

两台机器之间只有 SSH 这条链是验证过的:

```powershell
# 手工验证:能连上、能在笔记本上跑命令
ssh -i "$env:USERPROFILE\.ssh\dsh_laptop" 张九思@26.253.1.139 "echo ok"
# 搬文件
scp -i "$env:USERPROFILE\.ssh\dsh_laptop" <本地文件> "张九思@26.253.1.139:C:\206dash-data"
```

| 项 | 值 |
|---|---|
| 私钥 | `C:\Users\张九思\.ssh\dsh_laptop`(脚本里 `-SshKey` 的默认值) |
| 笔记本 Radmin 地址 | `26.253.1.139`(`-Laptop` 默认值 `张九思@26.253.1.139`) |
| 笔记本主机名 | `MikamoNeru`(脚本会打出来,认一下是不是这台) |
| 笔记本仓库 | `C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard` |
| 数据目标 | `C:\206dash-data`(`-LaptopData` 默认值) |

**密钥路径不对 / Radmin 没连 / 笔记本 sshd 没跑** —— 脚本预检会一句话说清是哪一个,
并且因为用了 `BatchMode=yes`,它**永远不会弹密码提示**(计划任务里弹提示 = 永远卡住)。

## 命令

```powershell
$s = 'tools\sync\sync-ssh.ps1'      # 在仓库根目录执行;也可以写全路径

# 0) 自检:不碰网络,改了脚本先跑这个
powershell -ExecutionPolicy Bypass -File $s -SelfTest

# 1) 探路:通道 + 笔记本仓库状态 + 数据目录现有文件(只读,什么都不改)
powershell -ExecutionPolicy Bypass -File $s -Test

# 2) 真同步(默认参数就够)
powershell -ExecutionPolicy Bypass -File $s

# 3) 挂计划任务:登录时 + 每 30 分钟(任务名 206dash-sync-ssh)
powershell -ExecutionPolicy Bypass -File $s -Register
powershell -ExecutionPolicy Bypass -File $s -Unregister     # 不想要了

# 4) 只想看它打算干什么,什么都不动
powershell -ExecutionPolicy Bypass -File $s -WhatIf

# 计划任务常用操作
Get-ScheduledTask -TaskName 206dash-sync-ssh | Get-ScheduledTaskInfo   # 上次结果/下次时间
Start-ScheduledTask   -TaskName 206dash-sync-ssh                       # 立刻跑一次
```

退出码:`0` 全成功 / `2` 参数不对 / `3` 通道不通 / `4` 仓库那步失败 /
`5` 自检没过 / `6` 数据传完了但仓库没成。计划任务的“上次结果”看这个码就知道断在哪。

`6` 有三种原因,小结里会写明是哪一种(不会含糊过去):

- **跳过** —— gate 拒绝(笔记本有**已跟踪**文件的改动),压根没碰它的仓库;
- **没成功(fetch)** —— 笔记本自己 `git fetch origin` 失败,`origin/main` 还是旧的。
  这种情况 `merge --ff-only` 常常回一句 “Already up to date.” 加退出码 0,
  **别被它骗了**:那句话是跟**过期的** `origin/main` 比出来的。所以脚本把 fetch 和
  merge 的退出码分开看(`FETCH_EXIT` / `MERGE_EXIT`),fetch 一失败就不算成功;
- **没成功(merge)** —— 快进报错,或者两边 HEAD 对不上。

## 同步过去什么

| 桌机上 | 笔记本上 | 说明 |
|---|---|---|
| `C:\Users\Public\206dash\van_capture_dm.csv` | `C:\206dash-data\` | 真车抓包 |
| `C:\Users\Public\206dash\drive5min.csv` | `C:\206dash-data\` | 真车抓包 |
| `C:\Users\Public\206dash\image-v3.bin` | `C:\206dash-data\` | 烧机字库 |
| `C:\Users\Public\206dash\theme-user.json` | `C:\206dash-data\` | 烧机主题 |
| `C:\Users\Public\206dash\206dash-transfer.zip` | `C:\206dash-data\` | 现成的传输包,**有就带、没有就跳过** |
| 仓库(代码) | 笔记本的同名 checkout | 桌机 `git push` → 笔记本 `git fetch` + `merge --ff-only origin/main` |

### 只发“大小不一样”的(土办法 rsync)

**Windows 上没有 `rsync`**,而整包重传 30~60 MB 不值得,所以脚本自己做了个最朴素的版本:

1. 先问笔记本:`C:\206dash-data` 里每个文件多少字节(一次 SSH 往返问完)
2. 桌机上逐个比大小:
   - 笔记本上没有 → 传
   - 大小不同 → 传
   - **大小一样 → 跳过**(不比时间戳、不比哈希)
3. 传完再问一次笔记本,核对大小;对不上才算失败

所以第二次跑几乎是瞬间结束(全跳过)。代价是:**只比大小,内容不同但大小刚好一样就发现不了** ——
对这几个文件(抓包 CSV、字库 bin)够用,毕竟它们只会整份重生成,不会被改成同样大小的另一份。

### 仓库那步会**先看一眼笔记本有没有本地改动**

笔记本的 checkout 如果有**已跟踪文件**的改动(**未暂存 / 已暂存 / 删除**都算),
脚本**直接停下不碰它**,只报出来 —— 它**不会** `stash` / `reset` / `clean`。
那些命令会把别人没提交的工作弄丢,而这是台在用的开发机。
处理办法:在笔记本上把这些改动提交掉或者挪走,再同步。

> **未跟踪文件不算数(2026-09-21 改的)。** gate 从 `git status --porcelain` 收窄成
> `git status --porcelain --untracked-files=no`,也就是只看已跟踪文件的改动。
>
> 为什么:未跟踪文件**挡不住快进合并**。万一某个未跟踪文件正好要被这次写进来的
> 已跟踪文件覆盖,git 自己会中止合并、原样留着那个文件 —— 它不会覆盖任何东西。
> gate 再拦一遍,只是把正常同步变成永远失败。当时就是这么翻的车:笔记本 checkout 里
> 躺着 **26 个**早期 sshd / Radmin 调试残留(`A.sshd.log`、`diagnose-sshd*.ps1`、
> `fix-route-metric.ps1` …),全是未跟踪文件,于是每次同步都拒绝快进、退出 6,
> 明明整条同步都是成功的。
>
> 那些残留**没有被删、没有被挪、也没有进 `.gitignore` / `.git/info/exclude`**
> —— 改的只是脚本这一边的判据。脚本仍然会**报出**未跟踪文件的个数
> (`UNTRACKED_COUNT`),只是不拿它拦人。

数据文件不受这个影响(它们进的是 `C:\206dash-data`,和仓库无关),继续照传。

最后两边的 HEAD 哈希都会打出来,对不上一眼就能看见。
**哈希相同 ≠ 这一步成功**:gate 拒绝、或者笔记本自己 `git fetch` 失败时,两边 HEAD
本来就可能一样(笔记本早就停在同一个提交上)。所以小结里“已快进 / 跳过(gate 拒绝) /
没成功(fetch 或 merge 报错)”是分开写的,不会拿哈希相同冒充“已同步”。

## 故意不同步什么(这是重点)

| 不同步 | 为什么 |
|---|---|
| `.pio` / `.pio-core` / `.tools` | 共 186 MB+ 的编译缓存和工具链,机器相关,传过去既慢又没用 |
| `.git` 目录本身 | 代码走 `git push`,不需要再搬一次仓库 |
| `__pycache__` / 各种 venv | 编译缓存,机器相关 |
| `C:\Users\Public\206dash` 里其它几十个文件 | 只挑上表那 5 个;**散落的脚本和日志不进同步** |
| 笔记本 → 桌机的任何东西 | 单向。笔记本上的新数据要**人工**拷回来 |
| `C:\206dash-sync` | 那是老 robocopy `/MIR` 的镜像目标(会删文件),已废弃 |

## 出问题先看这里

| 看到什么 | 意思 | 下一步 |
|---|---|---|
| `通道不通` | SSH 没连上 | 它会给三行:① Radmin 通不通 ② 笔记本 `sshd` 服务在不在 ③ 私钥路径对不对 —— 按顺序查 |
| 笔记本仓库 `有 N 项**已跟踪**文件的改动` | checkout 不干净,脚本拒绝动它 | 在笔记本上提交或挪走那几个**已跟踪**文件;未跟踪的残留文件不算数,不用删 |
| `笔记本自己 git fetch origin 失败` | 笔记本连不上它自己的远端(`remote.origin.url` 是 HTTPS 的 `github.com`) | 查笔记本的网络/DNS。实测笔记本到 `github.com:443` 会超时或“Connection was reset”,而桌机这边是通的(所以桌机 `git push` 一直没事)。这种情况下的 “Already up to date.” **不算数** |
| `两边 HEAD 不一致` | 笔记本没快进到最新 | 看上面 `MERGE:` 那几行;多半是被本地改动挡住了,或者 fetch 没成功 |
| `传完大小不对` | 文件没传完整 | 再跑一次;已经传好的会被大小比较跳过,不会重传 |
| 退出码 `6` | 数据是新的,但仓库没同步成功 | 小结里写明是“gate 跳过 / fetch 失败 / merge 失败”哪一种 —— 三种的处理办法不一样 |

计划任务(**要动任务库就得用管理员 PowerShell**):

- `-Register` / `-Unregister` 写的是系统任务库 `C:\Windows\System32\Tasks`,
  **普通权限会“拒绝访问”** —— 所以这两条(以及下面退役老任务那一条)都要在一个
  **“以管理员身份运行”** 的 PowerShell 里执行。**平时跑同步不需要管理员**。

  ```powershell
  # 管理员 PowerShell:
  powershell -ExecutionPolicy Bypass -File tools\sync\sync-ssh.ps1 -Register
  powershell -ExecutionPolicy Bypass -File tools\sync\sync-ssh.ps1 -Unregister
  ```

- **退役老的 SMB 计划任务**(`206dash-sync-to-laptop`,2026-09-21 那条路已废):
  同样要管理员。先看它在不在,再删:

  ```powershell
  # 管理员 PowerShell:
  Get-ScheduledTask        -TaskName 206dash-sync-to-laptop            # 看还在不在
  Unregister-ScheduledTask -TaskName 206dash-sync-to-laptop -Confirm:$false
  ```

  删掉之后桌机上就只剩 `206dash-sync-ssh` 一个同步任务了。
- 任务跑的是带**默认参数**的本脚本;要改目标/密钥就改脚本里的默认值,别在任务里塞参数。

## 为什么远端命令要编成 base64(改脚本的人必读)

**Windows OpenSSH 会把命令行重新拼一遍再交给远端,内层引号活不下来。**
实测 `ssh host 'if (x) { Write-Output "$($_.Name)" }'` 到了笔记本上双引号已经没了,
PowerShell 直接报“表达式只能作为管道的第一个元素”。

所以脚本里所有远端命令都是:本地把 PowerShell 正文编成 **UTF-16LE + base64**,
用 `powershell -EncodedCommand <b64>` 发过去。传输层只剩 `[A-Za-z0-9+/=]`,
中文路径也不会被改写(本机 8.3 短名是关的,取不到 `ZHANGJ~1`,
中文路径能过去**全靠**这一层)。`-SelfTest` 里有断言:解回来的字符串必须和原文逐字节相等。

另外两个坑,都在自检里守着:

- 生成远端脚本必须用**单引号** here-string(`@'...'@`)。双引号版本会在本地就把
  `$_.Name` 展开掉(实测展成了 `powershell.exe|292864`)。
- 调外部程序要用 `Invoke-Native`。`$ErrorActionPreference='Stop'` 之下,
  PowerShell 5.1 会把外部程序写到 stderr 的**正常输出**当终止性错误 ——
  `git push` 明明返回 0、只说了句 "Everything up-to-date",脚本就死在那儿了。
