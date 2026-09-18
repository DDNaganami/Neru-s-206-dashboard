# 206-dash

车型：2001-11 206 CC MUX，自动挡，白底积家
目标：原表仓加屏，原表可继续通电当备份

显示分工（已定）：
- 屏负责：车速、转速、水温（油量待定）
- 原表负责：挡位显示（屏不重复做）；警告灯是否屏上复刻，待定

数据源（已定）：
- 转速 / 水温 / 进气温度：有线 K 线 OBD（ELM327，ISO 9141/KWP2000，PID 010C/0105/010F）
  注意：206 是 K 线 + VAN（AEE2001）架构，不是 CAN，别买 CAN-only 适配器
  ★ 2026-09-17 用**蓝牙** ELM327 + 手机 App（EOBD）实测确认：这台车的 ECU
  确实回 010C / 0105 / **010F**（进气温度）。蓝牙版本身不能给 ESP32 用
  （S3 只有 BLE，没有蓝牙经典/SPP），它是"上车前先体检一次"的工具；
  车上那条通道仍是 USB 有线版接 `Serial1` —— 见 PURCHASE.md。
- 供电：OBD 座 16 脚（+12V，**常电**）+ 4/5 脚（GND）
- 车速：DIY 读 VAN 总线（SN65HVD230 模块 + 自研 van_wire 线路层，ESP32 直连）
  ⚠️ **OBD 座上没有 VAN**（只有 K 线 + 12V + GND）。VAN 落点（线号 9004/9005）：
    组合仪表连接器 5/10 脚（首选，插接件免破线，反正要拆仪表）
    收音机黑色 7 针插头 2/3 脚（备选）
    MFD 18 针 4/17 脚（备选）
  认线特征（MUX，对车 GND）：9004 ≈ 4V、9005 ≈ 1V、差分 ≈ 3V
  极性（哪根对应 DATA / DATA BAR）**尚未实测确认**，接反就两根对调
  SN65HVD230 需拆掉 120Ω 终端电阻
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
- obd_source：K 线 ELM327 非阻塞状态机，**轮流请求 PID 010C/0105/010F**
  （后两个响应是单字节 A-40）；
  ★ 一次问答在 ISO 9141-2 上要 50~100ms，三路轮一圈约 0.7~1.4s ——
  也就是**转速刷新率被降到 ≈0.7~1.4Hz**（加水温那一路时就已经是这样，
  加进气温度只是多占一格）。要提速就把两条慢弧降频，别砍 PID。
  文本解析在 obd_protocol.h/.cpp（纯函数，**表驱动**，加 PID 只动那张表）；
  坏帧值域钳制（转速≤9000、温度 -40..215）
- van_source：VAN 帧解析。车速/转速帧 IDEN 0x824（BSI→仪表，7 字节）：
  data[0..1]=转速×8，data[2..3]=车速×100 km/h（大端）
  依据 morcibacsi/psa_van_bus_packet_descriptions（307 实测、206 适用，实车需验证）
  坏帧值域钳制（车速≤300、转速≤9000）
- van_phy.h：VAN 物理层接缝（接口 + VanPhyStub）。当前 main 用桩占位；
  实驱动（SN65HVD230 + VanBus 库 / RMT）到货后实现 VanPhy，把解出的帧
  喂给 VanSource::onPacket()，数据层不动
- van_wire.h/.cpp：VAN **线路层**（已实现,纯逻辑,宿主机可测）——
  SOF 识别 → 4B5B（E-Manchester,每 5 槽丢弃第 5 位）→ 字节流 →
  IDEN/CMD 拆分 → CRC-15 校验 → 帧尾判据。含编码器（测试/仿真用）
  和 BitDecoder（喂边沿时间戳→出字节,带字节回调与队列两条路径）。
  关键设计:字节走**回调**(ByteSink)而不是等 drain 队列 —— 一个边沿区间
  内可能同时含数据字节和帧尾,等区间处理完再取会丢字节。
  帧间空隙用**超时重同步**(默认 1ms)处理:总线空闲可达几十毫秒,
  逐槽展开会把相位冲垮。
  未定项（实车必验,见 ACCEPTANCE.md）：FCS 约定、字节位序、SOF 槽数、
  206 实车 IDEN、ACK 位对帧尾判据的影响
