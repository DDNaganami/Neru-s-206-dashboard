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
  —— **是 git 检出**，branch `main`，origin = `https://github.com/DDNaganami/Neru-s-206-dashboard.git`。
  ★ **2026-09-21 这个检出被移动过**：从 `C:\Users\张九思\Documents\PlatformIO\Projects\Neru-s-206-dashboard`
  移到上面那个路径，为的是**和桌机同一个绝对路径**（真移动：没留副本、没建 junction，旧路径只留一行
  `README-已迁移.txt`）。移动前后 HEAD 都是 `9542efe`，仓库完好。为什么要对齐：文档/脚本里的命令
  两边通用，而且 **DSH 的工作区目录名是按工作区路径编码的** —— 两边路径一致，目录名才会一致，
  以后在笔记本上看 / 恢复会话不会因为路径错位而混乱（见 §7、§8）。
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
