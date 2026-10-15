# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是"读代码"，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `docs/BENCH-LOG.md` 与各专题文档）。
★ 本次改写：**2026-10-15 深夜收工**——① **屏幕故障（② 类）今晚又发一次，车主决定【挂起】、不再追**（§1/§3/§4）；
② 当前活跃线换成**编辑器「车主模式」+ 客户端打包**，四单已推（`fc9daf7` → `fa469fd` → `03e4ca4` → `b6d5881`，§6）；
③ 两个 **6 小时**串口长记录**已全部停止**（§3）；④ 固件仍是两块板 `a74b6d1`（§3）。

---

## 1. 下一步第一动作

★★ **屏幕故障（② 类）今晚已由车主决定【挂起】** ✓ —— **不再继续追这条线** ✗（处置与证据见 §3、§4）。
所以"下一步"不再是读串口日志，而是**把编辑器这一单收掉**。

★ **在跑的一单**（子代理，尚未落地，见 §6）：① 背景槽「**取景框**」；② 表情框选「**居中修正**」+ 断言；
③ **WebP 支持**。它交付后立刻做**车主两步验收**：

1. **在真浏览器里走一遍导出链路** —— 拖一张**主体不在中心**的 PNG（或用那张 16:9 的 WebP 背景）
   → 框选 / 取景 → 「**保存安装包**」⇒ 把 `assets.bin` 交给**父代理**逐槽核对
   （判据 = `check-image-bin.js` + `asset-package.js unpack` ✓，不看"导出按钮点成功了"）。
2. **在客户端界面里真刷一次**，确认偏移 `0x210000` ✓（`kind=assets`；见 `tools/app/server.py:112-115`）。
   ★ 刷写要**独占串口** ⇒ 现在 **COM8/COM9 都空闲**（§3），**动手前无需再等** ✓。

★ 编辑器静态文件的回归闸门 = `tools/theme-editor/check-owner-mode.js`（含 `.golden-hashes.json`
的**逐字节指纹**口径，`check-owner-mode.js:573-610`）与 `syntax-check-pages.js` ✓ —— 改页面必跑。

**接线侧没有待办项** ✓：**BLE OBD** ✓ 与 **VAN 抓帧 @ `GPIO44`** ✓ 两条数据通路都已实测通过
（2026-10-15 车主确认）；原先那条"接 VAN 前先确认 `GPIO44` 到底连到哪一边（怕默认选边被切给
板载 CH343P）"**作废** ✓ —— 44 是通的，实证比推断强。
接线口径 = `PINOUT.md` 的「装车接线」一节（`PINOUT.md:681`）。

## 2. 待办（按价值排）

1. **★ 编辑器那一单收尾 = §1 那两步验收**（子代理交付后）：取景框 / 居中断言 / WebP 三条都要在
   **真浏览器**里过一遍，再交给父代理核对 `assets.bin` ✓。★ **别只看单测绿** ✗ —— 这一单的坑
   全在真实拖拽与真实解码上（下面第 4 条那条"框选偏心"就是活例子）。
2. **★ 串口记录脚本补"静默留痕"**（今晚新发现，**待补、不急**）：`tools/serial-capture/watch-utf8.ps1`
   （`now-vanraw` 镜像里那份同一份）只在**真异常**时写 `--- port reopened ---`，
   而 `ReadTimeout`（`$p.ReadTimeout = 300`，`watch-utf8.ps1:50`）走的是 `continue`（同文件 `:76-83`）
   ⇒ **"串口静默"这种最重要的故障形态反而不留痕** ✗ —— 今晚就是因此**没覆盖到故障那一刻**（§3）。
   ⇒ 应加"**静默 N 秒就写一行 `--- silent N s ---`**" ✓（判据要能从日志一眼看出"它从哪一刻不说话了"）。
3. **副读数字号真上档**（可选，车主未定）：现在副表在 480 档取 **24 号**（`kReadoutFontPx[0][2]`，
   `lib/themetool/ui_theme.h:449-453`），**已经是副表那一档的顶了** ✗；要更大只能改固件 ——
   开 `LV_FONT_MONTSERRAT_26/28/32` + 加一档 + 同步 `theme_clamp` 上限 / `static_assert` / 用例 ✓
   （`ui_theme.h:442-476` 那段注释就是这条的口径）。
