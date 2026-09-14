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
- data_service：字段级多源合并与超时回退（3 秒无新数据回退下一源，行车中断线不黑屏）
  优先级：车速 Van > Sim；转速/水温 Obd > Van > Sim；油量/挡位 Sim
- obd_source：K 线 ELM327 非阻塞状态机，轮流请求 PID 010C/0105
- van_source：VAN 帧解析。车速/转速帧 IDEN 0x824（BSI→仪表，7 字节）：
  data[0..1]=转速×8，data[2..3]=车速×100 km/h（大端）
  依据 morcibacsi/psa_van_bus_packet_descriptions（307 实测、206 适用，实车需验证）
  物理层（SN65HVD230 + VanBus 库）接好后把帧喂给 onPacket()
- sim_source：无硬件时的默认假数据源

界面（已定，LVGL v9）：
- 无传统指针：外圈圆弧进度条承载车速/转速/水温，表盘中间放角色表情
- 左屏（原车速位）：车速弧 + 水温弧 + 表情；右屏（原转速位）：转速弧 + 表情
- ui_theme.h：所有颜色/弧角/半径/量程/表情占位参数集中于此，换皮只改这个文件
- 表情由 expression.cpp 状态机驱动（idle/blink/cruise/sport/redline/surprise）
- 占位表情为形状组合，换角色图时替换 face_apply() 为 lv_image 加载图片
- dash_display.h/.cpp：显示驱动接口，当前为无屏桩（DASH_DISPLAY_STUB=1），
  屏到货后填 ST7701S 实驱动即可，UI 层不动
- 开机动画（boot_anim.h/.cpp，纯时间逻辑）：淡入 → 双屏错峰扫表 → 表情睁眼眨眼；
  时长常量在 ui_theme.h（BOOT_*），按主循环频率刷、不受 5Hz 渲染节流；
  正常渲染带弧值缓动（kArcSmooth），开机→真实数据无缝过渡

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

构建注意：
- ESP32 工具链（MinGW g++）不支持非 ASCII 构建路径：中文路径下报
  g++ "Invalid argument" / "Arduino.h: No such file or directory"
- 已验证：项目 + Core 放纯 ASCII 路径（C:\Users\Public\206dash）编译通过
- 本机 PlatformIO Core 装在工作区 .pio-pylibs（pip --target），用
  python -m platformio 调用，PLATFORMIO_CORE_DIR 指向 ASCII 路径的 .pio-core
