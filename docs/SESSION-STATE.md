# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是"读代码"，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `docs/BENCH-LOG.md` 与各专题文档）。
★ 本次改写：**2026-10-17 傍晚（审计批收工）**（接 10-16 深夜那一轮）—— ① **编辑器 + 客户端【已完成】** ✓
（车主实机："**感觉好多了**" ✓，§1/§6）；② 屏幕故障（② 类）**仍【挂起】** ✓，判据与间接签名全部保留（§3/§4）；
③ ★★ **本轮新增**：车主"读一遍库看看有没有要优化的" ⇒ **五线只读审计 → 9 单修复**
（`13685ce` / `9f6f20e` / `fd57469` / `988da07` / `290079c` / `f0b70db`，**全在 `origin/main`** ✓）。
九单全是**宿主机上能验干净**的东西：三个安全口子（来源校验 / 刷写白名单 realpath / 关目录列表 + 反 SSRF）、
`arc_count` 越界闸门、主题缓冲 8 KB→4 KB、编辑器两个**静默** bug（裁边读空画布 / `unhex` 未定义）、
`pio.ps1` 的两处假失败 ⇒ **一块板都没插** ✓（★ 这是指**审计那一批**；当天晚些时候为红弧那单把两块板都刷了，见 ⑤）、**三份指纹逐字节未变** ✓（口径见 `ACCEPTANCE.md` 新一节）；
④ exe 已按源码重打（**17,915,287 B**）⇒ ⚠️ **必须完全退出旧客户端再开新的**（踩过两次）；
⑤ ★★ **同一天又落一单**（车主："**爆转和超速的红色表盘弧没有居中**"）：红弧原来**只加粗
INDICATOR**，而 LVGL 的弧 `radius` 是**外沿**、线宽只往**里**长 ⇒ 红带中线比灰轨道靠里 **3 px**。
修法 = 红区时**两个 part 一起加粗**（外沿不动、不裁剪），落帧实测中线差 1.98 → **1.07 px**、
内沿差 4.96 → **1.48 px**；判据 `tools/theme-editor/check-arc-centring.js`（36 项，含自检与非空判，
拿修复前的帧跑会红 4 项）。
★★ **两块板已重刷**（`esptool verify-flash 0x10000` 两边都 `digest matched` ⇒ 片上就是这份镜像；
主题分区回读逐字节未动），车主看过屏幕：**"改的很好"** ✓ —— 这条**已闭环，不用再改**。
⑥ ★ 同日还清了一笔文档债：README 的"最近进展"原先停在 **09-27**（三周没更新），已补到 10-17
并按代码订正六处**错**事实（PCLK 15→**12 MHz**、bounce 20→**30 行**、红区 5800→**5500**、
native 369→**387**、蜂鸣器**已砍**、OBD-BLE 早已落地）；★ 另把 **VAN 接收脚 `GPIO16`→`GPIO44`**
在四处文档里改准（16 不在 12PIN 排针上）—— 这条本来会让上车那天接错线。
⑦ 两个 **6 小时**串口长记录仍是**停止**状态、两份只读档案照旧（§3）；
⑧ ★ 车主 10-16 的口径不变："**先到这里，等三维支架建模和上车安装了**" ⇒ **下一阶段第一步仍在车主手上**（§1）。

---

## 1. 下一步第一动作 —— ★★ **下一阶段在车主手上：三维支架建模 → 上车安装**

★★ **编辑器 + 客户端这条线【已完成】** ✓ —— 车主在 PC 客户端里实机走完全程，评价"**感觉好多了**" ✓
（16 单全貌见 §6；能力清单与验收判据见 `ACCEPTANCE.md` 的
「**2026-10-16（深夜）—— 编辑器 + 客户端收口**」一节）。
★ 车主 2026-10-16 收工时的口径：**"先到这里，等三维支架建模和上车安装了"** ✓
⇒ **仓库里没有在跑的单** ✗，下一步的**第一步不在代码里** ✓（下面 1、2 两条都归车主）。

1. **★ 三维支架建模**（车主）—— 盘盖里能塞多少、两块 2.8C 怎么固定 / 视角多少，判据在
   `docs/WHEEL-REMOTE.md` §2：那里那句"盘盖里的可视面积很有限（**未核实**具体可用直径）"
   **仍是未决** ⏳ ⇒ 建模出尺寸后回填它 ✓。
2. **★ 上车安装 → 车测**（装好之后的第一件事，也是台面判据的**唯一真判据** ✓）。
   ★★ **现成文档两份，别重写** ✓：
   - **`docs/DRIVE-TEST.md`** = 上车运行单（§7「红线」+ 判据表；上一次完整车测是 **2026-09-28**）；
   - **`PINOUT.md`** 的「**装车接线**」一节（`PINOUT.md:681`）= 供电**方案 C**、12PIN 脚位、判据表。
   ★★ **供电已定案（2026-10-17 车主定）= 方案 C**：一个 **12V→5V / 2A** 降压模块 →
   **星形分两路** → **两块板各自的 12PIN `5V`(第11针) + `GND`**；**每路 1A 保险 + 可插拔**；
   模块放盘盖里（12V 走长线、5V 走短线）；`3V3` 只供 VAN 收发器（几毫安）。
   - ★ **3.3V 串接被否**（但**车主已实测两块板能开机** ✓）：主板那颗 LDO 只有 800 mA 要同时扛
     两块板、两颗 LDO 输出并联会反灌、峰值电流还没量 ⇒ 只当**应急接法**。
   - VAN 收发器挂主板 `3V3`，`TX/D` **短接 3V3**、**120Ω 拆掉**、`CANH/CANL` → 仪表插头 5/10 脚
     （9004/9005，**极性可能要反接**，上车看 `edges`/`frames` 现判）。
   - **OBD 座只给常驻 BLE 头**：12V 要么 1→2 分线（要确认 pin 7 通），要么走 **ACC 取电器**（更推荐）。
   - ⏳ **装车前两个数还没有**：① 两块板各自的**实际电流**（待机/满载/启动尖峰，`ARCHITECTURE` L6）；
     ② 只插原生 USB 时 `12PIN 5V` 针到底有没有 5V（决定"车上电时能不能插 USB"的严格程度）。
   ★★ **三条上车红线**（碰了会烧东西或白跑 ✗，原文在 `docs/DRIVE-TEST.md:242-251`）：
   ① **UART 那根 Type-C 绝不能插** ✓ —— 两个 5V 源**直接并联**（这条路上没有二极管），
   而且它把 43/44 从排针切给板载 CH343P ⇒ **VAN 接收（`GPIO44`）当场失效** ✗；
   ② **笔记本拔掉充电器、只用电池** ✓（别一边插市电一边接车；要插市电就得加 USB 隔离器）；
   ③ **两块板都静音 / 无蜂鸣器** ✓（`mute: 1 (loaded from NVS)` —— 蜂鸣器已在 §7 整条砍掉 ✓）。
3. **若车上仍见横纹 ⇒ 两条已知方向**（都是**加余量**，不是新机制）：
   ① **bounce 加档**：源码默认 10、车上一版已调到 **20 → 30** ✓
   （真正生效的是 `platformio.ini` 两个 env 的 `-DRGB_BOUNCE_LINES=30`，§8）；
   ★ 账：**+9.6 KB/块**（30 行 × 480 px = 14,400 B/块；相对 10 行的 9,600 B/块，两块合计多 9.6 KB）——
   代价是"CPU 从 PSRAM 搬的流量"更大，若横纹是**带宽型**（§5 的账）就会**更糟** ✗ ⇒ 这是**有方向的一步**，不是稳赚。
   ② **降 PCLK 12 → 10 MHz**（预计 vsync 约 36/s；判据口径见 §5 的 `rgb: vsync=…(+N/s)`）。
   ★★ 两条都**必须**用开机日志的 `rgb: bounce=` / `rgb: … pclk=…` 确认真的生效 ✓（env 的 `-D` 会覆盖源码默认值，§8）。
4. **轮上第三块板**（`docs/WHEEL-REMOTE.md`）：候选型号与排除项已写进它的 **§2.1** ✓，
   只差**人民币价**与 **mm 尺寸**（车主核 ✓）。已排除：M5Stack **Dial**（只有蜂鸣器）、
   微雪 **Touch-LCD-1.28**（无音频器件）。

**★ 屏幕故障（② 类）仍是【挂起】状态** ✓ —— 不再继续追这条线 ✗（处置与证据见 §3、§4；
10-16 收工时车主仍是这个口径 ✓，判据与"USB 外设卡死"那条新发现**原样有效** ✓）。
★ 已实测过的两条编辑器验收路径（本轮已由车主在客户端里走完 ✓，留作回归口径）：
真浏览器导出链路（`check-image-bin.js` + `asset-package.js unpack` 逐槽核对）+ 客户端里真刷一次
（偏移 `0x210000`，`kind=assets`；见 §8 的白名单锚点）。

