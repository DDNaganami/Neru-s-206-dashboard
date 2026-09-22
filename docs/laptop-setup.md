# 笔记本（MikamoNeru）开发环境

> 2026-09-21 实地跑通并核实。**本文的命令都是在笔记本本机上跑的**（不是桌机）。两台机器之间那条 SSH 通道见 §6。

## 1. 笔记本现状（实测）

| 项 | 值 |
|---|---|
| 主机名 / 用户 | `MikamoNeru` / `张九思` |
| Radmin 地址 | `26.253.1.139`（sshd 22，**只认密钥**，见 §6） |
| Windows PowerShell | **5.1.26100.9444** |
| Python | **3.14.7**（`python` 与 `py` 都有） |
| pip | **26.2.1** |
| Node | **v24.14.0** |
| git | **2.55.0.windows.5** |
| PlatformIO | **6.2.0**（`python -m platformio`，见 §2） |
| C: 余量 | **65.1 GB** |

装在哪、有什么：

- 项目副本：`C:\Users\张九思\206Dash\Neru-s-206-dashboard`
  —— **是 git 检出**，branch `main`，origin = `ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git`
  （**SSH over 443**；2026-09-21 当晚从 HTTPS 改过来的，配置与复原步骤见 §10）。
  ★ **2026-09-21 这个检出被移动过**：从 `C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard`
  移到上面那个路径，为的是**和桌机同一个绝对路径**（真移动：没留副本、没建 junction，旧路径只留一行
  `README-已迁移.txt`）。移动前后 HEAD 都是 `9542efe`，仓库完好。为什么要对齐：文档/脚本里的命令
  两边通用，而且 **DSH 的工作区目录名是按工作区路径编码的** —— 两边路径一致，目录名才会一致，
  以后在笔记本上看 / 恢复会话不会因为路径错位而混乱（见 §7、§8）。
- ⚠ **这个检出里有一处未提交的本地改动：`M src/main.cpp`（+49 / −9，内容是启用 OBD 串口）**
  —— **不要动它、不要提交、不要还原**（实测不影响 `git merge --ff-only`；详见 §10.7）。
