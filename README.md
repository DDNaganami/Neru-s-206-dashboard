# Neru's 206 Dashboard

2001 年 11 月的 206 CC MUX、自动挡、白底积家，原表仓里塞两块圆屏：左屏转速 + 水温，右屏车速 + 进气温度，表盘中间的表情代替指针。**原表继续通电**，挡位和警告灯仍由它负责 —— 这套屏只多看一眼车在干什么，摘掉它车照常开。总线是 K 线 + VAN（AEE2001），不是 CAN。

| | 左屏 · 转速表 | 右屏 · 速度表 |
|---|---|---|
| 外圈（缺口朝下） | 转速弧，刻度到 7000 | 车速弧，刻度到 210 |
| 内圈（缺口朝上、镜像） | 水温弧 | 进气温度弧 |
| 表情 | 只看转速，5 档：怠速 / 巡航 / 运动 / 高转 / 红区（5800 起） | 只看车速，5 档：静止 / 市区 / 快速路 / 高速 / 超速（>130） |

水温、进气温度只驱动弧和数字，不换脸。红区按实测断油 6300 提前到 5800，和表盘上限 7000 刻意分开。弧与数字的颜色、几何、量程放在 `lib/themetool/ui_theme.h` 和运行时主题里，换配色刷 16KB 的 `theme` 分区即可，固件不用重编。

数据三路：转速 / 水温 / 进气温度走 K 线 OBD（ELM327，PID `010C` / `0105` / `010F`），车速 `Van > Obd(010D) > Sim`。K 线是排队共享的窄管子，每多一个时隙其余字段就慢 ~25%，所以转速和车速每轮都问、温度每 4 轮问一次；哪个源超过 3 秒没有新数据就退到下一档，断线只是那一格回到假数据，不黑屏。

VAN **不在 OBD 座上**：线号 9004 / 9005，优先从组合仪表插头取（反正要拆表），收发器 SN65HVD230 板上 120Ω 必须拆掉。手机蓝牙 ELM327 只用来上车前体检 —— 地库里松刹车、车挪不到一米车速就跟着变，说明那一路来自车轮转速传感器而不是 GPS；S3 没有经典蓝牙 SPP，车上仍是 USB 有线 ELM。

## 现在到哪一步

VAN 协议已经解完并在真实抓包上验过：帧布局（SOF 10 槽 / IDEN 12 位 / CMD 4 位）、FCS（`crc15_van_iso`，0x0F9D / init 0x7FFF / 取反 / MSB-first，覆盖 IDEN+CMD+DATA）、槽时间（**8.25µs**，规范标称的 8.00µs 偏 3.1%，已作废）都有抓包支撑 —— 怠速 1704/1704 帧、行驶 34051/34051 帧 FCS 全中。

- **转速已标定**：0x824 的 `data[0..1]`，16 位大端 ÷8。点火瞬间 raw 从恒 0 跳到 6152（= 769 rpm，起动机拖动），怠速平台 453 帧均值 7237.6（= 904.7 rpm，sd 26.9），地面真值 900 rpm，偏差 0.52%。
- **车速字段与绝对刻度都已定案**：0x824 的 `data[2]`，单字节，停车窗口内恒 0，与位移累计量增长率回归 R² = 0.9942，加速度 p99 = 19.9 km/h/s 在物理上限内。**绝对刻度 2026-09-22 已用地面真标定**：`kSpeedScale = 2.56`（1 计数 = 2.56 km/h）—— 真值源是蓝牙 ELM327 的 `010D`，与板子记的 `0x824.data[2]` 双串口同源时间戳对齐后回归得到（368.2 s 行驶、599 个真值样本 100% 成功、284 对非零样本 R² = 0.9984）；数据可分辨的范围是 **2.55~2.57**，**2.56 是取整**。原始数据 `tools/serial-capture/drive-2026-09-22-van.csv` + `drive-2026-09-22-obd.csv`，定标过程与证据见 `VAN-PROTOCOL.md` 的「车速」一节。**旧口径「1 计数 = 1 km/h」与「按表盘脸的档位复核」都已作废**（前者实测差 2.5 倍；后者依赖已退货的 DualEye，手上无屏走不通）。**四段定速（20/40/60/80）的标定跑仍按用户决定不做** —— 以后顺手有 OBD `010D` 日志即可复核。