★ **编辑器静态文件的回归闸门**（改页面必跑 ✓）= `tools/theme-editor/check-owner-mode.js`
（含 `.golden-hashes.json` 的**逐字节指纹**口径）+ `smoke-pages.js` + `syntax-check-pages.js`；
客户端那一半 = `tools/app/check-app.js` / `check-live-flash.js` / `check-flash-offset.py` /
`check-port-roles.py` / `check-click-flash.py`（§6、`ACCEPTANCE.md` 新一节有清单）。

★ **改弧的几何/样式必跑**（2026-10-17 新增 ✓）= `tools/theme-editor/check-arc-centring.js`
（读 `preview/frames` 的落帧，判"红弧与灰轨道同心"；**先落帧**：`pio.ps1 -Cmd 'run -e pcpreview'`
+ 跑 `C:\206dash-build\pcpreview\program.exe`（工作目录 = 仓库根，可用 `THEME_FILE=` 指到任意主题）。
★ 顺带记住一条 LVGL 口径（本次踩的）：**弧的 `radius` 是外沿、线宽只往里长**
（`lv_draw_arc.c` 的 `rin = radius - w`）⇒ **两个 part 要同宽才同心**；
"负 padding 把粗的一侧往外推"不行（外沿会超出对象边界被裁一圈）。

**接线侧没有待办项** ✓：**BLE OBD** ✓ 与 **VAN 抓帧 @ `GPIO44`** ✓ 两条数据通路都已实测通过
（2026-10-15 车主确认）；原先那条"接 VAN 前先确认 `GPIO44` 到底连到哪一边（怕默认选边被切给
板载 CH343P）"**作废** ✓ —— 44 是通的，实证比推断强。
接线口径 = `PINOUT.md` 的「装车接线」一节（`PINOUT.md:681`）。

## 2. 待办（按价值排）

1. **★ 上车安装 + 车测（12V → 稳压）= §1 第 2 条**（唯一的**真判据** ✓；**第一步在车主手上** ✓）：
   把"台面轻微横纹 = 供电边缘"这条历史结论在车上定死；若车上仍有 ⇒
   按 §1 第 3 条（bounce 加档 / 降 PCLK）逐条试。★ 上车前先过 `docs/DRIVE-TEST.md` §7 那三条红线。
2. **★ 串口记录脚本补"静默留痕"**（10-15 深夜发现，**待补、不急**）：`tools/serial-capture/watch-utf8.ps1`
   （`now-vanraw` 镜像里那份同一份）只在**真异常**时写 `--- port reopened ---`，
   而 `ReadTimeout`（`$p.ReadTimeout = 300`，`watch-utf8.ps1:50`）走的是 `continue`（同文件 `:76-83`）
   ⇒ **"串口静默"这种最重要的故障形态反而不留痕** ✗ —— 当时就是因此**没覆盖到故障那一刻**（§3）。
   ⇒ 应加"**静默 N 秒就写一行 `--- silent N s ---`**" ✓（判据要能从日志一眼看出"它从哪一刻不说话了"）。
3. **副读数字号真上档**（可选，车主未定）：现在副表在 480 档取 **24 号**（`kReadoutFontPx[0][2]`，
   `lib/themetool/ui_theme.h:449-453`），**已经是副表那一档的顶了** ✗；要更大只能改固件 ——
   开 `LV_FONT_MONTSERRAT_26/28/32` + 加一档 + 同步 `theme_clamp` 上限 / `static_assert` / 用例 ✓
   （`ui_theme.h:442-476` 那段注释就是这条的口径）。
4. **轮上第三块板**（`docs/WHEEL-REMOTE.md`）：候选**已定** ✓（型号与排除项已填进它的 **§2.1** ✓）；
   还差**人民币价**与 **mm 尺寸**（车主核 ✓）。已排除：M5Stack **Dial**（只有蜂鸣器）、
   微雪 **Touch-LCD-1.28**（无音频器件）。
5. **遗留（已裁决，别再提议）**：`expression.cpp` 的表情**超速档仍是 >130 / ≤127**
   （`lib/dashcore/expression.cpp:60-61`），与告警 150 不一致 —— 车主明确说**不用对齐** ✓。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| **主板 COM9**（右 = 速度 + 进气） | 10-16 本批版本（含主题格式修订 **R2/R3/R4**；`41b1c28` + `1a757f7` 已刷 ✓） | **静音** ✓；10-15 深夜串口**静默过一次**（见下）⇒ 按**复位键**后恢复 ✓；主题分区 `0x210000` = `theme-live.json`，**回读逐字节验证过** ✓；素材 `0x254000` **未动** ✓ |
| **从板 COM8**（左 = 转速 + 水温） | 同上（两块板同版 ✓） | **静音** ✓；同一份主题、回读逐字节验证过 ✓ |

★ 本批 12 单里**只有前两单动固件**（`41b1c28` R2/R3 与 `1a757f7` R4）⇒ 两块板都重刷过 ✓
（`Hash of data verified.` + `[SUCCESS]`），**没有**碰主题 / 素材分区 ✓。
★ **这一版两块板都一样，含**：① 删掉**整条指示灯栏** ✓；② 砍掉**整条蜂鸣器** ✓；
③ 转速红区 **5500**（弧 + 加粗）✓；④ 高速 **150**（弧 + 大数字变红）✓；⑤ bounce **30 行** ✓；
⑥ **读数底片**（读数下面垫一块静态半透明近黑圆角块，深浅 **`readout.plate_opa` = 38/255 ≈ 15%**）✓；
⑦ **R2/R3**：转速 / 速度**数字颜色各自独立**（`rpm_color` / `speed_color`，缺键回落**文件里那份** `digit_color`）
+ 底板深浅进主题 ✓；⑧ **R4**：主读数的**单位标签跟随该屏数字颜色**（`unit_color` 保留但**已无渲染消费者**）✓。

### 3.1 ★★ 10-15 深夜屏幕故障（② 类）的处置与结论 —— **车主决定挂起**（10-16 仍挂起 ✓）

**现象**：主板（速度表）又出现"**偏移**" ✗（**不是**横纹、**不是**黑屏）。

**处置与证据（全部为实，别改口径）**：

- ✗ **串口日志没覆盖到那一刻**：主板串口在 **21:52:40 就静默**了（比报障早约 **1h55m** ✓），
  `onset4-master.log` 到 **EOF** 都没有任何复位 / 异常行 ✓。
- ✓ **到断线为止主板没复位**：`link: B uptime=` 共 **24,165 条严格递增** ✓
  （first=777ms、last=12,306,080ms ✓，**全篇仅开机一处回跳** ✓）。
- ★★ **从板一直仍收到主板的 ESP-NOW 数据** ⇒ 主板 **MCU 没死** ✓，**死的是它的 USB 串口输出通路** ✗
  —— 从板日志最后的 `link: locked` 与 `espnow:` 计数器仍在正常推进（`rx_frames=572,963`、
  `gap_rx=25ms`、`overflow=0` ✓）。
- ✓ 车主按**复位键**后：串口**立刻恢复** ✓（`master-afterreset.log` = **312 行**、开机横幅、
  `206 dash ok` ✓，其 `link: B uptime=` 69 条**零回跳** ✓）；屏上**偏移消失** ✓。
- ★ 开机横幅给出关键证据（原文照抄）：
  `boot: reason=POWERON n=51 (raw=1) up=242ms | prev=UNKNOWN (raw=11) prev_up=320min | role=MASTER`
  ⇒ 故障期间**芯片连续跑了 320 分钟没复位** ✓ ⇒ **排除"电气复位"这一形态** ✓；
  而**一次芯片复位就修好了串口** ✓ ⇒ 是**芯片内部的 USB 外设卡死** ✗，
  **不是**主机侧线 / 口坏 ✓。

★★ **新得到的一条"间接机器签名"**（本文档过去一直写"② 没有任何机器签名" ✗ —— **这一条要改**）：

> **"某块板串口静默 + 另一块板仍能收到它的链路数据" ⇒ 可预判该板进入了 ② 状态** ✓

★ 代价：**要靠另一块板来观测** ✓（单板自证不行 ✗）；且需要一块板在跑记录、另一块在另一台口上跑记录。

★ **`panelguard` 命中数 `0` 不能当证据** ✗ —— 健康的从板**也是 0** ✓ ⇒ 本版固件**可能根本不打印该行** ✓
（这一条**推翻**了旧写法"② 没有任何可检测信号：`panelguard` / `timeout` / `stall` / `fullrb` 全 0" ✗）。
⇒ 以后判据要用 **`timeout=` / `fullrb=` 这类真实计数器** ✓，别用"某行出现了几次" ✗。

★ **现状**：两个 **6 小时**长记录**已由车主决定全部停止** ✓（含清掉残留记录进程 ✓）：
- `onset4-master.log` = **7,995,146 B / 63,648 行**，**21:52:40 起彻底冻结** ✓
  （`sha256 258C6365D5200AF7756E44C5F97F9477AABFF035B678A9F287C9DEC7DB82C4C6`）；