- van_phy_wire.h/.cpp：线路层接到 VanPhy 上的边缘解码器（**整链已回归**）。
  硬件侧只需在 RO 脚电平变化时调 `onEdge(t_us, level)`,不碰任何寄存器。
  **职责三分（改这里之前先看完这条）**：
  ① 帧起点：`BitDecoder` 匹配到 SOF 的 10 个裸槽后，经
     `ByteSink::onFrameStart()` 回调 → relay 调 `FrameParser::beginFrame()`。
     必须是回调而不是"边沿返回后再判断"：SOF 命中与后续数据字节可能落在
     **同一次 `pushEdge`** 里，返回时字节早吐出去了。
  ② 帧尾：**只有 `finish()` 能关帧**。`onEdge()` 一律不准调 `endFrame()` ——
     边沿路径抢收会在缓冲已被消费成空之后把帧收掉，字节再也救不回来
     （实测症状 `finish 前: frames=1 pending=0 bytes=0`）。
     `finish()` 以 `FrameParser::inFrame()` 为准，不看解码器相位。
  ③ SOF 是**固定 10 TS 同步图案 `0000111101`**，不是 4B5B 数据字节：
     编码器写裸槽（`putSof()`），解码器按同一常数滑窗匹配。
     别用 `putByte(0x0F)` 生成它（会得到 `0000111111`，差第 9 槽），
     也别拿"折 10 槽 == 0x0F"当它正确的证据 —— 按规范折出来是 **0x0E**。
- van_phy.h 的 VanSink/VanSourceSink:物理层只依赖 VanSink 接口,
  数据源用 VanSourceSink 转一层(main.cpp 已接)。
- sim_source：无硬件时的默认假数据源

界面（已定，LVGL v9）：
- 无传统指针：外圈圆弧进度条承载车速/转速/水温，表盘中间放角色表情
- **屏与表的对应按法系车来（标致 206 实车）**：
  **左屏 = 转速表（带水温表）**，**右屏 = 速度表（带进气温度表）**。
  别按"左车速右转速"的日德习惯改回去。
  左屏：转速弧（外）+ 水温弧（内）+ 表情；
  右屏：车速弧（外）+ **进气温度弧**（内）+ 表情。
  **两块表左右对称**：两条副弧（水温 / 进气温度）几何完全相同
  （0→180、radius 168、width 10、`reverse=1`），只有屏号与颜色不同
  （水温绿 `0x7CFF6B`、进气琥珀 `0xFFB020`）—— 这条由
  `test-gauge-geometry.js` 的「进气温度弧 = 水温弧的镜像版」逐字段钉住。
  **外圈弧开口朝下、内圈副弧开口朝上**（产品决定，用户要求）：
  转速/车速弧 135→405（拱在上方），副弧 0→180（兜在下方）—— 一眼分得清。
  **涨幅方向也相反**：外圈弧从 start 端起涨；副弧 `reverse=1`
  （从 end 端起涨 = 镜像），因为"下方半圆"默认只会从右端开始亮，
  而水温/进气温度表该从左端起涨。
  建屏 / 开机扫表 / 正常渲染三处都走 `arc_set_progress()` —— 固定哪一端只在那一个地方决定。
  ★ **副表的定义集中在 `dash_ui.cpp` 的 `is_aux_kind()`**（水温或进气温度）：
  "大数字跟哪条弧"= 该屏第一条**非副表**的弧。加进气温度时如果只写
  `!= Coolant`，一条 `[Intake, Speed]` 顺序的屏会让速度表的大数字变成进气温度。
  另注：主题里的 `radius` 是弧的**外沿**半径（LVGL 把弧画在盒子内、`width` 往里长），
  预览若按"带宽居中在 radius"画就会整体外移半个带宽（实测外圈差 12 像素）。
- ui_theme.h：所有颜色/弧角/半径/量程/表情占位参数集中于此
- **主题是运行时可换的**（`lib/themetool/`）：`g_theme_ptr` 指向"闪存里的默认值"
  或"从 theme 分区读来的主题"，改配色只需重刷 16KB 的 theme 分区，固件不动。
  关键取舍：默认主题必须是 **const 静态**，不能是可写全局 —— 可写全局会让
  LVGL 无法把样式对象折叠进 rodata，一次多吃约 36KB DRAM，直接把
  esp32dev 撑爆（实测 `dram0_0_seg overflowed by 8872 bytes`）。
  因此所有主题宏都是**转发宏**（`#define THEME_BG_COLOR (g_theme.bg_color)`），
  换主题时不需要重新编译任何消费方代码。
  解析失败一律**回退默认值继续跑**，绝不让主题问题导致黑屏。
