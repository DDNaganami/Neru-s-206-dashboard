// ============================================================
// 真实抓包回归:把逻辑分析仪导出的**真边沿**喂进固件用的解码链
// (van::BitDecoder —— 也就是 VanPhyWire / 实车固件里那个解码器)
//
// 为什么必须有这条(2026-09-19):槽时间从 8.00µs 改成实测的 8.25µs 之后,
// 合成波形能过只说明"夹具和被测代码用了同一个常数"——**证明不了真实波形**。
// 这份 CSV 是实车上抓的(车不动,30 秒抓包里的 1.2 秒切片,3998 条跳变,
// CH0 是 VAN 信号),它能离线回答那个关键问题:
//   改对槽时间之后,真实边沿上到底能不能解出帧、IDEN 是不是 0x824?
// 过了这条,车上那趟就不必再拿"frames 是不是 > 0"当判据了 —— 只剩上车核真实帧率、
// 以及用表盘脸的档位复核刻度(2026-09-20 起不再要求 20/40/60/80 定速跑)。
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
// ★★ 关于"哪些帧算数"(2026-09-19 更新,别按旧结论读):
//   van_wire 的 FrameParser 只把 **FCS 校验通过**的帧算成 frames
//   (parseFrameBytes 找不到吻合的 CRC-15 候选长度就返回 false)。
//   曾经这里写着"CRC-15 约定未定 ⇒ frames 恒为 0"——**那条已经作废**:
//   FCS 约定已用这份抓包 + 5 分钟行驶抓包(共 17106+66 帧)定案:
//     FCS = crc15_van_iso(poly 0x0F9D / init 0x7FFF / 输出取反 / MSB-first),
//     覆盖 IDEN(12 位)+CMD(4 位)+全部数据;线上是 16 位大端字段
//     = (crc << 1),最低位恒 0(那一位与 EOD 槽构成 E-Manchester 违约)。
//   所以本测试现在分三层断言:
//     ① **字节层**(证明槽时间/位解码这条链是对的):SOF 命中次数、字节数,
//        并且存在 IDEN == 0x824 的报文;
//     ② **整数µs 精度**:截成整数µs 喂进去结果与 ns 喂法完全一致;
//     ③ **包层(FCS)**:整链必须真的收出帧 —— frames > 0、frames == fcs_ok,
//        而且收出来的帧里必须有 IDEN=0x824 的车速帧。
//        这条以前只能"钉住 frames=0 的现状",现在反过来钉"FCS 必须过"。
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

// 与 van_phy_gpio.cpp 的 kIdleCloseUs 对齐(那边是设备端 TU,宿主机编不进来)。
// ★ 改这个值必须**同时**改固件那份,并重跑本文件的三条用例:
//   门限决定"哪些帧被并进同一个缓冲",黄金值会跟着变。
//   70µs 的依据(实测)见 van_phy_gpio.cpp 的注释与 tools/van-decode/gap_stats.py:
//   帧内最大间隔 49.0µs < 70 < 帧间最小空闲 95.5µs。
const uint32_t kIdleCloseUs = 70;

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