- `onset4-slave.log` = **9,063,805 B / 58,537 行**，**23:53:29 起冻结** ✓
  （`sha256 4F17E4893742F92075EF6E62A1FDC7ACCB3745C3CF20363AEFB153B1C85D5FBD`）；
- ⇒ 两份都是**只读档案**，别再指望它继续长 ✗；`tools/app` 服务**见 §6 的更正**（★ 它其实在跑）；
- ★ **COM8 / COM9 现在都空闲可打开** ✓（`tools/app/server.py` 的 `/api/ports` 能列出两块板，
  说明**没有进程占着口** ✓）。

★ **主题（两块板同一份）= 单一真源 `C:\206dash-scratch\theme-live.json`**
（**2580 B**，md5 `73af24423025606d2ec1475bc312ccb4`）：`digit_cy=88` / `unit_cy=110` ✓、
`coolant_color = intake_color = digit_color = 0xBF4153` ✓；**其余与原始底稿相同** ✓。
两块板都**回读逐字节验证过** ✓（判据 = `esptool read_flash 0x210000 0x4000` 回读 + 比对）。
★★ **注意（10-16 本批新增）**：主题格式已修到 **R4**（`rpm_color` / `speed_color` / `plate_opa` 进主题、
主读数单位跟随本屏数字色）⇒ 上面那份 `theme-live.json` 是 **R2 之前**的快照 ✗；
**下次改主题前先从板子上 `read_flash 0x210000 0x4000` 回读一份当出发点** ✓，
否则会把已生效的 R2/R3/R4 键静默写回旧值（§8 那条"单一真源"的坑正是这件事）。

★★ **要长跑记录 / 要刷写时不要开着 `tools/app` 主题编辑器服务** ✗ —— 它一开串口就会动 DTR/RTS、
把 DTR/RTS 拉住 ⇒ **板子被按在复位里 ⇒ 屏全黑、连背光都没有**（2026-10-15 的真因，见 `ACCEPTANCE.md`
同日晚第 6 节 ✓）。抓串口一律用 **DTR/RTS 全 false** 的 `tools/serial-capture/probe-retry.ps1` ✓。
★ **更正（10-16）**：**编辑器的「写进板子」现在会开串口刷写**（`kind=assets`、偏移 `0x210000`）
⇒ 开着客户端又去挂长记录，**同样会踩上面这条** ✗ —— 两件事不要同时做。

★ **横纹判据分三层**（先看逐笔计数器）：`inv=` → `flush=` → `copy_avg=`。
好档：`inv=` **2 万~23 万 px²/s**、`flush=` 21~44/s、`copy_avg=` 166~253 µs ✓；
坏档签名（逐帧碰 LVGL 样式那一次）：`inv=856,980`、`flush=80/s`、`copy_avg≈950 µs` ✗。
（10-15 深夜复位后那份日志就是好档：`inv=60325px2`、`flush=27/s`、`copy_avg=233us`、`timeout=0` ✓ —— 留作对照。）

★ **协议**：②（横纹 / 偏移 / 黑屏）出现 ⇒ **先别复位**、立刻通知（**但这条已被车主挂起**，见 §4）；
①（黑屏重启）⇒ 几秒内抓串口要 `boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。
★ 台面上 ② 唯一有效的恢复手段仍然是**按复位键**（重初始化 / 外设重启全 ✗）—— 10-15 深夜**再次实证** ✓。
★ 打开串口采集**不会**复位板子 ✓（10-14 那份日志起点无 `boot:` 行为证）；
但 **esptool 复位**常把它弄哑 ⇒ 唯一可靠恢复是**重刷**。
★ 回滚产物还留在 `C:\206dash-scratch\`：`fw-prev-good-2bc5b38-slave.bin`、`fw-new-a076acd-slave.bin`、
`rollback-*.log`、`streak-slave.log`、`fix-verify.log` —— ★ 回滚 A/B 靠这些**留档产物**，
别再指望**板上**还留着旧档 ✗。

## 4. ② 类的已知结论（省得重试）

★★ **2026-10-15 深夜：车主决定【挂起】这条线** ✓ —— 下面是**留档**，不是"下一步" ✗。
理由：故障**不影响功能**（数据链路、表情、读数全在跑 ✓），且**唯一的恢复手段（按复位键）已确认** ✓；
再往下追要买设备 / 拆台面，车主判定**不值当** ✗。

| 手段 | 结果 |
|---|---|
| 只重写面板寄存器（`r`） | ✗ |
| `r` + `rgb 外设重启` / 重配时钟 | ✗ |
| `R`（`del` + `panel_create`） | ✗✗ 画面卡死 |
| 按复位键（真正一次启动） | ✓ 唯一有效（10-15 深夜再次实证 ✓） |
| 逐帧无条件碰 LVGL 样式（2026-10-15 的坏档） | ✓ 可修 —— **先比后设**（`af52b98`）；★ 这一档**有**机器签名：`inv=` ×5~6、`flush=` 80/s、`copy_avg≈950 µs` |

⇒ ★★ **更正（10-15 深夜）**：旧写法"② **本身没有可检测信号**" ✗ **不再成立** ——
  现在**有一条间接机器签名**：**"某块板串口静默 + 另一块板仍能收到它的链路数据"** ✓（§3.1）。
  但"没有签名"仍对**其它形态**成立：**横纹若伴随 `inv=` 暴涨，那是软件逐帧路径，不是 ②** ✓。
⇒ ★ 今晚的结论：故障期间**芯片没复位**（`prev_up=320min` ✓）、**一次复位就修好串口** ✓
  ⇒ 形态是**芯片内部 USB 外设卡死** ✗ —— **不是**电气复位 ✓，**不是**主机侧线 / 口坏 ✓。
⇒ 10-11 那夜的收窄结论仍成立：**② 与芯片复位不是必然同时发生**（瞬态打芯片 5 次、打面板 0 次）。
⇒ 10-15 早上那次"偏移自愈"**已结案**：`boot: reason=UNKNOWN n=33 | prev=POWERON prev_up=30min`
  ⇒ 是一次**上电复位**（笔记本恢复时 USB 反复上下电，≈10:03）⇒ **§20"只有复位才有效"继续成立** ✓
  （见 `docs/BENCH-LOG.md` 的「2026-10-15 结案」）。
⇒ 待做 A/B（**已随挂起一并搁置** ✗）：换**带独立供电的 USB hub**（手边暂无设备）。
★ 台面历史上偶发的**轻微**横纹按「**笔记本 USB 供电边缘**」记录 —— 不是固件缺陷 ✓。

## 5. 面板时序 vs 官方（2026-10-11 对照：消隐区没问题）+ PCLK 的来龙去脉

来源：`C:\206dash-scratch\ws-28c-demo\...\Display_ST7701.h`（微雪官方示例）

| 参数 | 官方 | 我们（`src/dash_display_rgb.cpp:267-275`） | 一致 |
|---|---|---|---|
| HPW/HBP/HFP | 8/10/50 | 8/10/50 | ✓ 逐字相同 |
| VPW/VBP/VFP | 2/18/8 | 2/18/8 | ✓ 逐字相同 |
| 极性 | 默认 | `PCLK_ACTIVE_NEG=0`、HS/VS idle=0 | ✓ |
| **PCLK** | 30 MHz（Arduino 例程） | **12 MHz**（`RGB_PIXEL_CLOCK_HZ`，同文件 `:263`） | ★★ 见下 |

★★ **更正（2026-10-14，"PCLK 是 ② 的嫌疑"这个说法作废）**：读源码注释
（`src/dash_display_rgb.cpp:241-266`）可见 PCLK 是**故意一路降下来的** ——
**30 → 18 → 15 → 12 MHz**，原因是**"横纹随刷新移动"**＝面板消耗速率逼近 PSRAM 搬运带宽
（`pclk × 2 B`：18 MHz 就 36 MB/s，而 CPU 从 PSRAM 搬只有 ~30 MB/s 量级 + 还要和重绘抢）。
⇒ **往 30 MHz 走是已知更糟的方向**（当年 30 MHz 整屏花），**不要拿它做 ② 的 A/B**；
真要试只能往**更低**走。
★ 副产品：`rgb: vsync=…(+N/s)` 的 N 就是 PCLK 的**第一手判据** ——
**12 MHz→约 44、15→约 54、18→约 65、30→约 108**
（今晚复位后实测 `vsync=1219(+43/s)` ✓，与 12 MHz 那一档吻合 ✓）。
★ 2026-10-15 复核：两块板刷完当日改动后 `rgb: … pclk=12000000Hz` **仍未变** ✓；
⇒ "降 PCLK 12 → 10 MHz"（预计 vsync 约 36/s）**从"随 ② 搁置"改成"车测仍见横纹时才试"** ✓
（② 那条故障线本身**仍然挂着** ✗；这一条现在是**横纹**的备用手段，见 §1 第 3 条）。

## 6. 客户端（tools/app）现状 —— ★★ **这条线【已完成】** ✓（车主实机："感觉好多了"）

- **三个屏**（`a8b2c11` 把七个页签收成 3 个）：刷固件 / 表盘 / 工具；去术语 + 设计系统。
  `check-app.js` 卡着"恰好 3 个屏 + 能力表 + 术语扫描"三道（现在 **95 条 `say()`** ✓）。
- ★★ **10-15 深夜的四单**（`fc9daf7` → `fa469fd` → `03e4ca4` → `b6d5881`）：
  - `fc9daf7` —— **车主模式**（默认）+ **表情框选器** + **3 预设** + 告警文案按"蜂鸣器已删"改写 ✓；
  - `fa469fd` —— **槽位清单 11 格**（背景 ×1 + 表情 ×10）搬进车主模式的**图片区** ✓（技术参数仍留高级）；
  - `03e4ca4` —— **配色页与图片页合并成一个页面** ✓：两个**页内分区**共用同一份状态；
    `tools/theme-editor/image-editor.html` 变成**无脚本重定向薄壳** → `index.html#images` ✓；
  - `b6d5881` —— **打进客户端** ✓：表盘屏**只留一个编辑器**；编辑器静态文件**真打进 exe** ✓
    （7 条真实请求清单 ✓）。
