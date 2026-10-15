# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是"读代码"，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `docs/BENCH-LOG.md` 与各专题文档）。
★ 本次改写：**2026-10-15 收工**——两块板都在 `a74b6d1`（灯条已删 / 蜂鸣器已砍 / 红区 5500 /
高速 150 / bounce 30 行 / 读数底片 15%）；主题有了**单一真源** `theme-live.json`（§3）；
两个 **6 小时**串口长记录正在跑（§3）；`tools/app` 主题编辑器服务今天已关掉（§3）。

---

## 1. 下一步第一动作

★★ **横纹（②）再出现时：先别按复位，也别开串口 —— 去读现成的长记录。**

1. **读 `C:\206dash-scratch\onset4-slave.log`（从板 COM8）/ `onset4-master.log`（主板 COM9）**
   —— 这两个 6 小时记录今天就在跑（§3）。
   ★ **不要为了"再看一眼"去开串口** ✗：开串口会动 DTR/RTS、可能把板子复位，现场当场毁掉 ✓。
2. **判据只有两条**：
   - **无**新的 `boot: reason=` **且** **无** `port reopened` ⇒ **面板自己在连续运行后丢了状态**
     ⇒ 治**时序余量**：**降 PCLK 12 → 10 MHz** ✓（代价 = 刷新率 **43 → 36 Hz** ✗）。
   - **有**任一 ⇒ 是一次**电气 / USB 事件**（上电、复位、端口被重开）⇒ 治**供电** ✓（§4）。
3. 结论出来之前不动固件；真要上车复现，照 `docs/DRIVE-TEST.md` 执行。

**接线侧没有待办项** ✓：**BLE OBD** ✓ 与 **VAN 抓帧 @ `GPIO44`** ✓ 两条数据通路都已实测通过
（2026-10-15 车主确认）；原先那条"接 VAN 前先确认 `GPIO44` 到底连到哪一边（怕默认选边被切给
板载 CH343P）"**作废** ✓ —— 44 是通的，实证比推断强。
接线口径 = `PINOUT.md` 的「装车接线」一节（`PINOUT.md:681`）。

## 2. 待办（按价值排）

1. **★ 横纹分流 = §1 那两条**：先读 `onset4-*.log` 判"面板丢状态 vs 电气事件"，再决定是
   降 PCLK（12 → 10 MHz）还是治供电。★ **没有新证据就先别动** —— 台面已无横纹、车上不犯即结案 ✓。
2. **副读数字号真上档**（可选，车主未定）：现在副表在 480 档取 **24 号**（`kReadoutFontPx[0][2]`，
   `lib/themetool/ui_theme.h:449-453`），**已经是副表那一档的顶了** ✗；要更大只能改固件 ——
   开 `LV_FONT_MONTSERRAT_26/28/32` + 加一档 + 同步 `theme_clamp` 上限 / `static_assert` / 用例 ✓
   （`ui_theme.h:442-476` 那段注释就是这条的口径）。
3. **轮上第三块板**（`docs/WHEEL-REMOTE.md`）：候选型号与排除项已填进它的 **§2.1** ✓；
   还差**人民币价**与 **mm 尺寸**（车主核 ✓）。已排除：M5Stack **Dial**（只有蜂鸣器）、
   微雪 **Touch-LCD-1.28**（无音频器件）。
4. **遗留（已裁决，别再提议）**：`expression.cpp` 的表情**超速档仍是 >130 / ≤127**
   （`lib/dashcore/expression.cpp:60-61`），与告警 150 不一致 —— 车主明确说**不用对齐** ✓。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| **主板 COM9**（右 = 速度 + 进气） | `a74b6d1`（HEAD 这一版） | **静音**（蜂鸣器已整条砍掉 ✓）；主题分区 `0x210000` = `theme-live.json`，**回读逐字节验证过** ✓；素材 `0x254000` **未动** ✓ |
| **从板 COM8**（左 = 转速 + 水温） | `a74b6d1`（同版） | **静音** ✓；同上，**同一份主题、回读逐字节验证过** ✓ |

★ **这一版两块板都一样，含**：① 删掉**整条指示灯栏** ✓；② 砍掉**整条蜂鸣器** ✓；
③ 转速红区 **5500**（弧 + 加粗）✓；④ 高速 **150**（弧 + 大数字变红）✓；⑤ bounce **30 行** ✓；
⑥ **读数底片**（读数下面垫一块静态半透明近黑圆角块，不透明度 **15% = 38/255**）✓。

★ **主题（两块板同一份）= 单一真源 `C:\206dash-scratch\theme-live.json`**
（**2580 B**，md5 `73af24423025606d2ec1475bc312ccb4`）：`digit_cy=88` / `unit_cy=110` ✓、
`coolant_color = intake_color = digit_color = 0xBF4153` ✓；**其余与原始底稿相同** ✓。
两块板都**回读逐字节验证过** ✓（判据 = `esptool read_flash 0x210000 0x4000` 回读 + 比对）。

★ **正在跑**：两个 **6 小时**串口长记录 —— `C:\206dash-scratch\onset4-slave.log`（COM8）与
`onset4-master.log`（COM9），**父代理挂的** ✓（子代理起的后台任务随它结束而死 ✗）。
判据：挂完隔 **20~30 秒**看 `Length` / mtime 有没有在长 ✓（只看"任务报 running"不够 ✗）。

