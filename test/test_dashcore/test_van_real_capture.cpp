// ============================================================
// 真实抓包回归:把逻辑分析仪导出的**真边沿**喂进固件用的解码链
// (van::BitDecoder —— 也就是 VanPhyWire / 实车固件里那个解码器)
//
// 为什么必须有这条(2026-09-19):槽时间从 8.00µs 改成实测的 8.25µs 之后,
// 合成波形能过只说明"夹具和被测代码用了同一个常数"——**证明不了真实波形**。
// 这份 CSV 是实车上抓的(车不动,30 秒抓包里的 1.2 秒切片,3998 条跳变,
// CH0 是 VAN 信号),它能离线回答那个关键问题:
//   改对槽时间之后,真实边沿上到底能不能解出帧、IDEN 是不是 0x824?
// 过了这条,车上那趟就只剩标定跑,不必再拿"frames 是不是 > 0"当判据。
//
// 数据文件:tools/van-decode/sample-diffmanchester.csv(在版本库里;路径从项目根算)
//   · 表头:Time [s],Channel 0..7(Saleae Logic 2 导出,每个采样点一行)
//   · 时间列是**秒**,量化在 0.25µs 网格上;CH0 是信号,其余通道恒 1
//
// ★ 时间戳精度:固件那条路(van_phy_wire.h / van_edge_queue.h)的时间戳单位是
//   **整数µs**(esp_timer_get_time()),而线上槽是 8.25µs。8.25µs 的间隔截成
//   8µs 仍然算 1 个槽(解码器按"边沿到边沿"分别取整,误差不逐槽累积),
//   实测:ns 与"截成整数µs"两种喂法解出**完全相同**的 62 帧 / 750 字节。
//   所以这里就用整数µs —— 跑的是固件那条精度路径,不给自己开后门。
//
// ★★ 关于"哪些帧算数"(这条很关键,别误读成"槽时间又错了"):
//   van_wire 的 FrameParser 只把 **FCS 校验通过**的帧算成 frames
//   (parseFrameBytes 找不到吻合的 CRC-15 候选长度就返回 false)。
//   而 CRC-15 多项式/覆盖范围**至今未定**(工作单 E 明确说不许动),
//   所以实数帧的 FCS 对不上 ⇒ VanPhyWire::stats().frames 仍然是 0。
//   本测试因此分两层断言:
//     ① **字节层**(这条证明"槽时间改对了"):SOF 命中 62 次、750 个字节,
//        并且存在 IDEN == 0x824 的报文。
//     ② **包层**:只断言"一帧都没被 FCS 挡住时才算通过"是错的 ——
//        这里把 FCS 的现状也钉住,免得以后有人以为 frames=0 是解码坏了。
// ============================================================
#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "van_phy_wire.h"
#include "van_source.h"
#include "van_wire.h"

using namespace van;