- `C:\206dash-data\` —— 真车抓包 + 烧机素材（见 §4）。
- **仓库根目录有 26 个未跟踪文件**：`setup-radmin-remote.ps1`、`expose-dsh-gui.ps1`、`fix-route-metric.ps1`、`install-standalone-openssh.ps1`、`*.sshd.log`、`sshd-diagnose.log`… 都是当初搭这条 SSH 通道的现场。
  ★ **别删**。已核对过：这些文件名与仓库树**不冲突**，所以 `git pull` 不会覆盖它们（真要撞上 git 也会先拒绝，不会闷头删）。
- **没有** `C:\Users\Public\206dash`；**没有** zig / MinGW；**没有**桌机那套 `.pio-core` / `.pio-pylibs`。

## 2. 环境是怎么装的（走的干净路线，不是拷贝）

笔记本本来就有 Python + pip + 外网 ⇒ 直接装，**没有**从桌机拷工具链：

```
python -m pip install -U platformio
```

装到 `C:\Users\张九思\AppData\Local\Python\pythoncore-3.14-64\Lib\site-packages\platformio`。

★ **笔记本没有设 `PLATFORMIO_CORE_DIR`、也没有设 `PYTHONPATH`** ⇒ PlatformIO 用默认的 `%USERPROFILE%\.platformio`。
桌机那套（`PLATFORMIO_CORE_DIR=C:\Users\Public\206dash\.pio-core` + `PYTHONPATH=...\.pio-pylibs`）是**桌机自己的历史包袱**，笔记本上别抄、也不需要。

★ `...\pythoncore-3.14-64\Scripts` **不在 PATH 上** ⇒ 笔记本上**没有** `pio` / `platformio` 这个命令。一律写成 `python -m platformio`。

## 3. 在笔记本上编译 / 测试

固件（能编的都在这三个）：

```
cd C:\Users\张九思\206Dash\Neru-s-206-dashboard
python -m platformio run -e esp32s3        # 主力
python -m platformio run -e esp32s3-spi    # 两个显示 env，都 extends esp32s3
python -m platformio run -e esp32s3-rgb
```

拉代码：`git -C C:\Users\张九思\206Dash\Neru-s-206-dashboard pull --ff-only`
（origin 是 SSH over 443，见 §10。⚠ **在一条 SSH 会话里派生 `git fetch` / `pull` 会卡在数据阶段** ——
要在笔记本本机跑，或者用一次性计划任务，见 §10.4。）

✗ **`native` / `pcpreview` 在笔记本上编不过** —— 这两个 env 是 `platform = native`，要**宿主机 C 编译器**，笔记本上没有。
（桌机是靠 `C:\Users\Public\206dash\.tools\zigbin` 里那套 `cc/gcc/c++/g++.exe` 代理。★ 那几个代理把 zig 的路径**硬编码**成 `C:\Users\Public\206dash\.tools\pyzig\ziglang\zig.exe`，所以**光把 `zigbin` 拷过来没用**，必须连 zig 本体一起放到那个绝对路径上。）
想补，最省事的是装个 MinGW-w64，它直接提供 `gcc` / `g++`：

```
winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
```

装完新开一个终端（PATH 要刷新），再 `python -m platformio test -e native`。
**在那之前，`native` 上的那套单元测试只能在桌机上跑** —— 别把它当成笔记本的 bug 去查。

## 4. 数据（`C:\206dash-data\`）

SMB 共享那条路是坏的（见 §5），所以是直接从桌机 `scp` 过来的，四个文件 SHA256 都对过、与桌机源文件逐字节一致：

| 文件 | 字节 | SHA256（前 16 位） |
|---|---|---|
| `drive5min.csv` | 34,303,713（32.72 MiB） | `680026A89262CC21` |
| `van_capture_dm.csv` | 3,091,431（2.95 MiB） | `A97AC040EDAD85ED` |
| `image-v3.bin` | 560,524（547 KiB） | `C823BE26C239C4FF` |
| `theme-user.json` | 1,385 | `953ABD5759AB4534` |

合计 **37,957,053 B ≈ 36.20 MiB**。
前两个是真车抓包（5 分钟行车 / 30 秒静止）；后两个是**已经刷进 213 号板**的素材与主题。

★ 数据**故意不放** `C:\206dash-sync\`：那是共享文件夹、又是桌机同步脚本的 **robocopy `/MIR` 目标** —— 放进去等于下次同步成功时被删掉。

## 5. 还没弄好的 / 要人动手的

1. **桌机→笔记本那条 SMB 同步（`206dash-sync`）是坏的，得人来修。**
   计划任务 `206dash-sync-to-laptop` 状态 `Ready`，但上次结果是 **3** = 脚本自己的 `$EX_TEST` = **探路就没过**，压根没走到 robocopy（所以没有 `robocopy.log`，`C:\206dash-sync` 一直是空的）。
   根因不是"文件夹不存在"（脚本是这么猜的，猜错了），是 **SMB 认证被拒**：桌机 `net view \\26.253.1.139` → **系统错误 5，拒绝访问**，**所有**共享（`206dash-sync` / `scan` / `Users` / `C$`）都进不去。
   因为笔记本上 `张九思` 这个账号 `PasswordRequired=False`（**无密码**），而笔记本 `HKLM\SYSTEM\CurrentControlSet\Control\Lsa\LimitBlankPasswordUse = 1` —— Windows **不允许无密码账号走网络登录**。
   笔记本这侧的共享是没问题的：`206dash-sync → C:\206dash-sync` 已共享、LanmanServer 在跑、17 条文件共享防火墙规则都开着、ACL 给了 `Everyone` 完全控制。
   ⇒ 要么给这个账号设个密码（然后**得重新注册任务**让凭据对上 —— 脚本故意不肯把密码塞进计划任务），要么把 `LimitBlankPasswordUse` 改成 0（降安全性）。**两条都要人动手，且都要提权。**
2. **`native` / `pcpreview` 编不了** —— 见 §3，要装宿主机编译器。
3. 提权 / GUI 的事（本轮**都没做**，也没猜）：给账号设密码、改 `LimitBlankPasswordUse`、重新注册计划任务、装 MinGW。
4. 笔记本上没有 `C:\Users\Public\206dash`，也没走"拷桌机 `.pio-core`"那条备用路线 —— 不需要，pip 那条路已经通了。
5. **PlatformIO 那个 259 MB 的 `toolchain-riscv32-esp` 下载中途断掉了**（`IncompleteRead`，42/216 MB）
   —— **原因尚未查清**。★ **不要**把它当成和 GitHub 同因：那是 PlatformIO 自己的包下载（§2 那条
   `python -m pip install -U platformio` 的路线），和 git-over-ssh 那条链是两回事。要接着查就单独查。

## 6. SSH 通道

桌机 → 笔记本：

```
ssh -i $env:USERPROFILE\.ssh\dsh_laptop 张九思@26.253.1.139
scp -i $env:USERPROFILE\.ssh\dsh_laptop <本地> 张九思@26.253.1.139:'C:/目标路径'
```

该账号**没有密码**，只认 `dsh_laptop` 这把密钥（密钥本身无口令）。
★ 远程命令里**别用双引号** —— Windows OpenSSH 会把它们吃掉，把远程 PowerShell 语法搞坏。要跑多行就用
`powershell -NoProfile -EncodedCommand <base64(UTF-16LE)>`。

⚠ **留个记录：私钥 `dsh_laptop` 曾经落在共享文件夹里（暴露过一次）。**
本轮没有动 `~/.ssh`、没有改 sshd 和防火墙。**以后要轮换这把密钥** —— 换的时候两台机器一起换，别只换一头。

## 7. 移动 / 重命名这个 checkout 之前必须知道（2026-09-21 真踩过）

- **先把笔记本上的 VS Code（或 PlatformIO IDE 会话）关掉，再移动。**
  `.pio\libdeps` 被 PlatformIO 的进程占着时，**整棵目录都改不了名**，报
  “另一个程序正在使用此文件，进程无法访问”。实测：`.git`、`src`、`tools` … 子目录一个个都能改名，
  **只有根目录不行** —— 因为“祖先目录里有被占用的子孙”时，祖先本身也动不了。
- 还有一类**看不出来**的占用：某个进程的**当前目录就在仓库里**（VS Code 的集成终端、
  一个 `Set-Location <仓库>; platformio run …` 的包装脚本、或者**从仓库目录启动的 DSH**）。
  它不锁任何文件，但锁住目录本身，改名照样失败。确认（能改名 = 没被占，它会立刻改回来）：

  ```powershell
  Rename-Item -LiteralPath 'C:\Users\张九思\206Dash\Neru-s-206-dashboard\.pio' -NewName '.pio.probe'
  Rename-Item -LiteralPath 'C:\Users\张九思\206Dash\Neru-s-206-dashboard\.pio.probe' -NewName '.pio'
  Get-Process Code,pio* -ErrorAction SilentlyContinue
  ```

- 移动之后**别留副本、别建 junction** —— 两份 checkout 比一份难管得多。

## 8. 在笔记本的 DSH 里看桌机那边的会话（2026-09-21）

- 桌机的同步脚本会把**最新会话记录**放到 `C:\206dash-data`：
  `session-<会话目录名>.jsonl.zstd`（原件）+ `对话记录.jsonl`（解压出来的可读版）。
  那是**给阅读 / 检索**用的，不动笔记本的 `.dsh`。
- 要在**笔记本的 DSH** 里看到这次对话，会话文件要放在
  `C:\Users\张九思\.dsh\sessions\--C-Users-~5F20~4E5D~601D-206Dash--\session-49039670-ec47-46bb-8eb0-30e765c65e42\session.v3.jsonl.zstd`。
  工作区目录名 = `C:\Users\张九思\206Dash` 编码出来的名字，**两边对齐之后就是同一个名字**；
  会话目录名**照台式机原样镜像**（DSH 自己建的目录名带 `session-` 前缀，文件里 `id` 字段也是同一个值）。
- **前提**：笔记本上的 DSH 必须**从 `C:\Users\张九思\206Dash` 启动**，会话才会出现在那个工作区下。
- ⚠ **打开历史看可以，别在这个会话上接着跑** —— 两台机器的串口 / 设备 / 路径都不一样，
  在搬过来的会话上继续执行容易把两边状态搅乱。要看就**新开**一个会话。
- 只复制 `session.v3.jsonl.zstd`，**不要**复制 `storages\session_projcache\sessions\<id>.json`：
  查过 DSH 自己的包文档（`@deepseek-ai/dsh-session-projection-cache`）——“The session log remains
  authoritative”，那只是冷会话列表用的投影缓存，缺失 / 不兼容会被忽略或重建。
- ⚠ 反过来说：**从仓库目录启动的 DSH 进程会锁住仓库目录**（见 §7）—— 要移动仓库前先关它。

## 9. 误归档了怎么办（2026-09-21 真发生、已修复）

**DSH 的归档是单向的，没有取消归档功能** —— 这一条是查 DSH 自己的文档确认的（原文）：

- `node_modules\@deepseek-ai\dsh-workspace\README.zh.md:163`：
  「**归档是单向的**——被隐藏的会话保留其历史与位置，但目前没有取消归档操作；归档集合是持久的显示过滤器。」
- `node_modules\@deepseek-ai\dsh-client-ui-workspace\README.zh.md:109`：
  「**没有 Session 删除与取消归档控件**：会话可以归档，但已归档会话没有查看或取消归档入口；
  删除 Workspace 注册记录不会删除 Session。」

也就是说 **UI 里点不出"取消归档"** —— 只能改状态文件：

- 文件：`%USERPROFILE%\.dsh\storages\workspace.json` → `global.archivedSessionIds`（**只存 session id**）。
- 2026-09-21 笔记本那份里被归档的正是**当前会话** `session-49039670-ec47-46bb-8eb0-30e765c65e42`
  （它在 DSH 列表里"消失"就是这么来的）；台式机那份里归档的是旧会话
  `session-f924a386-da82-46dd-bd44-2e33a107c10f`（**没动**，只做了备份）。

修的顺序**不能乱**：

1. **先停掉 DSH 进程**。运行中改文件没用 —— 内存里那份旧集合会被写回去。
   （这也是 §7 那条"搬动 / 改 `.dsh` 之前先关 DSH"的另一半原因。）
2. **备份**：`Copy-Item workspace.json workspace.json.bak-<yyyyMMdd-HHmmss>`。
3. 把 `"archivedSessionIds": [ … ]` 清成 `[]`。
4. **写文件必须不带 BOM**：`[System.IO.File]::WriteAllText($p, $json, (New-Object Text.UTF8Encoding($false)))`。
5. **校验**：`([IO.File]::ReadAllText($p) | ConvertFrom-Json).global.archivedSessionIds.Count` 回读应为 `0`。

> ★ 读这个文件也要按 UTF-8 读：它**没有 BOM**，用 PowerShell 5.1 的 `Get-Content -Raw` 会按 ANSI(936)
> 解出乱码（实测 `C:\Users\寮犱節鎬漒\206Dash` 这种），`ConvertFrom-Json` 还会报
> "Unrecognized escape sequence"。一律用 `[IO.File]::ReadAllText()`。
>
> ★ 顺带一个坐标：笔记本 `workspace.json` 里注册的工作区 `path` **还是旧路径**
> （`…\Documents\PlatformIO\Projects\Neru-s-206-dashboard`）。所以重启 DSH 时**工作目录要选
> `C:\Users\张九思\206Dash`**，否则会话不会出现在对齐后的那个工作区下（见 §8）。

### 这台笔记本上另两条环境坑（都踩过）

- **sshd 对 exec 命令行有长度限制**：`powershell -EncodedCommand <base64>` 到 **2136 字符**就报
  `exec request failed on channel 0`；**1024 字符正常**。
  ⇒ 长脚本要**落地成文件再跑**（`scp` 上去 + `-File`），别整段塞进 ssh 命令行。
  （本仓库同步脚本的远端命令都远短于这个长度，所以没受影响。）
- **"SSH 注册计划任务 → 紧接着启动"那次，powershell.exe 起进程被拒**
  （报「程序无法运行: 拒绝访问」），而同一分钟里下一条一模一样的 `-EncodedCommand` 却正常
  ⇒ 看着是**偶发拦截**。**长驻进程别走这条路，让用户本地启动。**

## 10. 笔记本的 GitHub 通道（已修好，2026-09-21 当晚实测）

> ★ **先更正一条错结论**：以前写的“笔记本连不上 GitHub / 笔记本那边没有到 GitHub 的通道”
> **是错的**。笔记本侧的网络一直是好的，坏的只是这个 checkout 的配置。
> 误判来自**嵌套 SSH 会话**（见 §10.4）—— 当时的判断是错的，原因是嵌套 SSH 会话造成的假象。

### 10.1 笔记本侧的网络本身是好的（实测）

| 域名 | 笔记本解析 | 443/TCP |
|---|---|---|
| `github.com` | `20.205.243.166` | 通 |
| `ssh.github.com` | `20.205.243.160` | 通 |
| `api.github.com` | `20.205.243.168` | 通 |
| `codeload.github.com` | `20.205.243.165` | 通 |

- **解析不是污染**：四个都是 GitHub 的真实地址。（台式机那边 `github.com` / `api.github.com` /
  `codeload.github.com` 都被解析成 `127.0.0.1` —— 所以台式机只能走 SSH-over-443；笔记本这边是干净的。）
- 从笔记本 `ssh.github.com:443` 读到的 SSH 横幅是 **`SSH-2.0-af8ca74`**，与台式机读到的
  **完全一致** ⇒ 没有中间人。
- 路径 MTU 与台式机一致：DF ping 载荷 **1452 通**、**1472 需要分片**；各接口 `NlMtu` 都是 **1500**。
- 到 GitHub 的 IP 段（20/140 开头）**没有任何 VPN 路由**；两端都走 WLAN。

### 10.2 原来的 `exit 6` 是这三个原因（和“没通道”无关）

1. 笔记本的 remote 当时是 **HTTPS**（`https://github.com/DDNaganami/Neru-s-206-dashboard.git`）
   —— TLS 里带 `github.com` 的 **SNI 被 RST**，报 `Recv failure: Connection was reset`。
   （被重置的是**这一条 HTTPS 连接**，不是“GitHub 不可达”。）
