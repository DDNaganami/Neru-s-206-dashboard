# Neru's 206 Dashboard

一个 206 CC 车主自己搓的双屏仪表。

车是 2001 年 11 月的 206 CC MUX，自动挡，白底积家。原表仓里塞两块圆屏：左边转速 + 水温，右边车速 + 进气温度，中间用表情代替指针。原表继续通电，挡位和警告灯仍由它负责，这套屏只多看一眼车在干什么。

不是量产件，也不是「通用安卓中控」。总线是 **K 线 + VAN（AEE2001）**，不是 CAN。配色和表情图放在 flash 分区里，换皮肤不用重编固件。

## 两块表怎么分
左屏 · 转速表              右屏 · 速度表
外圈 转速弧（缺口朝下）      外圈 车速弧（缺口朝下）
内圈 水温弧（缺口朝上、镜像）  内圈 进气弧（缺口朝上、镜像）
表情只看转速                表情只看车速
怠速 / 巡航 / 运动 / 高转 / 红区
静止 / 市区 / 快速路 / 高速 / 超速（>130）
text水温、进气只驱动弧和数字，不换脸。表盘刻度按实车：转速到 7000，车速到 210。红区按断油 6300 提前到 5800，和刻度上限不是同一件事。

## 数据从哪来

| 字段     | 第一路                    | 兜底                  |
| -------- | ------------------------- | --------------------- |
| 转速     | 有线 ELM327，PID `010C`   | VAN（有的话）→ 假数据 |
| 水温     | `0105`                    | 假数据                |
| 进气温度 | `010F`（这台 ECU 确实给） | 假数据                |
| 车速     | VAN 广播（目标）          | OBD `010D` → 假数据   |

K 线是排队共享的窄管子：转速 / 车速每轮都问，温度每 4 轮问一次。哪个源超过 3 秒不更新，就退到下一档，行车中拔线不会黑屏。

VAN **不在 OBD 座上**。线号 9004 / 9005，优先从组合仪表插头取（反正要拆表），收音机 7 针和 MFD 也能接到同一对。收发器用 SN65HVD230，板上 120Ω 必须拆掉。

手机蓝牙 ELM327 只用来上车前体检（地库里松刹车，车速立刻跟着变，不是 GPS）。S3 没有经典蓝牙 SPP，车上那条通道仍是 USB 有线 ELM 接串口。

## 现在做到哪

已经能跑：

- 假数据驱动的双屏 UI，宿主机预览（`pcpreview` 落 BMP）
- 运行时主题 / 图片分区，换配色和表情不用重编译
- 网页编辑器：`tools/theme-editor/`
- K 线协议栈 + 按 ECU 位图决定要不要问车速
- VAN 线路层：SOF、4B5B、关帧；实车抓波后槽时间定为 **8.25µs（≈121 kbit/s）**，不是纸面上的 8.00µs
- 真帧已经能收下（FCS 按实测定案）；车速在 **0x824** 那一帧的 `data[2]`（单字节，1 计数 = 1 km/h），绝对刻度先按 1.0 上屏、用表盘脸的档位复核，不再要求定速跑
- 验证板：微雪 DualEye，两块 1.28" 240×240 GC9A01A（`esp32s3-spi`）。最终想放进表仓的是更大的圆屏，几何按 `THEME_DISPLAY_RES` 缩放

还没做完、也没假装做完：

- 有线 ELM 接到板上的实车刷新率（看串口 `SRC-Hz`）
- 车速字段 km/h 的**绝对刻度**（先按 1 计数 = 1 km/h 上屏，有 OBD `010D` 散点时再定；字段位置和停车为 0 这两条已闭环）
- VAN 上的转速 / 油量 / 门灯
- 表仓里的最终屏和结构件

细节、踩坑和验收证据在 [ACCEPTANCE.md](ACCEPTANCE.md)。分层和「为什么这么做」在 [ARCHITECTURE.md](ARCHITECTURE.md)。接线在 [PINOUT.md](PINOUT.md)。购物和「别买 CAN-only」在 [PURCHASE.md](PURCHASE.md)。

## 仓库怎么读
lib/dashcore/         纯逻辑：OBD / VAN / 表情 / 数据合并（宿主机可单测）
lib/themetool/        主题 JSON、图片镜像格式
src/                  上电、LVGL、屏驱动
tools/theme-editor/   配色页 + 图片打包
tools/van-decode/     逻辑分析仪 CSV → 槽时间 / 切帧
tools/serial-capture/ 串口抓 VAN 日志、回放
text编译目标：

| 环境          | 用途                               |
| ------------- | ---------------------------------- |
| `native`      | 单元测试                           |
| `pcpreview`   | 本机渲双屏 BMP                     |
| `esp32s3`     | S3 桩显示，先把串口 / VAN 跑通     |
| `esp32s3-spi` | DualEye 真屏                       |
| `esp32s3-rgb` | RGB 并口骨架（最终大屏备用）       |
| `esp32dev`    | 经典 ESP32，4MB，廉价回归          |

## 本机先看一眼

需要 PlatformIO 和 Node。

```bash
python -m platformio test -e native
python -m platformio run -e pcpreview -t exec
# Ctrl+C 之后用浏览器打开 preview/preview.html

node tools/theme-editor/test-face-stages.js
node tools/theme-editor/test-theme-json.js
node tools/theme-editor/test-gauge-geometry.js
node tools/theme-editor/test-image-blob-build.js
主题刷到 0x210000，图片刷到 0x254000。经典板 image 分区 1MB，S3 那份表是 8MB，偏移相同。编辑器里先选对目标板，否则 1MB 闸门会把 S3 装得下的图拦住。表情画布不要超过 320：再大会盖住内圈副弧。
上车之前

先充电宝供电，再接车 12V
认 VAN：两根对地电压明显不相等，且都不是 0V / 12V（这台车上大约 4V / 1V）
S3 原生 USB 口开监视器必须把 RTS/DTR 关掉，否则芯片会被按在复位态
逻辑分析仪抓 RX 时用 4MHz；导出 CSV 不要把时间列截成整微秒，8.25µs 的槽会被量成 8.00

这是给这台车、这个表仓写的。别的 206 MUX 可以当参考，引脚、帧和标度都以实车为准。
