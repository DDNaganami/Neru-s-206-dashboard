# 206-dash

车型：2001-11 206 CC MUX，自动挡，白底积家
目标：原表仓加屏，原表可继续通电当备份

显示分工（已定）：
- 屏负责：车速、转速、水温（油量待定）
- 原表负责：挡位显示（屏不重复做）；警告灯是否屏上复刻，待定

数据源（已定）：
- 转速 / 水温：有线 K 线 OBD（ELM327，ISO 9141/KWP2000，PID 010C/0105）
  注意：206 是 K 线 + VAN（AEE2001）架构，不是 CAN，别买 CAN-only 适配器
- 车速：DIY 读 VAN 总线（SN65HVD230 模块 + VanBus 库，ESP32 直连）
  VAN 在收音机 ISO A 座 2/3 脚；SN65HVD230 需拆掉 120Ω 终端电阻
  帧 ID 参考 pinterpeti.hu / graham.auld.me.uk / EEVblog 206 帖
- 不采购 V2C TINY 协议盒（它是给换新主机用的；除非将来换 RD45 主机）

阶段：
第一阶段：假数据驱动 UI
第二阶段：有线 OBD（K 线）
第三阶段：VAN 车速

软件分层（已定）：
- VehicleState：全项目唯一的状态结构
- 纯逻辑层在 lib/dashcore/（无 Arduino/LVGL 依赖，宿主机可直接单测）；
  src/ 只留平台与 UI（main、dash_display、dash_ui、boot_anim、ui_theme）
- data_service：字段级多源合并与超时回退（3 秒无新数据回退下一源，行车中断线不黑屏）
  优先级：车速 Van > Sim；转速/水温 Obd > Van > Sim；油量/挡位 Sim
  转速/水温按字段独立计时：一个 PID 死了不影响另一个字段继续用 OBD
- obd_source：K 线 ELM327 非阻塞状态机，轮流请求 PID 010C/0105（0105 响应为单字节 A-40）；
  文本解析在 obd_protocol.h/.cpp（纯函数）；坏帧值域钳制（转速≤9000、水温-40..215）
- van_source：VAN 帧解析。车速/转速帧 IDEN 0x824（BSI→仪表，7 字节）：
  data[0..1]=转速×8，data[2..3]=车速×100 km/h（大端）
  依据 morcibacsi/psa_van_bus_packet_descriptions（307 实测、206 适用，实车需验证）
  坏帧值域钳制（车速≤300、转速≤9000）
- van_phy.h：VAN 物理层接缝（接口 + VanPhyStub）。当前 main 用桩占位；
  实驱动（SN65HVD230 + VanBus 库 / RMT）到货后实现 VanPhy，把解出的帧
  喂给 VanSource::onPacket()，数据层不动
- sim_source：无硬件时的默认假数据源

界面（已定，LVGL v9）：
- 无传统指针：外圈圆弧进度条承载车速/转速/水温，表盘中间放角色表情
- 左屏（原车速位）：车速弧 + 水温弧 + 表情；右屏（原转速位）：转速弧 + 表情
- ui_theme.h：所有颜色/弧角/半径/量程/表情占位参数集中于此，换皮只改这个文件
  （表盘几何角度也统一在这里，原 gauge_geometry.h 已删除）
- 分辨率适配：所有尺寸按 480×480 基准书写，渲染统一乘 theme_scale()；
  换屏只改 THEME_DISPLAY_RES 一行（3.4" 圆屏 800×800 → 填 800），
  桩驱动的显示宽高也读这个常量，两处永远一致
- 表情由 expression.cpp 状态机驱动（idle/blink/cruise/sport/redline/surprise）；
  只读车速/转速，不依赖挡位（挡位留原表不进屏）
