# 第三方来源与依赖声明

本文件只做**登记**：把本仓库引用过的外部资料、用到的软件依赖与字体列清楚，并说明它们**各自
的归属与条款**。它**不改写**任何既有文档里对第三方的描述（那些描述留在 `VAN-PROTOCOL.md`、
`ARCHITECTURE.md`、`PINOUT.md`、`PURCHASE.md`、`ACCEPTANCE.md` 里，本文件只是索引）。

**总原则**：第三方内容**不属于本项目**，各自保留其原始条款。本项目对它们的**引用、转述与
实测复现，不改变其归属**；本项目的 MIT / CC BY 4.0 授权也**不覆盖**它们。

---

## 0. 各层许可证一览（谁归谁）

| 类别 | 归属 | 许可证 |
|---|---|---|
| 本仓库原创代码与工具 | 本项目 | **MIT**（`LICENSE`） |
| 本仓库原创文档（`*.md`） | 本项目 | **CC BY 4.0**（`LICENSE-DOCS`） |
| 本仓库美术素材与图片 / 二进制 | 本项目 | **保留所有权利**（`LICENSE-ARTWORK`） |
| 第三方协议 / 技术资料 | 各原作者 | 见 §1，**各自保留原始条款** |
| 第三方硬件资料（wiki / 原理图 / 数据手册） | 各厂商 | 见 §2，**各自保留原始条款** |
| 第三方软件依赖与字体 | 各项目 | 见 §3、§4，**各自保留原始条款** |

---

## 1. 协议 / 技术资料出处

`VAN-PROTOCOL.md` §1.2 把来源分成两层，本表**照它列**：

| 类别 | 内容 | 出处 | 在仓库里的位置 |
|---|---|---|---|
| **A. 本项目实测**（全部帧目录、字段偏移、位定义、速率、CRC 参数） | 2026-09-18 起的实车抓包与逻辑分析仪数据 | **本项目原创** | `VAN-PROTOCOL.md` §3~§6、`ACCEPTANCE.md`、`lib/dashcore/van_wire.h` 文件头 |
| **B. 外部资料**（**仅作背景**：VAN 帧的槽位结构、4B5B / E-Manchester、CRC-15 多项式、SOF 图案） | 外部技术资料 | **Graham Auld, *VAN bus line protocol*** | 被引用于 `VAN-PROTOCOL.md` §1.2、§2.3~§2.5 |
| **B. 外部资料** | VAN 分析器实现与帧目录描述 | **`morcibacsi/VanAnalyzer`**、**`morcibacsi/psa_van_bus_packet_descriptions`** | 被引用于 `VAN-PROTOCOL.md` §1.2、`ACCEPTANCE.md`（800~807 行一带）、`ARCHITECTURE.md`（86 行） |

**声明**：以上 B 类**只用来"提出假设"，不用来"当结论"**（`VAN-PROTOCOL.md` §1.2 原文口径）。
本项目已有多处**实测推翻或修正**外部资料的记录（槽时间 8.25 µs vs 标称 8.00 µs、车速字段是
单字节 `data[2]` 而非 16 位、SOF 是裸槽不走 4B5B）。**这些实测结论是本项目原创**，而外部
资料的**原始表述仍归其原作者**；本项目对它们的引用与复现**不改变其归属**，也不对其授予任何
本项目许可证。

> 引用规范：需要细节时**引原文，不抄原文**（`VAN-PROTOCOL.md` 文末「引用索引」的口径）。

---

## 2. 硬件资料（仅作引脚与电气事实的引用来源）

下列资料**只作为引脚、器件与电气事实的引用来源**。它们**没有入库**（原理图 PDF 当时只落在
`%TEMP%`，见 `ACCEPTANCE.md` 2478 行），本仓库只记录**引用链接与年份**，版权归各厂商。

