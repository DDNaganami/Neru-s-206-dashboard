# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是"读代码"，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `docs/BENCH-LOG.md` 与各专题文档）。
★ 本次改写：**2026-10-15 晚**（删灯条 / 砍蜂鸣器 / 红区 5500 / 高速 150 已上板；修掉一次
**自己改出来的**逐帧样式回归；两块板同在 `4fcee1a`）。

---

## 1. 下一步第一动作

★★ **今天三件事都收口了**：

1. **产品改动交付**：`a076acd`（34 files, +930/−1610）—— ① 删掉整条指示灯栏（六格）；② **砍掉蜂鸣器整条**
   （屏上不再有任何响声）；③ 转速红区 **5500**（弧的点亮段染红 + 加粗 1.25 倍）、高速 **150**（弧 + 速度大数字变红）。
   native **383 用例（381 通过 / 2 跳过）**、构建 `[SUCCESS]`（四项）；两块板各 4 段写入 + `Hash of data verified`。
2. **两条数据通路都实测通过 ✓（不再有待测项）**：**BLE OBD** ✓、**VAN 抓帧 @ `GPIO44`** ✓（2026-10-15 车主确认）。
   ⇒ 原先那条"接 VAN 模块前先确认 `GPIO44` 到底连到哪一边（怕默认选边被切给板载 CH343P）"**作废** ✓ ——
   44 是通的，实证比推断强。
3. **一次自己改出来的回归已修复归档**：`src/dash_ui.cpp` 逐帧**无条件**碰 LVGL 样式（`arc_apply_red`）
   ⇒ LVGL setter 不比较值、无条件 invalidate 整个包围盒 ⇒ 每帧重绘 **×5**（`inv=` 130k → **856,980 px²/s**）
   ⇒ 从板**持续横纹/偏移**。修复 `af52b98`（`ScreenUi::arc_red_mask` 位掩码，"**先比后设**"）；
   归档 `4fcee1a`（`ACCEPTANCE.md` +71 行、`BENCH-LOG.md` 更正一处误读）。

**下一步第一动作**：**两条数据通路都已实测通过 ✓** —— **BLE OBD** ✓、**VAN 抓帧 @ `GPIO44`** ✓
（2026-10-15 车主确认）；原先打算做的那份"接线还差什么"清点因此**作废**，接线侧**没有待办项**。
仅剩两件，都在 §2：① **带条件**的 bounce 加档（**只有车上仍见横纹才做**）；② 轮上板选型补价 / 尺寸。
下次上车就照 `docs/DRIVE-TEST.md` 执行（接线口径 = `PINOUT.md` 的「装车接线」一节，`PINOUT.md:681`）。

## 2. 待办（按价值排）

1. **★ 带条件：若车上仍见横纹** ⇒ 首选"**加大 bounce 行数**"（10 → 20）。
   当前 `RGB_BOUNCE_LINES = 10`（`src/dash_display_rgb.cpp:290`，= 微雪官方 2.8C 例程取值）：
   10 行 × 480 px × 2 B = **9.6 KB/块**，两块共 **19.2 KB** 内部 SRAM；+10 行 ⇒ +9.6 KB/块、两块
   **+19.2 KB**（合计 38.4 KB）。余量：今日实测 `heap=117 KB`、PSRAM 空闲 **7282 KB**
   （与 `docs/BLE-OBD.md:409` 同一读数）⇒ 完全放得下 ✓；480 行 ÷ 20 = 24 整除，满足
   `esp_lcd_new_rgb_panel` 的 `fb_size % bb_size == 0` ✓。
   ★ **它没有台面判据**（`inv=` 不受影响），只能靠车主眼睛 ⇒ **现在不动**；
   **台面已无横纹、车上不犯即结案** ✓。
   （历史：10 → 20 做过 A/B，见 `ACCEPTANCE.md`「副屏横纹」一节；后来"12 MHz 下 10 行够用"又把默认值降回 10。）