2. 笔记本 `~/.ssh` 里**当时只有 `dsh_desktop` 一把密钥，没有 GitHub 密钥**
   （现在是 `dsh_desktop` + `id_ed25519` 两把 + `known_hosts`）。
3. git 默认用的 ssh 是 **Git 自带那套**（`C:\Program Files\Git\usr\bin\ssh.exe`），
   它读的 HOME/.ssh 与 Windows OpenSSH 不是一个 ⇒ 报 `Host key verification failed`。

> **台式机为什么“看起来能连”**：台式机上 `github.com` 和 `api.github.com` 被解析成 **`127.0.0.1`**
> （域名被污染），所以台式机走的是 `ssh://git@ssh.github.com:443` 这条 **SSH-over-443** 绕行；
> **笔记本 DNS 干净，本来就能直连 GitHub**，只是 remote 写错了协议。

### 10.3 现在的配置（实测读回）+ 验证命令

| 项 | 值 |
|---|---|
| `remote.origin.url` | `ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git` |
| `core.sshCommand` | `C:/PROGRA~1/OpenSSH/ssh.exe -o BatchMode=yes -o StrictHostKeyChecking=accept-new`（**只有一行**） |
| GitHub 私钥 | `%USERPROFILE%\.ssh\id_ed25519`（+ `.pub`），与台式机私钥 SHA256 前 16 位一致：**`3FFE2C9BE5658ED7`** |
| `known_hosts` | `[ssh.github.com]:443` 的 rsa / ecdsa / ed25519 三条（写入用 `Add-Content`，实测**无 CRLF 问题**） |
| 能用的 ssh | **`C:\Program Files\OpenSSH\ssh.exe`**（下面那张表是另外两个的下场） |
| HEAD | `a2c03ed`（与台式机一致，2026-09-21 当晚快进到位） |

