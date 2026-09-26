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

**两块 2.8C 已经在手上并跑起来了**：微雪 ESP32-S3-LCD-2.8C（非触摸，480×480 RGB 并口，RGB565），一块当**主板**（右屏 / 车速+进气）、一块当**从板**（左屏 / 转速+水温），各自插自己的 `USB` 口供电与看日志，**两板之间一根线都没有**。显示侧已定案：PCLK 15 MHz、双 framebuffer、弹跳缓冲 **20 行**（18.75KB 那档会出横纹，见 `ACCEPTANCE.md`）、16 行绘制缓冲；主循环实测 ~1.4~2.3 万圈/秒。

板间链路已从"有线 UART"改为**无线 ESP-NOW 为主**（有线那一档**停用但一行不删**，见 `ARCHITECTURE.md` §8.3）：两块板同信道 6、不连 AP、`WIFI_PS_NONE`，地址自配置（广播 HELLO → 学到对端 MAC → 转单播 → 存 NVS）。

**时基已达标**（契约 §4 第一档 = 100ms）：TICK 的**发送**搬进了一个高优先级任务（优先级 15），主循环被抢占不再拖住它 —— 从板实测 **TICK 50/50 收到、`miss` 增量 0、`tick_age` ≤19ms、4.2 分钟持续负载全程 `locked`**（改动前同一套负载只有 30% `locked`）。搬它的过程中还挖出并修掉一个**既有缺陷**：真帧的"分发"路径把整帧丢进了局部变量（`LinkRx::feed()` 会把凑齐的帧交出来），实测**主板实发 199 帧/秒、从板只收到 ~6% 的 TICK**，而测量帧在同样空中 0% 丢包 ⇒ 丢在分发、不在空中。

**仍未达标（如实）**：测量工具自己的 `gap_max`(106~119ms)/p99(41~48ms) —— 因为 burst 帧仍由主循环发，它量的是"链路 + 我们的调度"而不是纯链路；以及 burst 上偶发的 **0.10% 边缘丢包**（10 次里 3 次各丢 2 帧）与 6 个 CRC 错。

VAN 协议已经解完并在真实抓包上验过：帧布局（SOF 10 槽 / IDEN 15 位 / CMD 5 位）、FCS（`crc15_van_iso`，0x0F9D / init 0x7FFF / 取反 / MSB-first，覆盖 IDEN+CMD+DATA）、槽时间（**8.25µs**，规范标称的 8.00µs 偏 3.1%，已作废）都有抓包支撑 —— 怠速 1704/1704 帧、行驶 34051/34051 帧 FCS 全中。

- **转速已标定**：0x824 的 `data[0..1]`，16 位大端 ÷8。点火瞬间 raw 从恒 0 跳到 6152（= 769 rpm，起动机拖动），怠速平台 453 帧均值 7237.6（= 904.7 rpm，sd 26.9），地面真值 900 rpm，偏差 0.52%。
- **车速字段与绝对刻度都已定案**：0x824 的 `data[2]`，单字节，`kSpeedScale = 2.56`（1 计数 = 2.56 km/h）—— 真值源是蓝牙 ELM327 的 `010D`，双串口同源时间戳对齐后回归（368.2 s 行驶、599 个真值样本、284 对非零样本 R² = 0.9984）；数据可分辨范围 **2.55~2.57**，**2.56 是取整**。原始数据 `tools/serial-capture/drive-2026-09-22-van.csv` + `-obd.csv`，证据见 `VAN-PROTOCOL.md` 的「车速」一节。**旧口径「1 计数 = 1 km/h」已作废**（实测差 2.5 倍）。**四段定速标定跑仍按车主决定不做**。
- **已解字段接进了数据层**：`0x4FC` 的灯位域（左/右/双闪/近光/仪表盘灯）、门信号、`0xE24` 的 VIN。**没有**在 VAN 上的：刹车、雨刮、远光闪 —— 这三条别指望 VAN。

还没做完的：有线 ELM327 接到板上的实车刷新率（看 `SRC-Hz` 行）、VAN 上的油量与门灯细项、表仓里的最终结构件、以及上面那两项链路时延。

**验证用的板子（历史）**：微雪 ESP32-S3-DualEye-Touch-LCD-1.28（240×240 GC9A01A ×2）已完成"真屏 240 档"验证并于 **2026-09-22 退货**。**240 档的结论不作废**：`esp32s3-spi` 环境、`ui_theme.h` 的 240 字号档、`tools/theme-editor/spec-240.json` 都保留且仍可编译，只是现在手上没有这块硬件。它的 ES8311 / 功放 / 喇叭座也随之不在手上 ⇒ "声音走 I2S"这条结论先保留，等最终音频硬件定了再说；**当前两块 2.8C 板载的是有源蜂鸣器（只能响/不响，已接进告警层）**。