2. **轮上板（第三块板）选型**：`docs/WHEEL-REMOTE.md` §2.1 候选型号表已就位，待补 **人民币价**与 **mm 尺寸**
   （微雪 1.46 有官方结构图可量）。已排除：M5Stack **Dial**（只有蜂鸣器）、微雪 **Touch-LCD-1.28**（无音频器件）。
3. **遗留（已裁决，别再提议）**：`expression.cpp` 的表情**超速档仍是 >130 / ≤127**（`lib/dashcore/expression.cpp:60-61`），
   与告警 150 不一致 —— 车主明确说**不用对齐** ✓。
4. **长跑记录（低优先）**：10-15 03:29 笔记本重启把主板记录杀了（丢了 4.65 h 之后全部）⇒ 要再跑就
   按小时分片 / 自动续采（现成件 `C:\206dash-scratch\probe-retry.ps1`）。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| 主板 COM9 | `4fcee1a`（修复版 + 今日三项改动） | **静音**（蜂鸣器已整条删）；主题 `0x210000` / 素材 `0x254000` 分区**未动** ✓；车主报**当前无横纹** ✓ |
| 从板 COM8 | `4fcee1a`（同版；今天走过 `a076acd` 复现 → 回滚 `2bc5b38` 取证 → `af52b98` 修复版） | **静音**；★ 回滚 A/B 靠**留档产物**，别再指望板上还留着旧档 |

★ 回滚产物（`C:\206dash-scratch\`）：`fw-prev-good-2bc5b38-slave.bin`（1,495,312 B）、
`fw-new-a076acd-slave.bin`（1,491,552 B）、`rollback-slave.log`、`rollback-verify.log`；
坏档对照 `streak-slave.log`、修复后验收 `fix-verify.log`。
★ **横纹判据分三层**（今天新学的）：先看 `inv=` / `flush=` / `copy_avg=`（逐笔计数器）⇒ 再怀疑 ② ⇒ 最后才怀疑硬件。
今天就靠 `inv=` 一击定位（坏档签名 `inv=856,980 px²/s`、`flush=80/s`、`copy_avg≈950 µs`；
修后从板 `inv=` 5.8万–17.3万、`flush=` 23–35/s、`copy_avg=` 166–253 µs；主板 `inv=` 2.3万–25.8万、`flush=` 21–44/s）。
★ **协议**：②（横纹/偏移/黑屏）出现 ⇒ **先别复位**，立刻通知；①（黑屏重启）⇒ 几秒内抓串口要
`boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。
★ 台面上 ② 唯一有效的恢复手段仍然是**按复位键**（重初始化/外设重启全 ✗）。
★ 打开串口采集**不会**复位板子 ✓（10-14 那份日志起点无 `boot:` 行为证）；但 **esptool 复位**常把它弄哑
⇒ 唯一可靠恢复是**重刷**。

## 4. ② 类的已知结论（省得重试）

