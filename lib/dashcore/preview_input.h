#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>   // strtof

// ============================================================
// pcpreview 的**输入注入**（2026-09-24 新增）
//
// ★ 这个头文件为什么放在 `lib/dashcore/`（而 IO 那半在 `src/preview_input.cpp`）:
//   native 用例要能 include 它（`[env:native]` 的包含路径里有 lib/dashcore）。
//   于是"注入的**语义**"（哪几个通道、怎么互相覆盖、控制文件的语法）在宿主机上
//   逐条测掉，而"读终端 / 读文件"那一层只在 pcpreview 里编 —— 这个分工与
//   `van_source.h`（纯解包，native 可测）↔ `van_phy_gpio.cpp`（真收帧，只进
//   固件）是同一套。
//   ★ 所以两个纯函数（`preview_apply_key` / `preview_apply_control_text`）
//     直接是**这个头文件里的 inline 定义**：pcpreview 与 native 用的是
//     逐字节同一份实现，而且 native 不需要多链接任何一个 .cpp
//     （`lib_archive = no` 的 native 构建**只编 lib/** —— 把纯函数留在
//      `src/` 的 .cpp 里就会变成两个未定义符号，这是踩过的）。
//
// 为什么需要它：宿主机预览原来只有一个"假数据源"（`sim_source` 按慢波扫表），
// 也就是**只能看**能不能画出来 —— 而这一轮要验的是"指示灯与告警**对输入的
// 反应**"（打灯、开门、超速、红区），那就必须能**手动喂输入**。
//
// 两条注入路径（都只存在于 `env:pcpreview`，设备固件里一行都没有）：
//   ① **键盘**：渲染窗口一开就能用，改完**当帧**生效（无需重启、无需文件）。
//   ② **控制文件** `preview/inject.txt`：每帧读一次（有则读、无则跳过），
//      里面 `键=值` 一行一条。用途是**可复现**：命令行/脚本/报告里贴一段文本
//      就能把界面摆到某个状态（而按键没法写进文档里一步一步复核）。
//
// 用法（键 → 含义）:
//   ←  / →      左 / 右转向灯            空格   双闪
//   L           近光                     P      仪表盘灯
//   D           门（"动过"，不是"门开着"）
//   O           超速（车速 = 130）        R      红区（转速 = 6000）
//   M           静音开关（蜂鸣器）
//   X  /  Esc   全部复位（回到假数据）
//
// 控制文件同一套键名（数值版，便于精确摆位）:
//   left=1 right=1 hazard=1 low_beam=1 position=1 door=1 mute=1
//   speed=140 rpm=6000 clear=1
//
// ★ 语义三条（这是"注入"最容易搞混的地方，写清楚）:
//   ① 注入**优先于**假数据与 VAN/OBD：只要 `speed` / `rpm` 被注入过，
//      就用注入值（不再被 sim 的慢波覆盖）。这**只发生在 pcpreview**。
//   ② 转向灯/双闪/门这几个注入位**直接写进 VehicleState**，不走
//      `0x4FC` 的解包路径，也不吃 600 ms 保持窗口 —— 因为注入的意图就是
//      "我要它一直亮着"（保持窗口是给**欠采样的真实帧**用的，不是给注入用的）。
//   ③ 注入**不产生任何协议行为**：不造 VAN 帧、不碰 `kSpeedScale`、
//      不改 `data_service` 的优先级。它只在 main 的快照上覆写几个字段。
// ============================================================

enum class PreviewKey : uint8_t {
  None = 0,
  Left, Right, Hazard, LowBeam, PositionLamp, Door, Overspeed, Redline, Mute, Clear
};

// 一份"手动注入的快照"。
// ★ 每个通道都有一个"设过没有"的旗标（`*_set`）—— 否则"用户按了右键"与
//   "从来没按过"没法区分，注入就会把其他状态一起冲掉。
struct PreviewInput {
  bool left = false, right = false, hazard = false;
  bool low_beam = false, position = false, door = false;
  bool left_set = false, right_set = false, hazard_set = false;
  bool low_beam_set = false, position_set = false, door_set = false;
  bool speed_set = false, rpm_set = false;
  float speed_kmh = 0.0f;
  float rpm = 0.0f;
  bool mute = false;          // 蜂鸣器静音（这个没有 *_set：默认不静音）
  bool any() const {
    return left_set || right_set || hazard_set || low_beam_set ||
           position_set || door_set || speed_set || rpm_set;
  }
};

