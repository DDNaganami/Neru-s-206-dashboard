// probe_van_callback.cpp —— 直接在宿主机上跑**真实的** VanPhyWire,
// 用仓库里的真抓包切片喂边沿,数一数 sink_->onPacket() 到底被调了几次。
//
// 为什么需要它(2026-09-22):
//   车载实测出现一个自相矛盾的现象 —— 物理层诊断行 `frames=458 fcs_ok=458`
//   在涨(这两个计数与 sink_->onPacket() 是**同一个 if(ok) 块**里的),
//   但加在 sink 里的嗅探计数器(`van_sniff_note`)和那句 "VAN %03X" 帧行
//   都**从不触发**。读代码得不出结论(只有一个 g_van_phy、一个 setSink 点),
//   所以改成"把真代码拉到宿主机上跑一遍",用事实定分晓。
//
// 编译(不需要 PlatformIO,直接用宿主 g++):
//   g++ -std=c++17 -I lib/dashcore -I test/arduino_shim \
//       tools/van-decode/probe_van_callback.cpp lib/dashcore/van_wire.cpp \
//       lib/dashcore/van_phy_wire.cpp -o probe_van_callback
//
// 输入:CSV 格式与 tools/van-decode/sample-diffmanchester.csv 相同
//   Time [s],Channel 0,...
//   取值只用 Channel 0 的 0/1;时间列换算成整数 µs(固件接口就是整数 µs)。
//
// 输出:边沿数 / finish 返回 true 的次数 / sink 被回调的次数 / 失败原因计数。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "van_phy_wire.h"

// ---- 计数用的假 sink:只数数,不做任何格式化 ----
static uint32_t g_sink_calls = 0;
static uint32_t g_sink_fcs_ok = 0;
static uint16_t g_last_iden = 0;

class CountingSink : public VanSink {
 public:
  void onPacket(const VanPacket& pkt) override {
    ++g_sink_calls;
    if (pkt.fcs_ok) ++g_sink_fcs_ok;
    g_last_iden = pkt.iden;
  }
};

// 与 van_phy_gpio.cpp 的 kIdleCloseUs 保持一致(70µs)
static const uint32_t kIdleCloseUs = 70;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法: %s <capture.csv>\n", argv[0]);
    return 2;
  }

  // ---- 读 CSV:只要 Channel 0 的 0/1 与时间列 ----
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::printf("打不开 %s\n", argv[1]); return 2; }

  char line[512];
  std::vector<uint32_t> t_us;
  std::vector<uint8_t> lv;
  bool first = true;
  while (std::fgets(line, sizeof(line), f)) {
    if (first) { first = false; continue; }   // 表头
    char* p = line;
    // 时间列
    const double t = std::strtod(p, &p);
    if (p == line) continue;
    // 跳到第 1 个逗号后的 Channel 0
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
    const int ch0 = std::atoi(p);
    // 时间列可能是秒(如 -0.000036000)也可能是大整数 tick;统一成 µs
    const double us = (t > -1e6 && t < 1e6) ? (t * 1e6) : t;
    t_us.push_back((uint32_t)(int64_t)(us < 0 ? 0 : us));
    lv.push_back((uint8_t)(ch0 ? 1 : 0));
  }
  std::fclose(f);

  std::printf("读入边沿 %zu 条\n", t_us.size());
  if (t_us.empty()) return 2;

  // ---- 把真实代码跑起来 ----
  VanPhyWire phy;
  CountingSink sink;
  phy.begin();
  phy.setSink(&sink);

  uint32_t finish_true = 0, finish_false = 0;
  uint32_t last_edge_us = t_us[0];

  for (size_t i = 0; i < t_us.size(); ++i) {
    // 时间列若是从 0 起步的负数微调,这里保持原样;onEdge 只关心差值
    phy.onEdge(t_us[i], lv[i] != 0);
    last_edge_us = t_us[i];

    // 模拟固件的关帧时机:队列总是空的(这里直接喂),所以只要
    // "距上一条已喂边沿 > 70µs" 就尝试关帧 —— 与 vanIdleCloseReady 等价
    const size_t next = i + 1;
    const uint32_t gap = (next < t_us.size())
                             ? (uint32_t)(t_us[next] - t_us[i])
                             : kIdleCloseUs + 1;
    if (gap > kIdleCloseUs) {
      if (phy.finish()) ++finish_true; else ++finish_false;
    }
  }

  const VanPhyWire::Stats& st = phy.stats();
  std::printf("\n===== 结果 =====\n");
  std::printf("边沿数(Stats)      : %u\n", (unsigned)st.edges);
  std::printf("stats.frames       : %u\n", (unsigned)st.frames);
  std::printf("stats.frames_fcs_ok: %u\n", (unsigned)st.frames_fcs_ok);
  std::printf("stats.frames_dropped: %u\n", (unsigned)st.frames_dropped);
  std::printf("finish() 返回 true : %u\n", (unsigned)finish_true);
  std::printf("finish() 返回 false: %u\n", (unsigned)finish_false);
  std::printf(">>> sink 被回调次数 : %u\n", (unsigned)g_sink_calls);
  std::printf(">>> sink 里 fcs_ok  : %u\n", (unsigned)g_sink_fcs_ok);
  std::printf(">>> 最后一个 IDEN   : 0x%03X\n", (unsigned)g_last_iden);
  std::printf("\n判定:stats.frames 与 sink 回调次数**应当相等**。\n");
  std::printf("  相等 => 这条链在宿主机上是好的,车上那次矛盾另有原因;\n");
  std::printf("  不等 => 就是代码问题(看 finish()/sink 那段)。\n");
  return 0;
}
