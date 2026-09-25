// 测试程序入口:汇总注册各测试文件的用例。
// pio 会把 test_dashcore/ 下所有 .cpp 编进同一个可执行文件,main 只有一个。
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

void register_obd_protocol_tests(void);
void register_obd_source_tests(void);
void register_van_source_tests(void);
void register_van_replay_tests(void);
void register_van_wire_tests(void);
void register_van_phy_wire_tests(void);
void register_van_real_capture_tests(void);
void register_theme_store_tests(void);
void register_image_blob_tests(void);
void register_image_roundtrip_tests(void);
void register_data_service_tests(void);
void register_expression_tests(void);
void register_van_fields_tests(void);   // 已解字段接进数据层:灯位/门/VIN + 不抢既有优先级(2026-09-24)
void register_alerts_tests(void);       // 告警层:去抖/最短重复间隔/静音 + 蜂鸣器抽象(2026-09-24)
void register_ui_lamp_tests(void);      // 指示灯槽位几何 + 预览注入语义(2026-09-24)
void register_system_status_tests(void); // 系统状态层:数据不可信提示 + 诊断页(2026-09-24)
void register_buzzer_exio_tests(void);   // 真机蜂鸣器驱动:EXIO8 掩码/降级/夹时长/不阻塞(2026-09-24)
void register_panel_guard_tests(void);   // 面板健康守护:回读对账/按影子重写/背光/2秒周期/自愈(2026-09-24)
void register_boot_persist_tests(void);  // 跨重启留档:上一次 reason/上一次跑了多久/守护累计(2026-09-25)
void register_serial_cmd_tests(void);    // 串口单字符命令的判据层:d/m/b/r + 别多吃回放字符(2026-09-24)
void register_role_layout_tests(void);   // 角色 ↔ 屏幕的映射:哪块板显示哪一屏(2026-09-25)
void register_face_stage_tests(void);
void register_log_ring_tests(void);            // 日志环 + 节流闸门(2026-09-26)
void register_link_crc_coverage_tests(void);   // 双板链路 v1:CRC-15 检错覆盖枚举(§8 L4)
void register_link_frame_tests(void);          // 双板链路 v1:帧层(§2)—— 布局/帧长/CRC 覆盖/拒绝路径
void register_link_msg_tests(void);            // 双板链路 v1:消息载荷(§3)—— 逐字节打包/量纲/钳制
void register_link_time_tests(void);           // 双板链路 v1:时基(§4)—— tick 生成/偏移估计/三级超时
void register_link_phy_tests(void);            // 双板链路 v1:传输层(§1/§2 重同步)—— 假 PHY/非阻塞收发
void register_link_phy_uart_tests(void);       // 双板链路 v1:真实 UART 的接线口径(§0/§1.1)—— 43 发/44 收/回环脚
void register_link_app_tests(void);            // 双板链路 v1:数据接线(§1.2③/§3/§5)—— DATA 打包/节奏/第五档 Link

int main(void) {
  UNITY_BEGIN();
  register_obd_protocol_tests();
  register_obd_source_tests();
  register_van_source_tests();
  register_van_replay_tests();
  register_van_wire_tests();
  register_van_phy_wire_tests();
  register_van_real_capture_tests();
  register_theme_store_tests();
  register_image_blob_tests();
  register_image_roundtrip_tests();
  register_data_service_tests();
  register_expression_tests();
  // ★ 必须排在 face_stages 之前:test_face_stages.cpp 有一个**既有崩溃**
  //   (进程 exit 3,见 ACCEPTANCE.md),排在它后面的用例根本跑不到。
  register_link_crc_coverage_tests();
  register_link_frame_tests();
  register_link_msg_tests();
  register_link_time_tests();
  register_link_phy_tests();
  register_link_phy_uart_tests();
  register_link_app_tests();
  // ★ 2026-09-24 新增两组:已解字段(灯位/门/VIN)与告警层。
  //   排在 face_stages 之前(与上面那段的理由一致:face_stages 历史上崩过一次,
  //   排在它后面的用例跑不到 —— 崩溃虽已修掉,这条顺序纪律照旧保留)。
  register_van_fields_tests();
  register_alerts_tests();
  register_ui_lamp_tests();
  // ★ 2026-09-24 新增：系统状态层（"数据不可信"提示 + 诊断页字段映射）。
  //   同样排在 face_stages 之前（顺序纪律见上）。
  register_system_status_tests();
  // ★ 2026-09-24 新增：真机蜂鸣器驱动（TCA9554 的 EXIO8）—— 掩码只动目标位 /
  //   `Long` 降级 3 短哔 / 单次 ≤ 300ms / 静音不写寄存器 / 到点自动关。
  //   排在 face_stages 之前（顺序纪律见上）。
  register_buzzer_exio_tests();
  // ★ 2026-09-24 新增：面板健康守护（PanelGuard）—— 仪表盘必须常亮。
  //   它守的是"那颗 TCA9554 的输出寄存器（LCD_RST/LCD_CS 就在里面）与影子寄存器
  //   一不一致"以及"背光占空还对不对"；判据/时钟/输出全是注入的 ⇒ 宿主机可测。
  //   同样排在 face_stages 之前（顺序纪律见上）。
  register_panel_guard_tests();
  // ★ 2026-09-25 新增：**跨重启留档**（BootPersist）—— 起因是"累计启动次数 31 → 32
  //   多了一次，而上一次的复位原因没有留档 ⇒ 判不出那次是断电还是板子自己掉电"。
  //   它把三样东西写进 NVS 并在开机那一行一起打：上一次的 reason、**上一次运行了多久**
  //   （靠 10 分钟一次的心跳）、守护四个计数的跨重启累计。判据/NVS 全是注入的 ⇒ 宿主机可测。
  //   同样排在 face_stages 之前（顺序纪律见上）。
  register_boot_persist_tests();
  // ★ 2026-09-24 新增：串口单字符命令的判据层（`d`/`m`/`b`/`r`）。
  //   抽出来的理由是"这一层原来是 `src/main.cpp` 里的代码，而 native 只编 lib/"
  //   （见 `serial_cmd.h` 的文件头）；这一个口上同时跑日志与回放帧 ⇒
  //   "多认一个字符"会吃掉回放、"少认一个"会变成"按了没反应"，两个方向都要钉。
  register_serial_cmd_tests();
  // ★ 2026-09-25 新增：**角色 ↔ 屏幕的映射**（`dash_role_layout.h`）。
  //   起因是车主原话"副表怎么也给你刷成速度表了" —— 新板（从板镜像）上电显示的是
  //   速度表，而它该显示转速表 + 水温。这条映射以前**只存在于开机标签的文案里**
  //   （`MASTER (RIGHT)` / `SLAVE (LEFT)`），没有一行判据、也没有文档 ⇒ 本单把它
  //   写成代码 + 文档，并在这里逐条钉住（含"两块板**不是**同一屏"这条反向判据）。
  register_role_layout_tests();
  // ★ 2026-09-26 新增：**日志环 + 节流闸门**（`lib/dashcore/dash_log.h`）。
  //   这是本单那条硬约定的判据层："主循环永不因日志阻塞"——
  //   ①环满整行丢+计数 ②排空按预算、端口报满就立刻停手（不重试）
  //   ③丢弃/挡住都可观测。同样排在 face_stages 之前（顺序纪律见上）。
  register_log_ring_tests();
  register_face_stage_tests();
  return UNITY_END();
}