## 编译 / 测试 / 刷机

仓库路径里有中文，ESP32 工具链会在中文路径下报 `g++ Invalid argument`，所以**构建在纯 ASCII 副本上做**，两边用 robocopy 同步；本机的 PlatformIO Core 和工作区依赖都不在系统 python 里，`PYTHONPATH` 与 `PLATFORMIO_CORE_DIR` 两个环境变量不能省（宿主机编译器是 `.tools\zigbin` 里的 zig 转发桩）。下面每一行都在 `$copy` 目录里执行。

```powershell
$repo='C:\Users\张九思\206Dash\Neru-s-206-dashboard'
$copy='C:\206dash-scratch\now'
robocopy $repo $copy /E /XD .git .pio /NJH /NJS /NP      # 退出码 1/3 = 成功

$env:PYTHONPATH='C:\Users\张九思\206Dash\.pio-pylibs'
$env:PLATFORMIO_CORE_DIR='C:\206dash-scratch\pio-core-mix'
$env:PATH='C:\206dash-scratch\zigbin;' + $env:PATH

python -m platformio test -e native                 # 宿主机单元测试，不烧板
python -m platformio run -e pcpreview -t exec       # 本机渲双屏 BMP，Ctrl+C 后打开 preview/preview.html
python -m platformio run -e esp32s3-rgb-master-now -t upload --upload-port COM9   # 主板
python -m platformio run -e esp32s3-rgb-slave-now  -t upload --upload-port COM8   # 从板
```

编译目标（**两块板当前用哪两份，看 `-now` 那两行**）：

| 环境 | 用途 |
|---|---|
| `native` | 宿主机单元测试（当前 369 例 / 2 跳过 / 367 通过） |
| `pcpreview` | 本机渲双屏 BMP（**能手动喂输入**：键盘 `← → 空格 L P D O R M V K T X`，或 `preview/inject.txt` —— 见 `tools/theme-editor/README.md`） |
| `esp32s3-rgb-master-now` | ★ **主板（右屏）**：2.8C 显示 + ESP-NOW PHY + `-DLINK_ROLE=1`，日志只走原生 USB-CDC |
| `esp32s3-rgb-slave-now` | ★ **从板（左屏）**：同上，`LINK_ROLE` 取默认值 0 |
| `esp32s3-rgb` | 单板显示固件（不分角色、不走链路），日志与文本回放仍走 UART0 |
| `esp32s3-rgb-master` / `-slave` | **有线 UART 链路**那一档（43/44 让给链路）—— **停用但保留**，见 `ARCHITECTURE.md` §8.3 |
| `esp32s3` | S3 桩显示，先把串口 / VAN 跑通 |
| `esp32s3-vaninv` / `-vansniff` | VAN 抓帧盒用的两份（裸 S3 + SN65HVD230） |
| `esp32s3-spi` | 240 档验证用的真屏（**该板已退货，环境保留仍可编译**） |
| `esp32dev` | 经典 ESP32，4MB，廉价回归 |

★ 两块板的分区表**不一样**，这是刻意的：有线档用 `partitions-s3.csv`（1MB app 槽 ×2 + OTA 槽），**无线档用 `partitions-s3-now.csv`**（单个 2MB `factory` 槽，**没有 OTA 槽**）—— 因为 ESP-NOW 把整个 WiFi 协议栈链进来，镜像 **1.49MB**，塞不进 1MB 槽。两张表的 `theme`/`spiffs`/`image`/`coredump` 偏移**刻意相同**，`nvs` 也都在 `0x9000`（所以**刷固件不会丢静音设置**）。

`theme`（16KB @ `0x210000`）与 `image`（经典板 1MB、S3 8MB @ `0x254000`）两个分区是运行时可换的：换配色或换图不用重编固件，esptool 命令由 `tools/theme-editor/` 的页面按目标板打印。素材还可以**导成一个 `assets.bin` 合并包**（页面里导出/导入同一份文件），格式见 `tools/theme-editor/ASSET-PACKAGE.md`。

刷机与测试的硬约束，都是踩过的：

