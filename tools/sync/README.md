# tools/sync —— 桌机 → 笔记本 同步(SSH 路线)

这一套只干一件事:**把不能进 git 的大文件,从台式机同步到笔记本**。

- 单向,桌机是唯一数据源(笔记本上改的东西不会被传回来)。
  为什么不双向:两边都改过同一个文件时会互相覆盖,而且谁也说不清哪份是新的 ——
  双向比不自动同步还危险。
- 代码和文档**不走这条路**,走 `git push`。这里只搬“git 装不下 / 不该装”的东西。

## ★★ 更正:笔记本到 GitHub **有**通道 —— 2026-09-21 当晚修好,原来那句结论是错的

**先把错的结论划掉。** 本文原来(以及下面「出问题先看这里」那张表)写着“笔记本连不上它自己的远端 /
笔记本那边没有到 GitHub 的通道” —— **这个判断是错的**。笔记本那侧的网络本来就是好的,
坏的只是笔记本这个 checkout 的**配置**。

**当初为什么会误判:** 所有诊断都是在一条**嵌套 SSH 会话**里做的(桌机 `ssh` 进笔记本,
再在笔记本里派生 `ssh` / `git fetch`)。那条路**会在数据阶段卡死**,看起来和“没有通道”一模一样。
详见下面「★ 嵌套 SSH 会话」一段 —— 那条规矩请当硬规矩用。

### 笔记本侧网络:实测干净(以下都在笔记本本机上跑)

| 域名 | 笔记本解析 | 443/TCP |
|---|---|---|
| `github.com` | `20.205.243.166` | 通 |
| `ssh.github.com` | `20.205.243.160` | 通 |
| `api.github.com` | `20.205.243.168` | 通 |
| `codeload.github.com` | `20.205.243.165` | 通 |

- **解析不是污染**:这四个都是 GitHub 的真实地址。台式机那边 `github.com` / `api.github.com` /
  `codeload.github.com` 全被解析成 `127.0.0.1`(只有 `ssh.github.com` 是干净的 `20.205.243.160`)。
- 从笔记本 `ssh.github.com:443` 读到的 SSH 横幅是 **`SSH-2.0-af8ca74`**,与台式机读到的**完全一致**
  ⇒ 中间没有中间人。
- 路径 MTU 与台式机一致:DF ping 载荷 **1452 通**、**1472 报需要分片**;各接口 `NlMtu` 都是 **1500**。
- 到 GitHub 的 IP 段(20/140 开头)**没有任何 VPN 路由**;两端都走 WLAN。

### 原来的 `exit 6` 是这三个原因(与“有没有通道”无关)

1. 笔记本的 remote 当时是 **HTTPS** `https://github.com/DDNaganami/Neru-s-206-dashboard.git`
   —— TLS 里带 `github.com` 的 **SNI 被 RST**,报 `Recv failure: Connection was reset`。
   (被重置的是**这一条 HTTPS 连接**,不是“GitHub 不可达”。)
2. 笔记本 `~/.ssh` 里**当时只有 `dsh_desktop` 一把密钥,没有 GitHub 密钥**
   (现在是 `dsh_desktop` + `id_ed25519` 两把 + `known_hosts`)。
3. git 默认用的 ssh 是 **Git 自带那套**(`C:\Program Files\Git\usr\bin\ssh.exe`),
   它读的 HOME/.ssh 与 Windows OpenSSH 不是一个 ⇒ 报 `Host key verification failed`。

> **台式机为什么“看起来能连”**:台式机上 `github.com` 和 `api.github.com` 被解析成 **`127.0.0.1`**
> (域名被污染),所以台式机只能走 `ssh://git@ssh.github.com:443` 这条 **SSH-over-443** 绕行
> (本文所有 `git push` 走的就是它);**笔记本 DNS 干净,本来就能直连 GitHub**,
> 只是 remote 写错了协议 —— 所以这从来不是“笔记本没通道”,是“笔记本配错了”。

### 修好的四条(都在笔记本上执行,逐条实测)