还没做完的：有线 ELM327 接到板上之后的实车刷新率（看串口 `SRC-Hz` 行）、VAN 上的转速 / 油量 / 门灯、表仓里的最终屏和结构件。

**验证用的板子**是微雪 ESP32-S3-DualEye-Touch-LCD-1.28（ESP32-S3R8 + 两块 1.28" 240×240 GC9A01A），SPI 驱动已在 `src/dash_display_spi.cpp` 里按官方 wiki 与例程填好（`esp32s3-spi`，`VAN_RX_PIN=18`）。它承担的是**真屏 240 档**的验证（字号随分辨率缩放、图片素材导入、面谱分档），**验证已完成并被接受，该板已于 2026-09-22 退货**（owner 确认；退货原因未说）——**240 档的结论不作废**：`esp32s3-spi` 环境、`ui_theme.h` 的 240 字号档、`tools/theme-editor/spec-240.json` 都继续保留且仍然有效，只是**现在手上没有这块硬件**。当时板上一直没接 VAN 收发器：实车帧是在抓帧盒（裸 S3 + SN65HVD230）上验的，流程是车上抓包 → `tools/van-decode/` 离线解帧 → 用 `tools/serial-capture/replay.py` 贴回板子。这台板子退货前最后一次上电是 2026-09-18，那次的固件含槽时间 / CRC / 关帧门限三处改动，**不含 2026-09-20 的车速定标**（0.01 → 1.0 还没刷进去）。**当前手上的显示硬件只有裸 S3 抓帧盒**（**无屏**，只用于 VAN 抓帧）；要上屏得等 **2.8" 级圆屏**到货，最终显示架构（两块 S3、一板一屏、一条 UART）见 `ARCHITECTURE.md`。最终要放进表仓的是 2.8" 级圆屏，几何按 `THEME_DISPLAY_RES` 缩放，选屏硬约束（可视直径 ≤ Ø89、两屏外径 < 95）在 `PURCHASE.md`。

## 编译 / 测试 / 刷机

仓库路径里有中文，ESP32 工具链会在中文路径下报 `g++ Invalid argument`，所以**构建在纯 ASCII 副本上做**，两边用 robocopy 同步；本机的 PlatformIO Core 和工作区依赖都不在系统 python 里，`PYTHONPATH` 与 `PLATFORMIO_CORE_DIR` 两个环境变量不能省（宿主机编译器是 `.tools\zigbin` 里的 zig 转发桩）。下面每一行都在 `$copy` 目录里执行。

```powershell
$repo='C:\Users\张九思\206Dash\Neru-s-206-dashboard'
$copy='C:\Users\Public\206dash\Neru-s-206-dashboard'
robocopy $repo $copy /E /XD .git .pio /NJH /NJS /NP      # 退出码 1/3 = 成功

$env:PYTHONPATH='C:\Users\张九思\206Dash\.pio-pylibs'
$env:PLATFORMIO_CORE_DIR='C:\Users\Public\206dash\.pio-core'
$env:PATH='C:\Users\Public\206dash\.tools\zigbin;' + $env:PATH

python -m platformio test -e native                 # 宿主机单元测试，不烧板
# 同一套环境也可以直接用装好的 PlatformIO（不必设 PYTHONPATH）：
#   & 'C:\.platformio\penv\Scripts\platformio.exe' test -e native
python -m platformio run -e pcpreview -t exec       # 本机渲双屏 BMP，Ctrl+C 后打开 preview/preview.html
python -m platformio run -e esp32s3-spi -t upload --upload-port COM4
python tools/serial-capture/capture.py COM4         # 抓复位后的完整开机日志
```