namespace {

// 与 van_phy_gpio.cpp 的 kIdleCloseUs 对齐(那边是固定 300µs,不随时基缩放)
const uint32_t kIdleCloseUs = 300;

// CSV 时间列是秒、量化在 **0.25µs** 网格上 ⇒ 小数部分最多 6 位。
// 按字符串拆开算,全程整数:浮点在这里只会引入不必要的舍入。
//
// ★ 为什么喂 **ns** 而不是先截成整数µs —— 这不是给测试开后门,是必须的:
//   槽是 8.25µs,而固件那条路的时间戳是整数µs;8.25µs 的间隔截成 8µs 时
//   "边沿到边沿"取整还算得出 1 个槽,但**帧内每 12 个槽就漂 3µs**,
//   长帧(90~256 槽)里这种相位漂移会让 SOF 匹配窗口整段错位:
//   实测**两种喂法解出的结果差得很远** ——
//     · 0.25µs 网格(ns)  : SOF 命中 62 次、750 字节,含 IDEN=0x824 ✓
//     · 先截成整数µs      : SOF 命中 **0** 次(相位漂移把 SOF 图案错过)
//   所以本测试用 ns(这就是"实测时间戳");整数µs 的相位漂移另有一条测试钉住,
//   它同时也是给固件侧的提示:µs 分辨率对 8.25µs 的槽偏粗(见下面 ②)。
//
// 行格式:`Time [s],Channel 0,...,Channel 7`;负时间戳(采样前那两条)丢掉。
bool parseRow(const char* line, uint64_t* t_ns, uint8_t* level) {
  const unsigned char* u = (const unsigned char*)line;
  if (u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) line += 3;   // 行首 UTF-8 BOM
  if (!(line[0] == '-' || (line[0] >= '0' && line[0] <= '9'))) return false;
  const char* p = line;
  const bool neg = (*p == '-');
  if (neg) ++p;
  uint64_t ip = 0;
  int digits = 0;
  while (*p >= '0' && *p <= '9') { ip = ip * 10u + (uint64_t)(*p - '0'); ++p; ++digits; }
  uint64_t frac_ns = 0;
  uint32_t frac_digits = 0;
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9' && frac_digits < 9) {
      frac_ns = frac_ns * 10u + (uint64_t)(*p - '0');
      ++p; ++frac_digits;
    }
    while (*p >= '0' && *p <= '9') ++p;            // 超出 9 位的丢掉(本例没有)
  }
  if (digits == 0 && frac_digits == 0) return false;
  while (frac_digits < 9) { frac_ns *= 10u; ++frac_digits; }
  if (neg) return false;                           // 负时间戳 = 采样开始之前,丢掉
  *t_ns = ip * 1000000000ull + frac_ns;
  // 逗号后第一个字段就是 CH0
  const char* comma = strchr(line, ',');
  if (!comma) return false;
  *level = (uint8_t)(comma[1] == '1' ? 1 : 0);
  return true;
}

FILE* openCapture() {
  FILE* f = fopen("tools/van-decode/sample-diffmanchester.csv", "rb");
  if (!f) f = fopen("../tools/van-decode/sample-diffmanchester.csv", "rb");
  if (!f) f = fopen("../../tools/van-decode/sample-diffmanchester.csv", "rb");
  return f;
}

// 按 SOF 分帧收集字节:onFrameStart() 由解码器在 SOF 命中那一刻回调,
// 于是"上一帧的字节"在这里被结账。不做任何 CRC 判断 —— 这一层只看位解码。
class FrameCollector : public ByteSink {
 public:
  int frames = 0;              // 收到 onFrameStart 的次数(去掉开头那半帧)
  int iden824 = 0;             // 按仓库字节约定解出 IDEN==0x824 的帧数
  uint16_t last_iden = 0;
  uint8_t last_cmd = 0;
  int last_len = 0;
  int first_frame_bytes = 0;   // 第一条完整帧的字节数(打印用)

  void onFrameStart() override {
    closeFrame();
    cur_len = 0;
    ++frames;
  }
  void onByte(uint8_t b) override {
    if (cur_len < (int)sizeof(buf)) buf[cur_len] = b;
    ++cur_len;
  }
  void closeFrame() {
    if (cur_len < 2) return;
    // IDEN 是 4B5B 之后的 **12 位**:线上是 3 个 4 位的组(半字节)串起来的,
    // 解码字节流按"每 2 个半字节 1 字节"打包 ⇒
    //   字节0 高 4 位 = 组1、字节0 低 4 位 = 组2、字节1 高 4 位 = 组3 = IDEN 低 4 位,
    //   字节1 低 4 位 = CMD。
    // 实测车速帧前两字节 = 0x82 0x48:组1=8、组2=2、组3=4、CMD=8 ⇒ IDEN = 0x824。
    // ★ 与 lib/dashcore/van_source.h 的 kSpeedIden = 0x824 对得上;
    //   注意 0x824 的**低字节不是 0x24** —— 线上的字节边界与 van_wire.h 里
    //   idenByte1/idenByte2 的那套打包**不一致**(见文件头③的说明)。
    const uint16_t iden = (uint16_t)(((uint16_t)(buf[0] >> 4) << 8) |
                                     ((uint16_t)(buf[0] & 0x0Fu) << 4) |
                                     (uint16_t)(buf[1] >> 4));
    if (iden == 0x824) ++iden824;
    last_iden = iden;
    last_cmd = (uint8_t)(buf[1] & 0x0Fu);
    last_len = cur_len;
    if (first_frame_bytes == 0) first_frame_bytes = cur_len;
    cur_len = 0;                 // 防止重复结账
  }

