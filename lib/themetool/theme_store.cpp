#include "theme_store.h"
#include <string.h>

// ★ 只为 `AlertsConfig` 的**定义**（theme_parse_alerts_json 要逐字段写它）。
//   头文件里只有前向声明 ⇒ 依赖被关在这一个 .cpp 里：
//   lib/themetool → lib/dashcore 的这**一个**方向，且只为"把文件里的那段读出来"。
//   `alerts.h` 本身只依赖 `vehicle_state.h`（纯结构体，无 Arduino/LVGL），
//   所以宿主机构建、pcpreview、设备构建三边都一样能编。
#include "alerts.h"

// ============================================================
// 极简 JSON 扫描器（只为读本项目的主题文件,不是通用 JSON 库）
//   - 支持:对象、数组、字符串、数字(十进制/0x 十六进制)
//   - 不支持:转义序列、Unicode、null（主题文件里用不到）
//   - 不认识的成员按值丢弃后继续,所以"多写了字段"不会解析失败
// ============================================================

namespace {

struct Scan {
  const char* p;
  const char* end;

  Scan(const char* s, uint32_t n) : p(s), end(s + n) {}

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
  }
  char peek() { ws(); return (p < end) ? *p : '\0'; }
  bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }

  // 数值:自己解析,不用 strtod。
  // newlib 的浮点解析会拉进好几 KB 的代码/表,在 esp32dev(320KB DRAM)上
  // 足以把 dram0_0_seg 顶爆(实测溢出 8.8KB)。主题里的数值只有
  // "可选的符号 + 整数/小数",手写足够。
  bool number(double* out) {
    ws();
    if (p >= end) return false;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }
    if (p >= end || *p < '0' || *p > '9') return false;

    double v = 0.0;
    while (p < end && *p >= '0' && *p <= '9') {
      v = v * 10.0 + (double)(*p - '0');
      ++p;
    }
    if (p < end && *p == '.') {
      ++p;
      double scale = 0.1;
      while (p < end && *p >= '0' && *p <= '9') {
        v += (double)(*p - '0') * scale;
        scale *= 0.1;
        ++p;
      }
    }
    *out = neg ? -v : v;
    return true;
  }

  // 整数（接受 0x 前缀的十六进制,颜色用）
  bool uintVal(uint32_t* out) {
    ws();
    if (p >= end) return false;
    const bool hex = ((end - p) >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
    const char* q = p + (hex ? 2 : 0);
    uint32_t v = 0;
    int digits = 0;
    while (q < end) {
      int d;
      const char c = *q;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else break;
      if (d >= (hex ? 16 : 10)) break;
      v = v * (hex ? 16u : 10u) + (uint32_t)d;
      ++q;
      ++digits;
    }
    if (digits == 0) return false;
    p = q;
    *out = v;
    return true;
  }

  // 布尔：**两种写法都收** —— `true`/`false`（人手写 JSON 的习惯）
  // 与 `1`/`0`（本仓库编辑器导出的一贯写法，与 readout.show_units 那些一致）。
  // ★ 为什么不只认数字：这一段是给人改的，"true 被静默忽略、字段保持默认"
  //   会让人以为"改了没生效"（而日志上什么都不会说）。
  bool boolean(bool* out) {
    ws();
    if (p >= end) return false;
    if ((size_t)(end - p) >= 4 && strncmp(p, "true", 4) == 0) { p += 4; *out = true; return true; }
    if ((size_t)(end - p) >= 5 && strncmp(p, "false", 5) == 0) { p += 5; *out = false; return true; }
    uint32_t u = 0;
    if (!uintVal(&u)) return false;
    *out = (u != 0u);
    return true;
  }

  // 跳过任意一个值（用于忽略不认识的成员）
  void skipValue() {
    ws();
    if (p >= end) return;
    const char c = *p;
    if (c == '"') {
      ++p;
      while (p < end && *p != '"') ++p;
      if (p < end) ++p;
    } else if (c == '{' || c == '[') {
      const char open = c, close = (c == '{') ? '}' : ']';
      int depth = 0;
      while (p < end) {
        if (*p == '"') { ++p; while (p < end && *p != '"') ++p; if (p < end) ++p; continue; }
        if (*p == open) ++depth;
        else if (*p == close) { --depth; if (depth == 0) { ++p; return; } }
        ++p;
      }
    } else {
      while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
    }
  }

  // 遍历对象成员:对每个成员调 fn(key 指针, key 长度)。
  // fn 返回 false 表示"不认识这个键",值会被跳过。
  //
  // ★★ 2026-09-27 修的一处**静默截断**（本文件里最容易踩、后果又最不像 bug 的一处）：
  //   原来只有 `if (!fn(...)) skipValue();` —— 于是"**认识这个键、但值不是它期望的类型**"
  //   这条路上，值**一个字节都没被消费**，扫描器就停在值中间；紧接着的
  //   `eat(',')` / `eat('}')` 都失败 ⇒ **整个对象从这里停止解析**，
  //   后面所有字段**静默按默认值处理**。
  //   实测症状（用例 test_alerts_unknown_keys_and_wrong_types 第一次就红了）：
  //     `{"alerts":{"overspeed_kmh":"ninety","redline_rpm":6500}}`
  //     → `redline_rpm` 读成 5500（默认值，★ 2026-10-15 从 5800 降到 5500），
  //       而日志上什么异常都没有。
  //   人写的文件里这种错很常见（把颜色写成字符串、把数字加了引号），
  //   表现却是"我改的主题只生效了一半" —— 所以在这里一次性堵住：
  //   **fn 声称认识这个键、却一个字节都没读 ⇒ 当成不认识，把值跳过继续。**
  //   ★ 这一处同时覆盖了子对象/数组那两处（`eat('{')`/`eat('[')` 失败时同样"没消费"）。
  template <typename Fn>
  void object(Fn fn) {
    if (!eat('{')) return;
    if (peek() == '}') { eat('}'); return; }
    for (;;) {
      ws();
      if (p >= end) return;
      if (!eat('"')) { ++p; continue; }
      const char* kbegin = p;
      while (p < end && *p != '"') ++p;
      const size_t klen = (size_t)(p - kbegin);
      if (p < end) ++p;
      if (!eat(':')) return;
      ws();
      const char* vbegin = p;
      const bool known = fn(kbegin, klen);
      if (!known || p == vbegin) skipValue();
      if (eat(',')) continue;
      eat('}');
      return;
    }
  }
};

