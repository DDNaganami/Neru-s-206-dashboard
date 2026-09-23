#pragma once
#include <stdint.h>
// 只为 VIN 的定长缓冲常量(kVanVinChars = 17):**不重复写 17 这个数字**,
// 免得 van_source 那头改了长度这里悄悄错位。van_source.h 只 include <stdint.h>,
// 不含本文件,所以没有循环包含。
#include "van_source.h"

enum class Gear : uint8_t {
  P, R, N, D, M3, M2, M1
};

struct VehicleState {
  float speed_kmh = 0;
  float rpm = 0;
  float coolant_c = 20;
  // 进气温度(OBD PID 010F)。2026-09 用户用蓝牙 ELM327 + EOBD 实测:
  // 206 CC 的发动机 ECU 支持这一项,所以把它做成速度表上的副表
  // (与转速表上的水温表对称)。它是**慢变量**、不参与表情(和冷却液一样),
  // 只驱动一条弧 + 一个数字;缺数据时按字段独立回退(见 data_service)。
  float intake_c = 20;
  float fuel_pct = 75;
  Gear gear = Gear::P;  // 屏不显示挡位（原表负责），字段保留给将来逻辑
  bool ign = true;

  // ============================================================
  // ★★ 2026-09-24 新增:VAN 上**已实测解出**、但一直没接进数据层的四类字段。
  //
  // 为什么全部**只有 VAN 一个来源**(⇒ data_service 里标注恒为 `Van`,不与
  // Obd/Sim/Link 抢字段):这四类在 K 线 OBD 上**根本没有对应的 PID**
  // (转向灯/灯位/门/VIN 都不是 OBD-II 标准项),所以不存在"第二来源",
  // 也就不存在优先级问题 —— 这一条是"没动既有优先级语义"的根据,不是省略。
  //
  // 为什么这些字段**不参与表情**:表情只看车速与转速(见 expression.cpp);
  // 它们是**指示灯层**(dash_ui 的灯槽位)与**告警层**(alerts)的输入。
  // ============================================================

  // 灯位(0x4FC 的 data[5] 位域,§4.3/§4.4)。**这是"最近一次解出的值"**,
  // 不是"现在亮着"—— 转向灯在闪,0x4FC 又只有 4.7 帧/秒(欠采样),
  // 所以"现在亮不亮"必须过保持窗口,判据在 VanSource::lightsRecent()。
  // 上层(UI / alerts)一律用 `vind.left_on` 这一组**已经过保持窗口**的结果。
  bool indicator_left = false;    // 左转向灯(bit2 = 0x04),已过保持窗口
  bool indicator_right = false;   // 右转向灯(bit3 = 0x08),已过保持窗口
  bool hazard = false;            // 双闪(bit2|bit3 = 0x0C),已过保持窗口
  bool position_lamp = false;     // 仪表盘灯(bit7 = 0x80,灯杆第 1 档)
  bool low_beam = false;          // 近光(bit6 = 0x40,灯杆第 2 档)

  // 门信号:**只有"动过没有"**,没有"哪扇门 / 开着还是关着"。
  //   左右门可分辨性 = **未解**(§6 撤回①:找不到能区分左右的字节);
  //   "==1 就是门开着"被 §4.6 明确否掉(脉冲段内还在 00↔01 跳变)。
  //   ⇒ 名字就叫 activity,别改成 door_open —— 那会把"未解"说成"已解"。
  bool door_activity = false;

  // VIN(0xE24,17 字节明文 ASCII 广播,§4.7)。固定 18 字节(17 + '\0'),
  // 与 VanSource 的缓冲同一个口径;没收到过就是空串(不是"未知"占位符)。
  char vin[kVanVinChars + 1] = {0};
};

static constexpr float kSpeedMax = 210.0f;

// 转速表**表盘刻度上限**(用户 2026-09-18 确认:上限 7000 转)。
// ★ 这个数是"弧画满时对应多少转",不是发动机的物理极限 ——
//   ui_model 用它算 rpm_t(弧的填充比例),所以必须等于表盘刻度上限。
//   ★ 这里踩过一次反向的坑(值得记住):本文件原来就是 7000,2026-09 被"改成
//   6000"并写着"实车实测上限 6000" —— 那个前提是错的。后果与"弧填不满"相反:
//   **6000 转以上弧就不动了**(真实 7000 转时弧早已满格),而且红区判定
//   (`kRpmRedlineFrom`)也跟着按 6000 的刻度推。用户 2026-09-18 更正:表盘到 7000。
//   教训:表盘量程这种事要问**看表的人**,别从"发动机断油 6500"倒推。
//   换表/换量程时改这里,表情的红区阈值另有一套(kRpmRedlineFrom),别混。
static constexpr float kRpmMax = 7000.0f;

// 实车转速地标(用户提供,用来定表情分档):
//   点火怠速 900 / 稳定巡航 2000 / 表盘上限 7000
//   (断油点 6300,用户实测 2026-09-18 —— 红区阈值按它定,见 expression.cpp)
static constexpr float kRpmIdleNominal   = 900.0f;
static constexpr float kRpmCruiseNominal = 2000.0f;