```powershell
# ① remote 换成 SSH over 443(和台式机同一条 URL)
git remote set-url origin ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git

# ② 密钥:把台式机的 ~/.ssh/id_ed25519(+ .pub)复制到笔记本 %USERPROFILE%\.ssh\id_ed25519
#    两端私钥 SHA256 前 16 位一致:3FFE2C9BE5658ED7
icacls "$env:USERPROFILE\.ssh\id_ed25519" /inheritance:r /grant:r "$env:USERNAME:R"

# ③ known_hosts:用 keyscan 写进去(写入用的是 Add-Content,实测无 CRLF 问题)
ssh-keyscan -p 443 -t rsa,ecdsa,ed25519 ssh.github.com

# ④ 让 git 用 OpenSSH,而不是 Git 自带那套
git config --replace-all core.sshCommand 'C:/PROGRA~1/OpenSSH/ssh.exe -o BatchMode=yes -o StrictHostKeyChecking=accept-new'
git config --get-all core.sshCommand      # ★ 必须只有一行
```

两个**必须这么写**的细节(都实测踩过):

- **路径要用 8.3 短名 `C:/PROGRA~1/OpenSSH/ssh.exe`**:`C:\Program Files\OpenSSH\ssh.exe`
  带空格,而 PowerShell 把传给原生 exe 的引号吞掉,git 只收到 `C:/Program`,
  报 `cannot spawn C:/Program`。
- ★ **必须用 `--replace-all`,改完必须 `git config --get-all core.sshCommand` 确认只有一行**:
  笔记本 `.git/config` 里当时**真的有两条 `sshCommand`**,**最后一条生效** ⇒ 一直在用错的那条。
  实测(git 2.x,本机复现):`core.sshCommand` 有**多个值**时,普通的
  `git config core.sshCommand '<新值>'` **改不动**,直接报
  `error: cannot overwrite multiple values with a single value`,那两条原样躺着 ——
  所以只能 `--replace-all`,而且改完不核对等于没改。

另外,笔记本上三个 `ssh.exe` 只有一个是能用的(**换机时会再遇到**):

| 可执行文件 | 结果 |
|---|---|
| `C:\Program Files\OpenSSH\ssh.exe` | ✅ 正常(现在 `core.sshCommand` 指的就是它) |
| `C:\Windows\System32\OpenSSH\ssh.exe` | ❌ **会挂死**(连 stdin 接 NUL 也挂) |
| `C:\Program Files\Git\usr\bin\ssh.exe` | ❌ 认证不过(读不到我们的密钥)⇒ `Host key verification failed` |

### 修完之后的实测结果

- 笔记本 `git config --get-all core.sshCommand` = **一行**;
  `remote.origin.url` = `ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git`。
- 笔记本 `known_hosts` 里 `[ssh.github.com]:443` 的 rsa / ecdsa / ed25519 三条指纹
  **与 GitHub 官方公布的主机密钥逐个一致**(ed25519 = `SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU`)。
- 在笔记本上(**会话之外**)跑 `git fetch`,日志 **`face40c..a2c03ed  main -> origin/main`** ——
  笔记本 HEAD 随即变成 **`a2c03ed`**,`git merge --ff-only` 快进成功;`git reflog` 里是
  `merge origin/main: Fast-forward`,`.git/FETCH_HEAD` 指向
  `ssh://ssh.github.com:443/DDNaganami/Neru-s-206-dashboard`。
  ⇒ **笔记本这条腿现在是通的**,本文原来“笔记本自己 fetch 不会成功”的默认假设不再成立。

### ★ 嵌套 SSH 会话:在 SSH 会话里再派生 ssh / git fetch 会**在数据阶段卡死**(最坑的一条)

症状(2026-09-21 实测,全部发生在“桌机 `ssh` → 笔记本”这条会话里):

- 认证**成功**,远端 `git-upload-pack` **已经启动**,SSH 通道也 **open confirm** 了,
- 然后**一个字节都不再回来**;`-vvv` 日志停在
  `channel 0: open confirm rwindow 32000 rmax 35000`。

实测**三种派生方式都挂**:① 内联调用 ② `Start-Process` 重定向 ③ `Start-Process cmd` + stdin 接 NUL。
而**同一个 ssh 手动加 `-T`** 只用了 **0.6 秒**就通 —— 密钥、网络、主机密钥全都是好的,
挂的只是“**在一条 SSH 会话里再开一条 SSH 去搬数据**”这件事。

