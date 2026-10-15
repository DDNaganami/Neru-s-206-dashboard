# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是"读代码"，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `BENCH-LOG.md` 与各专题文档）。
★ 本次改写：**2026-10-14**（从板 4 小时记录跑满、0 复位；改跑主板 10 小时）。

---

## 1. 下一步第一动作

★★ **结案：早上的"偏移自愈" = 一次上电复位（POWERON）** ✓ —— 判据：`boot: … prev=POWERON (raw=1)
prev_up=30min`（复位计数 `n=33`），与"笔记本恢复时 USB 反复上下电"吻合（≈10:03）。
⇒ **§20"只有复位才有效"继续成立**，不改写。完整记录见 `docs/BENCH-LOG.md` 的
「2026-10-15 结案」。
★ 串口也已恢复：卡住是**驱动层残留句柄**（连 COM1 都 `Access denied`、无进程占用）⇒
**拔插一次原生 USB 重新枚举**即可；等待期间用 `probe-retry.ps1`（自动重试采集）才没错过横幅。

接下来（按价值排）：

1. **重启长跑记录**，但要换一种**能从笔记本重启里活下来**的写法（按小时分片 / 自动续采；
   现成件：`C:\206dash-scratch\probe-retry.ps1`）—— 昨晚 4.65 h 之后全丢就是教训。
2. **量程的设备半场**（§2 第 3 条）：主题要真进板子（`0x210000`）并由面板画出来 ——
   用从板 COM8（左屏=水温）+ 车主的眼睛；★ **动板子前先问一声**。
3. ★ **上车红线新增一条**：横幅里 `usb: personality=ch343p` 说明**UART 口有电（线插着）**
   ⇒ **UART 那根 Type-C 必须拔掉**，否则 43/44（含 `VAN_RX_PIN=44`）被切给板载 CH343P ✗。

## 2. 待办（按价值排）