- **图片资源也是运行时可换的**（`lib/themetool/image_blob.*`）：图片放在
  `image` 分区，用 `esp_partition_mmap` 只读映射，再组装成 `lv_image_dsc_t`
  交给 LVGL。选 `LV_IMAGE_SRC_VARIABLE` 而不是 `LV_IMAGE_SRC_FILE` 的理由：
  前者**不复制像素**（后者会），所以 mmap 指过去不额外吃 DRAM ——
  这对只有 320KB RAM 的 esp32dev 是硬要求。
  mmap 的 handle 存成 static 且永不 unmap：一旦被回收，那块虚拟地址会被
  别的映射复用，LVGL 就会读到别人的数据。
  空分区（全 0xFF）和任何坏镜像都**拒收并降级**，同样是"资源出问题不能让固件崩"。
  角色编号：1 = 背景，**保留编号（一律不复用）** 2（开机帧）/ 5（左屏惊喜）/
  7（右屏红区）/ 9、10（开机图）/ 11、16（眨眼图）/ 14、15、19、20（冷车、过热图），
  在用的 8 个是 **左屏 3=常态 4=红区 12=巡航 13=运动**、
  **右屏 6=常态 8=超速 17=巡航 18=运动**（8 号当年是"惊喜"，只改名不改号）。
  复用保留编号会让老 image.bin 里那几张图突然变成别的表情，而且不报错 ——
  **新角色从 21 开始接**。
  表情图用 RGB565A8（带 alpha 平面，能叠在弧上），背景用 RGB565。
  存储预算：一张 240×240 表情 RGB565A8 = 169KB，一张 480×480 背景 RGB565 = 450KB。
  8 张表情 + 一张 240 背景 = 1.46MB 仍然超 1MB，**降到 192×192 就是
  8×108+72 = 936KB —— 能装下了**。这也是"把状态数压到每屏 4 个"的直接原因。
  换 S3（16MB flash）时把 image 分区放大到 4MB 以上即可，分区表在 partitions.csv。
- ui_theme.h 原先把颜色/几何写死在编译期；现在这些宏转发到运行时指针，
  `theme_clamp()` 统一钳制值域（越界值不会画到屏外或触发 LVGL 断言）
- 分辨率适配：所有尺寸按 480×480 基准书写，渲染统一乘 theme_scale()；
  换屏只改 THEME_DISPLAY_RES 一行（3.4" 圆屏 800×800 → 填 800），
  **这一项仍是编译期**（不是运行时），因为缓冲区大小要静态分配；
  桩驱动的显示宽高也读这个常量，两处永远一致
- 表情由 face_stages.h + expression.cpp 状态机驱动，**每屏一套、各看各的表**：
  · **左屏（转速表）只看转速**：常态(<1800) / 巡航(≥1800) / 运动(≥3500) / 红区(≥5000)
    三档阈值按**实车地标**推（用户实测：点火怠速 900、稳定巡航 2000、表盘上限 6000）：
    1800 包住怠速 900 并给起步留余量；巡航 2000 稳稳落在巡航档；
    红区在上限前 1000 转开始报红（83% 刻度）—— 不是"踩到断油才亮"。
  · **右屏（速度表）只看车速**：常态(<30) / 巡航(≥30) / 运动(≥90) / 超速(>130)
    车速四档的前三个是 城区/快速路/高速 取 30/90（还没有实车数据）；
    第 4 档超速是用户定的 130（进入 >130、退出 ≤127 有迟滞，
    免得定速巡航压在 130 上下时脸来回跳）。
    ★ 这一档原来叫"惊喜"（急加速 > 15 km/h/s 的 400ms 瞬态），已改掉：
    它不是按车速分的档，所以阶段模拟里点不出来；而且 15 km/h/s ≈ 4.2 m/s²
    对 206 CC 自动挡（0-100 约 11s）基本到不了，那张图导进去永远不亮。
    改成超速之后，车速组和转速组一样是 **4 档 4 个状态**，
    每个状态都能在模拟器里点出来 —— 这也是用户报"这个档选不出来"的修法。
  · **表盘刻度上限 `kRpmMax = 6000`**（vehicle_state.h）与红区阈值刻意分开：
    弧画满的位置看表盘，表情报红的位置看发动机。两者都随实车数据走。
  · **水温不参与表情**：它只驱动水温弧与水温数字。
    理由：一套水温表情要再加 4 张图（冷车/过热 × 两屏），而 image 分区只有 1MB；
    而且水温是慢变量，屏幕下方本来就有弧和数字，再让脸跟着变信息重复、
    还容易误读（看到"冷车脸"会以为车有问题，其实只是刚启动）。
  · `face_update()` 返回 `FaceSet{left, right}`；`Face` 枚举是**两屏共用的档位名**，
    哪一屏有哪些状态由 `kFaceLeftStates` / `kFaceRightStates` 明确列出，
    并有测试断言"列出的状态必须有图、没列出的必须没有"。
  · 稳态**只由数据决定，与时间无关**（`test_time_independent` 连跑 20 秒断言不变）。
    历史：曾经有过"眨眼 Blink"状态，因为只跟时间有关、说不出什么工况会触发，
    已被用户否掉并删除（编号 11/16 保留不复用）。
