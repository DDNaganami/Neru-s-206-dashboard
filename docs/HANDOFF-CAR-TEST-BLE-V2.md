# 交接单：BLE-OBD v2 车上验证（**给笔记本侧的 agent：请严格按本文件执行**）

> 写给**笔记本那边的 agent**：这一单你只做三件事 ——
> **① 按 §3 的顺序测；② 按 §6 的模板回话；③ 把判据行提交进仓库。**
> 不要自行改方案、不要在车上改宏/刷机（除非车主明确要求，且按 §7 的流程）。
>
> 桌面侧已经做完的部分、以及为什么这么做，写在 `docs/BLE-OBD.md` §14（含台面证据）。
> 本文件**不重复论证**，只给动作与判据 —— 判据行都是从今晚的真实串口日志里抄的，可以逐字 grep。

---

## 0. 一句话背景（读完就够，别扩展）

桌面侧把"启动闸门"的条件从 `#if LINK_ROLE == 1 && OBD_BLE`（只装主板）改成 `#if OBD_BLE`
（**谁背 BLE，谁的链路 PHY 就让路**），并把从板的 PHY 启动搬进闸门。
机旁用**桌面假诊断头**验证：从板上电 **6~10 秒**连上，闸门随后**提前开**，ESP-NOW 照常起，
两条路同时活着 80+ 秒（`SRC speed=link rpm=obd coolant=obd intake=obd | 2400rpm 90.0C 42.0C`）。
**车上未验 —— 这一单就是去验它。**

- 相关提交：`d66d743`（本单固件改动）+ `e48b2db`（桌面假头工具）
- **两块板上的镜像已经都是 `d66d743` 编出来的**（桌面侧刚刷过），出发前不用再刷。

## 1. 出发前（30 秒，别省）

> ★★★ **2026-09-29 凌晨注：本节的镜像尺寸与 commit 已经过期 —— 重跑前必须按下面重取。**
> 原因：从板固件在 `d66d743` 之后**修掉了一个把 BLE 建连超时写成 15 毫秒的 bug**
> （`setConnectTimeout` 的单位是毫秒，不是秒；详见 `CAR-TEST-BLE-V2-RESULT.md` 附五 ⑤.11）。
> 那正是本单 §3.2 一直判"失败"的**根因**。
> ⇒ 重跑时的正确值：
> - **从板**：`esp32s3-rgb-slave-obdtest`，`firmware.bin` = **1,708,592 B**
> - **主板**：`esp32s3-rgb-master-now`，`OBD_BLE=0`（**分工已定：主板不读 OBD**）
> - commit：**以 `git log --oneline -1` 的实际输出为准**（别再用 `d66d743` 这个字面值）
>
> ★ 预期变化：§3.2 的 `等了 XXXXms` 应从 **15000** 掉到 **1000 左右**
>   （实测 `onConnect` 用 350~969ms）⇒ 左屏"模拟值 + 数据不可信"的窗口从 15 秒缩到约 1 秒。
> ★ 现场应急（新增）：串口敲 **`t`** 可切建连超时档 {3000, 30000, 1000, 10000}，
>   **不必刷机**（这就是本单 §5「不要在车上改宏/刷机」那条纪律的配套手段）。

1. `git -C <repo> log --oneline -1` ⇒ 用它打印出来的那个 commit（**不要照抄本文里的字面值**）；
   不是最新的就先 `git pull`。
2. 板子镜像（**已刷好，不要重刷**）：
   - **从板（本单的测试对象）**：`esp32s3-rgb-slave-obdtest` —— 带 BLE + 带闸门；
     固件 **1,708,592 B**（含 2026-09-29 凌晨那个建连超时修复）
   - 主板：`esp32s3-rgb-master-now` —— **不带 BLE**，链路主发；固件 **1,497,456 B**