★ 把同一条 `git fetch` 放到**会话之外**跑(一次性计划任务)**立刻成功**(就是上面那条 `face40c..a2c03ed`)。

⇒ **规矩:不要用嵌套 SSH 会话去诊断或执行长时间的 git-over-ssh 操作。**
要么让用户在笔记本本地跑,要么用一次性计划任务(`Register-ScheduledTask` + `Start-ScheduledTask`,
跑完 `Unregister`)。**原来那句“笔记本没有到 GitHub 的通道”就是这么误判出来的 ——
当时的判断是错的,原因是嵌套 SSH 会话造成的假象。**

### 另外两条现状(别顺手改掉)

- 笔记本 checkout 里有一处**未提交的本地改动 `M src/main.cpp`**(**+49 / −9**:启用 OBD 串口 ——
  `OBD_SERIAL` / `OBD_RX_PIN 17` / `OBD_TX_PIN 18` / `kObdBaud 38400` / `attachObdSerial()`)。
  ★ **不要动它、不要提交、不要还原。** 实测它**不影响 `git merge --ff-only`**
  (`face40c → a2c03ed` 这次快进照过,那处改动原样留着)。
  ⚠ 但它是**已跟踪**文件的改动,所以**下一轮同步脚本的 gate 会拒绝快进、判 `refused` / 退出 6**
  (见下面「仓库那步会先看一眼笔记本有没有本地改动」)—— 那是预期行为,**不要**去 stash / checkout 它。
- **PlatformIO 那个 259 MB 的 `toolchain-riscv32-esp` 下载中途断掉**(`IncompleteRead`,42/216 MB)
  —— **原因尚未查清**。**不要**把它当成和 GitHub 同因:那是 PlatformIO 自己的下载,和 git 这条链无关。

## ★ SMB / robocopy 那条路**已废弃**(2026-09-21)

现在用的只有 SSH。`sync-to-laptop.ps1` 和它的计划任务 `206dash-sync-to-laptop`
**都不再用了**,别再照着老文档去配共享。
那个老任务**已经不在桌面机上了**(2026-09-21 实测:`Get-ScheduledTask`、`schtasks /query`、
`C:\Windows\System32\Tasks` 三处都搜不到 `206dash`)——**不需要再执行任何清理命令**。
(曾经写在这里的“用管理员 PowerShell 去 Unregister”已经删掉:照着执行只会得到
“找不到任何 TaskName 属性等于 … 的 MSFT_ScheduledTask 对象”,白跑一趟管理员窗口。)

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
| 笔记本仓库 | `C:\Users\张九思\206Dash\Neru-s-206-dashboard`(**和桌机同一个绝对路径**,2026-09-21 搬过来的,见下面「两边路径对齐」) |
| 数据目标 | `C:\206dash-data`(`-LaptopData` 默认值) |

**密钥路径不对 / Radmin 没连 / 笔记本 sshd 没跑** —— 脚本预检会一句话说清是哪一个,
并且因为用了 `BatchMode=yes`,它**永远不会弹密码提示**(计划任务里弹提示 = 永远卡住)。

## 两边路径对齐(2026-09-21)

笔记本那个 checkout **搬到了和桌机完全同一个绝对路径**:

| | 路径 |
|---|---|
| 旧(笔记本) | `C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard` |
| 现在(两边都一样) | `C:\Users\张九思\206Dash\Neru-s-206-dashboard` |

- 搬的是**真移动**(`Move-Item`,同一个卷上就是改名),旧路径**没有留副本**、**没有建 junction**,
  只在旧路径放了一个一行说明 `README-已迁移.txt`(写明新路径和日期)。
- 为什么对齐:① 文档 / 脚本里的命令两边通用,不用再记两套路径;② **DSH 的工作区目录名是按
  工作区路径编码出来的**(见下面「笔记本上的 DSH」),两边路径一样,目录名才会一样,
  以后在笔记本上看 / 恢复会话不会因为路径错位而对不上。
- `tools/sync/sync-ssh.ps1` 的 `-LaptopRepo` 默认值已经改成新路径。