★★ **要长跑记录时不要开着 `tools/app` 主题编辑器服务** ✗ —— 它会占串口、把 DTR/RTS 拉住
⇒ **板子被按在复位里 ⇒ 屏全黑、连背光都没有**（2026-10-15 的真因，见 `ACCEPTANCE.md`
同日晚第 6 节 ✓）；**今天已经把它关掉** ✓。抓串口一律用 **DTR/RTS 全 false** 的
`tools/serial-capture/probe-retry.ps1` ✓。

★ **横纹判据分三层**（先看逐笔计数器）：`inv=` → `flush=` → `copy_avg=`。
好档：`inv=` **2 万~23 万 px²/s**、`flush=` 21~44/s、`copy_avg=` 166~253 µs ✓；
坏档签名（逐帧碰 LVGL 样式那一次）：`inv=856,980`、`flush=80/s`、`copy_avg≈950 µs` ✗。
★ **协议**：②（横纹 / 偏移 / 黑屏）出现 ⇒ **先别复位**，立刻通知；①（黑屏重启）⇒ 几秒内抓串口要
`boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。
★ 台面上 ② 唯一有效的恢复手段仍然是**按复位键**（重初始化 / 外设重启全 ✗）。
★ 打开串口采集**不会**复位板子 ✓（10-14 那份日志起点无 `boot:` 行为证）；
但 **esptool 复位**常把它弄哑 ⇒ 唯一可靠恢复是**重刷**。
★ 回滚产物还留在 `C:\206dash-scratch\`：`fw-prev-good-2bc5b38-slave.bin`、`fw-new-a076acd-slave.bin`、
`rollback-*.log`、`streak-slave.log`、`fix-verify.log` —— ★ 回滚 A/B 靠这些**留档产物**，
别再指望**板上**还留着旧档 ✗。

## 4. ② 类的已知结论（省得重试）

| 手段 | 结果 |
|---|---|
| 只重写面板寄存器（`r`） | ✗ |
| `r` + `rgb 外设重启` / 重配时钟 | ✗ |
| `R`（`del` + `panel_create`） | ✗✗ 画面卡死 |
| 按复位键（真正一次启动） | ✓ 唯一有效 |
| 逐帧无条件碰 LVGL 样式（2026-10-15 的坏档） | ✓ 可修 —— **先比后设**（`af52b98`）；★ 这一档**有**机器签名：`inv=` ×5~6、`flush=` 80/s、`copy_avg≈950 µs |

⇒ ② **本身没有可检测信号**（`panelguard` / `timeout` / `stall` / `fullrb` / `boot: reason` 全 0）——
  但"没有签名"只对 ② 成立：**横纹若伴随 `inv=` 暴涨，那是软件逐帧路径，不是 ②** ✓。
⇒ 10-11 那夜的收窄结论：**② 与芯片复位不是必然同时发生**（瞬态打芯片 5 次、打面板 0 次）。
⇒ 10-15 早上那次"偏移自愈"**已结案**：`boot: reason=UNKNOWN n=33 | prev=POWERON prev_up=30min`
  ⇒ 是一次**上电复位**（笔记本恢复时 USB 反复上下电，≈10:03）⇒ **§20"只有复位才有效"继续成立** ✓
  （见 `docs/BENCH-LOG.md` 的「2026-10-15 结案」）。
⇒ 待做 A/B：换**带独立供电的 USB hub**（手边暂无设备）—— §1 判成"电气事件"时最终也归到这条。
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
**12 MHz→约 44、15→约 54、18→约 65、30→约 108**。
★ 2026-10-15 复核：两块板刷完今日改动后 `rgb: … pclk=12000000Hz` **仍未变** ✓；
⇒ 若 §1 判成"面板丢状态"，就往 **12 → 10 MHz** 试（预计 vsync 约 36/s，§1 的代价那一条）。

## 6. 客户端（tools/app）现状

- **三个屏**（`a8b2c11` 把七个页签收成 3 个）：刷固件 / 表盘 / 工具；去术语 + 设计系统。
  `check-app.js` 卡着"恰好 3 个屏 + 32 项能力表 + 术语扫描"三道。
- 预览表盘已内联；★ **预览帧必须同步进仓库** `preview/frames/`（150× `l_*.bmp` + 150× `r_*.bmp`，
  今日实点仍是 150 + 150 ✓）—— 渲染器写进的是镜像目录，不同步就"图裂" ✗（已踩过一次）。
- 无线刷写「上传后自动验槽」：`/api/ota-verify` 已**幂等**（板子已在 OTA 模式就不再发 `u`，免得重计时）。
  ★ 口径修正：**只有服务端端点**是 curl 实测过的；客户端 `verifySlot` 那条路修好后**未再单独实测**。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（9,228,688 B；`build-exe.ps1` **默认 onedir** ✗，
  要加 `-OneFile`）。
- ★ **两个 6 小时记录在跑的这段时间，别启动这个服务** ✗（它占串口 ⇒ 板子被按在复位、屏全黑，
  见 §3）—— 要动它请等记录收工。
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
  日志都落 `C:\206dash-scratch\`（今天在跑的 = `onset4-slave.log` / `onset4-master.log`，§3）
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