bool keyIs(const char* k, size_t klen, const char* name) {
  return klen == strlen(name) && strncmp(k, name, klen) == 0;
}

// 读一个 ArcStyle 对象
void readArc(Scan& s, ArcStyle& a) {
  s.object([&](const char* k, size_t klen) -> bool {
    uint32_t u = 0; double d = 0;
    if (keyIs(k, klen, "kind")) {
      if (s.uintVal(&u)) {
        a.kind = (ArcKind)(u <= (uint32_t)ArcKind::Intake ? u : 0u);
      }
      return true;
    }
    if (keyIs(k, klen, "start_deg"))   { if (s.number(&d)) a.start_deg = (int32_t)d; return true; }
    if (keyIs(k, klen, "end_deg"))     { if (s.number(&d)) a.end_deg = (int32_t)d; return true; }
    if (keyIs(k, klen, "radius"))      { if (s.number(&d)) a.radius = (int32_t)d; return true; }
    if (keyIs(k, klen, "width"))       { if (s.number(&d)) a.width = (int32_t)d; return true; }
    if (keyIs(k, klen, "track_color")) { if (s.uintVal(&u)) a.track_color = lv_color_hex(u); return true; }
    if (keyIs(k, klen, "track_opa"))   { if (s.uintVal(&u)) a.track_opa = (uint8_t)u; return true; }
    if (keyIs(k, klen, "value_color")) { if (s.uintVal(&u)) a.value_color = lv_color_hex(u); return true; }
    // 涨幅方向:1 = 从 end 端起涨(镜像)。见 ui_theme.h 的 ArcStyle 说明。
    if (keyIs(k, klen, "reverse"))     { if (s.uintVal(&u)) a.reverse = (uint8_t)(u ? 1 : 0); return true; }
    return false;
  });
}

