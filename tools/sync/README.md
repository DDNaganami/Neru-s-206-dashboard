# tools/sync —— 桌机 → 笔记本 自动同步

这一套只干一件事:**把不能进 git 的大文件,从台式机同步到笔记本**。

- 单向,桌机是唯一数据源(笔记本上改的东西不会被传回来)。
  为什么不双向:两边都改过同一个文件时会互相覆盖,而且谁也说不清哪份是新的 ——
  双向比不自动同步还危险。
- 代码和文档**不走这条路**,走 `git push`。这里只搬“git 装不下 / 不该装”的东西。

## 实测过的网络情况(2026-09-21)

| 项 | 结果 |
|---|---|
| 台式机 Radmin 地址 | `26.177.134.224` |
| 笔记本 Radmin 地址 | **`26.253.1.139`**(脚本里 `-LaptopHost` 的默认值) |
| `ping 26.253.1.139` | 通(往返 2 ms) |
| TCP **445** | 开 ✔ —— 所以 SMB 这条路能用,**不需要**再上 Syncthing |
| TCP 139 / 22 / 22000 / 873 | 139 开;22、22000、873 关 |
| `\\26.253.1.139\c$` | 当时打不开,原因是**笔记本上还没有 `C:\Users\Public\206dash-sync` 这个文件夹**(不是权限被拒) |

换机器、或者 Radmin 重新分配了地址:**改 `-LaptopHost`**,或改脚本里那个默认值。

## 第一次使用前的两步

1. **笔记本上确认 Radmin 已连接**:两台机器都要在线、进同一个 Radmin 网络。
   笔记本上 `ipconfig` 应该能看到一个 `26.x.x.x` 的地址(就是上面那个 `26.253.1.139`)。
   台式机这边可以先自测:`ping 26.253.1.139` 或
   `powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -Test`。
2. **目标机上要有一个能写的文件夹**。两条路,选一条:

   **A. 命名共享(推荐;不需要两台机器账号一致)**
   在**笔记本**上:
   ```powershell
   # 1) 建文件夹(名字随意,这里和共享名一致最好记)
   New-Item -ItemType Directory -Force -Path 'C:\206dash-sync'
   # 2) 共享出去,并给当前账号读/写(需要管理员 PowerShell)
   net share 206dash-sync=C:\206dash-sync /GRANT:"$env:USERNAME",FULL
   ```
   也可以右键文件夹 → 属性 → 共享 → 加上自己 → 权限选“读/写”。
   以后同步都带上共享名:
   ```powershell
   powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -ShareName 206dash-sync
   ```

   **B. 管理共享 `c$`(不用建共享,但要求两台机器账号同名同密码)**
   两台机器用**同一个用户名 + 同一个密码**的本地账号登录,这样 `\\26.253.1.139\c$`
   免密可用。之后必须在笔记本上把目标文件夹建出来:
   ```powershell
   # 在笔记本上执行
   New-Item -ItemType Directory -Force -Path 'C:\Users\Public\206dash-sync'
   ```
   然后直接跑(默认就是这条路):
   ```powershell
   powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1
   ```

   账号实在不想统一,就用本次性的凭据(不落盘):
   ```powershell
   powershell -ExecutionPolicy Bypass -File sync-to-laptop.ps1 -ShareName 206dash-sync -Credential (Get-Credential)
   ```

## 命令

```powershell
$s = 'tools\sync\sync-to-laptop.ps1'   # 在仓库根目录执行;也可以写全路径

# 0) 自检:不碰网络。改了脚本先跑这个
powershell -ExecutionPolicy Bypass -File $s -SelfTest

# 1) 探路:Radmin 通了吗 / 445 通吗 / 共享能写吗(会打一张判决表 + 一句“怎么办”)
powershell -ExecutionPolicy Bypass -File $s -Test
powershell -ExecutionPolicy Bypass -File $s -Test -ShareName 206dash-sync

# 2) 真同步(默认会先自动探路,探不通就不传)
powershell -ExecutionPolicy Bypass -File $s -ShareName 206dash-sync

# 3) 挂计划任务:登录时 + 每 30 分钟(任务名 206dash-sync-to-laptop)
powershell -ExecutionPolicy Bypass -File $s -ShareName 206dash-sync -Register
powershell -ExecutionPolicy Bypass -File $s -EveryMinutes 10 -ShareName 206dash-sync -Register  # 改成 10 分钟

# 4) 不想要自动同步了
powershell -ExecutionPolicy Bypass -File $s -Unregister

# 计划任务常用操作
Get-ScheduledTask -TaskName 206dash-sync-to-laptop | Get-ScheduledTaskInfo   # 上次结果/下次时间
Start-ScheduledTask   -TaskName 206dash-sync-to-laptop                       # 立刻跑一次
(Get-ScheduledTask -TaskName 206dash-sync-to-laptop).Actions                 # 看它到底跑什么
```