> ⚠ **移动 / 重命名这个 checkout 之前,先在笔记本上关掉 VS Code(或 PlatformIO IDE 会话)。**
> 这不是洁癖,是 2026-09-21 真踩到的:`.pio\libdeps` 被 PlatformIO 的进程占着时,
> **整棵目录都改不了名**,报 "另一个程序正在使用此文件,进程无法访问"。
> 扫描下来 `.git`、`src`、`tools` 这些子目录一个个都能改名,**只有根目录不行** ——
> 因为"祖先目录里有被占用的子孙"时,祖先本身也动不了。
>
> 一句话确认有没有被占(把 `<仓库>` 换成实际路径,能改名就是没被占):
>
> ```powershell
> # ① 改名探测:成功=没被占(它会立刻改回来)
> Rename-Item -LiteralPath '<仓库>\.pio' -NewName '.pio.probe'; Rename-Item -LiteralPath '<仓库>\.pio.probe' -NewName '.pio'
> # ② 看还有没有 PlanformIO / VS Code 的进程
> Get-Process Code,pio* -ErrorAction SilentlyContinue
> ```
>
> ★ 还有一类**看不出来**的占用:某个进程的**当前目录就在仓库里**。
> 它不锁任何文件,但锁住目录本身,改名照样失败 —— 典型来源:VS Code 的集成终端、
> 一个 `Set-Location <仓库>; platformio run …` 的包装脚本、或者**从仓库目录启动的 DSH**。
> 遇到"子目录全都能改名、只有根目录不行",先想想有没有这种东西在跑。

## 笔记本上的 DSH:会话是从哪来的、怎么放进去(2026-09-21)

笔记本 `.dsh` 里能看到的会话,是靠**目录树**认的,不是靠某个索引文件:

```
%USERPROFILE%\.dsh\sessions\<工作区编码目录>\<会话目录>\session.v3.jsonl.zstd
```

- **工作区编码目录**:桌机上是 `--C-Users-~5F20~4E5D~601D-206Dash--`(即 `C:\Users\张九思\206Dash`
  编码出来的名字)。两边路径对齐之后,笔记本上的同名目录就是同一个名字。
- **前提**:笔记本上的 DSH 必须**从对齐后的工作区**(`C:\Users\张九思\206Dash`)启动,
  会话才会出现在那个工作区的列表里。
- 会话目录名**照台式机原样镜像**,别自己改:DSH 自己建的目录名带 `session-` 前缀
  (例如 `session-49039670-ec47-46bb-8eb0-30e765c65e42`),而文件里 `id` 字段也是同一个带前缀的值
  —— 两边必须一致,镜像源目录名最稳。
- **只复制、不移动、不覆盖更新的那份**:目标已存在时 —— 大小一样就跳过;目标比源**新**就**不要覆盖**
  (免得把笔记本上更完整的记录盖掉)。
- **不复制** `storages\session_projcache\sessions\<id>.json`:查过 DSH 自己的包文档
  (`@deepseek-ai/dsh-session-projection-cache` README)—— "The session log remains authoritative",
  那只是给冷会话列表省 I/O 的**投影缓存**,缺失 / 不兼容会被忽略或重建。会话日志本身才是数据。
- ⚠ **打开历史看可以,别在这个会话上接着跑**:两台机器的串口 / 设备 / 路径都不一样,
  在一个从桌机搬过来的会话上继续执行,很容易把两边状态搅乱。要看就在笔记本上**新开**一个会话。
- 装好之后,**笔记本的 DSH 一旦跑起来,它自己可能会往这个会话文件里追加事件**
  (2026-09-21 实测:装完约 1 分钟后 +79 字节,文件仍是完整可解压的日志)—— 那是 DSH
  在正常使用这个会话,**不是**同步脚本干的(同步脚本永远只写 `C:\206dash-data`)。

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
  ★ 2026-09-21 起笔记本侧 remote 已是 SSH over 443、**fetch 实测会成功**(见开头「更正」一节)。
  真再出现 `nofetch`,**别再用嵌套 SSH 会话去复现** —— 那会得到假的“连不上”(见「★ 嵌套 SSH 会话」);
- **没成功(merge)** —— 快进报错,或者两边 HEAD 对不上。