// 读一个 ScreenTheme 对象
void readScreen(Scan& s, ScreenTheme& sc) {
  s.object([&](const char* k, size_t klen) -> bool {
    uint32_t u = 0;
    // ★★ `arc_count` 在**解析处**就钳到 kMaxArcs。
    //   `(uint8_t)u` 只截位、不封顶：写 4 就是 4、写 300 会变成 44（300 & 0xFF）
    //   ⇒ 只截位的写法会把"荒唐输入"变成"看着合法的数字"。
    //   而这个数**是 `ScreenTheme::arcs[]` 的唯一上界**，下游 `dash_ui.cpp`
    //   拿它当循环上界（建弧 / 开机扫表 / 逐帧刷新三处）⇒ 放过去就是越界读
    //   `arcs[i]`（i ≥ kMaxArcs 那份根本没建/没解析）+ 把 nullptr 当弧对象交给 LVGL。
    //   ★ 两条独立防线，都要：这里是**入口**（外部文件），dash_ui 里还有一道
    //     （`ui.arc_count` 赋值处），theme_clamp() 也钳 —— 三条都在，代价是一次比较。
    if (keyIs(k, klen, "arc_count")) {
      if (s.uintVal(&u)) sc.arc_count = (uint8_t)(u > (uint32_t)kMaxArcs ? (uint32_t)kMaxArcs : u);
      return true;
    }
    if (keyIs(k, klen, "show_face")) { if (s.uintVal(&u)) sc.show_face = (uint8_t)(u ? 1 : 0); return true; }
    if (keyIs(k, klen, "arcs")) {
      if (!s.eat('[')) return true;
      uint8_t i = 0;
      if (s.peek() == ']') { s.eat(']'); return true; }
      for (;;) {
        if (i < kMaxArcs) readArc(s, sc.arcs[i]);
        else s.skipValue();
        ++i;
        if (s.eat(',')) continue;
        s.eat(']');
        return true;
      }
    }
    return false;
  });
}

// 数一下根对象的花括号是否配平（粗校验:挡住被截断的文件）
bool bracesBalanced(const char* s, uint32_t len) {
  int depth = 0;
  for (uint32_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c == '"') {                       // 跳过字符串
      ++i;
      while (i < len && s[i] != '"') ++i;
      continue;
    }
    if (c == '{') ++depth;
    else if (c == '}') { --depth; if (depth < 0) return false; }
  }
  return depth == 0;
}

}  // namespace