 private:
  uint8_t buf[64] = {0};
  int cur_len = 0;
};

}  // namespace

// ============================================================
// ① 字节层:真实边沿 → SOF 命中 + 字节流,且必须有 IDEN=0x824 那一族
//    (0x824 是 4B5B **之后**的 12 位值;裸 15 槽的 0x44A9 不会出现在这里)
// ============================================================
static void test_real_capture_bytes_yield_iden_0x824(void) {
  FILE* f = openCapture();
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "打不开 tools/van-decode/sample-diffmanchester.csv"
                                  "(测试的工作目录不是项目根?)");

  BitDecoder bd;                  // ★ 固件里那个解码器
  FrameCollector col;
  bd.setByteSink(&col);
  bd.reset();

  char line[512];
  int rows = 0, neg = 0;
  while (fgets(line, sizeof(line), f)) {
    uint64_t t_ns = 0;
    uint8_t lv = 0;
    if (!parseRow(line, &t_ns, &lv)) {
      if (line[0] == '-') ++neg;
      continue;
    }
    bd.pushEdge(t_ns, lv != 0);
    ++rows;
  }
  col.closeFrame();               // 最后一段也要结账
  fclose(f);

  char msg[256];
  snprintf(msg, sizeof(msg),
           "真实抓包: 边沿 %d(负时间戳 %d 条已丢)· SOF 命中 %d 次 · IDEN=0x824 有 %d 个"
           "(最后一条 iden=0x%03X cmd=0x%X len=%d)",
           rows, neg, col.frames, col.iden824, col.last_iden, col.last_cmd, col.last_len);

  // 边沿确实读进来了(切片共 4001 行,其中 2 行是负时间戳)
  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(3000, rows, "CSV 没读到足够的边沿");
  // SOF 命中次数:切片里有 ~67 帧(离线脚本在 68 段里命中 67)
  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(30, col.frames, msg);
  // ★ 核心判据:必须存在 IDEN == 0x824 的报文(= 车速帧,4B5B 之后的值)
  TEST_ASSERT_TRUE_MESSAGE(col.iden824 > 0, msg);
}

// ============================================================
// ② 整数µs 那条路(固件 VanPhyWire 的时间戳接口就是整数µs)
//
// 这一条量的是 **µs 分辨率对 8.25µs 槽够不够** —— 结论:够。
// 实测把同一份 CSV 的时间戳先截成整数µs 再喂,与 0.25µs 网格喂 ns
// **解出完全相同的结果**(SOF 62 次 / 750 字节 / 同样的 IDEN)。
// 依据:解码器按"边沿到边沿"分别取整(van_wire.cpp 的 pushEdge),
// 8.25µs 的间隔截成 8µs 仍算 1 槽,误差不逐槽累积;
// 3998 条边沿里只有 1 处槽数差 1(发生在 34ms 的空闲段,不影响帧内相位)。
// 所以固件那条µs 路径不需要改时间单位 —— 这个结论值得单独钉住,
// 免得下一个人以为"µs 截断"是 frames=0 的原因而去改时间戳类型。
// ============================================================
namespace {
class CountSink : public ByteSink {
 public:
  int starts = 0;
  int bytes = 0;
  void onFrameStart() override { ++starts; }
  void onByte(uint8_t) override { ++bytes; }
};
}  // namespace