| 资料 | 用途 | 出处 |
|---|---|---|
| 微雪（Waveshare）`ESP32-S3-Touch-LCD-2.8C` **官方原理图**（1 页 PDF；重抓件 `SHA256 = 01EAE811F2B777B3F919108ACB48C61E79734055F517A07D516185EF26299D76`） | 逐脚读出的板载连接（`FSUSB42UMX` U10、`CH343P` U9、`ME6217C33M5G` U5、`ETA6098` U1、`J9` SH1.0 12P 等） | <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf>（引用见 `ARCHITECTURE.md` §8 L1/L6、`ACCEPTANCE.md` 2423~2478 行） |
| 微雪 `docs.waveshare.com` 文档站（`ESP32-S3-Touch-LCD-2.8C`、`ESP32-S3-LCD-2.8C`） | 器件描述、`Onboard Resources` 原文、引脚 / UART / 12PIN 表 | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C> ／ <https://docs.waveshare.com/ESP32-S3-LCD-2.8C> |
| 微雪 wiki 网页版 | 旁证（当时对本机返回 HTTP 403，**未用网页正文当依据**） | `waveshare.com/wiki/...`（见 `ACCEPTANCE.md` 2433 行说明） |
| 微雪 `ESP32-S3-DualEye-Touch-LCD-1.28` 原理图 / 例程 / wiki | **已退货的 240 档验证板**：18PIN FPC（L7/L9）、GPIO1 功能选择电阻、SPI 驱动填写依据 | 引用见 `PINOUT.md` §「18PIN FPC 那两个座到底是什么」（645~653 行）、`README.md`、`tools/theme-editor/README.md` |
| 屏驱动 IC 资料：**ST7701S**（2.8" 480×480 RGB 用）、**GC9A01A**（1.28" 240×240 用） | 显示驱动初始化与电气参数的引用来源 | 引用见 `ARCHITECTURE.md`（294 行：*料表就带 ST7701S 规格书*）、`PURCHASE.md`（641~649 行）、`src/dash_display_spi.cpp` |
| 其它厂商页面（Dwin、Ronbo、Adafruit、TJC、屏库等） | 采购对比中的**尺寸 / 接口事实**引用 | 见 `PURCHASE.md` 的对比表（各条自带链接） |

**声明**：以上资料的**版权与条款归各厂商**；本仓库只做**事实性引用**（"这个脚连到那颗芯片"、
"这块屏是多少 pin"），**没有再分发这些资料的任何副本**，也不把它们纳入本项目的 MIT / CC BY 4.0。
`PINOUT.md`、`ARCHITECTURE.md` 里由本项目**实测得出**的接线结论（例如"`edges` 涨、`frames=0`
⇒ 对调那两根线"）属于本项目原创。

---

## 3. 软件依赖

### 3.1 固件侧（`platformio.ini` 声明）

| 依赖 | 版本 | 许可证 | 声明 / 核对来源 |
|---|---|---|---|
| **LVGL**（`lvgl/lvgl`） | `platformio.ini` 声明 `^9.3.0`；本机解析到 **9.6.0** | **MIT** | 依赖自带 `library.json` → `"license": "MIT"`；仓库根 `LICENCE.txt` → *"MIT licence / Copyright (c) 2025 LVGL Kft"* |
| **Arduino core for ESP32**（`framework = arduino`；PlatformIO 包 `framework-arduinoespressif32`） | 本机 **4.20017.260907+sha.dcc1105b** | **LGPL-2.1-or-later** | 包自带 `package.json` → `"license": "LGPL-2.1-or-later"` |
| **ESP-IDF**（Arduino core 内捆绑，构成其 SDK 主体） | 本机构建宏 `IDF_VER="v4.4.7-dirty"` | **Apache-2.0** | 版本见 §3.5 的 `IDF_VER` 核对法；**许可证按 ESP-IDF 上游声明**（本机那份平台包里**没有**随附 LICENSE 文件，故此处不是从本地文件核对出来的） |
| **FreeRTOS**（ESP-IDF 内组件，本仓库用到其头文件路径） | 随 IDF v4.4.7 | **MIT** | 随 ESP-IDF 分发；同上一行，按上游声明 |

> ★ `platformio.ini` 只声明了**平台名**（`platform = espressif32`）、**板子**（`board = esp32dev` /
> `esp32-s3-devkitc-1`）与 `framework = arduino`，**没有钉住具体版本号**；上表的版本号取自
> **本机实际解析到的构建环境**，不是文件里的声明。要点是**平台包由 PlatformIO 在构建时下载**，
> **不入库**。

### 3.2 测试与宿主机侧

| 依赖 | 版本 | 许可证 | 声明 / 核对来源 |
|---|---|---|---|
| **Unity**（PlatformIO 测试框架 `test_framework = unity`） | `library.json` → **2.6.1** | **MIT** | 自带 `library.json` → `"license": "MIT"`；自带 `LICENSE` → *"The MIT License (MIT) / Copyright (c) 2007-25 Mike Karlesky, Mark VanderVoord, & Greg Williams"* |
| **pyserial**（`tools/serial-capture/capture.py`、`replay.py`、`replay-drive.py` 直接 `import serial`） | **3.5** | **BSD**（BSD-3-Clause 家族；以 PyPI 元数据 `License: BSD` 为准） | 本机 `.pio-pylibs/pyserial-3.5.dist-info/METADATA` → `License: BSD`、`Classifier: License :: OSI Approved :: BSD License` |

