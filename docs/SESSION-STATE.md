# 当前状态（一页纸）—— 每轮开工先读它，收工更新它

**为什么有这份文件**：长会话里最贵的不是“读代码”，而是**重复读**和**复述历史**。
把"现在在哪、下一步做什么、锚点在哪"写成一页，任何一轮（包括被压缩之后的那一轮）
读这一页就能接着干，不必回头翻日志与源码。

★ 维护约定：**每轮收工把它改写成最新的**（不要追加历史，历史进 `BENCH-LOG.md` /
`RGB-PANEL-2.8C.md` 等专题文档）。

---

## 1. 手头在做什么

- **主线**：客户端（`tools/app` = 206Dash 开发台）打磨 + 主题/量程链。
- **并行的观察项**：台面两块板的 `②` 类故障（面板/外设丢状态）与"无法归因复位"。
- **已停的工作**：固件侧对 `②` 的自愈尝试（三条全试过，见 §4）。

## 2. 下一步第一动作（下次从这里开始，不用摸索）

1. **左右映射对调**（用户已点头要查）：
   - 出帧处自洽：`src/dash_display.cpp:307-313`（`l_` ← `fb_left`，`r_` ← `fb_right`）✓
   - ⇒ 要查的是**上游**：`src/` 里 `fb_left` / `fb_right` 的**赋值处**，看它按"左/右"还是按"角色/屏序"赋；
   - 契约在 `tools/theme-editor/test-role-orientation.js:54/56`：**右=主板=速度+进气**、**左=从板=转速+水温**；
   - 修完跑 `pio run -e pcpreview -t exec` 重出帧，看预览页一眼即可验收（不碰板子）。
2. **量程上板验证**（需用户同意刷机）：把进气量程改一组明显不同的值（如 `0~120`）
   刷主板 ⇒ 看右屏副表弧的进度是否跟着变。
3. **预览页内联**已由用户口头确认"效果不错" ✓；`量程`控件已确认出现 ✓。

## 3. 两块板的现状（别乱动）

| 板 | 固件 | 状态 |
|---|---|---|
| 主板 COM9 | 含 `panel_create()` + `R` 全量重建 + 重建后整屏重绘（`b85927f`/`1754d00`/`c503a6d`） | 画面正常、静音 |
| 从板 COM8 | 上一版（含 `rgb 外设重启`，**没有 `R`**） | 画面正常、静音；**仍是"故障候选板"** ⇒ 出 `②` 时优先用它做实验，但那之前不能刷它 |

★ **协议**：`②`（横纹/偏移/黑屏）出现 ⇒ **先别复位**，立刻通知；
`①`（黑屏重启）⇒ 几秒内抓串口要 `boot: reason` + 有没有 backtrace。
★ **别再发 `R`**：打在活着的故障屏上会卡死（`RGB-PANEL-2.8C.md` §20.5）。

## 4. `②` 类的已知结论（省得重试）

| 手段 | 结果 |
|---|---|
| 只重写面板寄存器（`r`） | ✗ |
| `r` + `rgb 外设重启` / 重配时钟 | ✗ |
| `R`（`del` + `panel_create`） | ✗✗ 画面卡死 |
| 按复位键（真正一次启动） | ✓ 唯一有效 |

⇒ ② **没有可检测信号**（`panelguard`/`timeout`/`stall`/`boot: reason` 全 0）；
⇒ 主要假设＝**台面 USB 供电偶发瞬态**（芯片侧有 `POWERON`/`UNKNOWN` 签名的复位为证）；
⇒ 待做 A/B：换**带独立供电的 USB hub**（手边暂无设备）；
⇒ 车测（12V→稳压）才是真判据。

## 5. 客户端（tools/app）现状

- 页签 7 个：概览 / 无线刷写 / **开发板状态** / 主题配色 / 图片素材 / 预览表盘 / 文件与烧写。
- 无线刷写：上传后**自动验槽**（`/api/board-status` + `/api/ota-verify`：串口发 `u` → 读 `/status` →
  用 `running`/`target` 翻转当判据）——**端到端尚未跑过**（需要板子进 OTA 模式）。
- 预览表盘：已**内联**（iframe 3 → 2）；`check-app.js` 与 `node --check` 全过。
- 单文件 exe：`tools/app/dist/206Dash-Workbench.exe`（8.8 MB，`build-exe.ps1 -OneFile`）。

## 6. 常用锚点（不要再重新搜）

- 面板重初始化：`src/dash_display_rgb.cpp:1643`（`dash_display_panel_reinit`）
- 创建段抽取：`panel_create()`（同文件，`dash_display_init` 之前）
- 停顿自愈：`src/main.cpp`（`loop_probe_begin`，`kPanelHealStallMs=1000`）
- 主题量程四键解析：`lib/themetool/theme_store.cpp`（`theme_parse_json` 根级键链）
- 编辑器量程控件：`tools/theme-editor/index.html:756`（水温）/ `:766`（进气）
- OTA 一键脚本：`tools/serial-capture/ota-cycle.ps1`；日志分析：`soak-report.ps1`
- 构建镜像目录：`C:\206dash-scratch\now-vanraw`（源码仓库是中文路径，编不了）；
  构建必须带 `PYTHONUTF8=1`（否则 GBK 报错/`TypeError NoneType`）