3. 两块板**现在是静音的**（NVS `mute=1`，桌面侧为台面测试按的 `m`）。
   上车想要蜂鸣器就发一个小写 `m`（回话 `mute: 0 (saved)`）；再发一次回到静音。
4. 带上：笔记本 + 两根 USB 线（读串口用）。★ 车上纪律见 §5。

## 2. 本单**只**判一件事

> **BLE 能不能在建连窗口里连上，并且连上之后扛得住。**

**不是**"OBD 数据对不对"（那一条今晚已经用假头在台面验过），
**也不是**"链路质量好不好"（那要顺带量，见 §3.3）。

## 3. 执行顺序（严格按这个顺序；每步都有判据）

### 3.1 台面 30 秒自检（还在桌上、还没拔线时做）

1. 两块板各抓 12 秒（**必须用这个脚本**：它按 UTF-8 解码；用别的抓法中文会变成 `??????`）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\serial-capture\watch-utf8.ps1 `
       -Port COM8 -Seconds 12 -Out .\pre-slave.log
   powershell -ExecutionPolicy Bypass -File tools\serial-capture\watch-utf8.ps1 `
       -Port COM9 -Seconds 12 -Out .\pre-master.log
   ```

2. **认板子永远看 `role=`，不许靠 COM 号**。要看到：

   | 板子 | 必须出现的行 |
   |---|---|
   | 主板 | `role=MASTER` **且** `obd: 本机未启用(-DOBD_SERIAL=0,-DOBD_BLE=0)` |
   | 从板 | `role=SLAVE` **且** `obd: BLE 那条路已挂上(目标服务 FFF0 / 通知 FFF1 / 写 FFF2)` |

   ⇒ **对不上就停下**，把这两行原文发回桌面侧，别继续。

   > 读日志时带 `-Encoding UTF8`（文件是板子原始输出）：
   > `Get-Content .\car-on.log -Encoding UTF8 | Select-String '启动闸门|state=|SRC '`

   > 注：`role=` 那行只在开机时打一次。抓不到就按一下板子 RST（或重新上电）再抓。

### 3.2 车上：**先只拧到 ON，不发动**

1. 两块板照原样接车上（**别动线**），笔记本连**从板**的 USB。
2. 抓 60 秒：`watch-utf8.ps1 -Port <从板口> -Seconds 60 -Out .\car-on.log`
3. **判据（只看这三行）**：

   | 结果 | 串口上应当出现的原文 |
   |---|---|
   | ✅ 成功 | `link: 启动闸门开了 —— BLE 已连上(ready),等了 XXXXms ⇒ 现在启链路 PHY(射频让出来了)` |
   | ❌ 失败 | `link: 启动闸门开了 —— BLE 没连上(到 15s 上限:仪表优先,不再等 OBD),等了 15000ms ⇒ …` |
   | 连上没连上 | `obd-ble: state=ready … conn=1` **或** `obd-ble: state=connecting … fails=N lastFail=0x..` |

   - `XXXX`（等待毫秒）就是本单最重要的数字：**5000~15000 之间的任意值都算成功**
     （越小越好；它是"BLE 多久建好连"）。
   - 顺带记下：`obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready` 出现的时间。
   - ❌ 失败时**必须原样抄下这一整行**（这是这一单最有价值的产物）：
     `obd-ble: state=connecting peer=… conn=0 drop=0 connects=0 cs=A/A radio=… win=W cap=C fails=F lastFail=0xNN`
4. **成功 → 进 §3.3；失败 → 直接跳 §7**（不要在车上刷机）。

   > 预期现象（不是故障）：上电头 **6~15 秒**左屏是模拟值 + "数据不可信"角标，
   > 因为闸门就是在这段时间里把射频让给 BLE。

### 3.3 发动 + 开一小段（5~10 分钟）

1. 抓四段（各 60 秒）：发动瞬间的从板、开 5 分钟后的从板、主板两段。
2. **判据**：

   - **维持**：`obd-ble: state=ready … conn=1 drop=0` 是不是**一直**保持。
     `drop=` 一变就记下（涨几次 + 大约在什么时刻 + 当时在做什么）。
   - **数据**：从板 `SRC speed=link rpm=obd coolant=obd intake=obd | …`
     （左屏转速/水温来自 OBD）；主板右屏的进气应经链路来
     （主板侧看 `link: B … intake=NN`，`intake` 不再是 0）。
   - **共存质量**（这一条今晚没量过，是本单的附加产出）：
     `link: locked tick_age=…ms`、`espnow: tx_fail=/done_fail=/overflow=`、
     `meas rx:` 那行的 p99 / 丢包。
     ★ 必须**两种状态各抓一次**（BLE 连着 / BLE 没连上）才有对比意义。
   - ★ **出现任何 `boot: reason=` 都要抄下来**（尤其 `reason=TASK_WDT` / `reason=WDT` / `PANIC`），
     并记下上一轮跑了多久（`prev_up=`）。桌面侧台面上见过**一次**硬 WDT
     （条件：对端一直广播却每次拒绝连接，跑了约 10 分钟；之后 100 秒未复现，见 §14.7 末尾）。
3. 别在现场调参。**只记录，不修**。

## 4. 台面那两条坑（会让人误判成"板子连不上"）

1. **桌面假头一个进程只接一次连接**：客户端一走开（板子复位/重刷），这个实例就再也接不上
   第二个（一直 `0x0D`，而广播还在），`stop/start` 广播也救不回来。
   ⇒ 台面上每次重启板子**之前**先重启 `tools/bt-obd/obd-ble-sim.py`。
   ⇒ 也因此：**车上"连不上"绝不能用台面那组 277/277 来类比**（那组已被标为受污染，见 §14.5）。
2. **抓串口必须用 `watch-utf8.ps1`**（否则中文全是 `?`，判据行读不出来）。

## 5. 车上纪律（与本单无关，但必须守）

- 笔记本**只用电池**（连着车上时不要插车充/点烟器）。
- **绝不允许两个 5V 源同时喂一块板**。
- 车地**只在一处**接。
- 模块 `TX` 不许悬空；该接 `3.3V` 的照旧。
- 认板子只看 `role=`，**永远不要靠 COM 号**。
- 别碰 `tools/theme-editor/theme.json`；别跑 `pio clean`。
- **不要在车上改宏/刷机。**

## 6. 回话格式（照这个模板填，别写散文）

回话**就是**一个文件：`docs/CAR-TEST-BLE-V2-RESULT.md`，内容按下面填空，然后把日志提交进
仓库（见 §6.2），最后 `git push`。

### 6.1 模板

```md
# BLE-OBD v2 车上验证结果（<日期> <时间>，笔记本侧）