笔记本上三个 `ssh.exe`，只有一个能用：

| 可执行文件 | 结果 |
|---|---|
| `C:\Program Files\OpenSSH\ssh.exe` | ✅ 正常（`core.sshCommand` 指的就是它，shell PATH 里也是它） |
| `C:\Windows\System32\OpenSSH\ssh.exe` | ❌ **会挂死**（连 stdin 接 NUL 也挂） |
| `C:\Program Files\Git\usr\bin\ssh.exe` | ❌ 认证不过（读不到我们的密钥）⇒ `Host key verification failed` |

验证命令（**都在笔记本本机上跑**，不要在桌机 `ssh` 过来的那条会话里跑 —— 原因见 §10.4）：

```powershell
$r = 'C:\Users\张九思\206Dash\Neru-s-206-dashboard'
git -C $r config --get-all core.sshCommand      # 应只有一行
git -C $r config --get-all remote.origin.url    # 应是 ssh://git@ssh.github.com:443/...
ssh-keygen -lf "$env:USERPROFILE\.ssh\known_hosts"          # 三条指纹
& 'C:\Program Files\OpenSSH\ssh.exe' -T -p 443 git@ssh.github.com   # 应回 "Hi <用户名>! You've successfully authenticated..."
git -C $r fetch --prune origin                  # 应成功并更新 origin/main
```

