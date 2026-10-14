# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是"读代码"，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `BENCH-LOG.md` 与各专题文档）。
★ 本次改写：2026-09-28（声音调研收口 + 4 小时记录任务进行中）。

---

## 1. 下一步第一动作

**盯住正在跑的 4 小时记录（从板 COM8）**，到点收口：

- 收口命令：`tools\serial-capture\soak-report.ps1 -Log C:\206dash-scratch\soak-slave-1848.log`
  （后台 job `pwsh-8` 在写，**4 小时**、约 **22:48** 结束；日志 mtime 一直是"刚刚" ⇒ 确实在记）
- 到 **20:20** 的读数：**2.7 MB / 17,567 行**；`boot/reset` **0**、`panelguard` **0**、
  `rgb anomaly` **0**、`loop stall` **0**、`ble` **0** ⇒ 比上一轮"约 20 分钟就冒 WDT"干净得多 ✓
- **判据**：① 若出现 `boot: reason=`（尤其 `SW` / `raw=7`）⇒ 抓 WDT **任务名**，
  判定是不是"任务化 BLE 发现"那条路；② 全程 0 复位 ⇒ 那条怀疑**降级**，
  腾出手做 §2 第 2 条的 PCLK A/B。

## 2. 待办（按价值排）

1. **记录任务收口**（约 22:48）：出结论 + 更新 `docs/BENCH-LOG.md`；在此之前从板 COM8 **别刷**。
2. **PCLK A/B**（见 §5）：官方示例 **30 MHz**、我们 **12 MHz** ⇒ 面板 PLL 可能工作在窗口边缘，
   长时间后偶发丢同步且**不自愈**（正是 ② 的表现）。AB 需刷板 + 长时观察。
3. **量程上板验证**：进气量程改成明显不同的值（如 `0~120`）刷主板 ⇒ 看右屏副表弧进度是否跟着变。
4. **② 的"安全时机自愈"**：只有车测也犯才需要（要用**不释放内存**的重建，见 §4）。
5. ~~左右映射对调~~ **✓ 已修**：`lib/dashcore/dash_role_layout.h:118-120` 的 `themeIndexForScreen()`
   （预览档直接返回屏序、设备侧走角色规则）；用户已在预览页确认看得见 ✓。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| 主板 COM9 | 含 `panel_create()` + `R` + 重建后整屏重绘 + 量程四键解析 | 画面正常、静音 |
| 从板 COM8 | 上一版（含 `rgb 外设重启`，**没有 `R`**） | 画面正常、静音；**故障候选板**；★ **正被 4 小时记录占用**（别刷、别抢串口） |