- ★★ **10-15 深夜 → 10-16 的 12 单**（`96f4628` → `22ebbd5`，**归档那一批** ✓）—— 逐单一句话：

  | 提交 | 一句话 |
  |---|---|
  | `96f4628` | 背景加 **cover 取景框**（与表情"框外丢透明"语义**不同**）+ 修**表情居中**（根因是**初始构图**，不是产物管线）+ **WebP 支持**（浏览器解码后查 alpha）|
  | `f2c8a9d` | 配色区加「**水温颜色 / 进气颜色**」两块色板（接**已有** `readout.coolant_color` / `intake_color`，**不新增格式**）|
  | `b8617d3` | 新增 `check-preview-plate.js`：只读固件源码 + JS 镜像对账（**防预览与真机漂移**；当时故意退 1 = "镜像还没落地"）|
  | `41b1c28` | ★ 固件：**转速 / 速度数字颜色拆开** + **底板深浅进主题** = 主题格式修订 **R2/R3**；旧主题**回落 `digit_color`**；编辑器四色板 + 深浅滑杆 + 预览补底板 |
  | `1a757f7` | ★ 固件：**主读数单位跟随该屏数字颜色**（**R4**）—— `unit_color` 保留但**不再用于单位**；三份指纹**一个都没变** |
  | `420fca3` | 车主模式加「**打开安装包…**」（① 配色 / ② 图片 各一个）—— 与高级那两个**共用同一个 `#file-theme` 与同一条读取路径** ✓ |
  | `e58010d` | 端口下拉**标出角色**（主板 / 从板 / 未知不猜）+ 一次「**写进两块板**」（逐块报结果）|
  | `97a1672` | ★ **导入安装包时把图片装回槽位**（车主："没有看见图片载入"）+ `name` 字段改 **UTF-8** ⇒ **往返逐字节相同** ✓ |
  | `f5eac6b` | 「保存安装包」下载文件名**带时间戳**（只改文件名生成、**字节不变** ✓）+ 把「保存快照」与「写进板子」**写清是两件事** |
  | `a6b7d7e` | 「写进板子」**不再要求先保存**、**不产生下载**（内存打包 + POST）；保存只留给"想留快照" |
  | `5d4fce2` | 「点了写进板子没反应」两条真因修复：按钮搬到**表盘屏** + 每步都说话 + **界面来源统一** + 全站 **no-cache** |
  | `22ebbd5` | 编辑器面板**去掉"先保存"门槛**（`packCurrentAssets()` 一条打包路；版本/能力标记 `__OWNER_MODE_VERSION`）|
- ★★ **归档之后又落两单**（都在 `origin/main` ✓）—— 客户端这条线的**最后两块拼图**：

  | 提交 | 一句话 |
  |---|---|
  | `ab5a16c` | 外壳**读编辑器的版本 / 能力标记** ✓：缺 `flashNoSave`（或整页无标记）/ 缺 `exportAssetsBin`·`download` ⇒ 表盘屏**红横幅 + 牌子**喊「**编辑器页面太旧：…退出应用再重新双击 / Ctrl+F5**」并**拦下写板**（`doWrite` 第一句就 return ⇒ **一个 `/api/flash` 都不发** ✓）；**版本低但能力齐全 ⇒ 照写不误拦** ✓；iframe **跨源读不到 ⇒ 明说"不同源"** ✓；`/api/health` 新增 **`editor_version` / `editor_caps`**（`server.py` 的 `editor_markers()` **只读解析**"要发出去的那一份" ✓）|
  | ★★ `b0637b0` | **双击 exe = 真原生窗口** ✓：`pywebview 6.2.1` + `pythonnet 3.2.1` 驱动**系统自带的 WebView2** ✓ —— **不是 Electron/Tauri** ✗、**不自带浏览器内核** ✓。新增 `tools/app/native.py`（`import server` + **同一个** Handler ⇒ **没有第二套服务** ✓，**仍只听 `127.0.0.1`** ✓）；**关窗 = 停服务**（`win.events.closed` 里 `srv.shutdown()`），界面「退出应用」也会把窗关掉 ✓ |

- ★★ **「双击 exe 开真原生窗口」这条的要点**（`b0637b0`，**实测在打好后的 exe 上** ✓）：
  - 窗口标题 **`206Dash 表盘助手`** ✓、**有窗口句柄与任务栏条目** ✓；
  - ★★ **关窗 ⇒ 两层进程都退出、端口释放、HTTP 连不上** ✓（控制台打印「窗口关了 —— 正在停止本地服务…/窗口已关闭。」）；
    **反方向也成立**：界面右下角「退出应用」会关窗 ✓ ⇒ **不会出现"窗口关了服务还在"** ✗；
  - ★ **WebView2 里没有 Ctrl+F5** ✗ ⇒ 右下角新增「**重新载入这个窗口**」✓（服务端 no-cache 仍在 ✓，普通 reload 就够）；
    `--devtools` **默认关** ✓；
  - ✓ **缺 WebView2 / 窗口起不来 / 没装 pywebview ⇒ 明确提示 + 装法 + `--browser` 回退系统浏览器** ✓（**不静默** ✗）；
  - 参数：`--port`（默认 8787）/ `--repo` / `--devtools` / `--browser`（回退老路）/ **`--no-window`**（只起服务，给自动化测试用 ✓）；
    `run.bat` 也走 `native.py` ✓；
  - ★★ **exe 大小 = 17,908,113 B（17.08 MB）** ✓（pywebview + pythonnet 进包，**从 9.4 MB 变大属正常** ✓；
    **内嵌 11 个文件与源码逐字节相同** ✓）；`build-exe.ps1` 入口换 `native.py` +
    `--collect-all webview/clr_loader/pythonnet` + hidden-import `clr`，**仍 `--console`** ✓（日志是重点）；
  - ★ 顺手修了一个**真坑**：以前 PyInstaller 失败会因**旧 exe 还在**而误报 OK ✗ ⇒ 现在**先删旧 exe + 查退出码**、
    失败当场 `exit 1` ✓（这一单就靠它抓到 `native.py` 的语法错）；
  - ⚠ **换机器要注意**：`pywebview` / `pythonnet` / `clr_loader` 是装进
    `C:\206dash-scratch\pio-core-mix\penv` 的 ⇒ **换机器打包 / 直接跑 `run.bat` 都要重装** ✓（README 已写 ✓）；
  - ⚠ **这一单没验到的（照实记）**：① "**WebView2 真缺失**"那条分支只实跑了 `--browser`
    （同一个函数 ✓），**没在本机卸掉运行时验证** ✗；② **没有截图** ⇒ 判据是
    窗口标题 / 句柄 / 任务栏 + 关窗后端口与进程 ✓（**不是**视觉判据 ✗）；
  - ★ 判据：`check-app.js` +12（**共 95 条 `say()`** ✓）、`check-click-flash.py` **dev 与 exe 各 17 项全过** ✓
    （对 exe 用 `--no-window` ✓，测试不该弹窗口）；**三份指纹不变** ✓。
- ★★ **exe** = `tools/app/dist/206Dash-Workbench.exe`，本批重打了五次：
  **9,358,724 B**（`b6d5881`）→ **9,399,494 B**（`a6b7d7e`）→ **9,404,804 B**（`5d4fce2`）→
  **9,402,800 B**（`22ebbd5`）→ **9,410,505 B**（`ab5a16c`）→ ★★ **17,908,113 B**（`b0637b0`，
  **= 现在吃到的那个** ✓；变大是因为 pywebview + pythonnet 进了包，**不是又胖了内核** ✓）。
  ★ **每次内嵌 11 个文件都与源码逐字节相同** ✓；`build-exe.ps1` **默认 onedir** ✗，要加 `-OneFile`。