六个编译目标：

| 环境 | 用途 |
|---|---|
| `native` | 宿主机单元测试 |
| `pcpreview` | 本机渲双屏 BMP（**能手动喂输入**：键盘 `← → 空格 L P D O R M V K T X`，或 `preview/inject.txt` —— 见 `tools/theme-editor/README.md` 的「模拟页面上手动喂输入」与「系统状态这两条」） |
| `esp32s3` | S3 桩显示，先把串口 / VAN 跑通 |
| `esp32s3-spi` | 240 档验证用的真屏（GC9A01A 双 240×240）—— **该板 2026-09-22 已退货，现在手上没有这块硬件**（环境照旧保留、仍可编译） |
| `esp32s3-rgb` | RGB 并口骨架（最终大屏备用） |
| `esp32dev` | 经典 ESP32，4MB，廉价回归 |

`theme`（16KB @ `0x210000`）与 `image`（经典板 1MB、S3 8MB @ `0x254000`）两个分区是运行时可换的：换配色或换图不用重编固件，esptool 命令由 `tools/theme-editor/` 的两个页面按目标板打印（两份分区表的偏移刻意相同）。

刷机与测试的八条硬约束，都是踩过的：

- 刷写和监视**走板上的 UART 口（CH340）**，不是原生 USB 口：后者的复位是软复位请求，会把芯片留在 ROM 下载模式。
- S3 原生 USB 口开监视器必须 `monitor_rts = 0` / `monitor_dtr = 0`（`platformio.ini` 已写死），否则芯片被按在复位态。
- 两份分区表 CSV 必须**纯 ASCII**（PlatformIO 用系统编码读它），一个"★"就能让链接成功的构建在最后一步抛 `UnicodeDecodeError`。
- `.pio-pylibs` 里那份 pyserial 不在系统 python 里，直接敲 `python tools/serial-capture/capture.py` 会 `ModuleNotFoundError: No module named 'serial'`。
- 表情画布不要超过 320，再大会盖住内圈副弧。
- **（历史注记 —— 这条纪律已经不需要了）新用例曾经必须注册在 `test/test_dashcore/test_main.cpp` 里 `face_stages` 之前**：`test_face_stages.cpp` 当年有一个越界崩溃（`panic: index 4 out of bounds for type 'Face[4]'`，进程 exit 3），注册在它**之后**的用例根本跑不到 —— 2026-09-22 那 5 条 CRC 用例就是因此前置注册的。**该崩溃已在 `e97b13a` 修掉**（`Face seen[4]` 的容量改成由 `face_stages.h` 的 `kFaceStageCount` 推导，不再写死档数），随后两条同族"写死 4"的断言也在 `a75b94a` 修掉 ⇒ 现在 132 条用例全部跑得到，**注册顺序不再是约束**。来龙去脉留在这里，免得以后有人照旧文把新用例白往前塞。
- 跑 native 测试想看到测试里的输出有两条：① `python -m platformio test -e native` **默认只转发"用例结果行"**，测试里的 `printf` 表格要加 `-v` 才看得见；② `-v` 会把未解析的行 echo 到 GBK 控制台，**测试里的中文 `printf` 会抛 `UnicodeEncodeError` 并把用例统计打乱**（实测那一次 128 例被报成 124 例）⇒ 测试里的输出**一律纯 ASCII**（断言消息里的中文没事，PlatformIO 走 `\xNN` 转义路径），并且**要显式 `fflush(stdout)`**（当年是因为进程随后会崩、缓冲没人冲；那个崩溃已在 `e97b13a` 修掉，但 `fflush` 照旧保留 —— 用例一红进程也可能提前退出，同样会丢缓冲），否则整段输出会丢。
- 直接跑 native 测试二进制（`.pio\build\native\program.exe`）时 **CWD 必须是项目根**：4 条 `test_real_capture_*` 用例按相对路径读 `tools/van-decode/sample-*.csv`，从 `.pio\build\native` 里直接跑会读不到、**假红 4 条**（`platformio test` 自己会把 CWD 设成项目根，所以只有手动跑二进制时才踩得到）。