树版本：<git rev-parse --short HEAD>

## 1. 台面自检
- 主板：COM? role=<MASTER>  行：<原样粘贴>
- 从板：COM? role=<SLAVE>   行：<原样粘贴>

## 2. 只拧到 ON（关键判据）
- 闸门那一行（原样）：<粘贴>
- 结果：<成功 / 失败>
- 若成功：等待 XXXX = <数字> ms；`特征已配齐…state=ready` 出现在第 <N> 秒
- 若失败：`obd-ble: state=connecting …`（原样整行）：<粘贴>
- 其它 `obd-ble:` 行（前 10 行）：<粘贴>

## 3. 发动 + 行驶
- BLE 是否维持：<是/否>；`drop=` 变化：<无 / 几次 + 时刻>
- 从板 SRC 行（每次一段，2~3 行）：<粘贴>
- 主板 link: B 行（2~3 行）：<粘贴>
- 共存质量（BLE 连着时）：`link: locked tick_age=` <值>；`espnow:` <粘贴>；`meas rx:` <粘贴>
- 共存质量（BLE 没连上时，如果有）：<同上>
- 任何 `boot: reason=`：<无 / 原样粘贴 + prev_up=>

## 4. 现象（车主视角，可选）
- 左屏：<正常 / 模拟值+角标多久 / 重启过 / 其它>
- 蜂鸣器：<静音 / 响过（何时）>