三条主机密钥指纹（笔记本 `known_hosts` 里读出来的，**与 GitHub 官方公布的一致**）：
`SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU`（ed25519）、
`SHA256:p2QAMXNIC1TJYWeIOttrVc98/R1BUFWu3/LiyKgUfQM`（ecdsa）、
`SHA256:uNiVztksCsDhcc0u9e8BujQXVUpKZIDTMczCvj3tD2s`（rsa）
（官方页面：<https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/githubs-ssh-key-fingerprints>）。

### 10.4 ★ 嵌套 SSH 会话会在数据阶段卡死（最坑的一条，误判就是它造成的）

**在一条 SSH 会话里再派生 `ssh` / `git fetch`：认证会成功、远端 `git-upload-pack` 已经启动、
SSH 通道也 open confirm 了，然后一个字节都不再回来** —— `-vvv` 日志停在
`channel 0: open confirm rwindow 32000 rmax 35000`。

- 实测**三种派生方式都挂**：① 内联调用 ② `Start-Process` 重定向 ③ `Start-Process cmd` + stdin 接 NUL。
- 同一个 ssh 手动加 `-T` 只用了 **0.6 秒**就通 ⇒ 密钥、网络、主机密钥全都是好的。
- ★ 把同一条 `git fetch` 放到**会话之外**（一次性计划任务）**立刻成功**，日志
  `face40c..a2c03ed  main -> origin/main`；笔记本 HEAD 随即变成 **`a2c03ed`**
  （`git reflog` 里是 `merge origin/main: Fast-forward`，`.git/FETCH_HEAD` 指向
  `ssh://ssh.github.com:443/DDNaganami/Neru-s-206-dashboard`）。