bool theme_parse_json(const char* json, uint32_t len, Theme& t) {
  if (!json || len < 2) return false;
  if (!bracesBalanced(json, len)) return false;   // 被截断/括号不配 → 拒收

  // 以默认值为底:缺失字段自动继承默认值（这是"字段可缺失"的实现方式）
  theme_set_defaults(t);

  // 顶层两种写法都接受:
  //   {"theme": { ...字段... }}   ← 编辑器导出的形式
  //   { ...字段... }              ← 手写时更省事
  // 找 "theme" 键:先定位字符串,再确认后面跟的是冒号（避免匹配到
  // 名字里含 theme 的其它键）,最后让扫描器吃掉冒号并把值当主题对象读。
  const char* at = json;
  uint32_t left = len;
  const char* hit = strstr(json, "\"theme\"");
  if (hit) {
    Scan probe(hit, (uint32_t)(len - (uint32_t)(hit - json)));
    probe.eat('"');
    // 走完 "theme" 这个词
    probe.p += 5;
    if (probe.eat('"') && probe.eat(':')) {
      at = probe.p;
      left = (uint32_t)(probe.end - probe.p);
    } else {
      at = json;   // 不是键（可能是字符串值里出现的）,按裸根处理
      left = len;
    }
  }

  Scan s(at, left);
  if (s.peek() != '{') return false;   // 值必须是对象

  s.object([&](const char* k, size_t klen) -> bool {
    uint32_t u = 0; double d = 0;
    if (keyIs(k, klen, "bg_color"))        { if (s.uintVal(&u)) t.bg_color = u; return true; }
    if (keyIs(k, klen, "face_size"))       { if (s.number(&d)) t.face_size = (int32_t)d; return true; }
    if (keyIs(k, klen, "face_bg_idle"))    { if (s.uintVal(&u)) t.face_bg_idle = u; return true; }
    if (keyIs(k, klen, "face_bg_redline")) { if (s.uintVal(&u)) t.face_bg_redline = u; return true; }
    if (keyIs(k, klen, "face_ink"))        { if (s.uintVal(&u)) t.face_ink = u; return true; }
    // ★★ 2026-10-11: 量程键（原先只有结构体默认值 60~130 / 0~80，主题里根本没法调 ——
    //   编辑器那份"水温量程"是 face-stages.js 里的**硬编码副本**，所以只有水温、且改了也不生效）。
    //   加进这里之后 theme.json 才能真正决定量程，两个量的写法逐字对称。
    if (keyIs(k, klen, "coolant_min_c"))   { if (s.number(&d)) t.coolant_min_c = (float)d; return true; }
    if (keyIs(k, klen, "coolant_max_c"))   { if (s.number(&d)) t.coolant_max_c = (float)d; return true; }
    if (keyIs(k, klen, "intake_min_c"))    { if (s.number(&d)) t.intake_min_c  = (float)d; return true; }
    if (keyIs(k, klen, "intake_max_c"))    { if (s.number(&d)) t.intake_max_c  = (float)d; return true; }
    // ★ 2026-10-14：这四条原先又抄了一遍（与上面四条逐字相同的死代码 —— 前面已 return true，永远执行不到）。行为上无害，只删。
    if (keyIs(k, klen, "boot_fade_ms"))        { if (s.uintVal(&u)) t.boot_fade_ms = u; return true; }
    if (keyIs(k, klen, "boot_sweep_start_ms")) { if (s.uintVal(&u)) t.boot_sweep_start_ms = u; return true; }
    if (keyIs(k, klen, "boot_stagger_ms"))     { if (s.uintVal(&u)) t.boot_stagger_ms = u; return true; }
    if (keyIs(k, klen, "boot_sweep_rise_ms"))  { if (s.uintVal(&u)) t.boot_sweep_rise_ms = u; return true; }
    if (keyIs(k, klen, "boot_sweep_hold_ms"))  { if (s.uintVal(&u)) t.boot_sweep_hold_ms = u; return true; }
    if (keyIs(k, klen, "boot_sweep_fall_ms"))  { if (s.uintVal(&u)) t.boot_sweep_fall_ms = u; return true; }
    if (keyIs(k, klen, "boot_face_start_ms"))  { if (s.uintVal(&u)) t.boot_face_start_ms = u; return true; }
    if (keyIs(k, klen, "boot_face_blink_ms"))  { if (s.uintVal(&u)) t.boot_face_blink_ms = u; return true; }
    if (keyIs(k, klen, "face")) {
      s.object([&](const char* fk, size_t fkl) -> bool {
        uint32_t v = 0;
        if (keyIs(fk, fkl, "eye_l_x"))      { if (s.uintVal(&v)) t.eye_l_x = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "eye_r_x"))      { if (s.uintVal(&v)) t.eye_r_x = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "eye_y"))        { if (s.uintVal(&v)) t.eye_y = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "eye_normal_w")) { if (s.uintVal(&v)) t.eye_normal_w = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "eye_normal_h")) { if (s.uintVal(&v)) t.eye_normal_h = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "eye_surprise")) { if (s.uintVal(&v)) t.eye_surprise = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "eye_narrow_h")) { if (s.uintVal(&v)) t.eye_narrow_h = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_line_x")) { if (s.uintVal(&v)) t.mouth_line_x = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_line_y")) { if (s.uintVal(&v)) t.mouth_line_y = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_line_w")) { if (s.uintVal(&v)) t.mouth_line_w = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_line_h")) { if (s.uintVal(&v)) t.mouth_line_h = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_o_x"))    { if (s.uintVal(&v)) t.mouth_o_x = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_o_y"))    { if (s.uintVal(&v)) t.mouth_o_y = (uint8_t)v; return true; }
        if (keyIs(fk, fkl, "mouth_o_size")) { if (s.uintVal(&v)) t.mouth_o_size = (uint8_t)v; return true; }
        return false;
      });
      return true;
    }
    if (keyIs(k, klen, "readout")) {
      // ★★ 2026-10-15（主题格式修订 R2）：`rpm_color` / `speed_color` 是**新增键**
      //   （车主："转速/速度现在就拆开"）。两条纪律：
      //     ① **缺键回落 digit_color** —— 旧主题文件（只有 digit_color）必须照旧；
      //     ② 回落取的是**文件里那一份** digit_color，不是编译期默认 ——
      //        老主题把 digit_color 改成过别的颜色（例如 0x00FF88），
      //        回落后两屏必须还是那个颜色，否则拆分就悄悄改了老主题的画面。
      //   ⇒ 用两个旗子记住"这个键到底出现过没有"，等**整个 readout 对象读完**再回落：
      //     JSON 对象成员的顺序没有语义，digit_color 完全可能写在新键**后面**，
      //     在读到 rpm_color 的那一刻取 digit_color 会取到还没被覆盖的默认值。
      bool has_rpm_color = false, has_speed_color = false;
      s.object([&](const char* rk, size_t rkl) -> bool {
        if (keyIs(rk, rkl, "digit_color"))   { if (s.uintVal(&u)) t.readout.digit_color = u; return true; }
        if (keyIs(rk, rkl, "rpm_color"))     { if (s.uintVal(&u)) { t.readout.rpm_color = u; has_rpm_color = true; } return true; }
        if (keyIs(rk, rkl, "speed_color"))   { if (s.uintVal(&u)) { t.readout.speed_color = u; has_speed_color = true; } return true; }
        if (keyIs(rk, rkl, "unit_color"))    { if (s.uintVal(&u)) t.readout.unit_color = u; return true; }
        if (keyIs(rk, rkl, "coolant_color")) { if (s.uintVal(&u)) t.readout.coolant_color = u; return true; }
        if (keyIs(rk, rkl, "intake_color"))  { if (s.uintVal(&u)) t.readout.intake_color = u; return true; }
        // ★ 2026-10-15（主题格式修订 R3）：底板深浅。量纲 0..255 —— 与上面
        //   `track_opa`（readArc 里 `(uint8_t)u`）**同一套**，别写成 0..100 的百分比：
        //   文件里存的是 LVGL 的 opa 域，编辑器只把它**显示**成百分比。
        //   缺键 → 保持默认 38（theme_set_defaults），所以旧主题照旧。
        if (keyIs(rk, rkl, "plate_opa"))     { if (s.uintVal(&u)) t.readout.plate_opa = (uint8_t)u; return true; }
        if (keyIs(rk, rkl, "digit_font"))    { if (s.number(&d)) t.readout.digit_font = (uint8_t)d; return true; }
        if (keyIs(rk, rkl, "unit_font"))     { if (s.number(&d)) t.readout.unit_font = (uint8_t)d; return true; }
        if (keyIs(rk, rkl, "sub_font"))      { if (s.number(&d)) t.readout.sub_font = (uint8_t)d; return true; }
        if (keyIs(rk, rkl, "digit_cy"))      { if (s.number(&d)) t.readout.digit_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "unit_cy"))       { if (s.number(&d)) t.readout.unit_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "coolant_cy"))    { if (s.number(&d)) t.readout.coolant_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "intake_cy"))     { if (s.number(&d)) t.readout.intake_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "show_units"))    { if (s.uintVal(&u)) t.readout.show_units = (uint8_t)(u ? 1 : 0); return true; }
        if (keyIs(rk, rkl, "show_coolant"))  { if (s.uintVal(&u)) t.readout.show_coolant = (uint8_t)(u ? 1 : 0); return true; }
        if (keyIs(rk, rkl, "show_intake"))   { if (s.uintVal(&u)) t.readout.show_intake = (uint8_t)(u ? 1 : 0); return true; }
        return false;
      });
      // ★ 回落（读完之后，见上面 ②）：没出现的键 ⇒ 等于**文件里那份** digit_color。
      //   ★ 值写错类型（例如 "rpm_color": "red"）时 uintVal 失败、旗子不置位 ⇒
      //     同样走回落 —— 宁可回到"和拆分前一样"，也不要留一个没被赋值的新字段。
      if (!has_rpm_color)   t.readout.rpm_color   = t.readout.digit_color;
      if (!has_speed_color) t.readout.speed_color = t.readout.digit_color;
      return true;
    }
    if (keyIs(k, klen, "screens")) {
      if (!s.eat('[')) return true;
      uint8_t si = 0;
      if (s.peek() == ']') { s.eat(']'); return true; }
      for (;;) {
        // ★ 与 arc_count 同一类闸门：**数组上界 = 数组真实长度**，不写字面量。
        //   （原来这里是硬编码的 `si < 2`；`Theme::screens` 改成 kScreenCount 之后，
        //     两处同一个名字 ⇒ 以后加/减屏不会一边改一边漏。多出来的屏整段跳过。）
        if (si < kScreenCount) readScreen(s, t.screens[si]);
        else s.skipValue();
        ++si;
        if (s.eat(',')) continue;
        s.eat(']');
        return true;
      }
    }
    return false;
  });

  // 钳制**传进来的这个对象**（不是全局槽位）—— 单测与调用方都靠这一步
  // 拿到"安全可用"的主题。派生字段也在这里重算。
  theme_clamp(t);
  return true;
}