## 5. 结论（只填一句）
- <BLE 建连成功且维持 / 建连失败 / 建连成功但没维持 / 其它>
```

### 6.2 日志怎么进仓库

1. 每份抓到的原始日志裁成"判据行"（**保留原文，不要改写**），放到
   `tools/bt-obd/captures/car-<日期>-<标签>.log`，
   每个文件头加两行 ASCII 注释：这一份是什么、什么时候抓的。
   参考已有的 `tools/bt-obd/captures/bench-2026-09-28-*.log` 的格式。
2. 连同 `docs/CAR-TEST-BLE-V2-RESULT.md` 一起 `git add` + `git commit` + `git push`。
3. **别把整份 90KB 的原始日志提交**（判据行 + 关键上下文就够）。

## 7. 如果失败了（**先别动手，按这个顺序**）

1. **先不要刷机、不要改宏**。把 §3.2 那几行原样发回桌面侧 —— 失败也是有效结果，
   而且这次带回的是**真错误码**（`lastFail=0xNN`）与仲裁计数（`win/cap`），比车上现推值钱。
2. 如果车主**当场就想要 OBD 数据**（不想等下一版），才考虑切回今天已经跑过的那一对：
   **主板读 OBD（`-DOBD_BLE=1` + `-DRGB_BOUNCE_LINES=10`）+ 从板不带 BLE（`esp32s3-rgb-slave-now`）**。
   ★ 走这条路必须做到两条（都是踩过的坑）：
   - 改完宏 **必须验证镜像真的重编了**（看 `firmware.bin` 的大小/时间戳变化，并在开机日志里
     确认 `obd: BLE 那条路已挂上…` 真的出现；**别只看 `[SUCCESS]`**）；
   - 两块板**同源**、**同批刷**（先刷主板、确认能连上，再刷从板）。
3. 切回去之后，把"为什么没连上"的证据仍然按 §6 交回来（**两件事都要做**，别只做第二件）。

## 8. 可选：在笔记本上复现桌面侧那一轮（台面）

只有板子还在桌上、并且你想先自己复现一遍时才做（**要用笔记本的蓝牙当假诊断头**）：

```powershell
# 1) 装 pywinrt（桌面侧实测 cp314 有轮子；Python 3.9+ 都行）
python -m pip install winrt-runtime winrt-Windows.Devices.Bluetooth `
  winrt-Windows.Devices.Bluetooth.GenericAttributeProfile winrt-Windows.Devices.Bluetooth.Advertisement `
  winrt-Windows.Storage.Streams winrt-Windows.Foundation winrt-Windows.Foundation.Collections