## 文档在哪

| 文件 | 回答什么 |
|---|---|
| `ACCEPTANCE.md` | 逐次日志：每个结论的实测证据、踩过的坑、仍未验的项 |
| `ARCHITECTURE.md` | 分层与数据流，以及每个取舍的理由 |
| `PINOUT.md` | 接线：VAN 三处取信号、开箱先量什么、抓帧盒怎么用 |
| `PURCHASE.md` | 采购：清单、到货后的验证动作、别买 CAN-only |
| `tools/theme-editor/README.md` | 两个网页编辑器（配色 / 图片）、`image.bin` 格式与刷写偏移 |
| `tools/van-decode/` + `tools/serial-capture/` | 抓包解帧与串口回放的脚本 |

**逐次日志写 `ACCEPTANCE.md`，README 只放稳定信息和最近进展。**

## 许可证与引用

本仓库按内容分三层授权：**代码**（`src/` / `lib/` / `include/` / `test/` / `preview/` / `tools/` 下的源码与构建配置，含接线 / 引脚 / 分区这类事实性表格）= **MIT**，见 `LICENSE`；**文档**（全部 `*.md`，含本文与 `VAN-PROTOCOL.md`）= **CC BY 4.0**，见 `LICENSE-DOCS`；**美术素材与图片 / 二进制** = **单独声明、保留所有权利**，**不在** MIT 或 CC BY 4.0 覆盖范围内，见 `LICENSE-ARTWORK`。完整的分层范围说明（**不改动 MIT 正文**）另见 `LICENSING.md`。

引用或转载**文档**（尤其 `VAN-PROTOCOL.md` 的协议表）请按 CC BY 4.0 注明出处：仓库地址 <https://github.com/DDNaganami/Neru-s-206-dashboard> + 文件路径；机器可读的引用元数据在 `CITATION.cff`（GitHub 会据此显示「引用」按钮）。

**第三方**（协议 / 技术资料、硬件资料、软件依赖与字体）各自保留其原始条款，登记见 `THIRD-PARTY.md`；本项目对它们的引用与实测复现不改变其归属。

## 最近进展

- **2026-09-24** ★ **已解字段接进数据层 + 指示灯/告警框架（仍然零硬件）**：
  `0x4FC` 的灯位域（左/右/双闪/近光/仪表盘灯）与门信号、`0xE24` 的 17 字节 VIN 接进
  `data_service`（来源恒 `Van`、**没动既有优先级**，因为这三类在 OBD 上根本没有对应 PID）；
  pcpreview 上加**占位**指示灯槽位（六格，本机全是简单几何）与**输入注入**（键盘 / `preview/inject.txt`）；
  新增告警层 `lib/dashcore/alerts.*`（超速/红区/门/转向灯忘关，带去抖、最短重复间隔、静音）
  与**可替换的蜂鸣器抽象**（`buzzer.h`；真机那一档建议走 2.8C 的 TCA9554 `EXIO8` ——
  `ARCHITECTURE.md` §8 的 **L14 仍是建议、待 owner 裁决**）。
  素材上传端规格与转换补齐：`tools/theme-editor/asset-spec.js` + README 的「素材规格」一节 +
  `test-asset-spec.js`。★ 顺带修掉一个**一直存在**的构建缺口：`attachObdSerial()` 只在
  `ARDUINO` 那一支里定义，而调用点没被圈起来 ⇒ **pcpreview 在起点上就编不过**
  （现在给预览一份显式的空实现 + 一行日志）。