- ★ **三个指纹**（golden 现行值，逐字钉在 `tools/theme-editor/check-owner-mode.js` 的 ⑬ 一节 +
  `tools/theme-editor/.golden-hashes.json`）：
  `themeJson 1dcc7d40…6efa2e` / `imageBin 24fc3c87…da783c` / `assetsBin 96e02893…87747f` ✓。
  ★★ **口径（R2/R3 改了哪两份、为什么）**：
  - **`themeJson` 与 `assetsBin` 变了** ✓ —— `assetsBin` 是"主题段 + spiffs 空洞 + image 段"的**整包**，
    主题段就在里面 ⇒ **主题 JSON 一变它必然变** ✓。**证据**（不是推断）：同一套打包调用跑旧 / 新两页，
    全部差异 **1874 字节落在 `[665, 2974] ⊂ 主题段 [0, 0x4000)`**，而 **`[0x4000, end)` 逐字节相同** ✓；
  - **`imageBin` 不变** ✓（= 16524 B；图片那一段一个字节没动 ✓）；
  - ★ **此后各单必须三份全不变** ✓（`1a757f7` 起的每一单都是这么验的 ✓）。
  ★ **注意**：这是**旧素材**的指纹 ✓；**车主换素材后指纹必然变化** ✓，别把它当成"格式变了" ✗。
- ★ **车主新素材**（都已核对）：
  - `E:\痛一会\表情包_360圆形` = **10 张 360×360 PNG** ✓（转速表-1..5 + 速度表-1..5 ✓，
    正好对 **10 个表情槽** ✓；**360 > 320 会被缩到 320** ✓，**已经居中带透明 ⇒ 可跳过框选** ✓）；
  - 背景候选 = `.dsh-drop` 下那张 **WebP**：`Millennium_Intro.webp` = **1280×720（16:9）** ✓
    （VP8 有损，61536 B ✓）⇒ **必须取景** ✓（1:1 只能保留 **720×720** 的方形窗口，
    **宽度要裁掉约 44%** ✓）。
  ★★ 但**最快的恢复路径不是重新拖图** —— 用「**打开安装包…**」导入车主那份 `…-assets (3).bin`：
  配色 + **11 张图**（带缩略图）一次回来 ✓，再点「保存安装包」导出的就是**与它逐字节相同**的一份 ✓；
  重新拖那 10 张只是**观感等价**（同样的图、同样的尺寸档），**不保证像素逐字节相同** ✗。
- ★ **两个按钮与两个下拉（写盘那一条）**：表盘屏编辑器框上面就是
  「**写进这块板（配色+图片，不存文件）**」/「**写进两块板（不存文件）**」；
  端口下拉带角色：`COM9（MASTER · 右屏 速度+进气）` / `COM8（SLAVE · 左屏 转速+水温）` /
  认不出 ⇒ `COMx（角色未知）`；主板板载口 ⇒ `COMx（不是本项目的板）`。
  ★ 点下去**立刻有反馈**：「**正在写 COM8…**」→「**写完了 ✓（偏移 0x210000）**」✓；
  两块那条**逐块 await、逐块报结果**，失败**点名**哪块没写成 ✓（不假装成功 ✗）。
- ★★ **界面来源只许一处**（`tools/app/server.py` 的 `pages_source()` / `pages_info()`）：
  仓库里 `tools/app` 与 `tools/theme-editor` **都在** ⇒ 整份用**仓库**；否则整份用 **exe 包内** ✓
  ⇒ **杜绝"外壳新 / 编辑器旧"的混搭** ✗；`/api/health` 如实报 `pages_from` / `pages_dir` /
  `app_js.sha8` / `editor_html.sha8` / mtime ✓，页面右上角显示「**界面：仓库（…）**」✓。
  所有静态响应统一 **`Cache-Control: no-store, no-cache, must-revalidate`** ✓。
  ★★ **版本 / 能力自检 = 结构性防线**（`ab5a16c`）：外壳**读编辑器自己暴露的标记**（`ab5a16c` 之后），
  缺 `flashNoSave` / 整页无标记 / 缺 `exportAssetsBin`·`download` ⇒ **红横幅 + 牌子** +
  **拦下写板**（一个 `/api/flash` 都不发 ✓）；**版本旧但能力齐 ⇒ 照写不误拦** ✓；
  `/api/health` 的 **`editor_version` / `editor_caps`** 可只读核对 ✓（与 `pages_from`/sha8 一个用法）。
- **车主的验收** = **§1**：本条线**已完成** ✓（车主说完"感觉好多了" ✓，之后口径是"先到这里，等三维支架建模和上车安装"）；
  下面两条留作**回归口径**：真浏览器导出 → 父代理逐槽核对；再在客户端里真刷一次确认 `0x210000`。
- 预览表盘已内联；★ **预览帧必须同步进仓库** `preview/frames/`（150× `l_*.bmp` + 150× `r_*.bmp`，
  今日实点仍是 150 + 150 ✓）—— 渲染器写进的是镜像目录，不同步就"图裂" ✗（已踩过一次）。
  ★ 预览的**读数底板**已按固件镜像（层序 表情 → 底板 → 弧 → 读数 ✓），漂移由 `check-preview-plate.js` 只读对账 ✓。
  ★ 已知**对不齐**的一处（写进脚本注释了）：**字体墨迹宽高无法逐值对齐**（固件 LVGL Montserrat
  vs 浏览器系统字体）⇒ 只断言恒定部分（色 / opa / pad / radius / 层序 / 中心对齐）✓。
- 无线刷写「上传后自动验槽」：`/api/ota-verify` 已**幂等**（板子已在 OTA 模式就不再发 `u`，免得重计时）。
  ★ 口径修正：**只有服务端端点**是 curl 实测过的；客户端 `verifySlot` 那条路修好后**未再单独实测**。
- ★ **更正（10-15 深夜）**：`tools/app` 服务**其实在跑** ✗ —— `python tools/app/server.py`
  监听 **`127.0.0.1:8787`** ✓（`GET /` = 200、title「206Dash 表盘助手」✓）。**它不是坏东西**
  （它负责刷写，`/api/ports` 也刚用过 ✓），但 §3 那条坑**照样成立**：**长跑记录时别让它开串口** ✗。
- ★ 2026-10-15 跟着新阈值改的两处（都不是客户端 UI，但会顺着客户端走）：
  ① 预览注入键 `PreviewKey::Overspeed` 的注入值 **130 → 160**（`lib/dashcore/preview_input.h:175`；
  否则够不着新的 150 阈值、按键无反应），并新增用例钉住"注入值必须够得着阈值"；
  ② 编辑器模板 `tools/theme-editor/index.html` 与 `tools/theme-editor/theme-default.json` 已一并对齐 **150 / 5500** ✓。
- ★ 删蜂鸣器**没有**动客户端：`tools/app` 里没有蜂鸣器 / 静音 UI（`.css` 里那些 `muted` 是颜色变量名）✓。

### 6.1 台面网络两个坑（都踩过）

1. **`206dash-MASTER` 的 WiFi 配置文件原先不存在** ✗ ⇒ `netsh wlan connect` **静默失败**。
   现已生成 `C:\206dash-scratch\obd-ble-sim\wlan-206dash-MASTER.xml`（含正确 SSID hex）并加入系统 ✓。
   ★ 下次换板先确认配置文件在不在。
2. **Windows 扫描偶发不列出新热点** ✗（同段脚本有时 10 秒可见、有时 150 秒不见）。
   ⇒ 判据必须是「扫描里真出现 + 拿到 `192.168.4.x`」，**不能只看 connect 的返回值** ✓。

### 6.2 ★★ 10-16 换 exe / 刷写这条线上踩的三个坑（都写进 §8 锚点）

1. ★★ **换过 exe 必须"完全退出客户端"再启动** ✗ —— 否则你双击的**还是旧进程**、
   新逻辑**根本不生效**（10-16 真踩过一次 ✗）。判据 = 页面右上角那行
   「**界面：仓库 / exe 包内**（含 app.js 与编辑器页的 `sha8` + 时间 + 编辑器实际报的版本/能力）」✓，
   对不上就是没换成功；页面里也明写了"改了代码 / 换过 exe 之后：右下角「退出应用」，再重新双击那个 exe" ✓。
   ★ 原生窗口之后又多两种表现：**窗口还在 ⇒ 服务就还在**（关窗才会两层进程一起退 ✓）；
   **窗口里刷新不要按 Ctrl+F5（WebView2 没有 ✗）⇒ 用右下角「重新载入这个窗口」** ✓。