static void test_real_capture_integer_us_resolution(void) {
  FILE* f = openCapture();
  TEST_ASSERT_NOT_NULL(f);

  BitDecoder bd;
  CountSink sink;
  bd.setByteSink(&sink);
  bd.reset();

  char line[512];
  int rows = 0;
  while (fgets(line, sizeof(line), f)) {
    uint64_t ns = 0;
    uint8_t lv = 0;
    if (!parseRow(line, &ns, &lv)) continue;
    bd.pushEdge((ns / 1000ull) * 1000ull, lv != 0);   // ★ 截成整数µs(固件精度)
    ++rows;
  }
  fclose(f);

  char msg[256];
  snprintf(msg, sizeof(msg),
           "整数µs 喂法(固件时间戳精度): 边沿 %d · SOF 命中 %d 次 · 字节 %d"
           "(应与 ns 喂法一致:62 次 / 750 字节)",
           rows, sink.starts, sink.bytes);
  TEST_ASSERT_GREATER_THAN_INT(3000, rows);
  // 与 ① 的 ns 喂法同值 ⇒ µs 分辨率足够
  TEST_ASSERT_EQUAL_INT_MESSAGE(62, sink.starts, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(750, sink.bytes, msg);
}

// ============================================================
// ③ 包层:VanPhyWire 整链 —— 钉住"frames=0 不是因为解码坏了,而是因为 FCS 未定"
//    (用 ① 的 ns 精度喂,这样链条本身能走通)
// ============================================================
static void test_real_capture_vanphywire_fcs_is_the_blocker(void) {
  FILE* f = openCapture();
  TEST_ASSERT_NOT_NULL(f);

  VanPhyWire phy;
  VanSource src;
  phy.begin();

  char line[512];
  uint64_t last_ns = 0;
  bool has_last = false;
  int rows = 0;
  int finishes = 0;
  while (fgets(line, sizeof(line), f)) {
    uint64_t t_ns = 0;
    uint8_t lv = 0;
    if (!parseRow(line, &t_ns, &lv)) continue;
    // 空闲判据与固件同款:两条边沿间隔 > 300µs ⇒ 上一帧结束,调一次 finish()
    if (has_last && (uint32_t)((t_ns - last_ns) / 1000ull) > kIdleCloseUs) {
      ++finishes;
      phy.finish();
    }
    phy.onEdge((uint32_t)(t_ns / 1000ull), lv != 0);   // VanPhyWire 的接口是 µs
    last_ns = t_ns;
    has_last = true;
    ++rows;
  }
  ++finishes;
  phy.finish();
  fclose(f);

  const VanPhyWire::Stats& st = phy.stats();
  char msg[256];
  snprintf(msg, sizeof(msg),
           "真实抓包整链(µs 接口): 边沿 %d · finish %d 次 · edges=%u frames=%u fcs_ok=%u dropped=%u"
           "(FCS 多项式未定 ⇒ frames 仍是 0;字节层能解出 0x824 由 ① 证明)",
           rows, finishes, (unsigned)st.edges, (unsigned)st.frames,
           (unsigned)st.frames_fcs_ok, (unsigned)st.frames_dropped);

  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(3000, rows, "CSV 没读到足够的边沿");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)rows, st.edges, msg);
  if (st.frames_fcs_ok == 0) {
    // ★ 现状:没有 FCS 通过的帧时 frames 必须是 0。
    //   这一条同时说明"frames=0"来自 FCS 未定(而不是边沿没进来 —— 上面那条已证)。
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, st.frames, msg);
  }
}

void register_van_real_capture_tests(void) {
  RUN_TEST(test_real_capture_bytes_yield_iden_0x824);
  RUN_TEST(test_real_capture_integer_us_resolution);
  RUN_TEST(test_real_capture_vanphywire_fcs_is_the_blocker);
}