- 阶段表贯穿固件与网页：`lib/dashcore/face_stages.h` 是唯一事实来源，
  11 条用例（转速 4 档 + 车速 4 档 + 水温 3 档），**每行同时给出左右两屏的期望**，
  外加"缺图降级链"与"槽位→角色编号"两张表。
  两组硬规则都有两端测试钉住：
  ① 每组只变自己那一维（点速度档不能动转速表）；
  ② 只有该屏自己的那一路能改它的表情（转速组的右屏恒为常态、速度组的左屏恒为常态、
     水温组两屏都不动）。
- 表情状态机现在是**纯数据驱动、与时间无关**：眨眼删了、惊喜（瞬态）改成
  超速（稳态）之后，`face_update()` 里只剩一个"超速迟滞位"，
  `now` 参数保留但不用（有单测钉住"表情与时刻无关"）。
- **表盘数字读数**（转速/速度大数字 + 单位 + 水温数字）：
  · 大数字放表盘正上方（48 号），单位紧跟其下（18 号），水温数字在**转速表底部**
  · 大数字跟哪一路**由弧决定**（该屏第一条非水温弧），不看屏幕序号 ——
    以后把水温弧挪屏、或加第三条弧，读数自动跟着走
  · 格式写死在固件里：速度取整到 1 km/h、转速取整到 10 rpm、水温取整到 1 ℃；
    可调的是颜色/字号/位置/开关（主题里的 `readout` 段）
  · 竖直可用区间只有 59..120（上面是**外弧带 35..59**——`radius` 205 是外沿、
    `width` 24 往内长，不是旧文档里的 23..47；下面是表情图顶边 120），
    所以默认位置是 digit_cy=72 / unit_cy=107 —— 这条由单测
    test_readout_defaults_fit_gap 与预览帧的墨迹外框共同钉住
    （实测 48 号数字墨迹顶在 y=55，蹭进弧带 4 像素，刻意接受：读数画在弧之上）
  · 开机扫表期间数字栏是**空的**（标签以空文本创建，读数只在实际渲染时写入）；
    刻意不做淡入：给对象设 opa<255 会让 LVGL 开离屏层，这个驱动上会错位
- 表情/背景图片：dash_ui 里按"背景图(最底) → 弧 → 表情图 → 读数"的创建顺序叠层；
  **缺图有降级链**（face_stages.h 的 kFaceFallback）：只导入常态一张也能跑，
  运动缺图退巡航、超速缺图退运动…… 一张都没有才回落到程序化占位表情。
  左右屏各查各的：两套差分图不能互相顶替，否则角色会串
- dash_ui.cpp 的 faceResolve() 是固件端唯一的"状态→图片"解析入口，
  网页端 face-stages.js 的 resolve() 是它的镜像，两边由
  tools/theme-editor/test-face-stages.js 解析 face_stages.h 逐字段对账
- dash_display.h/.cpp：显示驱动接口，当前为无屏桩（DASH_DISPLAY_STUB=1）；
  非桩分支是 #error——只删宏会编译失败，防止上电黑屏；屏到货后
  先锁分辨率、写实驱动、换 S3+PSRAM，最后才删宏
