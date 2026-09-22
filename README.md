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
- **车速字段定案，绝对刻度还没有**：0x824 的 `data[2]`，单字节，停车窗口内恒 0，与位移累计量增长率回归 R² = 0.9942，加速度 p99 = 19.9 km/h/s 在物理上限内。**1 计数是否恰好 1 km/h 没有地面真值**，所以固件先按 **1.0 上屏**，用表盘脸的档位边界（30 / 65 / 95 / 130）复核 —— 每条边界都是 2 倍放大的，和路上真实车速对比一次就能反推常数。**四段定速（20/40/60/80）的标定跑已按用户决定取消**，以后哪趟顺手有 OBD 日志，采几个 `010D` 散点即可定标。

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
| `pcpreview` | 本机渲双屏 BMP |
| `esp32s3` | S3 桩显示，先把串口 / VAN 跑通 |
| `esp32s3-spi` | 240 档验证用的真屏（GC9A01A 双 240×240）—— **该板 2026-09-22 已退货，现在手上没有这块硬件**（环境照旧保留、仍可编译） |
| `esp32s3-rgb` | RGB 并口骨架（最终大屏备用） |
| `esp32dev` | 经典 ESP32，4MB，廉价回归 |

`theme`（16KB @ `0x210000`）与 `image`（经典板 1MB、S3 8MB @ `0x254000`）两个分区是运行时可换的：换配色或换图不用重编固件，esptool 命令由 `tools/theme-editor/` 的两个页面按目标板打印（两份分区表的偏移刻意相同）。

刷机与测试的七条硬约束，都是踩过的：

- 刷写和监视**走板上的 UART 口（CH340）**，不是原生 USB 口：后者的复位是软复位请求，会把芯片留在 ROM 下载模式。
- S3 原生 USB 口开监视器必须 `monitor_rts = 0` / `monitor_dtr = 0`（`platformio.ini` 已写死），否则芯片被按在复位态。
- 两份分区表 CSV 必须**纯 ASCII**（PlatformIO 用系统编码读它），一个"★"就能让链接成功的构建在最后一步抛 `UnicodeDecodeError`。
- `.pio-pylibs` 里那份 pyserial 不在系统 python 里，直接敲 `python tools/serial-capture/capture.py` 会 `ModuleNotFoundError: No module named 'serial'`。
- 表情画布不要超过 320，再大会盖住内圈副弧。
- **新用例必须注册在 `test/test_dashcore/test_main.cpp` 里 `face_stages` 之前**：`test_face_stages.cpp` 有一个**既有崩溃**（`panic: index 4 out of bounds for type 'Face[4]'`，进程 exit 3），注册在它**之后**的用例根本跑不到 —— 2026-09-22 那 5 条 CRC 用例就是因此前置注册的。
- 跑 native 测试想看到测试里的输出有两条：① `python -m platformio test -e native` **默认只转发"用例结果行"**，测试里的 `printf` 表格要加 `-v` 才看得见；② `-v` 会把未解析的行 echo 到 GBK 控制台，**测试里的中文 `printf` 会抛 `UnicodeEncodeError` 并把用例统计打乱**（实测那一次 128 例被报成 124 例）⇒ 测试里的输出**一律纯 ASCII**（断言消息里的中文没事，PlatformIO 走 `\xNN` 转义路径），并且因为进程随后会崩、缓冲没人冲，**要显式 `fflush(stdout)`**，否则整段输出会丢。

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

## 最近进展

- **2026-09-20** 车速字段定案并上屏：0x824 的 `data[2]` 单字节，`kSpeedScale` 0.01 → 1.0（旧值来自文档，`data[2..3]` 当 16 位大端解读过）。转速字段同时从"照抄文档"变成实测钉住（`kRpmScale = 0.125`）。测试 122 例 / 119 通过 / 2 跳过。
- **2026-09-20** 用户定下 1.0 上屏、看表盘复核的口径，并取消定速跑要求。
- **2026-09-19** 关帧门限 300µs → 70µs：帧内最长间隔 49.0µs < 70 < 帧间最短空闲 95.5µs，背靠背帧不再被并成一帧（同一份切片 61 帧 → 66 帧）。
- **2026-09-19** FCS 定案：`0x0F9D` / init `0x7FFF` / 输出取反 / MSB-first，覆盖 IDEN+CMD+DATA，17106/17106 帧通过 —— 真帧第一次被收下。
- **2026-09-18** 槽时间按实车抓波改成 **8.25µs**（≈121 kbit/s），规范标称的 8.00µs 作废。

细节都在 `ACCEPTANCE.md` 文末。

## 下一步

1. **真 VAN 抓帧已在裸 S3 抓帧盒上完成**（`VAN_RX_PIN=16`，接线见 `PINOUT.md`，实车结果见 `ACCEPTANCE.md`：`edges` / `frames` 都在涨）。`VAN_RX_PIN=18` / SH1.0 14PIN 那一版属于**已退货的 DualEye**，不再执行；**2.8" 最终板到货后要在新板上重做一次 VAN RX 实测**。
2. 上车后用表盘复核车速刻度：按路牌或跟车对一次真实车速，错了只改 `van_source` 的 `kSpeedScale`（判据：市区正常开到 40~50 还挂着"静止/挪车"脸 = 刻度偏小一倍）。★ 2026-09-22：DualEye 已退货 ⇒ 手上暂时没有屏，这一步要等 **2.8" 级最终板**到货、上屏之后才能做。
3. 有线 ELM327 接到板上，量四个字段的 `SRC-Hz`，再回头定"车速优先给谁"。
4. VAN 上找转速 / 油量 / 门灯，工具与流程照 `tools/van-decode/` 那套走。
5. 表仓最终屏与结构件：量出表玻到表盘纸的间隙，再定 2.8" 那两块。
6. 声音告警（转速进红区 5800 响一声）：默认固件合成三短音、零素材，可选上传自定义 WAV（16-bit 单声道 8~16 kHz，≤3 秒且 ≤96 KB，依据见 `ACCEPTANCE.md` 2026-09-20 条）；先等 ES8311 + I2S 通路能跑起来。★ 2026-09-22：DualEye 已退货 ⇒ 它那块 **ES8311 / 功放 / 喇叭座**不在手上，"声音走 I2S"这个结论**先保留**，但要等 **2.8" 级最终板**定型后按**新板的音频硬件**重新确认。