⇒ **规矩：不要用嵌套 SSH 会话去诊断或执行长时间的 git-over-ssh 操作。**
要么让用户在笔记本本地跑，要么用一次性计划任务（`Register-ScheduledTask` + `Start-ScheduledTask`，
跑完 `Unregister`，形状见 §10.6）。
**“笔记本没有到 GitHub 的通道”这个误判就是这么来的 —— 是嵌套 SSH 会话造成的假象。**

### 10.5 换机 / 重装时的复原步骤

1. **remote 换成 SSH over 443**：
   `git remote set-url origin ssh://git@ssh.github.com:443/DDNaganami/Neru-s-206-dashboard.git`
2. **放 GitHub 私钥**：把台式机的 `~/.ssh/id_ed25519`（+ `.pub`）复制到新机
   `%USERPROFILE%\.ssh\id_ed25519`，并收紧 ACL（只留自己可读）：
   `icacls <该文件> /inheritance:r /grant:r "$env:USERNAME:R"`
   核对：两端私钥 SHA256 前 16 位应都是 **`3FFE2C9BE5658ED7`**。
3. **写 `known_hosts`**：`ssh-keyscan -p 443 -t rsa,ecdsa,ed25519 ssh.github.com`
   （追加用 `Add-Content`，实测无 CRLF 问题）；再用 `ssh-keygen -lf` 对一下 ed25519 指纹
   必须是 `SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU`。