// 车速字段切片(行驶中,0x824 帧 12 个,车速 13→14 km/h)。
// 为什么单独要一份:原来那份 sample-diffmanchester.csv 是**停车**抓的,
// 车速字段全 0 —— "车速读得对"这条在那里证明不了。
FILE* openSpeedCapture() {
  FILE* f = fopen("tools/van-decode/sample-speed-824.csv", "rb");
  if (!f) f = fopen("../tools/van-decode/sample-speed-824.csv", "rb");
  if (!f) f = fopen("../../tools/van-decode/sample-speed-824.csv", "rb");
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
// ③ 包层:VanPhyWire 整链(µs 接口)——
//    真实边沿 → SOF → 字节 → **FCS 通过** → frames 增加 → 回调出 VanPacket
//
// 这条是本次工作的收工判据:改之前 frames 恒为 0(FCS 约定错);
// 改之后必须真的收出帧,而且要收出 0x824 那族车速帧。
// ============================================================
namespace {
// 记录回调出来的包(实车链路上这个 sink 就是 VanSource)
class PacketRecorder : public VanSink {
 public:
  int count = 0;
  int iden824 = 0;
  int ack_mismatch = 0;     // ACK 位与"该不该有 ACK"(cmd bit2=1 且 RTR=0)不符的帧数
  int ack_miss = 0;         // 该有 ACK 却没认出来
  int ack_extra = 0;        // 不该有 ACK 却认成有
  uint16_t bad_iden = 0;
  uint8_t bad_cmd = 0, bad_ack = 0;
  char bad_list[96] = {0};   // 前几个不符的 (iden/cmd/ack),失败时打进消息里
  int ack_seen = 0;
  VanPacket last{};
  VanSource* forward = nullptr;   // 非空 ⇒ 收到的包转交给数据源(实车链路就是这样)
  void onPacket(const VanPacket& p) override {
    last = p;
    ++count;
    if (forward) forward->onPacket(p);
    if (p.iden == VanSource::kSpeedIden) ++iden824;
    const bool expect = cmdExpectsAck(p.cmd);
    if (expect) ++ack_seen;
    if ((p.ack != 0) != expect) {
      if (ack_mismatch == 0) { bad_iden = p.iden; bad_cmd = p.cmd; bad_ack = p.ack; }
      ++ack_mismatch;
      if (expect) ++ack_miss; else ++ack_extra;
      if (strlen(bad_list) < 72) {
        char t[24];
        snprintf(t, sizeof(t), "%03X/%X/%u@%ums ", (unsigned)p.iden, (unsigned)p.cmd,
                 (unsigned)p.ack, (unsigned)p.rx_ms);
        strcat(bad_list, t);
      }
    }
  }
};
}  // namespace

static void test_real_capture_vanphywire_accepts_frames(void) {
  FILE* f = openCapture();
  TEST_ASSERT_NOT_NULL(f);

  VanPhyWire phy;
  PacketRecorder rec;
  phy.begin();
  phy.setSink(&rec);

  char line[512];
  uint64_t last_ns = 0;
  bool has_last = false;
  int rows = 0;
  int finishes = 0;
  while (fgets(line, sizeof(line), f)) {
    uint64_t t_ns = 0;
    uint8_t lv = 0;
    if (!parseRow(line, &t_ns, &lv)) continue;
    // 空闲判据与固件同款:两条边沿间隔 > kIdleCloseUs(70µs)⇒ 上一帧结束,调一次 finish()
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
  char msg[320];
  snprintf(msg, sizeof(msg),
           "真实抓包整链(µs 接口): 边沿 %d · finish %d 次 · edges=%u frames=%u "
           "fcs_ok=%u dropped=%u · 回调包 %d(其中 IDEN=0x824 有 %d 个, 应带 ACK 的 %d 个, "
           "ACK 位不符 %d 个[少认 %d / 多认 %d, 首个 iden=0x%03X cmd=0x%X ack=%u] %s",
           rows, finishes, (unsigned)st.edges, (unsigned)st.frames,
           (unsigned)st.frames_fcs_ok, (unsigned)st.frames_dropped,
           rec.count, rec.iden824, rec.ack_seen, rec.ack_mismatch,
           rec.ack_miss, rec.ack_extra, rec.bad_iden, rec.bad_cmd, rec.bad_ack,
           rec.bad_list);

  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(3000, rows, "CSV 没读到足够的边沿");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)rows, st.edges, msg);

  // ★ 核心判据(本次工作的收工线):FCS 必须过 ⇒ 收到真实帧。
  //   下面是**黄金值**(2026-09-19 实测跑出来的基线,别再放宽成 >0):
  //     边沿 3998 · 收出帧 **66**(另有 1 帧被丢,是切片开头那半帧/µs 截断的
  //     边界帧)· 回调包 66,其中 IDEN=0x824 车速帧 **24** 个。
  //   ★ 这组数字是**关帧门限改成 70µs 之后**的:同一份切片在旧门限 300µs 下
  //     只有 61 帧 / 23 个 0x824 —— 差的 5 帧正是背靠背帧被并帧丢掉的
  //     (切片里实测有 5 个帧边界 < 300µs;见 kIdleCloseUs 的实测说明)。
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(66u, st.frames, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(66u, st.frames_fcs_ok, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, st.frames_dropped, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(66, rec.count, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(24, rec.iden824, msg);
  // 最后一帧必须是 FCS 通过的(回调出来的包不该有 fcs_ok=0)
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, rec.last.fcs_ok, msg);
  // ★ ACK 位:实测帧尾那 2 个槽只在 cmd bit2=1 且 RTR=0 的帧上出现(见
  //   van_wire.h 的 cmdExpectsAck)。这份切片两种都有(0x824/0x8 无、0x464/0xC 有),
  //   所以这一条能验证解码器认不认得出来 —— 旧实现恒为 0(等于没认)。
  //
  //   ★ 这一条现在钉 **0**(以前只能钉 ≤6):门限 70µs 之后不再并帧,
  //     每个包的 ack 都取自它**自己**那一帧的帧尾。旧门限 300µs 下实测有 4 帧的
  //     ack 取自后一帧(1 少认 / 3 多认),因为两帧被并进同一个缓冲 ——
  //     并帧这个坑现在由这一条钉住:门限再被放大就会立刻变红。
  TEST_ASSERT_TRUE_MESSAGE(rec.ack_seen > 0, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rec.ack_mismatch, msg);
}

// ============================================================
// ④ 车速字段的**常量**钉在真实行驶抓包上(2026-09-20 定案)
//
// 要走这条的原因:`VanSource::kSpeedOffset` 曾经指向 data[2..3] 的 16 位
// x100 km/h 读法(来自公开文档),而实车抓包显示车速是 **data[2] 单字节、
// 1 计数 = 1 km/h**。光靠合成用例挡不住这种"整段字段布局改了"的回退,
// 所以这里直接用一份**行驶中**的真边沿切片跑完整链:
//   真边沿 → SOF → 4B5B → FCS → VanPacket → VanSource → speedKmh()/rpm()
// 黄金值是**离线算出来再写死**的(工具:tools/van-decode/,复现步骤见
// ACCEPTANCE.md 那条 0x824 记录),不是从被测代码反推的:
//   切片 tools/van-decode/sample-speed-824.csv(0.6s / 2168 条边沿,取自
//   10 分钟行驶抓包 t=259.30..259.90s 的**行驶段**)
//     整段收出 **33 帧**(全部 FCS 通过),其中 IDEN=0x824 的车速帧 **12** 个;
//     0x824 第一帧 data = 50 D8 0D 4B 56 E2 DF ⇒ rpm 2587.0 / speed 13
//     0x824 末帧   data = 55 C4 0E 55 57 1A F3 ⇒ rpm 2744.5 / speed 14
//   同一段里车速**单调从 13 涨到 14**、转速从 2587 涨到 2744 —— 两件事
//   一起发生,正是"加速中"的形态(不是巧合的常数)。
//
// ★ 这条同时挡两类回退:
//   · 把 kSpeedScale 改回 0.01 或把 kSpeedOffset 换成 16 位读法 ⇒ speed 立刻错;
//   · 把 kRpmOffset/kRpmScale 改掉 ⇒ rpm 立刻错。
// ============================================================
static void test_speed_field_constant_pinned_on_real_capture(void) {
  FILE* f = openSpeedCapture();
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "打不开 tools/van-decode/sample-speed-824.csv"
                                  "(测试的工作目录不是项目根?)");

  VanPhyWire phy;
  VanSource src;                 // ★ 被测对象:固件里那个数据源
  PacketRecorder rec;            // VanSink → 转交 VanSource(实车链路上就是它)
  phy.begin();
  phy.setSink(&rec);
  rec.forward = &src;
  src.begin();

  char line[512];
  uint64_t last_ns = 0;
  bool has_last = false;
  int rows = 0;
  while (fgets(line, sizeof(line), f)) {
    uint64_t t_ns = 0;
    uint8_t lv = 0;
    if (!parseRow(line, &t_ns, &lv)) continue;
    if (has_last && (uint32_t)((t_ns - last_ns) / 1000ull) > kIdleCloseUs) {
      phy.finish();
    }
    phy.onEdge((uint32_t)(t_ns / 1000ull), lv != 0);
    last_ns = t_ns;
    has_last = true;
    ++rows;
  }
  phy.finish();
  fclose(f);

  const VanPhyWire::Stats& st = phy.stats();
  char msg[288];
  snprintf(msg, sizeof(msg),
           "车速常量钉真实抓包(sample-speed-824.csv): 边沿 %d · frames=%u "
           "fcs_ok=%u 回调包=%d(其中 0x824 有 %d 个)· 末值 speed=%.4f rpm=%.4f"
           "(黄金值: 全部 33 帧 / 0x824 12 帧 / speed=35.84 / rpm=2744.5)",
           rows, (unsigned)st.frames, (unsigned)st.frames_fcs_ok, rec.count,
           rec.iden824, src.speedKmh(), src.rpm());

  TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1500, rows, "切片没读到足够的边沿");
  // 切片里所有报文族共 33 帧(0x824 占 12 个),全部 FCS 通过
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(33u, st.frames, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(33u, st.frames_fcs_ok, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(33, rec.count, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(12, rec.iden824, msg);
  // ★ 车速:单字节 data[2],1 计数 = **2.56 km/h**(2026-09-22 实测定标)。
  //   末帧 data[2]=0x0E(14) ⇒ 14 × 2.56 = 35.84
  TEST_ASSERT_TRUE_MESSAGE(src.hasSpeed(), msg);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(35.84f, src.speedKmh(), msg);
  // ★ 转速:16 位大端 data[0..1] × 0.125。末帧 0x55C4=21956 ⇒ 2744.5
  TEST_ASSERT_TRUE_MESSAGE(src.hasRpm(), msg);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2744.5f, src.rpm(), msg);
  // 常量本身也钉一份(改常量就会在这里变红,不用等到算错值)
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(2u, VanSource::kSpeedOffset, msg);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.56f, VanSource::kSpeedScale, msg);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, VanSource::kRpmOffset, msg);
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.125f, VanSource::kRpmScale, msg);
}

void register_van_real_capture_tests(void) {
  RUN_TEST(test_real_capture_bytes_yield_iden_0x824);
  RUN_TEST(test_real_capture_integer_us_resolution);
  RUN_TEST(test_real_capture_vanphywire_accepts_frames);
  RUN_TEST(test_speed_field_constant_pinned_on_real_capture);
}
