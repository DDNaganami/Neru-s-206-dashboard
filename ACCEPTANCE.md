# 验收

- [x] 工程能编译（esp32dev 已实测；构建路径需纯 ASCII，见 ARCHITECTURE.md）
- [x] 宿主机单元测试全绿（python -m platformio test -e native，当前 **64 例**）
- [x] VAN 线路层协议栈（van_wire.h/.cpp）：SOF 识别、4B5B（E-Manchester）解码、
      IDEN/CMD 拆分、CRC-15、帧尾收尾；编码器 + 解码器往返自洽
- [x] **VanPhyWire 整链回归**：GPIO 边沿 → 4B5B → 帧字节 → FCS 校验 → VanPacket
      → VanSource（车速/转速）。黄金向量取自公开真实抓包的 IDEN/CMD
      （8C4/C、8D4/C、5E4/C、4D4/E），连续多帧不丢字节。
      零依赖寄存器，`onEdge(t_us, level)` 即可喂真实中断。
- [x] VanPacket 扩到 **15 位 IDEN**（能表达 0x000/0xFFF 保留值）+ cmd + ack + fcs_ok
- [x] 回放语法扩到 15 位 IDEN 与显式 CMD（"VAN <iden> [cmd] <data...>"）
- [x] VanSourceSink 适配层：物理层 → 数据源的接线打通（main.cpp 已接）
- [x] **运行时主题（改配色不用重编译固件）**：`theme` 分区（`0x290000`/16KB）
      + JSON 解析（手写扫描器，不依赖 newlib 的 float 解析）+ 值域钳制；
      加载失败**自动回退默认主题**，串口打印原因。
      实测：`THEME_FILE=theme-demo.json` 与默认主题渲同一帧，6/6 采样点、
      3600/3600 采样像素都不同；损坏/截断的 JSON 被拒后仍能出 232/181 帧。