- 开机动画（boot_anim.h/.cpp，纯时间逻辑）：淡入 → 双屏错峰扫表 → 表情显形；
  时长常量在 ui_theme.h（BOOT_*），按主循环频率刷、不受 5Hz 渲染节流；
  正常渲染带弧值缓动（kArcSmoothPerSec=1.44/s 指数趋近，按实际时间算，
  渲染频率变化不改变手感），开机→真实数据无缝过渡。
  总时长 = `boot_face_start_ms + boot_face_blink_ms`（第二个字段名是历史遗留，
  现在是"显形后的一拍收尾"；以前是 +4 拍眨眼，眨眼删除后那 4 拍纯属白等，
  而开机期间 dash_ui_render 会早退、根本不显示真实数据）。

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
- **`-I include` 不能省**（esp32dev 的 build_flags）：LVGL 找 lv_conf.h 靠的是
  包含路径（`LV_CONF_INCLUDE_SIMPLE` → `#include "lv_conf.h"`）。少了这一条，
  `include/lv_conf.h` 根本不会被读，一切走 LVGL 内部默认值 ——
  而且**不报错**：14 号字体恰好是 LVGL 的默认值，看起来像"生效了"，
  直到要开 18/48 号字体时才炸出 `lv_font_montserrat_48 was not declared`。
  加上之后 RAM 反而从 37.5% 降到 32.7%（lv_conf.h 里 LV_MEM_SIZE=48KB
  比 LVGL 默认的 64KB 小），Flash 从 51.3% 升到 62.6%（两套字体点阵）。
- **分区表 CSV 必须纯 ASCII**（`partitions.csv` / `partitions-s3.csv`）：
  PlatformIO 的 `builder/main.py` 用 `open(partitions_csv)` **不指定编码**读它，
  中文 Windows 上就是 GBK 解码 —— 注释里一个"★"（UTF-8 = E2 98 85）足以让
  构建在最后一步 `checkprogsize` 抛 UnicodeDecodeError。症状极具误导性：
  固件**已经编译并链接成功**（"Successfully created … image"），
  看着却像编译失败。BOM 也不行（第一个字段会读成空）。
  由 `tools/theme-editor/test-image-blob-build.js` 的一条守卫盯着。
- **四个构建目标**（2026-09-18 起）：
  | env | 板子 | 分区表 | 用途 |
  |---|---|---|---|
  | `esp32dev` | 经典 ESP32（4MB，无 PSRAM） | `partitions.csv` | 廉价回归；**没有** PSRAM/双屏 |
  | `esp32s3` | **ESP32-S3 N16R8**（16MB + 8MB OPI PSRAM） | `partitions-s3.csv` | **实车与真屏**；`-DVAN_PHY_GPIO=1` 开硬件收帧 |
  | `native` | 宿主机 | — | 单元测试（`pio test -e native`） |
  | `pcpreview` | 宿主机 | — | 渲染落帧做像素核对 |
  两份分区表的 theme/image **偏移相同**（0x210000 / 0x254000），
  所以 esptool 命令两块板通用；只有 image 的大小不同（1MB vs 8MB）。
  设备专属代码（`ESP.*`、`esp_partition.h`、GPIO 中断）一律用 `ARDUINO`
  判定圈起来 —— pcpreview 用的是宿主机桩，不定义 `ARDUINO`，
  少了这层判定预览构建会直接编不过（踩过）。

测试（宿主机，不烧板）：
- test/test_dashcore/ 为 native 单元测试（Unity，当前 95 例，2 例需环境变量否则跳过），
  覆盖 OBD 文本解析、OBD 状态机（假串口）、VAN 解析与钳制、VAN 线路层
  （4B5B/CRC-15/帧字节契约/空闲不入队）、**VanPhyWire 整链**（边沿→包→车速/转速）、
  15 位 IDEN 与回放语法、主题 JSON 解析与钳制（含数字读数那一段）、
  图片镜像解析（坏镜像必须被拒 + 头部字节布局钉死 + 角色编号与表情槽位对账）、
  多源回退、表情状态机（**每屏一套独立**：左看转速、右看车速、水温不参与 +
  瞬态只属于速度表 + 稳态与时间无关）、
  **表情阶段表**（10 条用例，逐条断言 face_update 对**左右两屏**的输出；
  检查降级链一定能退到常态；检查每组只变自己那一维、每屏只被自己那一路驱动 ——
  这两条都是用户试用时抓出来的：点"速度·中"时转速表跟着动过）
- 跑法（纯 ASCII 路径下，env 变量同上）：
    python -m platformio test -e native