### 3.3 构建 / 烧录工具链（不在仓库内、构建时装）

| 工具 | 版本 | 许可证 | 备注 |
|---|---|---|---|
| **PlatformIO Core** | 本机 **6.2.0** | **Apache-2.0** | 构建与测试入口（`python -m platformio`）；本机装在 `.pio-pylibs` |
| **esptool** | 随 PlatformIO `tool-esptoolpy` 包：**4.11.0**（平台包版本串 `2.41100.260830`） | **GPL-2.0-or-later** | 仅作**烧录工具**由命令行调用（`python -m esptool`）；其源码不被本项目链接或再分发。核对来源：包自带 `package.json` → `"license": "GPL-2.0-or-later"` 与 `LICENSE`（GPLv2 全文） |
| **pyelftools** | 本机 **0.33** | **Public domain** | PlatformIO 的传递依赖；本机 `.pio-pylibs/pyelftools-0.33.dist-info/METADATA` → `License: Public domain` |
| **xtensa / riscv 工具链**（`toolchain-xtensa-esp32`、`toolchain-xtensa-esp32s3`、`toolchain-riscv32-esp`，GCC + newlib） | 随平台包 | **GPL-3.0-or-later（含 GCC 运行时例外）/ newlib 各自条款** | 仅作**编译器**在构建时使用；★ 按 **GCC / newlib 上游声明**列出（本机这几个工具链包里**没有**随附 `COPYING*` 文件，故未从本地文件核对） |

### 3.4 JavaScript / Node 侧 —— **没有第三方依赖**

- 仓库内**没有 `package.json`、没有 `package-lock.json`、没有 `node_modules`、没有 npm 依赖**。
- `tools/theme-editor/` 下的脚本只 `require` **Node 内置模块**（`fs`、`path`）与**同目录的自家
  文件**；两个 HTML 页面（`index.html`、`image-editor.html`）**不引用任何 CDN / 外部脚本**，
  只用浏览器原生 API（Canvas / `drawImage` 等）。
- ⇒ **JS 侧无需声明任何第三方许可证**（这些脚本本身是本项目代码，适用 MIT）。

### 3.5 核对方法（可自证）

上面每一条许可证都能**从依赖自己的声明重新核对**，不需要信本文件：

| 要核对 | 看哪里 |
|---|---|
| LVGL | `.pio/libdeps/<env>/lvgl/library.json` 的 `"license"`；仓库根 `LICENCE.txt` |
| Unity | `.pio/libdeps/<env>/Unity/library.json` 与 `LICENSE` |
| Arduino core | `<PLATFORMIO_CORE_DIR>/packages/framework-arduinoespressif32/package.json` |
| ESP-IDF 版本 | 构建宏 `IDF_VER`（`.pio/build/<env>/idedata.json` 的 `defines`） |
| pyserial / PlatformIO / pyelftools | `.pio-pylibs/<pkg>.dist-info/METADATA` 的 `License` 字段 |
| JS 依赖 | `git ls-files` 里搜 `package.json`（没有） |

---

## 4. 字体

| 字体 | 用途 | 许可证 | 来源 |
|---|---|---|---|
| **Montserrat**（LVGL 内置点阵字体，字号 **10 / 14 / 18 / 24 / 48**） | 屏上所有数字与单位（在 `include/lv_conf.h` 里逐个 `LV_FONT_MONTSERRAT_*` 打开） | **SIL Open Font License 1.1** | 由 **LVGL** 随库分发；许可原文在 LVGL 包内 `scripts/built_in_font/font_license/Montserrat/OFL.txt`，版权行 *"Copyright 2011 The Montserrat Project Authors (https://github.com/JulietaUla/Montserrat)"* |

**说明**：本仓库**没有自带任何字体文件**（无 `*.ttf` / `*.otf`），编译进固件的点阵数据来自
**LVGL 这个依赖**；字体本身的授权走 **SIL OFL 1.1**，**不适用**本项目的 MIT。LVGL 包里同时
带有 Lato、Noto 等其它字体的许可文件，但**本仓库没有使用它们**（`lv_conf.h` 只开了 Montserrat）。

---

## 5. 变更纪律

- 新增**第三方资料引用** → 在本文件登记（资料名 + 引用位置 + 归属），**不要**把它写成本项目所有。
- 新增**软件依赖** → 在 `platformio.ini` 声明的同时，在本文件 §3 补一行（依赖 + 版本 + 许可证 + 核对来源）。
- **不要**在既有文档里改写对第三方的描述（那些描述是本项目实测对照的记录，属于文档内容，
  适用 CC BY 4.0）；本文件只负责"登记与归属"。