退出码:`0` 成功 / `2` 参数不对 / `3` 探路没过 / `4` robocopy 失败 / `5` 自检没过。

## 同步过去什么

| 笔记本上 | 内容 |
|---|---|
| `data\` | `van_capture_dm.csv`、`drive5min.csv`(真车抓包)、`image-v3.bin`、`theme-user.json`(烧机那两个) |
| `tools\` | `capture-van-nopy.ps1`、`obd-log.ps1` —— 笔记本上拿到就能直接跑 |
| `206dash-transfer.zip` | 现成的传输包(桌机上有就带,没有就跳过) |
| `repo-snapshot.zip` | **脚本现场打包**的源码快照(不含 `.git`/`.pio`),给没装 git 的机器用 |

目标目录是**镜像**(`robocopy /MIR`):桌机没有的文件,笔记本那边会被删掉。
所以那个文件夹是脚本的地盘,**别往里放自己的东西**。桌机上的源文件永远不会被删。

## 故意不同步什么(这是重点)

| 不同步 | 为什么 |
|---|---|
| `.pio` / `.pio-core` / `.tools` | 共 186 MB+ 的编译缓存和工具链,机器相关,传过去既慢又没用 —— 这是这类脚本最经典的一个坑 |
| `.git` | 代码走 `git push`,不需要再搬一次仓库 |
| `__pycache__` / 各种 venv | 编译缓存,机器相关 |
| `probe` / `stale-tmp-tests` | 台机上的临时试验品,笔记本用不上(它们都留在桌机上,没有被动过) |
| 笔记本 → 桌机的任何东西 | 单向。笔记本上的新数据要**人工**拷回来 |

## 出问题先看这里

先跑 `-Test`,它会把原因分成三类,每类给一句能照做的事:

| 判决里看到 | 意思 | 下一步 |
|---|---|---|
| `SMB 端口 445` FAIL | Radmin 没连 / 防火墙没放行“文件和打印机共享” / 笔记本没开共享 | 笔记本上确认 Radmin 在线;防火墙放行文件共享;`net share` 看一眼 |
| `共享可读` FAIL,提示“文件夹不存在” | 445 和权限都没问题,只是那个文件夹还没建 | 笔记本上按上面 A 或 B 建好文件夹(或共享) |
| `共享可读` FAIL,提示“UNC 被拒” | 账号/权限问题 | 把共享权限给当前账号,或两台机器同名同密码,或加 `-Credential` |
| `共享可写` FAIL | 共享是只读的 | 笔记本上把共享权限从“只读”改成“读/写” |
| `ping` 只有 WARN | 对方防火墙吞了 ICMP,但 SMB 还能走 | 不用管,看 445 那行 |

其它:

- **robocopy 退出码**是位标志:`0`(没事可做)/ `1`(复制了)/ `2`(清了多余)/ `3`(1+2)
  **都算成功**;`≥8` 才是失败。脚本已经翻译好了,别被 `1` 吓到。
- **`-Credential` 不能配计划任务**:计划任务没法安全地存密码。想让自动同步带凭据,
  先把两台机器的账号统一(方案 B),再 `-Register`。
- **`-Register` 被策略拦**:开一个管理员 PowerShell 再跑那条命令。平时(探路/同步)不需要管理员。
- 同步中途断了是安全的:下次跑会接着传(`/MIR` 只补差异)。桌机这边的源文件始终只读。