- ★★ **烧写不要 patch 镜像自带头**：`esptool write_flash` 带显式 `--flash_mode qio --flash_freq 80m --flash_size 16MB`（**这三个值和 board JSON 一致**）会把从板打进 `TG0WDT_SYS_RST` 复位循环、app 日志零字节；同一批文件改用 `--flash_mode keep --flash_freq keep --flash_size keep` 立刻正常。**一律用 `keep`。**
- ★ 两块板**各自插自己的 `USB`（原生）Type-C 口**就能烧写与看日志（这两晚一直这么干，COM9=主板 / COM8=从板）。插哪个 Type-C 会改变 43/44 的归属（见 `ARCHITECTURE.md` §8.2），那也是"有线链路停用"的背景之一。
- ★ **别对同一个项目目录同时跑两个 PlatformIO**：实测会出现"上传 1 秒返回、什么都没烧"的假成功；判断依据是板子的 uptime 有没有归零。
- 开原生 USB 口的监视器必须 `monitor_rts = 0` / `monitor_dtr = 0`（`platformio.ini` 已写死），否则芯片可能被按在复位态 —— **开串口本身就可能把芯片踢进下载模式**，读完要确认板子还在跑。
- 分区表 CSV 必须**纯 ASCII**（PlatformIO 用系统编码读它），一个"★"就能让链接成功的构建在最后一步抛 `UnicodeDecodeError`。
- esptool v5 的进度条是 Unicode，控制台是 GBK 时会 `UnicodeEncodeError` 中断烧写 ⇒ 先 `$env:PYTHONIOENCODING='utf-8'`。
- `.pio-pylibs` 里那份 pyserial 不在系统 python 里，直接敲 `python tools/serial-capture/capture.py` 会 `ModuleNotFoundError: No module named 'serial'`。
- 表情画布不要超过 320，再大会盖住内圈副弧。
- native 测试：① 默认只转发"用例结果行"，测试里的 `printf` 要加 `-v`；② `-v` 会把未解析的行 echo 到 GBK 控制台，**测试里的中文 `printf` 会抛 `UnicodeEncodeError`** ⇒ 测试输出一律纯 ASCII，并且要显式 `fflush(stdout)`；③ 直接跑 `.pio\build\native\program.exe` 时 **CWD 必须是项目根**（4 条 `test_real_capture_*` 按相对路径读 CSV，否则假红 4 条）。

串口命令（`lib/dashcore/serial_cmd.h` 是唯一口径）：`b` 蜂鸣器试响 · `m` 静音开关（存 NVS）· `r` 面板重新初始化 · `d` 诊断页 · `w` RF 测速/测丢包（只在无线档构建里有效）。

## 文档在哪

| 文件 | 回答什么 |
|---|---|
| `ACCEPTANCE.md` | **逐次日志**：每个结论的实测证据、踩过的坑、仍未验的项 |
| `ARCHITECTURE.md` | 分层与数据流，以及每个取舍的理由（§8.3 = 链路选型定案；§7.5 = 日志不许阻塞主循环） |
| `docs/LINK-TWO-BOARD.md` | 双板链路：无线档的 **10 分钟实测清单**、烧写/排查、它测不到什么 |
| `docs/LINK-LOOPBACK.md` | 单板回环（一块裸 S3，另一件事，与双板链路无关） |
| `docs/RGB-PANEL-2.8C.md` | 2.8C 显示：bring-up 结论、笔刷/撕裂那些真话、蜂鸣器、构建环境 |
| `docs/PREVIEW.md` | 预览页能证明什么、**不能**证明什么 |
| `PINOUT.md` | 接线：VAN 三处取信号、开箱先量什么、抓帧盒怎么用 |
| `PURCHASE.md` | 采购：清单、到货后的验证动作、别买 CAN-only |
| `tools/web/index.html` | **网页统一入口（导航页）**：四张卡片 + 工作流顺序（配色 → 图片 → 刷写 → 预览）；`tools/web/open.bat` 同效 |
| `tools/theme-editor/README.md` | 两个网页编辑器（配色 / 图片）、`image.bin` 格式、合并包与刷写偏移 |
| `tools/van-decode/` + `tools/serial-capture/` | 抓包解帧与串口回放的脚本 |

**文档纪律（2026-09-27 车主定）：**
1. **`README.md` 每天更新一次** —— 它放**稳定信息 + 最近进展**（"现在到哪一步"、"下一步"、踩过的硬约束）。
2. **`ACCEPTANCE.md` 是逐次日志** —— 每一次实测的原始数字、每一处撤回、每一个未验项都写在那儿；README 不复述细节，只指过去。
3. 任何"和事实相反"的旧结论**必须当场改掉或标注作废**（本仓库有过多次：1 计数 = 1 km/h、槽时间 8.00µs、"只有裸 S3 抓帧盒"、"刷写必须走 CH340"）——**留旧文不改比留空更糟**。

## 许可证与引用

本仓库按内容分三层授权：**代码**（`src/` / `lib/` / `include/` / `test/` / `preview/` / `tools/` 下的源码与构建配置，含接线 / 引脚 / 分区这类事实性表格）= **MIT**，见 `LICENSE`；**文档**（全部 `*.md`，含本文与 `VAN-PROTOCOL.md`）= **CC BY 4.0**，见 `LICENSE-DOCS`；**美术素材与图片 / 二进制** = **单独声明、保留所有权利**，**不在** MIT 或 CC BY 4.0 覆盖范围内，见 `LICENSE-ARTWORK`。完整的分层范围说明（**不改动 MIT 正文**）另见 `LICENSING.md`。