## 同步过去什么

| 桌机上 | 笔记本上 | 说明 |
|---|---|---|
| `C:\Users\Public\206dash\van_capture_dm.csv` | `C:\206dash-data\` | 真车抓包 |
| `C:\Users\Public\206dash\drive5min.csv` | `C:\206dash-data\` | 真车抓包 |
| `C:\Users\Public\206dash\image-v3.bin` | `C:\206dash-data\` | 烧机字库 |
| `C:\Users\Public\206dash\theme-user.json` | `C:\206dash-data\` | 烧机主题 |
| `C:\Users\Public\206dash\206dash-transfer.zip` | `C:\206dash-data\` | 现成的传输包,**有就带、没有就跳过** |
| `%USERPROFILE%\.dsh\sessions\…\session.v3.jsonl.zstd`(最新的那个会话) | `C:\206dash-data\session-<会话目录名>.jsonl.zstd` | DSH 会话记录**原件**(以后能被 DSH 打开) |
| 同上,解压出来的可读版 | `C:\206dash-data\对话记录.jsonl` | 给人看 / 搜的明文 JSONL |
| 仓库(代码) | 笔记本的**同一个绝对路径** | 桌机 `git push` → 笔记本 `git fetch` + `merge --ff-only origin/main`。笔记本侧 remote 也是 **SSH over 443**,2026-09-21 起实测快进成功(见开头「更正」一节) |

### 会话记录(对话记录)也跟着走(2026-09-21 owner 定的)

每次同步都会把桌机上**最新的那个会话**带过去,两个文件都落在 `C:\206dash-data`:

1. 桌机上递归找 `%USERPROFILE%\.dsh\sessions\` 下的 `session.v3.jsonl.zstd`,
   按**最后写入时间**取最新的那个(DSH 是边跑边往这个文件里追加的,所以"最后写入"= 现在正在用的会话);
2. 原样发一份,名字是 `session-<会话目录名>.jsonl.zstd`(会话目录名就是上一层目录名);
3. 再用**桌机的 Python 3.14** 解压出可读版 `对话记录.jsonl`
   (`python -c "from compression.zstd import decompress; …"` —— 这个模块 3.14 自带)。

几个**故意这么设计**的点:

- **它基本每次都会重发一遍**,这是预期的、不是 bug:会话记录一直在变,大小几乎每次都不一样,
  所以"比大小"这套判据每次都判"发"。就这一个文件、正常 1 MB 上下
  (2026-09-21 实测:`.zstd` 0.84 MB / 可读版 3.07 MB),重发一遍的代价可以忽略。
- **发之前先拍个快照**:会话文件是活的(DSH 正在往里写),边发边涨的话"传完复核大小"永远对不上,
  会被记成传输失败、退出 6 —— 明明文件已经到了。所以先复制到 `%TEMP%` 冻结一份,发的是冻结那份,
  复核的也是它;跑完就删。
- **解压失败不影响其它**:没有 Python / 没有 `compression.zstd` / 抄到半条 —— 都只打**一行警告**,
  原始那份照发,整条同步**不会**因此失败。
- **找不到会话记录**也只打一行就跳过(这台机器没跑过 DSH 也不该拦住抓包 CSV / 字库)。
- **超过 20 MB 只警告、仍然照发**:正常一份 `.zstd` 就在 1 MB 上下(实测 0.84 MB;解压出来的
  可读版约 3~4 倍 = 3.07 MB)。这条线量的是 `.zstd`,20 MB 说明
  这个会话大得离谱(或者哪里在刷日志),值得人看一眼;可它是真数据,不是错误,所以不跳过、不拦同步。
- **旧会话文件会攒在笔记本上**(每个会话一个 `session-<会话目录名>.jsonl.zstd`)。脚本**不删**它们 ——
  不替人做删除决定,要看哪次就翻哪个。
- ⚠ **`对话记录.jsonl` 以最近一次同步为准**:它每次都整份覆盖,别把它当成历史归档;
  要留某一次就自己改名存一份。
- ★ 这些**只放 `C:\206dash-data`**(供阅读 / 检索),**不写笔记本自己的 `.dsh` 会话树**。
  要装进笔记本的 DSH 里让人在 DSH 里看到,是**手工一步**(规格见上面「笔记本上的 DSH」)。

> 实测(2026-09-21 首次真跑):`session-5124ca8a-…jsonl.zstd` 882,403 字节 + `对话记录.jsonl`
> 3,220,641 字节,两个都传完并核对过大小;同一次里那 5 个散件**全部跳过**(大小一致)。

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

> ★ **注意:笔记本现在真的有一处“已跟踪改动”** —— `M src/main.cpp`(+49 / −9,OBD 串口,2026-09-21)。
> 所以下一轮同步**会**在这个 gate 上被拒、判 `refused` / 退出 6,那是**预期**的,不是回归。
> 这处改动**不要提交、不要 stash、不要还原**(实测它不影响 `git merge --ff-only`,
> 见开头「更正」一节最后两条)。

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
| 笔记本自己的 `.dsh` 会话树 | 同步只把会话记录放进 `C:\206dash-data`;**装进笔记本的 DSH 是手工一步**(规格见上面「笔记本上的 DSH」) |

## 已评估但不做(别再提)

| 已评估但不做 | 为什么 |
|---|---|
| **用 `git bundle` 把 git 对象从桌机中继到笔记本**(桌机打包 → scp → 笔记本解包) | 2026-09-21 owner 决定:**不做**。理由:① 笔记本正常它自己就能连 GitHub(★ 见开头「更正」一节:这一点 2026-09-21 当晚已实测修好,笔记本 `git fetch` 快进到 `a2c03ed`);② 仓库这条链**本来就照实报错** —— 笔记本自己 `git fetch` 失败时判 `nofetch`、**退出码 6**,不会拿那句 "Already up to date." 冒充成功,所以"笔记本连不上"这件事已经能被看见,不需要靠中继去绕。<br>★ **实测更正(2026-09-21 当晚)**:这里原来写的是“笔记本上 `github.com:443` **仍然连不上**” —— **那个结论是错的**,原因是**嵌套 SSH 会话造成的假象**(见开头「★ 嵌套 SSH 会话」)。真实原因是笔记本 remote 当时是 HTTPS(**TLS 里带 `github.com` 的 SNI 被 RST**,`curl 56 Connection was reset`)+ 没有 GitHub 密钥 + git 用错 `ssh.exe`;而**笔记本 DNS 是干净的、`ssh.github.com:443` 一直是通的**。修法正是把 `remote.origin.url` 换成 `ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git` 并配一把 GitHub 认的密钥(已完成),**不是**改成 bundle 中继。 |

## 出问题先看这里

| 看到什么 | 意思 | 下一步 |
|---|---|---|
| `通道不通` | SSH 没连上 | 它会给三行:① Radmin 通不通 ② 笔记本 `sshd` 服务在不在 ③ 私钥路径对不对 —— 按顺序查 |
| 笔记本仓库 `有 N 项**已跟踪**文件的改动` | checkout 不干净,脚本拒绝动它 | 在笔记本上提交或挪走那几个**已跟踪**文件;未跟踪的残留文件不算数,不用删 |
| `笔记本自己 git fetch origin 失败` | 笔记本 fetch 没成功,`origin/main` 还是旧的 | **先看开头「更正」一节:笔记本到 GitHub 是通的**(2026-09-21 当晚已修好 —— `remote.origin.url` = `ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git`,`core.sshCommand` 指向 `C:/PROGRA~1/OpenSSH/ssh.exe` 且只有一行,密钥是 `id_ed25519`)。★ 排查时**不要**用嵌套 SSH 会话去派生 `git fetch` —— 那条路会卡在数据阶段、伪装成“连不上”(见「★ 嵌套 SSH 会话」);要么让用户在笔记本本地跑,要么用一次性计划任务。这种情况下的 “Already up to date.” **不算数** |
| `两边 HEAD 不一致` | 笔记本没快进到最新 | 看上面 `MERGE:` 那几行;多半是被本地改动挡住了,或者 fetch 没成功 |
| `传完大小不对` | 文件没传完整 | 再跑一次;已经传好的会被大小比较跳过,不会重传 |
| `对话记录.jsonl` 每次都显示"发送" | **正常**,不是故障 —— 会话一直在变,大小每次都不一样(见上面「会话记录」) | 不用管;它是唯一一个基本每次都会重发的文件 |
| 笔记本 DSH 里看不到刚搬过去的会话 | 会话目录名 / 工作区目录名对不上 | 确认 DSH 是**从 `C:\Users\张九思\206Dash` 启动**的,且文件在 `.dsh\sessions\--C-Users-~5F20~4E5D~601D-206Dash--\session-<id>\session.v3.jsonl.zstd` |
| 移动 / 改名仓库时报"另一个程序正在使用此文件" | 笔记本上有进程占着那个目录(PlatformIO / VS Code / 某个"当前目录在仓库里"的进程) | 先关 VS Code(或 PlatformIO IDE 会话),再用「两边路径对齐」里那句改名探测确认;`.git`/`src` 能改名但根目录不行 = 根目录本身被占 |
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

- **老的 SMB 计划任务**(`206dash-sync-to-laptop`,2026-09-21 那条路已废):
  **已确认不存在,无需清理,也不需要管理员权限**。2026-09-21 实测三处都为空:

  ```powershell
  Get-ScheduledTask | Where-Object TaskName -match '206dash'   # 应为空
  schtasks /query /fo csv | Select-String 206dash              # 应为空
  Get-ChildItem C:\Windows\System32\Tasks -Filter *206dash*    # 应为空
  ```

  如果这时还去执行 `Unregister-ScheduledTask -TaskName 206dash-sync-to-laptop -Confirm:$false`,
  会报“找不到任何 TaskName 属性等于 … 的 MSFT_ScheduledTask 对象” ——
  那是**正常结果**(它本来就不在),不是命令写错了。

  桌机上现在只剩 SSH 那条同步任务(`206dash-sync-ssh`,前提是 `-Register` 过)。
- 任务跑的是带**默认参数**的本脚本;要改目标/密钥就改脚本里的默认值,别在任务里塞参数。

## 为什么远端命令要编成 base64(改脚本的人必读)

**Windows OpenSSH 会把命令行重新拼一遍再交给远端,内层引号活不下来。**
实测 `ssh host 'if (x) { Write-Output "$($_.Name)" }'` 到了笔记本上双引号已经没了,
PowerShell 直接报“表达式只能作为管道的第一个元素”。

所以脚本里所有远端命令都是:本地把 PowerShell 正文编成 **UTF-16LE + base64**,
用 `powershell -EncodedCommand <b64>` 发过去。传输层只剩 `[A-Za-z0-9+/=]`,
中文路径也不会被改写(本机 8.3 短名是关的,取不到 `ZHANGJ~1`,
中文路径能过去**全靠**这一层)。`-SelfTest` 里有断言:解回来的字符串必须和原文逐字节相等。

另外三个坑,都在自检里守着:

- 生成远端脚本必须用**单引号** here-string(`@'...'@`)。双引号版本会在本地就把
  `$_.Name` 展开掉(实测展成了 `powershell.exe|292864`)。
- 调外部程序要用 `Invoke-Native`。`$ErrorActionPreference='Stop'` 之下,
  PowerShell 5.1 会把外部程序写到 stderr 的**正常输出**当终止性错误 ——
  `git push` 明明返回 0、只说了句 "Everything up-to-date",脚本就死在那儿了。
- ★ **两头的控制台编码必须都是 UTF-8**(2026-09-21 加 `对话记录.jsonl` 时踩出来的)。
  远端命令的输出是**字节流**,解成什么由编码决定,和 base64 那一层无关:
  笔记本默认按 OEM 代码页(zh-CN = 936/GBK)吐字节,而桌机 PowerShell 5.1 按
  `[Console]::OutputEncoding` 解。不一致时**中文文件名会变乱码**,于是
  `对话记录.jsonl` 在"列目录"里永远认不出来:每次都判"笔记本上没有"→ 重发,
  发完复核又认不出 → 记成"传完大小不对" → **假的失败、退出 6**。
  所以脚本头部有 `[Console]::OutputEncoding = [Text.Encoding]::UTF8`,
  每个远端脚本正文开头也有同一句。ASCII 在所有编码里都一样,那几个散件不受影响。