- **网页端与固件的一致性**（四套 JS 镜像，Node 里跑）：
    node tools/theme-editor/test-face-stages.js     # 解析 face_stages.h 逐字段对账
    node tools/theme-editor/test-theme-json.js      # 主题文件的 0x 颜色能不能读回来
    node tools/theme-editor/test-gauge-geometry.js   # 表盘朝向 + 弧带半径语义(编辑器侧)
    node tools/theme-editor/test-image-blob-build.js
    node tools/theme-editor/syntax-check-pages.js   # 三个页面的内联脚本语法
  test-face-stages.js 的存在理由：表情导入页的"阶段模拟"是用户刷图前**唯一**
  能看到的证据；网页那份表和固件那份不一致，预览就是在骗人。
  test-theme-json.js 的存在理由：主题文件是**固件与编辑器共读**的同一个文件，
  固件接受 `0xRRGGBB` 而 `JSON.parse` 不接受 —— 少了这一层就是
  "固件读得进去、编辑器导入报语法错"（实测踩过）。
  test-gauge-geometry.js 的存在理由：LVGL 的 `lv_arc` 是 0°=3 点钟、顺时针，
  而 canvas 的 `arc()` 约定**完全一样**，所以角度不需要任何偏移；
  两个编辑器曾经都写成 `(d - 90)`，把预览里的表整块逆时针转了 90° ——
  固件是对的，但用户先看到预览，于是问"表的方向是不是要向右转 90 度"。
  这类"约定对不上"不报错、只是看着怪，只能靠断言钉住。
  同一条测试还管另外两件事：默认主题的**两条弧开口方向相反**（转速朝下、水温朝上），
  以及 `radius` 是**外沿**半径、预览必须画在 `radius - width/2`（固件实测
  radius 205/width 24 → 弧带 181..205；预览原来画在 193..217，整体外移 12 像素）。
  固件那一侧的朝向由 check-preview-frame.js 的「表盘朝向」检查兜着（读真实落帧）。
- **跨语言格式核对**（图片镜像由 JS 生成、C 读取，编译期看不出不一致）：
    pwsh tools/theme-editor/test-image-roundtrip.ps1
  它用 JS 打一个镜像并生成可读的 manifest，再让固件解析器读同一个文件、
  逐字段逐字节对账（C 侧用例见 test_image_roundtrip.cpp，未设环境变量时自动跳过）。
  这个核对实测抓出过两个真 bug，都是"编译通过、肉眼看不出来"的类型。
- **像素级验收**（真屏到货前唯一的"证据"）：
    # ASCII 路径下，先造测试图，再带 IMAGE_BLOB 跑预览
    node tools/theme-editor/make-test-blob.js <ascii>\test-image.bin
    $env:IMAGE_BLOB='<ascii>\test-image.bin'; .\.pio\build\pcpreview\program.exe
    node tools/theme-editor/check-preview-frame.js preview\frames\l_0140.bmp redline yes left yes
    node tools/theme-editor/check-preview-frame.js preview\frames\r_0140.bmp idle yes right yes
  （帧号别随手改：假数据是波形，先扫一遍两屏表情序列再挑**平台段**的帧 ——
   140 帧两侧都稳定，100 帧左屏已经是 cruise 了。）
  check-preview-frame.js 除了背景/弧/表情图层与透明通道，还核对**读数**：
  数字/单位/水温三处墨迹的**外接框**（能区分"画的是数字"和"忘了清空的
  LVGL 默认文本 Text"）、右屏不该有水温数字、开机期间数字栏必须为空。
- 本机没有宿主机 gcc：装了便携 zig（pip ziglang，见 .tools\pyzig），
  并用 .tools\zigbin 里的 gcc/g++/cc/c++ 转发桩调用 zig cc/c++
  （源码 .tools\zigwrap.c）。跑测试前把 zigbin 加进 PATH。
  有真 gcc 的机器不需要转发桩，直接跑 pio test 即可。
- 测试用 Arduino 最小桩在 test/arduino_shim/，只进测试编译，不进固件
- 接线预案见 PINOUT.md（ESP32-S3 双 SPI 屏 + OBD UART1 + VAN UART2）；
  **OBD 座上没有 VAN**，车速要从组合仪表（首选）/ 收音机 / MFD 取，
  极性问题尚未实测确认 —— 见 PINOUT.md「接线：三处取信号」
