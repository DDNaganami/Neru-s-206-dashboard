# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是“读代码”，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不追加历史；历史进 `BENCH-LOG.md` 与各专题文档）。

---

## 1. 下一步第一动作

**左右映射对调**（用户已点头要查）。已确认的事实：

- 出帧处**自洽**：`src/dash_display.cpp:307-313`（`l_` ← `fb_left`，`r_` ← `fb_right`）✓
- ⇒ 要查的是**上游**：`src/` 里 `fb_left` / `fb_right` 的**赋值处**（按“左/右”还是按“角色/屏序”）
- 契约：`tools/theme-editor/test-role-orientation.js:54/56` —— **右=主板=速度+进气**、**左=从板=转速+水温**
- 现象（用户截图）：预览页左图是 188 km/h + 32 ℃、右图是 1350 rpm + 90 ℃ ⇒ 与契约相反 ✗
- 修完跑 `pio run -e pcpreview -t exec` 重出帧，预览页看一眼即可验收（不碰板子）

## 2. 别的待办（按价值排）

1. **PCLK A/B**（见 §5）：官方示例 **30 MHz**、我们 **12 MHz** ⇒ 面板 PLL 可能工作在窗口边缘，
   长时间后偶发丢同步且**不自愈**（正是 ② 的表现）。AB 需刷板 + 长时观察。
2. **量程上板验证**：进气量程改成明显不同的值（如 `0~120`）刷主板 ⇒ 看右屏副表弧进度是否跟着变。
3. **② 的“安全时机自愈”**：若车测也犯才需要（要用**不释放内存**的重建，见 §4）。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| 主板 COM9 | 含 `panel_create()` + `R` + 重建后整屏重绘 + 量程四键解析 | 画面正常、静音；写完本页时停在 OTA 模式（10 分钟自动退回） |
| 从板 COM8 | 上一版（含 `rgb 外设重启`，**没有 `R`**） | 画面正常、静音；**仍是“故障候选板”** |

★ **协议**：②（横纹/偏移/黑屏）出现 ⇒ **先别复位**，立刻通知；
①（黑屏重启）⇒ 几秒内抓串口要 `boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。

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
| **PCLK** | **30 MHz** | **12 MHz** | ✗ **唯一差异** → §2.1 的 A/B |

## 6. 客户端（tools/app）现状

- 页签 7 个：概览 / 无线刷写 / 开发板状态 / 主题配色 / 图片素材 / 预览表盘 / 文件与烧写。
- 无线刷写「上传后自动验槽」（`/api/board-status` + `/api/ota-verify`）：**两条路径都已实测** ✓
  （成功 `ok=True running=app0 target=app1 waited=3s`；失败：等满 70s + 可读错误）
- 预览表盘已内联（iframe 3→2）；`check-app.js` / `node --check` 全过。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（8.8 MB；`build-exe.ps1` **默认 onedir** ✗，要加 `-OneFile`）

### 6.1 台面网络两个坑（2026-10-11 实测，都踩过）

1. **`206dash-MASTER` 的 WiFi 配置文件原先不存在** ✗ ⇒ `netsh wlan connect` **静默失败**。
   现已生成 `C:\206dash-scratch\obd-ble-sim\wlan-206dash-MASTER.xml`（含正确 SSID hex）并加入系统 ✓。
   ★ 下次换板先确认配置文件在不在。
2. **Windows 扫描偶发不列出新热点** ✗（同段脚本有时 10 秒可见、有时 150 秒不见）。
   ⇒ 判据必须是「扫描里真出现 + 拿到 `192.168.4.x`」，**不能只看 connect 的返回值** ✓。

## 7. 常用锚点（不要再重新搜）

- 面板重初始化：`src/dash_display_rgb.cpp:1643`；创建段：同文件 `panel_create()`
- 时序宏：`src/dash_display_rgb.cpp:267-277`；出帧：`src/dash_display.cpp:307-313`
- 停顿自愈：`src/main.cpp`（`loop_probe_begin`，`kPanelHealStallMs=1000`）
- 主题量程四键解析：`lib/themetool/theme_store.cpp`（`theme_parse_json` 根级键链）
- 编辑器量程控件：`tools/theme-editor/index.html:756`（水温）/ `:766`（进气）
- OTA 一键脚本：`tools/serial-capture/ota-cycle.ps1`；日志分析：`soak-report.ps1`
- 构建：镜像目录 `C:\206dash-scratch\now-vanraw`（中文路径编不了）；**必须带 `PYTHONUTF8=1`**
- 工具输出闸门：`maxInlineBytes: 50000` 在 `dsh-base/cordis.patch.yml`（**宿主平面，预设改不了** ✗）