# 2) 让笔记本冒充诊断头（另开一个窗口；★ 每次板子重启前先重启它，见 §4 第 1 条）
python tools\bt-obd\obd-ble-sim.py --mode idle --rpm 2400 --coolant 90 --intake 42 --speed 55
# 3) 复位从板，抓 40 秒串口，期望：
#    obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready
#    link: 启动闸门开了 —— BLE 已连上(ready),等了 XXXXms
#    SRC speed=link rpm=obd coolant=obd intake=obd | …2400rpm 90.0C 42.0C…
```

★ 笔记本的蓝牙适配器必须是"支持外设角色"的（桌面侧 Intel 是 True）；
不支持就跳过这一节，直接做 §3 —— **车上那一轮才是本单的目的**。

---

## 9. 主动扫描（`setActiveScan(true)`）的去留 —— **桌面侧的判定**（2026-09-29）

> 这一行是 B 方案留下的实验改动，`f0e3214` 明确写"去留需桌面侧决定"。判定的结论是：

**① 今天重跑车上验证单时：先别动它。** 今天要验的是"修好的建连超时"，
一次只改一个变量 —— 现在镜像里是"主动扫描 + 修好的超时"，也正是台面上**连上真头**
（`real-dongle-CONNECTED` / `unitsfix-default-timeout`）的那一版。

**② 车上跑完之后，用下面 5 分钟的台面 A/B 决定它**，判据与配方都写好了。

### 9.1 当初为什么要它 —— 以及那个理由为什么站不住

- 当初的依据是 `obd-ble-sim.py --selftest` 打出来的
  `VERDICT: FFF0 NOT in the advertisement -> board would never find us`，
  据此认为"Windows 把服务 UUID 放进 scan response ⇒ 被动扫描永远匹配不上"。
- ★ **那句话是工具自己的 bug**：本机 watcher **看不到自己的广播**（Windows 会滤掉本机地址），
  所以它逐条打的其实是**邻居设备**的广播 —— "FFF0 不在广播里"必然出现，与我们的广播无关。
  该判决已从工具里删掉（改成只报观察、并写明判据只能来自对端）。
- ⇒ **"被动扫描匹配不上"这条结论从未被证据支持**；真正让所有连接失败的是
  **15ms 建连超时**（`setConnectTimeout` 单位写错，见 `CAR-TEST-BLE-V2-RESULT.md` ⑤.11）。

### 9.2 现在手上有的两侧证据

| 事实 | 证据 |
|---|---|
| **真头**的 FFF0 **在广播包里** | 车上**被动**固件打出 `state=connecting peer=aa:bb:cc:12:22:33`（`isAdvertisingService(FFF0)` 命中 ⇒ 只能来自 ADV_IND） |
| 被动固件连**假头**失败 | 本执行单 §8 实测（假头 66 秒 `subs=0 cmd=0`）—— ★ 但那时超时还是 15ms，**不能**把失败归因给扫描模式 |
| 主动固件（超时仍 15ms）能连假头 | B 方案实测 —— 最可能只是"假头广播密，15ms 窗口里偶尔抓得到" |
| 修好超时 + 主动 | 真头连上、`timeout=3000` 那版 150 秒零掉线、默认档 19 次 `ready` |

### 9.3 A/B 配方（5 分钟，台面；真头要能供电/在被读到的地方）

1. 从板改回被动：`lib/dashcore/obd_transport_ble.cpp` 里 `scan_->setActiveScan(true)` → `false`，
   重编 `esp32s3-rgb-slave-obdtest` 并刷入。
   ★ **必须验证镜像真的重编**（`firmware.bin` 大小/时间戳变化 + 编译输出里编了
   `obd_transport_ble.cpp.o`）—— 别只看 `[SUCCESS]`。
2. 抓 60 秒（用 `tools/serial-capture/watch-utf8.ps1`）。
3. 判据：
   - ✅ `obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready` 出现，
     且 `state=ready … conn=1 fails=0` 维持 ≥60 秒 ⇒ **回被动**（少一次 SCAN_REQ/RSP 往返，
     对共存预算更友好，也回到原来的测试配置）；
   - ❌ 不出现（而主动那版能）⇒ **保留主动**，并把"为什么真头也需要主动"查清再定
     （那时它就是一条真结论，不再是 B 方案的副产品）。

---

## 附：本单涉及的文件

| 文件 | 作用 |
|---|---|
| `src/main.cpp` | 闸门条件、从板 PHY 启动、从板"就绪"日志、TX 任务守卫 |
| `tools/serial-capture/watch-utf8.ps1` | 按 UTF-8 抓串口（本单的判据工具） |
| `tools/bt-obd/obd-ble-sim.py` | 桌面/笔记本冒充诊断头（仅台面用） |
| `docs/BLE-OBD.md` §14 | 背景、台面证据、两条工具坑、仍未验清单 |
