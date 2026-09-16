#pragma once
#include <stdint.h>
#include "vehicle_state.h"

// ============================================================
// 表情状态机
//
// 每个状态对应**一张可导入的差分图**(左屏 8 张 / 右屏 8 张,见 image_blob.h)。
// 枚举顺序就是表情图的槽位顺序 —— dash_ui 直接用 (int)Face 当下标,
// 所以**只能在 Count 之前追加**,不能插队(会让已导入的图错位)。
//
// 状态从哪来?三路数据各有一条"低/中/高"分档,优先级从高到低:
//   惊喜(急加速,瞬时 400ms) > 红区(转速) > 过热(水温) > 冷车(水温)
//   > 运动(速度或转速高) > 巡航(速度或转速中) > 常态
// 分档阈值集中在 expression.cpp 顶部,阶段用例表在 face_stages.h,
// 两边由 test_face_stages.cpp 钉住 —— 改阈值必须同步改那张表。
// ============================================================
enum class Face : uint8_t {
  Idle = 0,     // 常态(转速/速度都低、水温正常)
  Blink,        // 眨眼(常态下的周期性动作)
  Cruise,       // 巡航(速度 >= 30 或转速 >= 2500)
  Sport,        // 运动(速度 >= 90 或转速 >= 4500)
  Redline,      // 红区(转速 >= 6000)
  Surprise,     // 惊喜(急加速,瞬时)
  Cold,         // 冷车(水温 < 70:暖机中)
  Hot,          // 过热(水温 >= 105)
  Count
};

struct FaceState {
  Face face;
  Face base;       // surprise/blink 结束后回到谁
  uint32_t until_ms;
};
Face face_update(const VehicleState& s, uint32_t now);
const char* face_name(Face f);

// 清空状态机的记忆(上一次速度/时刻/瞬态截止时间/眨眼计时)。
// 用途:换数据源(模拟↔OBD)时避免把"两个源之间的速度差"误判成急加速;
// 宿主机测试里逐条跑阶段用例时也必须先清,否则上一条用例的速度会串进加速度。
void face_reset();