4. **轮上第三块板**（`docs/WHEEL-REMOTE.md`）：候选型号与排除项已填进它的 **§2.1** ✓；
   还差**人民币价**与 **mm 尺寸**（车主核 ✓）。已排除：M5Stack **Dial**（只有蜂鸣器）、
   微雪 **Touch-LCD-1.28**（无音频器件）。
5. **遗留（已裁决，别再提议）**：`expression.cpp` 的表情**超速档仍是 >130 / ≤127**
   （`lib/dashcore/expression.cpp:60-61`），与告警 150 不一致 —— 车主明确说**不用对齐** ✓。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| **主板 COM9**（右 = 速度 + 进气） | `a74b6d1`（HEAD 这一版） | **静音** ✓；今晚串口**静默过一次**（见下）⇒ 按**复位键**后恢复 ✓；主题分区 `0x210000` = `theme-live.json`，**回读逐字节验证过** ✓；素材 `0x254000` **未动** ✓ |
| **从板 COM8**（左 = 转速 + 水温） | `a74b6d1`（同版） | **静音** ✓；**整晚没复位、没静默** ✓；同一份主题、回读逐字节验证过 ✓ |

★ **这一版两块板都一样，含**：① 删掉**整条指示灯栏** ✓；② 砍掉**整条蜂鸣器** ✓；
③ 转速红区 **5500**（弧 + 加粗）✓；④ 高速 **150**（弧 + 大数字变红）✓；⑤ bounce **30 行** ✓；
⑥ **读数底片**（读数下面垫一块静态半透明近黑圆角块，不透明度 **15% = 38/255**）✓。

### 3.1 ★★ 今晚（2026-10-15 晚）屏幕故障（② 类）的处置与结论 —— **车主决定挂起**

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

★★ **要长跑记录 / 要刷写时不要开着 `tools/app` 主题编辑器服务** ✗ —— 它一开串口就会动 DTR/RTS、
把 DTR/RTS 拉住 ⇒ **板子被按在复位里 ⇒ 屏全黑、连背光都没有**（2026-10-15 的真因，见 `ACCEPTANCE.md`
同日晚第 6 节 ✓）。抓串口一律用 **DTR/RTS 全 false** 的 `tools/serial-capture/probe-retry.ps1` ✓。

★ **横纹判据分三层**（先看逐笔计数器）：`inv=` → `flush=` → `copy_avg=`。
好档：`inv=` **2 万~23 万 px²/s**、`flush=` 21~44/s、`copy_avg=` 166~253 µs ✓；
坏档签名（逐帧碰 LVGL 样式那一次）：`inv=856,980`、`flush=80/s`、`copy_avg≈950 µs` ✗。
（今晚复位后那份日志就是好档：`inv=60325px2`、`flush=27/s`、`copy_avg=233us`、`timeout=0` ✓ —— 留作对照。）

★ **协议**：②（横纹 / 偏移 / 黑屏）出现 ⇒ **先别复位**、立刻通知（**但今晚这一条已被车主挂起**，见 §4）；
①（黑屏重启）⇒ 几秒内抓串口要 `boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。
★ 台面上 ② 唯一有效的恢复手段仍然是**按复位键**（重初始化 / 外设重启全 ✗）—— 今晚**再次实证** ✓。
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
★ 2026-10-15 复核：两块板刷完今日改动后 `rgb: … pclk=12000000Hz` **仍未变** ✓；
⇒ "降 PCLK 12 → 10 MHz"（预计 vsync 约 36/s）**随 ② 一并搁置** ✗。

## 6. 客户端（tools/app）现状

- **三个屏**（`a8b2c11` 把七个页签收成 3 个）：刷固件 / 表盘 / 工具；去术语 + 设计系统。
  `check-app.js` 卡着"恰好 3 个屏 + 32 项能力表 + 术语扫描"三道。
- ★★ **2026-10-15 深夜的四单（已全部推到 `origin/main` ✓，HEAD = `origin/main` = `b6d5881` ✓）**：
  - `fc9daf7` —— **车主模式**（默认）+ **表情框选器** + **3 预设** + 告警文案按"蜂鸣器已删"改写 ✓；
  - `fa469fd` —— **槽位清单 11 格**（背景 ×1 + 表情 ×10）搬进车主模式的**图片区** ✓（技术参数仍留高级）；
  - `03e4ca4` —— **配色页与图片页合并成一个页面** ✓：两个**页内分区**共用同一份状态；
    `tools/theme-editor/image-editor.html` 变成**无脚本重定向薄壳** → `index.html#images` ✓；
  - `b6d5881` —— **打进客户端** ✓：表盘屏**只留一个编辑器**；客户端内「保存安装包」走 `/api/flash`
    （`kind=assets`、**偏移固定 `0x210000`**、端口可选 ✓）；编辑器静态文件**真打进 exe** ✓
    （7 条真实请求清单 ✓）；exe = `tools/app/dist/206Dash-Workbench.exe` **9,358,724 B** ✓。