引用或转载**文档**（尤其 `VAN-PROTOCOL.md` 的协议表）请按 CC BY 4.0 注明出处：仓库地址 <https://github.com/DDNaganami/Neru-s-206-dashboard> + 文件路径；机器可读的引用元数据在 `CITATION.cff`。

**第三方**（协议 / 技术资料、硬件资料、软件依赖与字体）各自保留其原始条款，登记见 `THIRD-PARTY.md`。

## 最近进展

- **2026-09-27** ★★ **显示与链路各自定案一件，且都过了"人的眼睛"这一关**：
  **① 副屏横纹定案 = 弹跳缓冲太小**。同样 10 次 burst × 25s 持续负载、唯一变量 `-DRGB_BOUNCE_LINES`：20 行没横纹、10 行横纹明显（判据是**车主的眼睛** —— 六项面板计数器在两种取值下**全都正常**，这类错位发生在"显存 → 玻璃"之间，软件读不回玻璃）。无线档两份 env 定为 20。
  **② 链路时基达标 = 把 TICK 的发送搬进高优先级任务**（选项 (a)）：从板 TICK **3.8/s → 50.0/s**、`miss` 增量 **3531 → 0**、`tick_age` p95 **483ms → 19ms**、持续负载下 **`locked` 30% → 100%**；改动后请车主再看了一眼屏幕确认**横纹没有回来**（调度类改动只有眼睛能判）。
  搬它的过程中还挖出并修掉一个**既有**缺陷：真帧在"分发"这一步被整帧丢掉（`LinkRx::feed()` 会把凑齐的帧通过 `out` 交出来，而调用处的 `Frame` 是局部变量）—— 实测主板实发 199 帧/秒、从板只收到 ~6% 的 TICK，而测量帧在同样空中 0% 丢包。
  另修三处只在真链路上才暴露的问题：收环永久死锁（空环时把可用空间算成 `N-H`，H 飘过 240 就再也绕不回去）、"一个包里塞多帧而接收端按一包一帧判"（丢包 69% 的真因）、测量信封漏进车辆数据（屏上冒 `coolant=9.0C`，逐字节对上了信封布局）。
- **2026-09-25/26** ★ **2.8C 上板 + 整条素材链 + 板间链路两份镜像**：2.8C bring-up 收尾（撕裂/残影/条纹、PCLK 与弹跳缓冲定案）；`assets.bin` **单文件导出/导入**（页面与 CLI 产物逐字节一致）；网页导航页与各页返回按钮；"数据不可信"角标 + 诊断页；面板守护（2s 回读、异常自动重初始化）；复位原因落盘（NVS）；**日志绝不阻塞主循环**（8KB 环 + 每圈预算）；两块板的角色标签（`MASTER (RIGHT)` / `SLAVE (LEFT)`）；素材缺两张脸（role 13 左·运动 / role 17 右·快速路）待补。
- **2026-09-24** 已解字段接进数据层（`0x4FC` 灯位域 / 门 / `0xE24` VIN）+ 指示灯与告警框架（超速 / 红区 / 门 / 转向灯忘关）+ 可替换蜂鸣器抽象；预览页加输入注入；素材上传端规格补齐。
- **2026-09-22** 车速绝对刻度定标 `kSpeedScale` 1.0 → **2.56**（R² = 0.9984，推翻旧口径）。
- **2026-09-19/20** FCS 定案、关帧门限 300µs → 70µs、槽时间改 8.25µs、车速字段上屏。

细节都在 `ACCEPTANCE.md` 文末；上面每条的原始数字都能在那儿按日期查到。

## 下一步

1. **把 burst 的发送也搬进那个高优先级任务**：现在测量工具的 `gap_max`/p99 量的是"链路 + 主循环调度"，
   搬完之后那两个数才真正代表链路；顺带能验证 burst 上那 0.10% 边缘丢包是不是同一原因。
2. 真 VAN 抓帧在 2.8C 上重做一次（`VAN_RX_PIN=42`，把抓帧盒退休）。
3. 链路上加"VAN 原始帧转发"（主板 → 从板），从板就可以不用自己接 VAN。
4. 从板 SD 卡记录（1-bit SDMMC / GPIO 2·1·42，非阻塞写、开机对时）。
5. 有线 ELM327 接到板上，量四个字段的 `SRC-Hz`，再定"车速优先给谁"。
6. VAN 上找油量与门灯细项；表仓最终结构件（量表玻到表盘纸的间隙）。
7. 声音告警：两块 2.8C 板载**有源蜂鸣器**已能响；自定义 WAV 那条等最终音频硬件定了再说。
8. 素材还缺两张脸（role 13 左·运动 / role 17 右·快速路）—— 等车主补图。