- [x] **图片资源（放 flash 分区，改图不用重编译固件）**：
      `image` 分区（`0x254000`/**1MB**）+ `esp_partition_mmap` 只读映射
      （像素不进 DRAM）+ `lv_image_dsc_t` 组装；空分区/坏镜像一律拒收并降级。
      并且**真的接上了**（`src/image_load.cpp` 由 `setup()` 调用）——
      曾经漏接：`imageBlobLoad()` 写好了却没人调，刷了图片在设备上完全静默。
      实测（pcpreview，`IMAGE_BLOB` 指向 JS 生成的 1088 字节镜像，输出 262 帧）：
      `image ok: 5 张,数据 372 字节,镜像 1088 字节`。
      这证明"JS 打包 → 宿主机加载 → 索引可用"整条链通了。
      设备端的 `esp_partition_find_first`/`mmap` 分支仍需上板确认
      （见下面那条待办里的串口输出）。
- [x] **图片角色支持双屏两套**：`ImageRole` 有 8 个在用 ——
      背景 / 开机动画帧 + 左屏三表情（车速表）+ 右屏三表情（转速表）。
      **开机画面不用图片**：它是程序化的（`boot_anim.cpp` 的淡入 + 扫表 + 睁眼），
      `boot_stagger_ms` 已让两屏错峰启动，观感上就是两个不同的开机过程；
      用位图要多占 144KB 和一套时序逻辑，不值。
      编号 9/10 作为**保留**留着不回收（曾用于开机图）——
      复用已用过的编号会让别人已导出的 image.bin 静默变成另一个角色，
      两边都有测试断言它们没被占用。
      编号是"界面下拉框 / 打包器 / 固件"三方共用契约，两边都被钉死
      （C 的 `test_role_ids`、JS 的"角色编号"一节），往返测试还核对中文名。
- [x] **JS 打包器 ↔ 固件解析器的往返一致性测试**（`tools/theme-editor/test-image-roundtrip.ps1`）：
      Node 侧 121 项断言 + 用 JS 打镜像（覆盖全部 9 个角色）+ 让
      **固件自己的解析器**逐字段、逐行、逐字节对账。
      这个核对实测抓出四个真 bug，全都是编译期看不出来的：
      索引项名字用了上一轮循环的遗留变量（每项名字都变成最后一张）、
      像素循环漏 x 偏移（每行重复第一个像素）、
      `offset` 是 u16 把镜像卡在 64KB、结构体对齐算错（44 vs 48）。
- [x] 固件侧 P0 准备就绪（**未上板，仅静态验证**）：
      `setup()` 会打印 `206 dash ok`（放在最前，后续初始化失败也能看到）；
      每 5 秒打印 `SRC speed/rpm/coolant | v=..km/h ..rpm ..C` —— 带数值，
      因为桩驱动丢弃画面，"假数据扫表"只能靠串口确认；
      sim 车速按 `0.5*(sin(t*0.15)+1)*210` 扫满 0→210（周期约 42 秒，
      实测 5 秒间隔能看到明显变化）。以上字符串已确认存在于
      `.pio/build/esp32dev/firmware.bin` 中。
      上电后还会多两行便于定位：`theme: ...` 和
      `image ok: N 张,...` / `image none: 无图片资源,背景用主题纯色`。
- [ ] 刷到板子并观察上述输出（**未做：本机只有 COM1，无任何 USB 转串口设备，
      板子未插。插上后 `pio run -e esp32dev -t upload` 即可，约 10 分钟**）。
      要确认的行：`206 dash ok` / `theme: ...` / `image none: ...`（没刷图片时）
      / `van phy: stub` / 每 5 秒的 `SRC ... | v=... vrpm ...C`（车速应从 0 扫到 210）。
      若之后刷了 image.bin，这里应变成 `image ok: N 张,...` ——
      那是"分区表 + mmap + 格式"三者同时正确的证据。
- [ ] 真屏到货：锁分辨率（改 ui_theme.h 的 THEME_DISPLAY_RES）→ 写实驱动
      （dash_display.cpp，#error 保护）→ 删 DASH_DISPLAY_STUB；板子换 S3+PSRAM
- [ ] **把图片画到屏幕上**（`dash_ui.cpp` 接 `image_blob`）。
      只做了"格式 + 解析 + 加载 + 单测 + 编辑器"，**图层结构没动** ——
      因为这件事的唯一验收方式是看真屏，桩驱动丢弃画面。
      等真驱动能出图再接，否则只能证明"编译过了"。
- [ ] 实车 K 线 OBD：010C/0105 持续更新，断线 3 秒回退假数据
- [ ] 实车 VAN：0x824 车速帧格式验证（不符先用 configureSpeedFrame 现场改）

## 分区表（`partitions.csv`，改这里必须同步改三处常量）
| 名称 | 类型/子类型 | 偏移 | 大小 | 谁在用 |
|---|---|---|---|---|
| `nvs` | data/nvs | `0x9000` | 20 KB | WiFi 等 |
| `otadata` | data/ota | `0xe000` | 8 KB | OTA 状态 |
| `app0` | app/ota_0 | `0x10000` | 1 MB | 固件（现占 512 KB） |
| `app1` | app/ota_1 | `0x110000` | 1 MB | OTA 备份槽（**必须与 app0 等大**） |
| `theme` | data/0x40 | `0x210000` | 16 KB | 运行时主题 JSON |
| `spiffs` | data/spiffs | `0x214000` | 256 KB | 未用（本项目没有文件系统） |
| `image` | data/0x41 | `0x254000` | **1 MB** | 图片资源（mmap 只读） |
| `coredump` | data/coredump | `0x354000` | 64 KB | 崩溃转储 |

严丝合缝排到 `0x364000`（3.39 MB / 4 MB），无重叠、无空洞。

三处必须一起改：`lib/themetool/theme_store.h` 的 `THEME_MAX_BYTES`、
`lib/themetool/image_blob.h` 的 `IMAGE_PARTITION_BYTES`、
`tools/theme-editor/image-blob-build.js` 的 `PARTITION_BYTES`
（以及 README 和 `esptoolCommand()` 里的刷写偏移 `0x254000`）。

**这张表踩过的坑：**

1. **分区重叠**：`image` 一开始写成 `0x3DC000 + 0x20000`，正好越过
   `coredump` 的 `0x3F0000`，构建报 `CSV Error: Partitions overlap`。
   空洞不会报错，重叠一定报 —— 所以宁可排满也别留缝。
2. **两个 app 槽必须等大**。从 1.25 MB 降到 1 MB 是为了腾出 512 KB 给
   `image`；但 app0/app1 一旦不等大，OTA 升级会失败。
3. **128 KB → 1 MB 的直接原因**：图片资源。去掉开机图后需要的是
   **2 背景 + 6 表情 = 8 张**，240×240 时共 900 KB（余 124 KB）——
   128 KB 连一张 240×240 都放不下。
   逐帧满屏开机动画仍然放不下：480×480 × 12 帧 × 2 屏 = 10.5 MB，
   比整块 flash 还大 —— 所以开机画面走程序化扫表，不碰图片资源。

## 图片格式（`image.bin`）的两个关键决定

- **`offset` 是 u32，不是 u16。** 曾经是 u16，把整个镜像的像素数据卡在
  64 KB 以内 —— 一张 480×480 背景就要 450 KB，1 MB 分区等于白给。
  而且它**不报错**：偏移溢出后设备端拿到错误的像素位置，画面是花的，
  但日志一切正常。
- **索引项 44 字节、没有任何对齐空洞。** 这条是被测试逼出来的：
  手算对齐错了两次（一次漏算两个 u32 之间的洞、一次多算尾部填充），
  两次都是 `test_entry_field_offsets` 先发现。现在 C 侧用
  `offsetof`/`sizeof` 断言、JS 侧用字节断言，把同一张表各钉一遍，
  往返测试再逐字节对账。

## 实车必验清单（van_wire 的未定项，到货后逐条确认）

1. **FCS 约定**：公开描述是"CRC-15、覆盖 IDEN+CMD+DATA、多项式
   x^15+x^14+x^10+x^8+x^7+x^4+x^3+1"，但用 VanAnalyzer readme 的 5 帧真实
   导出做全枚举（32768 个多项式 × 6 种字段拆解 × 3 种位序变换 × 2 种字节序）
   都复现不出其 FCS。到货后抓一帧原始位流重新定位。
2. **字节内位序**：现按 MSB-first（与 VanAnalyzer 实现一致）。
3. **SOF 槽数**：规范写 10 TS（0000111101），VanAnalyzer 按 8 TS 整字节处理；
   本实现取后者（能与公开真实抓包对得上）。
4. **IDEN 宽度**：线上两个字节只承载 **12 位** IDEN + 4 位 CMD。
   规范字段是 15 TS（bit12..14 落在 SOF 区域内），当前布局没有它们的
   位置 —— 结构里保留了 15 位 iden，但字节编解码只搬运 12 位。
   公开样例是 0x8C4，本项目假设 206 实车为 0x824 —— 必须实测。
5. **ACK 位与帧尾**：解码器**不再**用"8 个连续 recessive"当帧尾
   （帧内合法数据会带出 8 个连续 recessive，实测 8A 22 5A 这帧就有一段，
   照它收尾会在帧中途截断）。现在帧的完整性**由 FCS 校验判定**，
   总线空闲只负责复位。实车要确认 ACK 位行为（`Stats::frames` 与
   `ackDominant()` 可观测）。

## 构建环境备忘

- 本机用便携 zig 作宿主机编译器（`.tools/zigbin` 转发桩）；zig 有全局缓存
  （`%LOCALAPPDATA%\zig`），出现"改了代码但行为不变"时先清它。
- PlatformIO 的 native 测试产物在 `.pio/build/native`，怀疑构建陈旧时整目录删掉重来。
- 工作副本在中文路径（`C:\Users\张九思\206Dash`），**构建必须在纯 ASCII 副本
  （`C:\Users\Public\206dash\Neru-s-206-dashboard`）上做**，两侧用 robocopy 同步。
  中文路径下 ESP32 工具链会报 `g++ Invalid argument`。
- **`tools/theme-editor/test-image-roundtrip.ps1` 必须存成带 BOM 的 UTF-8**。
  Windows PowerShell 5.1 读没有 BOM 的 .ps1 会按 GBK 解释，脚本里的中文
  会变成乱码进而**破坏字符串引号配对**，报
  `Missing argument in parameter list`。用 `edit`/`write` 改完这个文件后要补 BOM：
  `[System.IO.File]::WriteAllBytes($p, [byte[]](0xEF,0xBB,0xBF) + $bytes)`
- 同理**别用 `Get-Content` 去看 UTF-8 文件**：PS 5.1 按 GBK 读，显示乱码
  但文件本身是好的（这次差点因此去"修"一个没坏的 manifest）。
  要确认内容请用 read 工具或 `[System.Text.Encoding]::UTF8.GetString()`。

## 相关文档

- `PURCHASE.md`：采购清单（含到货后的验证动作、安全提示）
- `PINOUT.md`：引脚预案 + **「接线两段」**（VAN 到车上的接法、极性待定）
- `tools/theme-editor/README.md`：两个编辑器（主题 / 图片）的用法、
  二进制格式、分区偏移、限制
- `ARCHITECTURE.md`：模块划分与数据流