// ============================================================
// 主题文件里的 `alerts` 段（告警 / 蜂鸣器参数）—— 2026-09-27 新增
//
// 与配色那一段**语义完全一致**：缺字段保持原值、越界钳制、未知字段忽略。
// 定位方式见 theme_store.h 的说明（根上或 theme 里面都认）。
// ============================================================
bool theme_parse_alerts_json(const char* json, uint32_t len, AlertsConfig& cfg) {
  if (!json || len < 2) return false;

  // 找 `"alerts"` 这个**键**：先找字符串，再确认它后面跟的是冒号 ——
  // 与 theme_parse_json 找 `"theme"` 用的是同一套办法（避免匹配到
  // 名字里含 alerts 的其它键，或某个字符串值里出现的 alerts）。
  const char* hit = strstr(json, "\"alerts\"");
  if (!hit) return false;

  Scan probe(hit, (uint32_t)(len - (uint32_t)(hit - json)));
  probe.eat('"');
  probe.p += 6;                      // 走过 alerts 这 6 个字母
  if (!(probe.eat('"') && probe.eat(':'))) return false;

  Scan s(probe.p, (uint32_t)(probe.end - probe.p));
  if (s.peek() != '{') return false;  // 是别的类型（数组/字符串）⇒ 当没有这一段

  s.object([&](const char* k, size_t klen) -> bool {
    uint32_t u = 0; double d = 0; bool b = false;
    if (keyIs(k, klen, "overspeed_kmh"))      { if (s.number(&d)) cfg.overspeed_kmh = (float)d; return true; }
    if (keyIs(k, klen, "overspeed_hyst_kmh")) { if (s.number(&d)) cfg.overspeed_hyst_kmh = (float)d; return true; }
    if (keyIs(k, klen, "redline_rpm"))        { if (s.number(&d)) cfg.redline_rpm = (float)d; return true; }
    if (keyIs(k, klen, "redline_hyst_rpm"))   { if (s.number(&d)) cfg.redline_hyst_rpm = (float)d; return true; }
    if (keyIs(k, klen, "door_debounce_ms"))   { if (s.uintVal(&u)) cfg.door_debounce_ms = u; return true; }
    if (keyIs(k, klen, "turn_signal_on_ms"))  { if (s.uintVal(&u)) cfg.turn_signal_on_ms = u; return true; }
    if (keyIs(k, klen, "debounce_ms"))        { if (s.uintVal(&u)) cfg.debounce_ms = u; return true; }
    if (keyIs(k, klen, "beep_min_interval_ms")) { if (s.uintVal(&u)) cfg.beep_min_interval_ms = u; return true; }
    if (keyIs(k, klen, "beep_ms"))            { if (s.uintVal(&u)) cfg.beep_ms = u; return true; }
    if (keyIs(k, klen, "only_highest"))       { if (s.boolean(&b)) cfg.only_highest = b; return true; }
    return false;                     // 不认识的键 ⇒ 扫描器跳过它的值（向前兼容）
  });

  // ★ 钳制与主题配色同一条纪律：**文件是外部输入**，越界值在这一处被挡下来。
  alerts_config_clamp(cfg);
  return true;
}