2. ★ **整份页面只有一个来源** ✓（`pages_source()`）：`/api/health` 报 `pages_from` / `pages_dir` /
   `sha8` ✓ ⇒ 不再出现"外壳新 / 编辑器旧"的**混搭** ✗（那正是"按钮在、点了没反应"最难查的一种）。
   ★ 附带：以前编辑器页走 `SimpleHTTPRequestHandler`、带 `Last-Modified` 但**没有** `Cache-Control`
   ⇒ 浏览器**启发式缓存**，只有 Ctrl+F5 才更新 ✗ —— 现在**全站 no-cache** ✓
   （`Cache-Control: no-store, no-cache, must-revalidate` + `Pragma` / `Expires`）。
3. ★★ **`Popen(stdout=PIPE)` 必须把管道读掉** ✗✗ —— 测试脚手架里 exe 的 stdout 只开了 PIPE **却从来没读**，
   ⇒ 管道写满之后 **exe 的请求线程会卡在写日志上**，表现就是"**请求不回来 / 点了没反应**" ✓。
   ★ 这一条当天**先被误判成"测试脚手架的问题"** ✗，后来才查实 ✓
   ⇒ 教训：**"点了没反应"要先分辨是"旧页面 / 旧进程"还是"真卡死"** ✓（10-16 两种都碰到了 ✓）。

## 7. 声音：**蜂鸣器已整条砍掉**（2026-10-15）⇒ 板上现在**完全无声**

- `a076acd` 删掉的：4 个驱动文件 + `dash_buzzer_set()` / `BUZZER_EXIO_BIT` / 自检段 + 串口 `b`/`m`
  + NVS 静音 + 2 秒上限。★ **同芯片的 `LCD_RST`/`LCD_CS` 与 `tca9554_set()` 未动** ✓。
- 板上本来就**没有**喇叭座 / 功放 / 音频芯片 —— 原先唯一发声件是**有源蜂鸣器**（`TCA9554` 的 `EXIO8`，
  只会响 / 不响）；⇒ 要"**会出声**"只能靠**轮上第三块板**（`docs/WHEEL-REMOTE.md`，选型见其 §2.1）。
- 调研结论留档在 `docs/RGB-PANEL-2.8C.md` **§2.1 / §2.2**：
  **I2C 传不了音频**（实测写一次扩展器 141–178 µs ⇒ 8 kHz 采样要 1.2 倍主循环时间）；
  **微雪没有**"I2C 就能出声"的模块（它家音频板是 I2C 配寄存器 + I2S 走数据）；
  真要做的四档（0 脚 `DFR0760` TTS / 0 脚 `PCA9685` / 1 脚 `GPIO43`+PWM / 3 脚 I2S 飞线）与代价见 §2.2。
- ★ 顺带记的引脚事实：`GPIO4`=`BAT_ADC`、`GPIO42`=TF 卡 `D0`、`GPIO16`=触控版 `TP_INT`
  （出处是微雪 wiki 表，**本仓未核原理图** ⇒ 按"外部资料"对待）。

## 8. 常用锚点（不要再重新搜）

**★ 今天学到的硬知识（每条一句话，都是踩过的）**

- **逐帧碰 LVGL 样式必须先比值后设** ✓ —— setter 不比较值、无条件 invalidate 整个包围盒 ⇒
  每帧重绘 ×5 ⇒ 持续横纹；判据是 `inv=` **2 万~23 万 px²/s** 档（回到 85.7 万即写进了逐帧 ✗）。
- **主题改动的判据必须用 `esptool read_flash` 回读 + 逐字节 / 哈希比对** ✓，
  **不能用文件字节数** ✗ —— 改 `digit_cy 72→88` 之后文件**仍是 2579 B** ✓。
- **主题单一真源 = `C:\206dash-scratch\theme-live.json`** ✓ —— **每轮改动都从它出发** ✗ 不要从
  仓库原始底稿（`tools/theme-editor/local/theme-fixed.json`）出发，否则**静默冲掉上一轮的改动** ✓。
  ★★ **10-16 追加**：格式已修到 **R4**，那份文件是 **R2 之前**的快照 ⇒ **下次改主题前先从板子
  `read_flash 0x210000 0x4000` 回读一份当出发点** ✓（否则 R2/R3/R4 的键会被静默写回旧值 ✗）。
- ★★ **"某块板串口静默 + 另一块板仍收到它的链路数据" ⇒ 该板进了 ② 状态** ✓（10-15 深夜新增，
  唯一的**间接机器签名**，**要靠另一块板观测** ✓）。
- ★ **`panelguard` 命中数 0 不等于没发生** ✗ —— 健康的从板也是 0 ✓ ⇒ 判据用 **`timeout=` / `fullrb=`**
  这类真实计数器 ✓，别用"某行出现了几次" ✗。
- ★ **串口日志的"静默"本身要留痕** ✓ —— `watch-utf8.ps1` 的 `ReadTimeout` 走 `continue`
  ⇒ 板子不说话时日志里**什么都没有** ✗ ⇒ 应加"**静默 N 秒写 `--- silent N s ---`**"✓（§2 第 2 条）。
- "**给字体描边**"在**点阵字体**上是 **no-op** ✗（LVGL 只在 FreeType + 矢量 + ThorVG 下真描边；
  LVGL 9.6 也没有 `text_shadow_*` ✗）—— 写那几个 setter 只会多占内存、画面零变化。
- 读数颜色**别算对比度** ✗ —— 背景是**表情**（还可能是图片），算不准；做法 = **跟已被车主验证
  可读的元素同色** ✓（今天就是 `digit_color` 那一个值解决了副读数）。
- **"屏黑且无背光" ≠ 板子坏** ✗ ⇒ 第一件事是查**有没有程序占着串口** ✓；
  抓串口一律用 **DTR/RTS 全 false** 的 `tools/serial-capture/probe-retry.ps1` ✓。
- **长记录必须"父代理"挂** ✓（子代理起的后台任务随它结束而死 ✗；判据 = 挂完 20~30 秒后看日志
  的 `Length`/mtime 是否在长 ✓）。
- 构建三件套：`PYTHONIOENCODING=utf-8` **不能省** ✓（否则刷写崩、板子变黑 ✓）；
  `PLATFORMIO_BUILD_DIR` **必须纯 ASCII** ✓（`C:\206dash-scratch\final-builds\<master|slave>`）；
  **别用裸 `python`** ✓（商店假货：退出码 0、零输出 ✓）。
- `platformio.ini` **每个 env 的 `-DRGB_BOUNCE_LINES` 覆盖源码默认值** ✓ ⇒ 改档位必须**同时改 env 的
  `-D`**（判据看开机日志的 `rgb: bounce=` 行 ✓，不是看你改了哪个文件）。

**★ 10-15 深夜的新锚点**

- 串口静默脚本的缺口：`tools/serial-capture/watch-utf8.ps1:50`（`ReadTimeout = 300`）与
  `:76-83`（超时走 `continue`）；唯一的标记行在 `:93`（`--- port reopened … ---`）
- 故障期 / 复位后的两份**只读档案**（都已冻结，别再等它长 ✗）：
  `C:\206dash-scratch\onset4-master.log`（63,648 行）、`onset4-slave.log`（58,537 行）、
  `master-afterreset.log`（312 行，含那条 `prev_up=320min` 的开机横幅 ✓）

**★★ 10-16 的新锚点（客户端 + 编辑器这条线，每条一句话）**

- ★★ **双击 exe = 真原生窗口**（`b0637b0`）✓：`tools/app/native.py`（**新**，窗口宿主）＝
  `import server` + `ThreadingHTTPServer(("127.0.0.1", port), app_server.Handler)`（**同一个** Handler，
  **没有第二套服务** ✓）+ `pywebview`（`gui="edgechromium"` = **系统自带 WebView2**）；
  窗口标题常量 `WINDOW_TITLE = "206Dash 表盘助手"`、1280×900；**关窗 = `srv.shutdown()`**（`win.events.closed`），
  反向「退出应用」（`POST /api/quit`）⇒ 看门狗 `win.destroy()` ✓；回退路 `open_in_browser()` + `--browser`；
  ★ 打包：`tools/app/build-exe.ps1` 入口 = `native.py`、`--collect-all webview/clr_loader/pythonnet`、
  `--hidden-import clr`、**仍 `--console`**；**先删旧 exe、失败 `exit 1`**（防"旧 exe 还在 ⇒ 误报 OK"✗）。
- ★★ **WebView2 窗口里没有 Ctrl+F5** ✗ ⇒ 右下角「**重新载入这个窗口**」（`#b-reload`，普通 reload 就够，
  服务端 no-cache 仍在 ✓）；`--devtools` **默认关** ✓。