★ **协议**：②（横纹/偏移/黑屏）出现 ⇒ **先别复位**，立刻通知；
①（黑屏重启）⇒ 几秒内抓串口要 `boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。
★ 两块板都**静音**（`mute: 1`，NVS 保存）；**OTA 模式黑屏是设计**（10 分钟超时自动退回）。

## 4. ② 类的已知结论（省得重试）

| 手段 | 结果 |
|---|---|
| 只重写面板寄存器（`r`） | ✗ |
| `r` + `rgb 外设重启` / 重配时钟 | ✗ |
| `R`（`del` + `panel_create`） | ✗✗ 画面卡死 |
| 按复位键（真正一次启动） | ✓ 唯一有效 |

⇒ ② **没有可检测信号**（`panelguard`/`timeout`/`stall`/`boot: reason` 全 0）；
⇒ 主要假设＝**台面 USB 供电偶发瞬态**（芯片侧有 `POWERON`/`UNKNOWN` 签名复位为证）；
⇒ 待做 A/B：换**带独立供电的 USB hub**（手边暂无设备）；车测（12V→稳压）才是真判据。

## 5. 面板时序 vs 官方（2026-10-11 对照：消隐区没问题）

来源：`C:\206dash-scratch\ws-28c-demo\...\Display_ST7701.h`（微雪官方示例）

| 参数 | 官方 | 我们（`src/dash_display_rgb.cpp:267-277`） | 一致 |
|---|---|---|---|
| HPW/HBP/HFP | 8/10/50 | 8/10/50 | ✓ 逐字相同 |
| VPW/VBP/VFP | 2/18/8 | 2/18/8 | ✓ 逐字相同 |
| 极性 | 默认 | `PCLK_ACTIVE_NEG=0`、HS/VS idle=0 | ✓ |
| **PCLK** | **30 MHz** | **12 MHz** | ✗ **唯一差异** → §2 第 2 条的 A/B |

## 6. 客户端（tools/app）现状

- **三个屏**（`a8b2c11` 把七个页签收成 3 个）：刷固件 / 表盘 / 工具；去术语 + 设计系统。
  `check-app.js` 卡着"恰好 3 个屏 + 32 项能力表 + 术语扫描"三道。
- 预览表盘已内联；★ **预览帧必须同步进仓库** `preview/frames/`（150× `l_*.bmp` + 150× `r_*.bmp`）——
  渲染器写进的是镜像目录，不同步就"图裂" ✗（已踩过一次）。
- 无线刷写「上传后自动验槽」：`/api/ota-verify` 已**幂等**（板子已在 OTA 模式就不再发 `u`，免得重计时）。
  ★ 口径修正：**只有服务端端点**是 curl 实测过的；客户端 `verifySlot` 那条路修好后**未再单独实测**。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（9,228,688 B；`build-exe.ps1` **默认 onedir** ✗，要加 `-OneFile`）

### 6.1 台面网络两个坑（2026-10-11 实测，都踩过）

1. **`206dash-MASTER` 的 WiFi 配置文件原先不存在** ✗ ⇒ `netsh wlan connect` **静默失败**。
   现已生成 `C:\206dash-scratch\obd-ble-sim\wlan-206dash-MASTER.xml`（含正确 SSID hex）并加入系统 ✓。
   ★ 下次换板先确认配置文件在不在。
2. **Windows 扫描偶发不列出新热点** ✗（同段脚本有时 10 秒可见、有时 150 秒不见）。
   ⇒ 判据必须是「扫描里真出现 + 拿到 `192.168.4.x`」，**不能只看 connect 的返回值** ✓。

## 7. 声音（扬声器）：**owner 已裁决"先不弄"**（2026-09-28）

调研结论已留档在 `docs/RGB-PANEL-2.8C.md` **§2.1 / §2.2**：

- 板上**没有**喇叭座/功放/音频芯片 —— 唯一发声件是**有源蜂鸣器**（`TCA9554` 的 `EXIO8`，只会响/不响）；
- **I2C 传不了音频**（实测写一次扩展器 141–178 µs ⇒ 8 kHz 采样要 1.2 倍主循环时间）；
  **微雪没有**"I2C 就能出声"的模块（它家音频板是 I2C 配寄存器 + I2S 走数据）；
- 真要做的四档（0 脚 `DFR0760` TTS / 0 脚 `PCA9685` / 1 脚 `GPIO43`+PWM / 3 脚 I2S 飞线）与代价见 §2.2；
- ★ 顺带新记的引脚事实：`GPIO4`=`BAT_ADC`、`GPIO42`=TF 卡 `D0`、`GPIO16`=触控版 `TP_INT`
  （出处是微雪 wiki 表，**本仓未核原理图** ⇒ 按"外部资料"对待）。

## 8. 常用锚点（不要再重新搜）

- 面板重初始化：`src/dash_display_rgb.cpp:1643`；创建段：同文件 `panel_create()`
- 时序宏：`src/dash_display_rgb.cpp:267-277`；出帧：`src/dash_display.cpp:307-313`
- 停顿自愈：`src/main.cpp`（`loop_probe_begin`，`kPanelHealStallMs=1000`）
- 左右映射：`lib/dashcore/dash_role_layout.h:118-120`
- 主题量程四键解析：`lib/dashcore/theme_store.cpp`（`theme_parse_json` 根级键链）
- 编辑器量程控件：`tools/theme-editor/index.html:756`（水温）/ `:766`（进气）
- 预览帧：`preview/frames/`（150×`l_*` + 150×`r_*`）；校验：`tools/theme-editor/check-preview-frame.js`
- OTA 一键脚本：`tools/serial-capture/ota-cycle.ps1`；日志分析：`soak-report.ps1`；记录：`watch-utf8.ps1`
- 构建：镜像目录 `C:\206dash-scratch\now-vanraw`（中文路径编不了）；**必须带 `PYTHONUTF8=1`**
  （否则 `checkprogsize` 解码 gbk 报错）；pcpreview 的 `lroundf` 兜底在 `src/main.cpp` **文件最顶部**
- 工具输出闸门：`maxInlineBytes: 50000` 在 `dsh-base/cordis.patch.yml`（**宿主平面，预设改不了** ✗）