4. **指定 ssh 可执行文件（★ 必须 `--replace-all`）**：
   ```powershell
   git config --replace-all core.sshCommand 'C:/PROGRA~1/OpenSSH/ssh.exe -o BatchMode=yes -o StrictHostKeyChecking=accept-new'
   git config --get-all core.sshCommand      # ★ 必须只有一行
   ```
   - 路径要用 **8.3 短名** 的原因：`C:\Program Files\OpenSSH\ssh.exe` 带空格，PowerShell 会把传给
     原生 exe 的引号吞掉，git 只收到 `C:/Program`（报 `cannot spawn C:/Program`）。
   - **必须 `--replace-all`** 的原因：笔记本 `.git/config` 里当时**真的有两条 `sshCommand`**，
     **最后一条生效** ⇒ 一直在用错的那条。实测（git 2.x，本机复现）：`core.sshCommand` 有**多个值**时，
     普通的 `git config core.sshCommand '<新值>'` **改不动**，直接报
     `error: cannot overwrite multiple values with a single value`，那两条原样躺着 ——
     所以只能 `--replace-all`，而且**改完不核对等于没改**。
5. **验证**：跑 §10.3 那几条（`ssh -T -p 443 git@ssh.github.com` 应回
   `Hi <用户名>! You've successfully authenticated, but GitHub does not provide shell access.`）。
6. ⚠ **验证也要在会话之外**（§10.4）：要么笔记本本地跑，要么一次性计划任务。

### 10.6 一次性计划任务（在会话之外跑 git-over-ssh 的形状）

```powershell
# 在笔记本本机上执行（不是在桌机 ssh 过来的那条会话里）；路径/日志名按实际填
$ps1 = 'C:\206dash-data\gitfetch-once.ps1'
[IO.File]::WriteAllText($ps1,
  "cd C:\Users\张九思\206Dash\Neru-s-206-dashboard`ngit fetch --prune origin *> C:\206dash-data\gitfetch.log`n",
  (New-Object Text.UTF8Encoding($false)))          # 无 BOM

Register-ScheduledTask -TaskName 206dash-gitfetch-once -Force -Action (New-ScheduledTaskAction `
  -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -File C:\206dash-data\gitfetch-once.ps1')
Start-ScheduledTask   -TaskName 206dash-gitfetch-once
Get-Content C:\206dash-data\gitfetch.log           # 期望看到 face40c..a2c03ed  main -> origin/main
Unregister-ScheduledTask -TaskName 206dash-gitfetch-once -Confirm:$false   # 跑完清理
```

### 10.7 顺带：仓库里那处未提交改动（别动它）

- `M src/main.cpp`，**+49 / −9**，内容是**启用 OBD 串口**：`OBD_SERIAL`、`OBD_RX_PIN 17`、
  `OBD_TX_PIN 18`、`kObdBaud 38400`、`attachObdSerial()`。
- ★ **不要动它、不要提交、不要还原。** 实测**不影响 `git merge --ff-only`**
  （`face40c → a2c03ed` 那次快进照过，这处改动原样留着）。
- ⚠ 但它是**已跟踪**文件的改动 ⇒ 桌机同步脚本的 gate 会因此拒绝快进、判 `refused` / 退出 `6`
  （见 `tools/sync/README.md` 的「仓库那步会先看一眼笔记本有没有本地改动」）。那是**预期行为**，
  **不要**为了让它过去而 `stash` / `checkout` 这处改动。