- 急加速惊喜阈值按时间归一：>15 km/h/s（≈0.42g）触发，与渲染帧率无关
- 占位表情为形状组合，换角色图时替换 face_apply() 为 lv_image 加载图片
- dash_display.h/.cpp：显示驱动接口，当前为无屏桩（DASH_DISPLAY_STUB=1）；
  非桩分支是 #error——只删宏会编译失败，防止上电黑屏；屏到货后
  先锁分辨率、写实驱动、换 S3+PSRAM，最后才删宏
- 开机动画（boot_anim.h/.cpp，纯时间逻辑）：淡入 → 双屏错峰扫表 → 表情睁眼眨眼；
  时长常量在 ui_theme.h（BOOT_*），按主循环频率刷、不受 5Hz 渲染节流；
  正常渲染带弧值缓动（kArcSmoothPerSec=1.44/s 指数趋近，按实际时间算，
  渲染频率变化不改变手感），开机→真实数据无缝过渡

机械尺寸（实测，2026-09）：
- 大表盘孔内径 Ø89 mm；台阶外沿 Ø100 mm（台阶宽 5.5 mm）
- 两表圆心距 95 mm → 两个 Ø100 台阶重叠 5 mm，遮罩环需做成 8 字形合并件
- 表盘处面板背面到后盖 22 mm，结构紧凑（基本不可用）
- 表壳内部：上部大半圆弧空腔最高 135 mm、向两侧收拢至 38 mm；下部均匀 25 mm 深 → 驱动板/PCB 放这些区域
- 装屏方案（已定）：拆转速/水温/车速三根指针，表盘纸不动，屏嵌在表玻与表盘纸之间、盖住两大表盘；油表与挡位显示保留原表
- 选屏硬约束：可视直径 ≤ Ø89；两屏外径 < 95（圆心距）；总厚度 ≤ 表玻-表盘纸间隙（待量）
- 2.8" 圆屏（可视 Ø70.7）留环 9.2 mm/边；3.4" 圆屏（可视 Ø86.4）留 1.3 mm/边
- LilyGO T-RGB 2.8" 整板 Ø96 > 95 且厚 18 mm，排除
- 待量：表玻到表盘纸间隙（表盘中心/上缘/下缘三点）、表玻曲率
- 平台硬约束（已定）：真屏双 480×480（或 800×800）全缓冲必须
  ESP32-S3 + PSRAM（建议 N16R8）；esp32dev 只有 320KB RAM，
  只用于桩开发/逻辑联调（现在靠 480×10 窄条缓冲才活得下来）。
  采购清单按 S3 列，别按 DevKit 买完线才发现画不动。

构建注意：
- ESP32 工具链（MinGW g++）不支持非 ASCII 构建路径：中文路径下报
  g++ "Invalid argument" / "Arduino.h: No such file or directory"
- 已验证：项目 + Core 放纯 ASCII 路径（C:\Users\Public\206dash）编译通过
- 本机 PlatformIO Core 装在工作区 .pio-pylibs（pip --target），用
  python -m platformio 调用，PLATFORMIO_CORE_DIR 指向 ASCII 路径的 .pio-core

测试（宿主机，不烧板）：
- test/test_dashcore/ 为 native 单元测试（Unity），覆盖 OBD 文本解析、
  OBD 状态机（假串口）、VAN 解析与钳制、多源回退、表情状态机
- 跑法（纯 ASCII 路径下，env 变量同上）：
    python -m platformio test -e native
- 本机没有宿主机 gcc：装了便携 zig（pip ziglang，见 .tools\pyzig），
  并用 .tools\zigbin 里的 gcc/g++/cc/c++ 转发桩调用 zig cc/c++
  （源码 .tools\zigwrap.c）。跑测试前把 zigbin 加进 PATH。
  有真 gcc 的机器不需要转发桩，直接跑 pio test 即可。
- 测试用 Arduino 最小桩在 test/arduino_shim/，只进测试编译，不进固件
- 接线预案见 PINOUT.md（ESP32-S3 双 SPI 屏 + OBD UART1 + VAN UART2）