1. **主板 10 小时记录收口**（见 §1）：与 10-11 那夜对齐量级后再下"是不是缓解了"的结论。
2. **② 的 A/B 不在 PCLK 上** —— 方向已排除，见 §5 ★★（差点白刷一轮）。
3. ~~量程验证~~ **✓ 两头都通了（2026-10-15，车主肉眼确认）**：
   - **宿主机半场**：`pcpreview` 三臂 A/B（`THEME_FILE` 换主题）—— 只改四个量程键就**真的改变像素**
     （294/300 帧有差异、集中在表盘区）；**对照臂**只改 `bg_color` ⇒ 几乎整屏变（证明主题确实被加载）；
     证据 `C:\206dash-scratch\ab\{A,B,C}\`；工具见 §8 的 `theme-ab` 两条。
   - **设备半场**：拿 `tools/theme-editor/local/theme-fixed.json`（= 板上原来那份 **2579 B**）的**副本**
     只改水温量程为 `0~100`，用 **`esptool write_flash 0x210000`** 写进主题分区
     （该分区 `0x210000/0x4000`，见 `partitions-s3-now.csv`；**只动主题、不碰固件**；必须带
     `PYTHONIOENCODING=utf-8`）⇒ 开机日志 `theme: 已加载 (2578 字节, bg=0x141414, face=200)` ✓
     ⇒ **车主：水温色带明显变长** ✓（量程 60~130→0~100 时，85 ℃ 的弧从 ~36% 变 ~85%）。
   - ★ 实验后已把**原主题写回**（`local/theme-fixed.json` 原样）⇒ 板子恢复改动前的量程 ✓。
   ⇒ 这条链（主题文件 → 主题分区 → 解析 → 面板）**闭环**。
   ★★ 坑：`tools/theme-editor/theme.json`（**1388 B**）与板上真正在用的那份（**2579 B**
   = `tools/theme-editor/local/theme-fixed.json`）**不是同一个文件** —— 做这类实验必须后者当底稿，
   否则会连带改掉别的键（我第一次就踩了，车主没看出来，但不严谨）。
4. **② 的"安全时机自愈"**：只有车测也犯才需要（要用**不释放内存**的重建，见 §4）。
5. ~~左右映射对调~~ **✓ 已修**：`lib/dashcore/dash_role_layout.h:118-120` 的 `themeIndexForScreen()`；
   用户已在预览页确认 ✓。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| 主板 COM9 | 最新（含 `panel_create()` + `R` + 重建后整屏重绘 + 量程四键解析） | 早上屏幕**偏移后自愈**（见 §1）；★ **10:05 我的串口探测可能把它复位过一次**；静音；串口**被别的进程占着** |
| 从板 COM8 | 上一版（含 `rgb 外设重启`，**没有 `R`**） | 空闲；静音；**故障候选板**；★ `app0` 里还留着**改动前**的固件 ⇒ 回滚 A/B 现成可用 ✓；★ 串口同样被占 |

★ **协议**：②（横纹/偏移/黑屏）出现 ⇒ **先别复位**，立刻通知；
①（黑屏重启）⇒ 几秒内抓串口要 `boot: reason` + 有没有 backtrace。
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

⇒ ② **没有可检测信号**（`panelguard`/`timeout`/`stall`/`boot: reason` 全 0）；
⇒ 10-11 那夜的收窄结论：**② 与芯片复位不是必然同时发生**（瞬态打芯片 5 次、打面板 0 次）；
⇒ 待做 A/B：换**带独立供电的 USB hub**（手边暂无设备）；**车测（12V→稳压）才是真判据**。

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
**12 MHz→约 44、15→约 54、18→约 65、30→约 108**（10-14 实测 +44 ⇒ 确认还是 12 MHz）。

## 6. 客户端（tools/app）现状

- **三个屏**（`a8b2c11` 把七个页签收成 3 个）：刷固件 / 表盘 / 工具；去术语 + 设计系统。
  `check-app.js` 卡着"恰好 3 个屏 + 32 项能力表 + 术语扫描"三道。
- 预览表盘已内联；★ **预览帧必须同步进仓库** `preview/frames/`（150× `l_*.bmp` + 150× `r_*.bmp`）——
  渲染器写进的是镜像目录，不同步就"图裂" ✗（已踩过一次）。
- 无线刷写「上传后自动验槽」：`/api/ota-verify` 已**幂等**（板子已在 OTA 模式就不再发 `u`，免得重计时）。
  ★ 口径修正：**只有服务端端点**是 curl 实测过的；客户端 `verifySlot` 那条路修好后**未再单独实测**。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（9,228,688 B；`build-exe.ps1` **默认 onedir** ✗，要加 `-OneFile`）

### 6.1 台面网络两个坑（都踩过）

1. **`206dash-MASTER` 的 WiFi 配置文件原先不存在** ✗ ⇒ `netsh wlan connect` **静默失败**。
   现已生成 `C:\206dash-scratch\obd-ble-sim\wlan-206dash-MASTER.xml`（含正确 SSID hex）并加入系统 ✓。
   ★ 下次换板先确认配置文件在不在。
2. **Windows 扫描偶发不列出新热点** ✗（同段脚本有时 10 秒可见、有时 150 秒不见）。
   ⇒ 判据必须是「扫描里真出现 + 拿到 `192.168.4.x`」，**不能只看 connect 的返回值** ✓。

## 7. 声音（扬声器）：**owner 已裁决"先不弄"**（2026-10-14）

调研结论留档在 `docs/RGB-PANEL-2.8C.md` **§2.1 / §2.2**：

- 板上**没有**喇叭座/功放/音频芯片 —— 唯一发声件是**有源蜂鸣器**（`TCA9554` 的 `EXIO8`，只会响/不响）；
- **I2C 传不了音频**（实测写一次扩展器 141–178 µs ⇒ 8 kHz 采样要 1.2 倍主循环时间）；
  **微雪没有**"I2C 就能出声"的模块（它家音频板是 I2C 配寄存器 + I2S 走数据）；
- 真要做的四档（0 脚 `DFR0760` TTS / 0 脚 `PCA9685` / 1 脚 `GPIO43`+PWM / 3 脚 I2S 飞线）与代价见 §2.2；
- ★ 顺带新记的引脚事实：`GPIO4`=`BAT_ADC`、`GPIO42`=TF 卡 `D0`、`GPIO16`=触控版 `TP_INT`
  （出处是微雪 wiki 表，**本仓未核原理图** ⇒ 按"外部资料"对待）。

## 8. 常用锚点（不要再重新搜）

- 面板重初始化：`src/dash_display_rgb.cpp:1643`；创建段：同文件 `panel_create()`
- 时序宏：`src/dash_display_rgb.cpp:267-277`；**PCLK 宏与其来龙去脉**：同文件 `:241-266`（宏在 `:263`）
- 出帧：`src/dash_display.cpp:307-313`；停顿自愈：`src/main.cpp`（`kPanelHealStallMs=1000`）
- 左右映射：`lib/dashcore/dash_role_layout.h:118-120`
- 主题量程四键解析：`lib/dashcore/theme_store.cpp`（`theme_parse_json` 根级键链）
- 编辑器量程控件：`tools/theme-editor/index.html:756`（水温）/ `:766`（进气）
- 预览帧：`preview/frames/`；校验：`tools/theme-editor/check-preview-frame.js`
- 主题 A/B（宿主机验证"改了主题到底有没有生效"）：`tools/theme-editor/theme-ab.ps1`
  （编译一次 + 三臂各跑 42 s + 自动比对；★ **别用 `pio run -t exec`** —— 预览程序跑完 150 帧
  **不会自己退出**，会把后面几臂全堵死）+ `theme-ab-diff.js`（逐像素比，报差异数/包围盒/最大通道差）
- 串口脚本：`tools/serial-capture/{ota-cycle,soak-report,watch-utf8}.ps1`；日志都落 `C:\206dash-scratch\`
- 构建：镜像目录 `C:\206dash-scratch\now-vanraw`（中文路径编不了；**它不是 git 仓库，是拷贝** ⇒
  改完仓库要同步过去，`robocopy $repo $copy /E /XD .git .pio`，**1/3=成功、≥8=失败**）；
  **必须带 `PYTHONUTF8=1`**（否则 `checkprogsize` 解码 gbk 报错）；刷写还要 `PYTHONIOENCODING=utf-8`
  （不设会让 upload 中途崩、板子变黑——`docs/BUILD-ENV.md` 坑 6）；
  pcpreview 的 `lroundf` 兜底在 `src/main.cpp` **文件最顶部**
- ★★ **入口别用裸 `python`**：这台机器上它指向 `…\WindowsApps\python.exe`（商店占位），
  **不打印不报错、退出码 0** ⇒ 一整轮"跑完了"其实什么都没产出。只用
  `tools/build/pio.ps1 -Cmd '…'` 或 `& C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe -m platformio …`。
  **判据永远看产物（mtime/哈希/`[SUCCESS]`），不看退出码**（pio 成功时也可能返回 1）。
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