// ------------------------------------------------------------
// 纯函数部分①：键 → 注入
// ★ 超速/红区注入的是**具体数值**（不是布尔）：告警判据是"车速 >= 阈值"，
//   所以得把速度真的顶上去，才看得见整条告警链的反应（迟滞、重复间隔、
//   屏上闪烁）。130 / 6000 分别高于 AlertsConfig 的默认阈值（120 / 5800），
//   test_ui_lamps.cpp 有一条用例把"必须高于阈值"钉住。
// ------------------------------------------------------------
inline bool preview_apply_key(PreviewInput& in, PreviewKey k) {
  switch (k) {
    case PreviewKey::Left:      in.left = !in.left;         in.left_set = true;      break;
    case PreviewKey::Right:     in.right = !in.right;       in.right_set = true;     break;
    case PreviewKey::Hazard:    in.hazard = !in.hazard;     in.hazard_set = true;    break;
    case PreviewKey::LowBeam:   in.low_beam = !in.low_beam; in.low_beam_set = true;  break;
    case PreviewKey::PositionLamp:
      in.position = !in.position; in.position_set = true; break;
    case PreviewKey::Door:      in.door = !in.door;         in.door_set = true;      break;
    case PreviewKey::Overspeed: in.speed_kmh = 130.0f; in.speed_set = true; break;
    case PreviewKey::Redline:   in.rpm = 6000.0f;      in.rpm_set = true;   break;
    case PreviewKey::Mute:      in.mute = !in.mute;                            break;
    case PreviewKey::Clear:
      in = PreviewInput{};      // 全部复位（含各 *_set 旗标）
      break;
    default:
      return false;
  }
  return true;
}

namespace preview_ctl {

// 大小写不敏感比较（纯 C++，不用 strcasecmp —— 那个在 MSVC/新标准里名字不定）
inline bool eqIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == '\0' && *b == '\0';
}

inline bool truthy(const char* v) {
  return eqIgnoreCase(v, "1") || eqIgnoreCase(v, "true") || eqIgnoreCase(v, "on") ||
         eqIgnoreCase(v, "yes");
}

// 取 "键" 与 "值"（都 trim 过；不做动态分配）
inline void splitKV(const char* line, char* key, size_t kcap, char* val, size_t vcap) {
  size_t i = 0;
  while (line[i] == ' ' || line[i] == '\t') ++i;
  size_t n = 0;
  while (line[i] && line[i] != '=' && line[i] != ' ' && line[i] != '\t' && n + 1 < kcap) {
    key[n++] = line[i++];
  }
  key[n] = '\0';
  while (line[i] == ' ' || line[i] == '\t') ++i;
  if (line[i] == '=') ++i;
  while (line[i] == ' ' || line[i] == '\t') ++i;
  n = 0;
  while (line[i] && line[i] != '\n' && line[i] != '\r' && line[i] != ' ' &&
         line[i] != '\t' && n + 1 < vcap) {
    val[n++] = line[i++];
  }
  val[n] = '\0';
}

}  // namespace preview_ctl

// ------------------------------------------------------------
// 纯函数部分②：控制文件文本 → 注入
//
// 语法：一行一条 `键=值`；`#` 开头（或行尾）是注释；空行跳过；键名大小写不敏感。
// 值：0/1（也认 true/on/yes）、或一个浮点数（speed / rpm）。
// 认不出的键**不报错也不静默**：返回值里只算认得的那几条（调用方据此打一行摘要）。
// ------------------------------------------------------------
inline int preview_apply_control_text(PreviewInput& in, const char* text) {
  using namespace preview_ctl;
  if (text == nullptr) return 0;
  int n = 0;
  const char* p = text;
  while (*p) {
    const char* eol = p;
    while (*eol && *eol != '\n') ++eol;
    char line[96];
    size_t len = (size_t)(eol - p);
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    p = (*eol == '\n') ? eol + 1 : eol;

    char* hash = strchr(line, '#');
    if (hash) *hash = '\0';
    char key[32], val[32];
    splitKV(line, key, sizeof(key), val, sizeof(val));
    if (key[0] == '\0') continue;

    if (eqIgnoreCase(key, "left"))          { in.left = truthy(val);     in.left_set = true; n++; }
    else if (eqIgnoreCase(key, "right"))    { in.right = truthy(val);    in.right_set = true; n++; }
    else if (eqIgnoreCase(key, "hazard"))   { in.hazard = truthy(val);   in.hazard_set = true; n++; }
    else if (eqIgnoreCase(key, "low_beam")) { in.low_beam = truthy(val); in.low_beam_set = true; n++; }
    else if (eqIgnoreCase(key, "position")) { in.position = truthy(val); in.position_set = true; n++; }
    else if (eqIgnoreCase(key, "door"))     { in.door = truthy(val);     in.door_set = true; n++; }
    else if (eqIgnoreCase(key, "mute"))     { in.mute = truthy(val);     n++; }
    else if (eqIgnoreCase(key, "speed"))    { in.speed_kmh = strtof(val, nullptr); in.speed_set = true; n++; }
    else if (eqIgnoreCase(key, "rpm"))      { in.rpm = strtof(val, nullptr);       in.rpm_set = true; n++; }
    else if (eqIgnoreCase(key, "clear"))    { if (truthy(val)) { in = PreviewInput{}; n++; } }
    // 其它键：不认（返回值里不算它，调用方据此打一行提示）
  }
  return n;
}

// ------------------------------------------------------------
// IO 部分（**只在 pcpreview 里编**，实现见 src/preview_input.cpp）
// ------------------------------------------------------------
// 初始化：键盘钩子 + 控制文件路径（不传就用 preview/inject.txt）。
// 只在 pcpreview 构建里有实现；重复调用只初始化一次。
void preview_input_begin(const char* ctl_path);

// 每轮主循环调一次：把这一轮攒下的按键、以及控制文件的内容合并进 `in`。
// 返回 true = 这一轮**有输入被处理**（主循环用它决定要不要打一行回执）。
bool preview_input_poll(PreviewInput& in);