- ★ **换过 exe ⇒ 必须"完全退出客户端"再启动** ✗ —— 否则跑的是**旧进程**、新逻辑不生效 ✓；
  判据 = 页面右上角「**界面：仓库 / exe 包内**（app.js sha8 · 编辑器 sha8 · 时间 · 编辑器报的版本/能力）」= `/api/health`
  的 `pages_from` / `pages_dir` / `app_js.sha8` / `editor_html.sha8` / **`editor_version`** / **`editor_caps`** ✓
  （`tools/app/server.py` 的 `sha8()` / `pages_info()` / `editor_markers()`；`pages_source()` 就是那 20 行判据）。
- ★★ **整份页面来源统一决定** ✓（`pages_source()`：仓库里 `tools/app` + `tools/theme-editor` **都在**
  ⇒ 整份用仓库，否则整份用包内 ✓）—— 为的是**杜绝"外壳新 / 编辑器旧"的混搭** ✗。
- ★ **全站 no-cache** ✓ —— `Handler.end_headers()` 一处统一加
  `Cache-Control: no-store, no-cache, must-revalidate`（+ `Pragma` / `Expires`）；
  以前编辑器页带 `Last-Modified`、没有 `Cache-Control` ⇒ 浏览器**启发式缓存**、只有 Ctrl+F5 才更新 ✗。
- ★★ **`Popen(stdout=PIPE)` 必须把管道读掉** ✗✗ —— 只开 PIPE 不读 ⇒ 管道写满 ⇒
  **exe 的请求线程卡在写日志上** ⇒ 表现为"**请求不回来 / 点了没反应**" ✓（10-16 先被误判成
  "测试脚手架问题" ✗，后查实 ✓；读掉它的写法见 `tools/app/check-click-flash.py` 起服务那一段 ✓）。
- ★ **"写进板子"与"保存安装包"是两件事** ✓：前者 = **内存打包 + POST**、**零下载** ✓
  （编辑器 `packCurrentAssets()` 一条打包路；`exportAssetsBin()` 里才有 `download()`）；
  后者 = 才**下载快照**、文件名**带时间戳** `assets-YYYY-MM-DD-HHMM.bin` ✓（只改文件名生成、**字节不变** ✓）。
- ★★ **导入容器必须把图片段装回槽位** ✓（否则只回来配色 ✗ —— 车主原话"没有看见图片载入"）；
  **往返判据 = 导出 → 导入 → 再导出逐字节相同** ✓（靠导入项携带**原始设备字节** `item.packed`
  透传 + 名字的原始字节 `nameRaw` ✓；**改过的项**立刻回到正常渲染路径 —— 收口在 `packedUnchanged()` ✓）。
  真文件实测：**11 格全填满、image 段 2,190,220 B 逐字节相同、整包逐字节相同** ✓。
- ★ **端口角色判据：认 MAC（`SER=`）与板子自报 `role=`，不认 COM 号** ✗（COM 号会变 ✓）；
  第三份证据 = 板子 AP 名 / BSSID（`netsh wlan show interfaces`）把"角色 ↔ MAC"绑上 ✓；
  **没有证据就显示"未知"，不许猜** ✗；缓存里每条都记**证据来源** ✓
  （现场读到 = 已确认 / 缓存 = 未确认 / 无证据 = 未知）。
  ★ 踩过的坑：**连着家里路由器去读 `/status`，差点把路由器的 MAC 记成板子角色** ✗（判据已钉死 ✓）。
- ★ **刷写请求体里没有偏移字段** ✓ —— `POST /api/flash` 只收 `{port, kind, path}`（外加 `X-Filename`），
  偏移由服务端 **`ALLOWED_FLASH[kind]` 白名单**取（`theme`/`assets` **都 = `0x210000`**）；
  刷写入口 `do_flash()` 干脆不吃 `offset=` 关键字（传了直接 `TypeError` ✓）。
  ★ 判据脚本：`tools/app/check-flash-offset.py`（**37 项**）、`check-port-roles.py`（**33 项**）、
  `check-click-flash.py`（**17 项**，**无头 Chrome 真点按钮**、0.25 s 内必须有可见反馈、
  请求体无偏移、没图片时说清原因、**下载目录无 `.bin`**、**旧编辑器页要喊"缺能力"且一个 flash 都不发** ✓、
  **版本低但能力齐 ⇒ 照写不误拦** ✓；dev 与 **exe** 都过 ✓，对 exe 用 **`--no-window`** 起服务；
  它在页面里把 `/api/flash` 拦掉 ⇒ **全程没碰板子** ✓）。
- ★ **编辑器版本 / 能力标记**（给外壳提前发现"老页面"）：`window.__OWNER_MODE_VERSION`
  （现 `"2026-10-15.3"`）+ `window.__OWNER_MODE_CAPS`（`flashNoSave` / `importRestoresImages` /
  `snapshotStampName` / `cliCommand`）+ DOM 副本 `<html data-owner-mode-version="…">`；约定**只加不改语义** ✓。
  ★ 外壳侧在 `tools/app/app.js`：`EDITOR_MIN_VERSION` / `EDITOR_NEED_CAPS` / `checkEditorApi()`；
  跨源读不到 ⇒ 明说"不同源"（**不静默** ✗）。
- ★ **golden 指纹的现行值**（改包必跑）：`themeJson 1dcc7d40…6efa2e`（2975 B）/
  `imageBin 24fc3c87…da783c`（16524 B）/ `assetsBin 96e02893…87747f`（295052 B）；
  逐字钉点 = `tools/theme-editor/check-owner-mode.js` 的 **⑬** 一节 + `tools/theme-editor/.golden-hashes.json`，
  重算器 `tools/theme-editor/make-golden-hashes.js --print`（**只算不写** ✓；取证那段说明在文件顶部 ✓）。
- ★ 编辑器「车主模式」的回归闸门：`tools/theme-editor/check-owner-mode.js`、
  `tools/theme-editor/smoke-pages.js`、`tools/theme-editor/syntax-check-pages.js`、
  `tools/theme-editor/check-crop-centering.js`、`check-webp.js`、`check-preview-plate.js`；
  薄壳 `tools/theme-editor/image-editor.html`（无脚本，meta refresh → `index.html#images`）

**★ 源码 / 文件位置**

- 面板全量重建：`src/dash_display_rgb.cpp:1350`（`dash_display_panel_rebuild`，判据 `del + panel_create + reinit=1`）；
  轻量重初始化：同文件 `:1378`；**创建段 `panel_create()`**：同文件 `:1027`
- 时序宏：`src/dash_display_rgb.cpp:267-275`；**PCLK 宏与其来龙去脉**：同文件 `:241-266`（宏 `RGB_PIXEL_CLOCK_HZ` 在 `:263`）
- **bounce 行数**：源码默认 `src/dash_display_rgb.cpp:297`（现值 **30**，改动理由与"env 的 `-D` 会覆盖它"
  的坑写在同文件 `:292-297`）；落到驱动：`:1054`（`bounce_buffer_size_px`）；`0` = 退回旧行为
  ★ env 里真正生效的那两个 `-D`（都在 `platformio.ini`）：`[env:esp32s3-rgb-master-now]` 的 `:461`
  与 `[env:esp32s3-rgb-slave-now]` 的 `:558`
- 换帧（bounce 模式下"重新锁存"靠它）：`src/dash_display_rgb.cpp:685`；
  轮询：`dash_display_poll()` `:1494`；停顿自愈：`src/main.cpp:1914`（`kPanelHealStallMs=1000`）
- ★★ **逐帧碰 LVGL 样式的正确写法**：`src/dash_ui.cpp:82`（`ScreenUi::arc_red_mask`）+ 调用点
  `:1550-1552`（"先比后设"）；可参照的正确范例 `digit_red`：`:74` / `:840-841`
- 读数底片：`src/dash_ui.cpp:66`（`plate_main/plate_sub`）、`measure_text` 按**字形墨迹**量 `:643`、
  建对象 `:757`；层序 **表情 → 底片 → 弧 → 读数**（口径在同文件 `:601-602` 注释）
  ★ **10-16 起深浅进主题**：`readout.plate_opa`（0..255，默认 **38** = 15%）——
  宏 `READOUT_PLATE_OPA`、默认常量与 `static_assert` 在 `lib/themetool/ui_theme.h`；
  漂移对账脚本 `tools/theme-editor/check-preview-plate.js`（**41 项**；★ 别忘了 `ts()` 那层
  480 基准缩放，**别把 480 的数当像素** ✗）
- ★ **10-16 的主题修订 R2/R3/R4**（本仓库**没有版本号字段**，只有这类显式修订注记 ✓）：
  R2/R3 = `rpm_color` / `speed_color` / `plate_opa` 进 `ReadoutTheme`（旧主题**回落"文件里那份"
  `digit_color`**，回落发生在整个 `readout` 对象读完**之后** —— JSON 成员顺序无语义 ✓）；
  R4 = 主读数**单位标签**跟随该屏数字色（`digit_base_color(pk)`），`unit_color` **保留但无渲染消费者** ✓。
  锚点：`lib/themetool/ui_theme.h`（字段 + 转发宏 + `theme_clamp`）、`lib/themetool/theme_store.cpp`
  （三个新键的解析）、`src/dash_ui.cpp`（`digit_base_color()` 建屏与"退出红区回基础色"两处都走它）；
  JS 侧**唯一一份**同一规则的实现 = `tools/theme-editor/theme-json.js` 的 `applyReadoutFallbacks()` ✓
  ★ 样式仍**只在值变化时写一次**（沿用 `digit_red` 守卫）—— **没有**引入逐帧样式写入 ✓
