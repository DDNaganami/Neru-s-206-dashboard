#include "theme_store.h"
#include <string.h>

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
      if (!fn(kbegin, klen)) skipValue();
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
        a.kind = (ArcKind)(u <= (uint32_t)ArcKind::Coolant ? u : 0u);
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
    if (keyIs(k, klen, "arc_count")) { if (s.uintVal(&u)) sc.arc_count = (uint8_t)u; return true; }
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
    if (keyIs(k, klen, "coolant_min_c"))   { if (s.number(&d)) t.coolant_min_c = (float)d; return true; }
    if (keyIs(k, klen, "coolant_max_c"))   { if (s.number(&d)) t.coolant_max_c = (float)d; return true; }
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
      s.object([&](const char* rk, size_t rkl) -> bool {
        if (keyIs(rk, rkl, "digit_color"))   { if (s.uintVal(&u)) t.readout.digit_color = u; return true; }
        if (keyIs(rk, rkl, "unit_color"))    { if (s.uintVal(&u)) t.readout.unit_color = u; return true; }
        if (keyIs(rk, rkl, "coolant_color")) { if (s.uintVal(&u)) t.readout.coolant_color = u; return true; }
        if (keyIs(rk, rkl, "digit_font"))    { if (s.number(&d)) t.readout.digit_font = (uint8_t)d; return true; }
        if (keyIs(rk, rkl, "unit_font"))     { if (s.number(&d)) t.readout.unit_font = (uint8_t)d; return true; }
        if (keyIs(rk, rkl, "digit_cy"))      { if (s.number(&d)) t.readout.digit_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "unit_cy"))       { if (s.number(&d)) t.readout.unit_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "coolant_cy"))    { if (s.number(&d)) t.readout.coolant_cy = (int32_t)d; return true; }
        if (keyIs(rk, rkl, "show_units"))    { if (s.uintVal(&u)) t.readout.show_units = (uint8_t)(u ? 1 : 0); return true; }
        if (keyIs(rk, rkl, "show_coolant"))  { if (s.uintVal(&u)) t.readout.show_coolant = (uint8_t)(u ? 1 : 0); return true; }
        return false;
      });
      return true;
    }
    if (keyIs(k, klen, "screens")) {
      if (!s.eat('[')) return true;
      uint8_t si = 0;
      if (s.peek() == ']') { s.eat(']'); return true; }
      for (;;) {
        if (si < 2) readScreen(s, t.screens[si]);
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