- ★ **三个指纹**（改前 / 改后与 golden 逐字节一致 ✓）仍是
  `themeJson 2cf2c424…` / `imageBin 24fc3c87…` / `assetsBin 63fe3276…` ✓
  （逐字钉在 `tools/theme-editor/check-owner-mode.js:598-606` ✓）。
  ★ **注意**：这是**旧素材**的指纹 ✓；**车主换素材后指纹必然变化** ✓，别把它当成"格式变了" ✗。
- ⏳ **在跑的一单**（子代理，工作区里已见 `asset-spec.js` / `index.html` 有未提交改动 ✓）：
  ① 给**背景槽**加「**取景框**」（**cover 语义**：480×480 框内即画面、可平移 / 缩放、
  **不丢到透明** ✗ —— 与表情那套"框外丢透明"**不同** ✓）；
  ② 修「**表情框选后不够居中**」并加一条**可复现断言**（偏心框选 ⇒ 非透明像素包围盒中心 =
  目标中心 **±1 px** ✓；新文件 `tools/theme-editor/check-crop-centering.js`）；
  ③ 加 **WebP 支持**（浏览器解码 → 既有 canvas 路径 ✓；背景允许 ✓；表情用 WebP 仍必须**真有 alpha** ✓）。
- ★ **车主新素材**（都已核对）：
  - `E:\痛一会\表情包_360圆形` = **10 张 360×360 PNG** ✓（转速表-1..5 + 速度表-1..5 ✓，
    正好对 **10 个表情槽** ✓；**360 > 320 会被缩到 320** ✓，**已经居中带透明 ⇒ 可跳过框选** ✓）；
  - 背景候选 = `.dsh-drop` 下那张 **WebP**：`Millennium_Intro.webp` = **1280×720（16:9）** ✓
    （VP8 有损，61536 B ✓）⇒ **必须取景** ✓（1:1 只能保留 **720×720** 的方形窗口，
    **宽度要裁掉约 44%** ✓）。
- **待车主的两步验收** = **§1**（真浏览器导出 → 父代理逐槽核对；再在客户端里真刷一次确认 `0x210000`）。
- 预览表盘已内联；★ **预览帧必须同步进仓库** `preview/frames/`（150× `l_*.bmp` + 150× `r_*.bmp`，
  今日实点仍是 150 + 150 ✓）—— 渲染器写进的是镜像目录，不同步就"图裂" ✗（已踩过一次）。
- 无线刷写「上传后自动验槽」：`/api/ota-verify` 已**幂等**（板子已在 OTA 模式就不再发 `u`，免得重计时）。
  ★ 口径修正：**只有服务端端点**是 curl 实测过的；客户端 `verifySlot` 那条路修好后**未再单独实测**。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（**9,358,724 B** ✓；`build-exe.ps1` **默认 onedir** ✗，
  要加 `-OneFile`）。
- ★★ **更正（10-15 深夜）**：`tools/app` 服务**其实正在跑** ✗ ——
  `python tools/app/server.py` **两个进程**（都在 `23:41` 起），监听 **`127.0.0.1:8787`** ✓
  （`GET /` = 200、title「206Dash 表盘助手」✓）。**它不是坏东西**（它负责 §1 第 2 步的刷写，
  `/api/ports` 也刚用过 ✓），但 §3 那条坑**照样成立**：**长跑记录时别让它开串口** ✗。
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

**★ 今晚（10-15 深夜）的新锚点**

- 串口静默脚本的缺口：`tools/serial-capture/watch-utf8.ps1:50`（`ReadTimeout = 300`）与
  `:76-83`（超时走 `continue`）；唯一的标记行在 `:93`（`--- port reopened … ---`）