| 手段 | 结果 |
|---|---|
| 只重写面板寄存器（`r`） | ✗ |
| `r` + `rgb 外设重启` / 重配时钟 | ✗ |
| `R`（`del` + `panel_create`） | ✗✗ 画面卡死 |
| 按复位键（真正一次启动） | ✓ 唯一有效 |
| 逐帧无条件碰 LVGL 样式（2026-10-15 的坏档） | ✓ 可修 —— **先比后设**（`af52b98`）；★ 这一档**有**机器签名：`inv=` ×5~6、`flush=` 80/s、`copy_avg≈950 µs |

⇒ ② **本身没有可检测信号**（`panelguard`/`timeout`/`stall`/`fullrb`/`boot: reason` 全 0）——
  但"没有签名"只对 ② 成立：**横纹若伴随 `inv=` 暴涨，那是软件逐帧路径，不是 ②** ✓。
⇒ 10-11 那夜的收窄结论：**② 与芯片复位不是必然同时发生**（瞬态打芯片 5 次、打面板 0 次）。
⇒ 10-15 早上那次"偏移自愈"**已结案**：`boot: reason=UNKNOWN n=33 | prev=POWERON prev_up=30min`
  ⇒ 是一次**上电复位**（笔记本恢复时 USB 反复上下电，≈10:03）⇒ **§20"只有复位才有效"继续成立** ✓
  （见 `docs/BENCH-LOG.md` 的「2026-10-15 结案」）。
⇒ 待做 A/B：换**带独立供电的 USB hub**（手边暂无设备）。
★ 台面历史上偶发的**轻微**横纹按「**笔记本 USB 供电边缘**」记录 —— 不是固件缺陷，也不是今天那次回归的遗留 ✓。

## 5. 面板时序 vs 官方（2026-10-11 对照：消隐区没问题）

来源：`C:\206dash-scratch\ws-28c-demo\...\Display_ST7701.h`（微雪官方示例）

| 参数 | 官方 | 我们（`src/dash_display_rgb.cpp:267-277`） | 一致 |
|---|---|---|---|
| HPW/HBP/HFP | 8/10/50 | 8/10/50 | ✓ 逐字相同 |
| VPW/VBP/VFP | 2/18/8 | 2/18/8 | ✓ 逐字相同 |
| 极性 | 默认 | `PCLK_ACTIVE_NEG=0`、HS/VS idle=0 | ✓ |
| **PCLK** | 30 MHz（Arduino 例程） | **12 MHz** | ★★ 见下 |

★★ **更正（2026-10-14，"PCLK 是 ② 的嫌疑"这个说法作废）**：读源码注释
（`src/dash_display_rgb.cpp:241-266`）可见 PCLK 是**故意一路降下来的** ——
**30 → 18 → 15 → 12 MHz**，原因是**"横纹随刷新移动"**＝面板消耗速率逼近 PSRAM 搬运带宽
（`pclk × 2 B`：18 MHz 就 36 MB/s，而 CPU 从 PSRAM 搬只有 ~30 MB/s 量级 + 还要和重绘抢）。
⇒ **往 30 MHz 走是已知更糟的方向**（当年 30 MHz 整屏花），**不要拿它做 ② 的 A/B**；
真要试只能往**更低**走。
★ 副产品：`rgb: vsync=…(+N/s)` 的 N 就是 PCLK 的**第一手判据** ——
**12 MHz→约 44、15→约 54、18→约 65、30→约 108**。
★ 2026-10-15 复核：两块板刷完今日改动后 `rgb: … pclk=12000000Hz` **仍未变** ✓。

## 6. 客户端（tools/app）现状

- **三个屏**（`a8b2c11` 把七个页签收成 3 个）：刷固件 / 表盘 / 工具；去术语 + 设计系统。
  `check-app.js` 卡着"恰好 3 个屏 + 32 项能力表 + 术语扫描"三道。
- 预览表盘已内联；★ **预览帧必须同步进仓库** `preview/frames/`（150× `l_*.bmp` + 150× `r_*.bmp`，
  今日实点仍是 150 + 150 ✓）—— 渲染器写进的是镜像目录，不同步就"图裂" ✗（已踩过一次）。
- 无线刷写「上传后自动验槽」：`/api/ota-verify` 已**幂等**（板子已在 OTA 模式就不再发 `u`，免得重计时）。
  ★ 口径修正：**只有服务端端点**是 curl 实测过的；客户端 `verifySlot` 那条路修好后**未再单独实测**。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（9,228,688 B；`build-exe.ps1` **默认 onedir** ✗，要加 `-OneFile`）
- ★ 2026-10-15 跟着新阈值改的两处（都不是客户端 UI，但会顺着客户端走）：
  ① 预览注入键 `PreviewKey::Overspeed` 的注入值 **130 → 160**（`lib/dashcore/preview_input.h`；
  否则够不着新的 150 阈值、按键无反应），并新增用例钉住"注入值必须够得着阈值"；
  ② 编辑器模板 `tools/theme-editor/index.html` 与 `tools/theme-editor/theme-default.json` 已一并对齐 **150 / 5500** ✓。
- ★ 删蜂鸣器**没有**动客户端：`tools/app` 里没有蜂鸣器/静音 UI（`.css` 里那些 `muted` 是颜色变量名）✓。

### 6.1 台面网络两个坑（都踩过）

1. **`206dash-MASTER` 的 WiFi 配置文件原先不存在** ✗ ⇒ `netsh wlan connect` **静默失败**。
   现已生成 `C:\206dash-scratch\obd-ble-sim\wlan-206dash-MASTER.xml`（含正确 SSID hex）并加入系统 ✓。
   ★ 下次换板先确认配置文件在不在。
2. **Windows 扫描偶发不列出新热点** ✗（同段脚本有时 10 秒可见、有时 150 秒不见）。
   ⇒ 判据必须是「扫描里真出现 + 拿到 `192.168.4.x`」，**不能只看 connect 的返回值** ✓。

## 7. 声音：**蜂鸣器已整条砍掉**（2026-10-15）⇒ 板上现在**完全无声**

- `a076acd` 删掉的：4 个驱动文件 + `dash_buzzer_set()` / `BUZZER_EXIO_BIT` / 自检段 + 串口 `b`/`m`
  + NVS 静音 + 2 秒上限。★ **同芯片的 `LCD_RST`/`LCD_CS` 与 `tca9554_set()` 未动** ✓。
- 板上本来就**没有**喇叭座/功放/音频芯片 —— 原先唯一发声件是**有源蜂鸣器**（`TCA9554` 的 `EXIO8`，只会响/不响）；
  ⇒ 要"**会出声**"只能靠**轮上第三块板**（`docs/WHEEL-REMOTE.md`，选型见其 §2.1）。
- 调研结论留档在 `docs/RGB-PANEL-2.8C.md` **§2.1 / §2.2**：
  **I2C 传不了音频**（实测写一次扩展器 141–178 µs ⇒ 8 kHz 采样要 1.2 倍主循环时间）；
  **微雪没有**"I2C 就能出声"的模块（它家音频板是 I2C 配寄存器 + I2S 走数据）；
  真要做的四档（0 脚 `DFR0760` TTS / 0 脚 `PCA9685` / 1 脚 `GPIO43`+PWM / 3 脚 I2S 飞线）与代价见 §2.2。
- ★ 顺带记的引脚事实：`GPIO4`=`BAT_ADC`、`GPIO42`=TF 卡 `D0`、`GPIO16`=触控版 `TP_INT`
  （出处是微雪 wiki 表，**本仓未核原理图** ⇒ 按"外部资料"对待）。

## 8. 常用锚点（不要再重新搜）

- 面板全量重建：`src/dash_display_rgb.cpp:1343`（`dash_display_panel_rebuild`，判据 `del + panel_create + reinit=1`）；
  轻量重初始化：同文件 `:1371`；**创建段 `panel_create()`**：同文件 `:1020`
- 时序宏：`src/dash_display_rgb.cpp:267-277`；**PCLK 宏与其来龙去脉**：同文件 `:241-266`（宏 `RGB_PIXEL_CLOCK_HZ` 在 `:263`）
- **bounce 行数**：`src/dash_display_rgb.cpp:290`（`RGB_BOUNCE_LINES`，默认 10）；落到驱动：`:1047`
  （`bounce_buffer_size_px`）；`0` = 退回旧行为（屏上只有 `fbs[0]`）
- 换帧（bounce 模式下"重新锁存"靠它）：`src/dash_display_rgb.cpp:678`（`draw_bitmap(panel,0,0,W,1,g_fb[g_back])`）；
  轮询：`dash_display_poll()` `:1487`；停顿自愈：`src/main.cpp:1914`（`kPanelHealStallMs=1000`）
- ★★ **逐帧碰 LVGL 样式的正确写法**：`src/dash_ui.cpp:75`（`ScreenUi::arc_red_mask`）+ 调用点 `:1320`
  （"先比后设"）；可参照的正确范例 `digit_red`：`:67` / `:624`
- 左右映射：`lib/dashcore/dash_role_layout.h:118-120`
- 告警阈值：红区 5500 / 高速 150 —— `lib/dashcore/alerts.h:82` / `:71`（退出迟滞：红区退 5300）；
  ★ **真正决定"脸变红"的门槛**在 `lib/dashcore/expression.cpp:56`（`kRpmRedlineFrom = 5500.0f`，已一并降下来）
- 表情超速档（**已裁决不对齐，别改**）：`lib/dashcore/expression.cpp:60-61`（>130 进 / ≤127 退）
- 主题量程四键解析：`lib/themetool/theme_store.cpp`（`theme_parse_json` 根级键链）
- 编辑器量程控件：`tools/theme-editor/index.html:759`（水温量程）/ `:769`（进气量程）
- 主题分区写入：`esptool write_flash 0x210000`（只动主题、**不碰固件**；必须带 `PYTHONIOENCODING=utf-8`）；
  分区表 `partitions-s3-now.csv`（theme=`0x210000/0x4000`）；★ 底稿必须用
  `tools/theme-editor/local/theme-fixed.json`（**2579 B** = 板上真正在用的那份），
  **不是** `tools/theme-editor/theme.json`（**1388 B**，不是同一个文件）
- 预览帧：`preview/frames/`；校验：`tools/theme-editor/check-preview-frame.js`
- 主题 A/B（宿主机验证"改了主题到底有没有生效"）：`tools/theme-editor/theme-ab.ps1`
  （编译一次 + 三臂各跑 42 s + 自动比对；★ **别用 `pio run -t exec`** —— 预览程序跑完 150 帧
  **不会自己退出**，会把后面几臂全堵死）+ `theme-ab-diff.js`（逐像素比，报差异数/包围盒/最大通道差）
- 串口脚本：`tools/serial-capture/{ota-cycle,soak-report,watch-utf8,capture-van-nopy,probe-retry}.ps1`；
  日志都落 `C:\206dash-scratch\`
- **今天那两单的留档**：`ACCEPTANCE.md`（**仓库根**，不在 `docs/`）末两节 =
  「删整条指示灯栏 + 砍蜂鸣器 + 红区 5500 / 高速 150」与「逐帧 LVGL 样式写入导致横纹/偏移」；
  `docs/BENCH-LOG.md` = 10-15 的更正与结案
- 构建设计：镜像目录 `C:\206dash-scratch\now-vanraw`（中文路径编不了；**它不是 git 仓库，是拷贝** ⇒
  改完仓库要同步过去，`robocopy $repo $copy /E /XD .git .pio`，**1/3=成功、≥8=失败**）；
  **必须带 `PYTHONUTF8=1`**（否则 `checkprogsize` 解码 gbk 报错）；刷写还要 `PYTHONIOENCODING=utf-8`
  （不设会让 upload 中途崩、板子变黑——`docs/BUILD-ENV.md` 坑 6）；
  pcpreview 的 `lroundf` 兜底在 `src/main.cpp` **文件最顶部**
- ★★ **入口别用裸 `python`**：这台机器上它指向 `…\WindowsApps\python.exe`（商店占位），
  **不打印不报错、退出码 0** ⇒ 一整轮"跑完了"其实什么都没产出。只用
  `tools/build/pio.ps1 -Cmd '…'` 或 `& C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe -m platformio …`。
  **判据永远看产物（mtime/哈希/`[SUCCESS]`），不看退出码**（pio 成功时也可能返回 1）。
  系统化写在 `docs/BUILD-ENV.md` **坑 8**；同类还有 **坑 9**：`PLATFORMIO_BUILD_DIR` 也**必须纯 ASCII**
  （`C:\206dash-scratch\final-builds\<master|slave>`，一块板一个子目录）。
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
