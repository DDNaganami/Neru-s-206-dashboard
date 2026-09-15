# 引脚（预案）

ESP32 型号：ESP32-S3 N16R8（硬约束：真屏双 480×480 全缓冲必须
S3 + PSRAM，esp32dev 320KB RAM 只够跑桩，见 ARCHITECTURE.md；
N16R8 八线 PSRAM 走 flash 总线，GPIO33-37 仍可用）
屏型号：（待定）

以下为按「双 SPI 屏 + K 线 OBD + VAN 监听」预分配的引脚，未实测；
接好线后把实际值更新回这张表，并同步 main.cpp 里的串口初始化。

功能            GPIO   说明
------------    ----   ------------------------------------------------
屏 SPI SCK      36     两屏共用一条 SPI 总线
屏 SPI MOSI     35
屏 SPI MISO     37     多数屏只写不读，可省
左屏 CS         4
左屏 DC         5
左屏 RST        6
左屏背光       7      PWM
右屏 CS         8
右屏 DC         9
右屏 RST        10
右屏背光       11     PWM
K 线 OBD RX     18     UART1 ← ELM327 TX（38400 8N1，Serial1）
K 线 OBD TX     17     UART1 → ELM327 RX
VAN RX          16     UART2 ← SN65HVD230 RO
VAN TX          15     UART2 → SN65HVD230 DI（纯监听可不接）
VAN DE/RE       GND    SN65HVD230 收发使能：纯监听直接接地（或拆掉）

约束：
- 避开 strapping 脚（GPIO0/3/45/46）、flash 脚（GPIO26-32）、USB 脚（19/20）。
- 上表按 N16R8 排的 SPI；若买 N16R2（四线 PSRAM 占 GPIO33-37），
  SPI 需整体挪到 5-11 区段之外的空闲脚。
- SN65HVD230 已拆 120Ω 终端电阻（见 ARCHITECTURE.md），RO/DI 为 3.3V 电平，
  与 ESP32 直连。
- 屏模块（如 T-RGB 2.8）到货后以模块丝印为准，本表只是占位。