- **2026-09-22** ★ **车速绝对刻度定标**：`kSpeedScale` 1.0 → **2.56**（1 计数 = 2.56 km/h）。第一次拿到地面真值 —— 蓝牙 ELM327 的 `010D` 与板子的 `0x824.data[2]` 双串口同源时间戳对齐，284 对非零样本回归 R² = 0.9984、斜率 95% 置信区间 [2.543, 2.567] ⇒ **取整 2.56**（数据只能分辨 2.55~2.57）。推翻旧口径「1 计数 = 1 km/h」（实测差 2.5 倍）。原始数据 `tools/serial-capture/drive-2026-09-22-{van,obd}.csv`，协议侧汇总见 `VAN-PROTOCOL.md`。
- **2026-09-20** 车速字段定案并上屏：0x824 的 `data[2]` 单字节，`kSpeedScale` 0.01 → 1.0（旧值来自文档，`data[2..3]` 当 16 位大端解读过）。转速字段同时从"照抄文档"变成实测钉住（`kRpmScale = 0.125`）。测试 122 例 / 119 通过 / 2 跳过。
- **2026-09-20** 用户定下 1.0 上屏、看表盘复核的口径，并取消定速跑要求。
- **2026-09-19** 关帧门限 300µs → 70µs：帧内最长间隔 49.0µs < 70 < 帧间最短空闲 95.5µs，背靠背帧不再被并成一帧（同一份切片 61 帧 → 66 帧）。
- **2026-09-19** FCS 定案：`0x0F9D` / init `0x7FFF` / 输出取反 / MSB-first，覆盖 IDEN+CMD+DATA，17106/17106 帧通过 —— 真帧第一次被收下。
- **2026-09-18** 槽时间按实车抓波改成 **8.25µs**（≈121 kbit/s），规范标称的 8.00µs 作废。

细节都在 `ACCEPTANCE.md` 文末。

## 下一步

1. **真 VAN 抓帧已在裸 S3 抓帧盒上完成**（`VAN_RX_PIN=16`，接线见 `PINOUT.md`，实车结果见 `ACCEPTANCE.md`：`edges` / `frames` 都在涨）。`VAN_RX_PIN=18` / SH1.0 14PIN 那一版属于**已退货的 DualEye**，不再执行；**2.8" 最终板到货后要在新板上重做一次 VAN RX 实测**。
2. ~~上车后用表盘复核车速刻度~~ **已作废（2026-09-22）**：绝对刻度当天已用地面真值定标（蓝牙 ELM327 `010D` 回归 ⇒ `kSpeedScale = 2.56`），不需要再靠表盘反推；而且 DualEye 已退货、手上无屏，这条路本来也走不通。**以后复核刻度只用真值源**：跑一趟带 `-ObdPort` 的 `tools/serial-capture/drive-log.ps1`（采 `010D`），按 `VAN-PROTOCOL.md` 的「车速」一节复算即可；只有真值差出 2 倍才需要动 `kSpeedScale`。
3. 有线 ELM327 接到板上，量四个字段的 `SRC-Hz`，再回头定"车速优先给谁"。
4. VAN 上找转速 / 油量 / 门灯，工具与流程照 `tools/van-decode/` 那套走。
5. 表仓最终屏与结构件：量出表玻到表盘纸的间隙，再定 2.8" 那两块。
6. 声音告警（转速进红区 5800 响一声）：默认固件合成三短音、零素材，可选上传自定义 WAV（16-bit 单声道 8~16 kHz，≤3 秒且 ≤96 KB，依据见 `ACCEPTANCE.md` 2026-09-20 条）；先等 ES8311 + I2S 通路能跑起来。★ 2026-09-22：DualEye 已退货 ⇒ 它那块 **ES8311 / 功放 / 喇叭座**不在手上，"声音走 I2S"这个结论**先保留**，但要等 **2.8" 级最终板**定型后按**新板的音频硬件**重新确认。