- 左右映射：`lib/dashcore/dash_role_layout.h:106-113`（`roleThemeIndex()`：主板 ⇒ 速度表、从板 ⇒ 转速表）
- 告警阈值：红区 5500 / 高速 150 —— `lib/dashcore/alerts.h:82` / `:71`（退出迟滞：红区退 5300，`hyst=200`）；
  ★ **真正决定"脸变红"的门槛**在 `lib/dashcore/expression.cpp:56`（`kRpmRedlineFrom = 5500.0f`，已一并降下来）
- 表情超速档（**已裁决不对齐，别改**）：`lib/dashcore/expression.cpp:60-61`（>130 进 / ≤127 退）
- 字号档位表（副表档 = `kReadoutFontPx[0][2]` = 24）：`lib/themetool/ui_theme.h:442-476`
- 主题量程四键解析：`lib/themetool/theme_store.cpp`（`theme_parse_json` `:234`，`readout` 段 `:310` 起）
- 编辑器量程控件：`tools/theme-editor/index.html:759`（水温量程）/ `:769`（进气量程）
  （★ 合并成单页后行号可能已漂移，**认锚点文字不认行号** ✓）
- 主题分区写入：`esptool write_flash 0x210000`（只动主题、**不碰固件**；必须带 `PYTHONIOENCODING=utf-8`）；
  分区表 `partitions-s3-now.csv`（theme=`0x210000/0x4000`）；
  ★★ **出发点**：单改主题时用 `C:\206dash-scratch\theme-live.json`（2580 B，**R2 之前**的快照 ✗）——
  ★ **但它已不含 R2/R3/R4 的键** ⇒ **10-16 起一律先从板子 `read_flash 0x210000 0x4000` 回读一份**
  再改 ✓（§3 那条）； 另外两份**不要**当出发点：`tools/theme-editor/local/theme-fixed.json`
  （原始底稿 **2579 B**）、`tools/theme-editor/theme.json`（**1388 B**，不是同一个文件）
- **客户端刷写分区白名单**：`tools/app/server.py`（`ALLOWED_FLASH`：`theme`/`assets` **都 = `0x210000`**、
  `image = 0x254000`）；刷写入口 `do_flash()`（**页面传的 kind 不信、只从白名单取** ✓，且**不收 `offset=`** ✓）；
  HTTP 路由 `/api/flash`；静态响应 no-cache 在 `Handler.end_headers()`；页面来源在 `pages_source()` /
  `pages_info()` / `sha8()`（★ 改这几处要同时过 `check-flash-offset.py` 37 项 + `check-app.js`）
- 预览帧：`preview/frames/`；校验：`tools/theme-editor/check-preview-frame.js`
- 主题 A/B（宿主机验证"改了主题到底有没有生效"）：`tools/theme-editor/theme-ab.ps1`
  （编译一次 + 三臂各跑 42 s + 自动比对；★ **别用 `pio run -t exec`** —— 预览程序跑完 150 帧
  **不会自己退出**，会把后面几臂全堵死）+ `theme-ab-diff.js`（逐像素比，报差异数/包围盒/最大通道差）
- 串口脚本：`tools/serial-capture/{ota-cycle,soak-report,watch-utf8,capture-van-nopy,probe-retry}.ps1`；
  日志都落 `C:\206dash-scratch\`（`onset4-*.log` 仍是**已停**的只读档案，§3）
- **留档（最新一节在最后）**：`ACCEPTANCE.md`（**仓库根**，不在 `docs/`）最新一节 =
  「**2026-10-16（深夜）—— 编辑器 + 客户端收口**」（本批 12 单的 bug 与判据 + 收尾补记 `ab5a16c`）；
  ★ **`b0637b0`（原生窗口）那一单还没有写进 `ACCEPTANCE.md`** ✗ —— 判据只记在 §6 与提交信息里，
  下次要回查"窗口为什么是真的"就从这里走 ✓；
  上一节 = 「2026-10-15（晚）—— UI 迭代：位置回归、主题真源、描边死路、读数底片、黑屏真因」；
  `docs/BENCH-LOG.md` = 10-15 的更正与结案
- 构建设计：镜像目录 `C:\206dash-scratch\now-vanraw`（中文路径编不了；**它不是 git 仓库，是拷贝** ⇒
  改完仓库要同步过去，`robocopy $repo $copy /E /XD .git .pio`，**1/3=成功、≥8=失败**）；
  **必须带 `PYTHONUTF8=1`**（否则 `checkprogsize` 解码 gbk 报错）；刷写还要 `PYTHONIOENCODING=utf-8`
  （不设会让 upload 中途崩、板子变黑——`docs/BUILD-ENV.md` **坑 6**）；
  pcpreview 的 `lroundf` 兜底在 `src/main.cpp` **文件最顶部**
- ★★ **入口别用裸 `python`**：这台机器上它指向 `…\WindowsApps\python.exe`（商店占位），
  **不打印不报错、退出码 0** ⇒ 一整轮"跑完了"其实什么都没产出。只用
  `tools/build/pio.ps1 -Cmd '…'` 或 `& C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe -m platformio …`。
  **判据永远看产物（mtime/哈希/`[SUCCESS]`），不看退出码**（pio 成功时也可能返回 1）。
  系统化写在 `docs/BUILD-ENV.md` **坑 8**；同类还有 **坑 9**：`PLATFORMIO_BUILD_DIR` 也**必须纯 ASCII**
  （`C:\206dash-scratch\final-builds\<master|slave>`，一块板一个子目录）；
  **坑 10** = env 的 `-D` 覆盖源码默认值（bounce 那条就是它）✓
- ★ harness 的 shell 是 **Windows PowerShell 5.1**（**2026-10-21 实测仍是**：工具进程就是
  `C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe`，`PSEdition=Desktop`）⇒ 三条连带：
  ① 自己写的 `.ps1` **必须纯 ASCII**（无 BOM 的 UTF-8 会被按 GBK 读，连 `C:\Users\张九思\…` 这种
  字面路径都会解析坏 ⇒ robocopy 报 16）；② `*> log` 会**吞掉原生命令的 stderr**（gcc 的原话就没了）
  ⇒ 用 `cmd /c "… > log 2>&1"`；③ `$ErrorActionPreference='Stop'` 会把原生命令的**第一条 stderr**
  当终止错误、把构建掐死 ⇒ 用 `'Continue'` + 显式判据。
  ★★ **2026-10-21：车主装好了 pwsh 7.6.7**（`WindowsApps\pwsh.exe`，`PSEdition=Core`）。
  但它**没有**替换掉工具 shell —— 上面三条**照旧成立**。需要 7.x 能力时显式用
  `pwsh -NoProfile -File <脚本>`（实测 `tools\build\pio.ps1` 在 7.6.7 下跑 `pcpreview` 是
  **SUCCESS**）；★ 别从 5.1 里 `pwsh -Command '…含双引号…'`，引号会被 5.1 吃掉
  （坑 11，见 `docs/BUILD-ENV.md`）。
- ★ 掐死/中断过的 pcpreview 会留下 `program.exe` 占着文件 ⇒ 下一次链接报
  **"拒绝访问"**（日志里只有这一行）⇒ 先 `Get-Process program | Stop-Process -Force`。
- 构建环境事实（**2026-10-17 重新实测**，这一条上一版写反了）：`C:\.platformio\penv\…`（pio.ps1 用）
  与 `C:\206dash-scratch\pio-core-mix\penv\…`（跑 `tools/app/*.py` 那套检查脚本用）**两套都在**；
  `C:\206dash-repo` 与 `C:\206dash-build` **都存在、而且本轮真的在用** ——
  `C:\206dash-build` 是 `pio.ps1` 里 `PLATFORMIO_BUILD_DIR` 的默认值（native / pcpreview /
  两个 `-now` 的产物都在它下面），`C:\206dash-repo` 是 `-now` 两档必须的 **ASCII 镜像**
  （`robocopy <repo> C:\206dash-repo /MIR /XD .git .pio`，`docs/BUILD-ENV.md` 坑 5）。
  ★ 上一版写"已不存在"是错的：照着它做会以为不用镜像，直接在 CJK 路径下建 `-now` ⇒
  `TypeError: … __fspath__ returns … NoneType`。镜像在用的是 `now-vanraw`（`-now` 两档）与 `now`。
- 工具输出闸门：`maxInlineBytes: 50000` 在 `dsh-base/cordis.patch.yml`（**宿主平面，预设改不了** ✗）