- 三个指纹的**逐字钉点**：`tools/theme-editor/check-owner-mode.js:598-606`；golden 文件
  `tools/theme-editor/.golden-hashes.json`，重算器 `tools/theme-editor/make-golden-hashes.js --print`（只算不写）
- 客户端刷写分区的**白名单**：`tools/app/server.py:112-115`（`theme`/`assets` **都 = `0x210000`**）；
  真正的刷写入口 `do_flash()` 同文件 `:264-276`（**页面传的 kind 不信、只从白名单取** ✓）；
  HTTP 路由 `/api/flash` 同文件 `:534`
- 编辑器「车主模式」的回归闸门：`tools/theme-editor/check-owner-mode.js`、
  `tools/theme-editor/syntax-check-pages.js`、`tools/theme-editor/check-crop-centering.js`（在跑那一单新增）；
  薄壳 `tools/theme-editor/image-editor.html`（无脚本，meta refresh → `index.html#images`）
- 故障期 / 复位后的两份**只读档案**（都已冻结，别再等它长 ✗）：
  `C:\206dash-scratch\onset4-master.log`（63,648 行）、`onset4-slave.log`（58,537 行）、
  `master-afterreset.log`（312 行，含那条 `prev_up=320min` 的开机横幅 ✓）

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
  ★★ **出发点永远是 `C:\206dash-scratch\theme-live.json`（2580 B = 板上真正在跑的那份）** ✓ ——
  **不是** `tools/theme-editor/local/theme-fixed.json`（原始底稿 **2579 B**），
  **也不是** `tools/theme-editor/theme.json`（**1388 B**，不是同一个文件）
- 预览帧：`preview/frames/`；校验：`tools/theme-editor/check-preview-frame.js`
- 主题 A/B（宿主机验证"改了主题到底有没有生效"）：`tools/theme-editor/theme-ab.ps1`
  （编译一次 + 三臂各跑 42 s + 自动比对；★ **别用 `pio run -t exec`** —— 预览程序跑完 150 帧
  **不会自己退出**，会把后面几臂全堵死）+ `theme-ab-diff.js`（逐像素比，报差异数/包围盒/最大通道差）
- 串口脚本：`tools/serial-capture/{ota-cycle,soak-report,watch-utf8,capture-van-nopy,probe-retry}.ps1`；
  日志都落 `C:\206dash-scratch\`（今晚在跑的 `onset4-*.log` **已停**，§3）
- **今天的留档**：`ACCEPTANCE.md`（**仓库根**，不在 `docs/`）最后一节 =
  「2026-10-15（晚）—— UI 迭代：位置回归、主题真源、描边死路、读数底片、黑屏真因」；
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
- ★ harness 的 shell 是 **Windows PowerShell 5.1**（不是 pwsh 7）⇒ 三条连带：
  ① 自己写的 `.ps1` **必须纯 ASCII**（无 BOM 的 UTF-8 会被按 GBK 读，连 `C:\Users\张九思\…` 这种
  字面路径都会解析坏 ⇒ robocopy 报 16）；② `*> log` 会**吞掉原生命令的 stderr**（gcc 的原话就没了）
  ⇒ 用 `cmd /c "… > log 2>&1"`；③ `$ErrorActionPreference='Stop'` 会把原生命令的**第一条 stderr**
  当终止错误、把构建掐死 ⇒ 用 `'Continue'` + 显式判据。
- ★ 掐死/中断过的 pcpreview 会留下 `program.exe` 占着文件 ⇒ 下一次链接报
  **"拒绝访问"**（日志里只有这一行）⇒ 先 `Get-Process program | Stop-Process -Force`。
- 构建环境事实（2026-10-14 实测）：`C:\.platformio\penv\…`（pio.ps1 用）与
  `C:\206dash-scratch\pio-core-mix\penv\…`（本轮用）**两套都在**；
  `C:\206dash-repo`、`C:\206dash-build` **已不存在**（文档旧写法）；
  镜像在用的是 `now-vanraw`（`-now` 两档）与 `now`。
- 工具输出闸门：`maxInlineBytes: 50000` 在 `dsh-base/cordis.patch.yml`（**宿主平面，预设改不了** ✗）
