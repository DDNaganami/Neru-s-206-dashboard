#include <unity.h>
#include "test_helpers.h"
#include "fake_obd_transport.h"   // FakeSerialAsTransport：把假串口包成 ObdTransport
#include "obd_source.h"
#include "obd_protocol.h"   // 0100 位图那两条纯函数(不是数据帧,单独一层)

// 驱动状态机直到 fake 串口发出指定命令
static bool drive_until_tx(FakeSerial& fake, ObdSource& obd, uint32_t& t,
                           const char* cmd, int max_steps) {
  for (int i = 0; i < max_steps; ++i) {
    t += 50;
    obd.tick(t);
    if (fake.sent(cmd)) return true;
  }
  return false;
}

void test_obd_init_sequence(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  TEST_ASSERT_TRUE(fake.sent("ATZ\r"));

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "ATE0\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "ATL0\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "ATH0\r", 30));
  // AT 序列之后先问一次支持的 PID 位图(0100)—— 车速那一路要不要占时隙由它决定
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 30));
  // 位图喂进来之后才轮到数据轮询
  fake.feed("41 00 BE 3E B8 13\r");
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));
  // 轮询:010C → 0105 → 010F(进气温度)→ 回到 010C
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0105\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010F\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 30));
}

// 0100 位图:**怎么读**这条必须钉死 —— 位序写错会得到一个"看起来合理"的
// 错误答案(比如把 0x0D 判成不支持,于是车速那一路永远不开)。
void test_obd_supported_pids_bits(void) {
  uint32_t mask = 0;
  // 真实形态:0xBE 0x3E 0xB8 0x13
  //   A=0xBE(1011 1110) → PID 01=1 02=0 03=1 04=1 05=1 06=1 07=1 08=0
  //   B=0x3E(0011 1110) → PID 09=0 0A=0 0B=1 0C=1 0D=1 0E=1 0F=1 10=0
  TEST_ASSERT_TRUE(parseSupportedPids("41 00 BE 3E B8 13", &mask));
  TEST_ASSERT_EQUAL_HEX32(0xBE3EB813u, mask);
  TEST_ASSERT_TRUE(pidSupported(mask, 0x01));   // A 的最高位是 1
  TEST_ASSERT_TRUE(pidSupported(mask, 0x0C));   // 转速
  TEST_ASSERT_TRUE(pidSupported(mask, 0x05));   // 水温
  TEST_ASSERT_TRUE(pidSupported(mask, 0x0F));   // 进气温度
  TEST_ASSERT_TRUE(pidSupported(mask, 0x0D));   // ★ 车速:落在这张位图里
  TEST_ASSERT_FALSE(pidSupported(mask, 0x02));  // 这一位是 0
  TEST_ASSERT_FALSE(pidSupported(mask, 0x09));  // B 的最高位也是 0
  // D=0x13(0001 0011)→ 位图**最右端** PID 0x20 是 1、它左边的 0x1E 是 0
  // (这条原本我写成 0x20 = false,是"想当然";位图两端各钉一个才靠得住)
  TEST_ASSERT_TRUE(pidSupported(mask, 0x20));
  TEST_ASSERT_FALSE(pidSupported(mask, 0x1E));
  // 边界:位图只覆盖 01..20,超出范围一律 false(不要靠"越界读到的 0"侥幸)
  TEST_ASSERT_FALSE(pidSupported(0xFFFFFFFFu, 0x00));
  TEST_ASSERT_FALSE(pidSupported(0xFFFFFFFFu, 0x21));
  TEST_ASSERT_TRUE(pidSupported(0xFFFFFFFFu, 0x20));
  // 首字节全 1 时 PID 0x01 必须为真(那条映射的另一端)
  TEST_ASSERT_TRUE(pidSupported(0xFFFFFFFFu, 0x01));
  // 只有最低位为 1 → 只有 PID 0x20 支持
  TEST_ASSERT_TRUE(pidSupported(0x00000001u, 0x20));
  TEST_ASSERT_FALSE(pidSupported(0x00000001u, 0x1F));
  // 带 ECU 地址头也要能命中
  TEST_ASSERT_TRUE(parseSupportedPids("8F 41 00 BE 3E B8 13", &mask));
  TEST_ASSERT_EQUAL_HEX32(0xBE3EB813u, mask);
  // 残帧/无关行:必须拒(拿半个位图去猜比不猜更糟)
  TEST_ASSERT_FALSE(parseSupportedPids("41 00 BE 3E", &mask));
  TEST_ASSERT_FALSE(parseSupportedPids("41 0C 1A F8", &mask));
  TEST_ASSERT_FALSE(parseSupportedPids("NO DATA", &mask));
  // 位图**不能**被当成数据帧
  uint8_t pid = 0;
  uint16_t raw = 0;
  TEST_ASSERT_FALSE(parseObdLine("41 00 BE 3E B8 13", &pid, &raw));
}

// ECU 说支持 010D → 轮询表里就该有它;说不支持 → 一个时隙都不给它占。
void test_obd_poll_table_follows_bitmap(void) {
  {
    FakeSerial fake;
    FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
    test_set_millis(0);
    obd.begin();
    uint32_t t = 1000;
    TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 40));
    fake.feed("41 00 BE 3E B8 13\r");            // 支持 0x0D
    TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010D\r", 60));
    TEST_ASSERT_TRUE(obd.speedSupportKnown());
    TEST_ASSERT_TRUE(obd.speedSupported());
    TEST_ASSERT_TRUE(obd.speedPolled());
  }
  {
    FakeSerial fake;
    FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
    test_set_millis(0);
    obd.begin();
    uint32_t t = 1000;
    TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 40));
    fake.feed("41 00 BE 16 B8 13\r");            // B=0x16(0001 0110)→ 0x0D 那一位清零
    // 喂完位图,让它把表建好:推进几轮,确认 010D **从不**被发出
    for (int i = 0; i < 200; ++i) { t += 50; obd.tick(t); }
    TEST_ASSERT_TRUE(obd.speedSupportKnown());
    TEST_ASSERT_FALSE(obd.speedSupported());
    TEST_ASSERT_FALSE(obd.speedPolled());
    TEST_ASSERT_FALSE(fake.sent("010D\r"));
    TEST_ASSERT_TRUE(fake.sent("010C\r"));      // 其它三路照常
  }
}

// 拿不到位图(有些 clone 不认 0100)时:不猜、不试,按默认三路继续跑。
// 这条同时钉住"不能被位图卡死"—— 超时后必须自己往下走。
void test_obd_support_query_timeout_keeps_polling(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 40));
  // 什么也不喂 → 等超时
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));
  TEST_ASSERT_FALSE(obd.speedSupportKnown());
  TEST_ASSERT_FALSE(obd.speedPolled());
  TEST_ASSERT_FALSE(fake.sent("010D\r"));
  // 转速照样能读
  fake.feed("41 0C 1A F8\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasRpm());
}

// 010D 车速:单字节 A = km/h
void test_obd_speed_parse(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 40));
  fake.feed("41 00 BE 3E B8 13\r");
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010D\r", 60));
  TEST_ASSERT_FALSE(obd.hasSpeed());

  fake.feed("41 0D 3C\r");                       // 0x3C = 60
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasSpeed());
  TEST_ASSERT_EQUAL_FLOAT(60.0f, obd.speed());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastSpeedMs());
  // 四路互不干扰
  TEST_ASSERT_FALSE(obd.hasRpm());
  TEST_ASSERT_FALSE(obd.hasCoolant());
  TEST_ASSERT_FALSE(obd.hasIntake());
}

// 实测刷新率:每秒结算一次,数出来的就是"这一个字段每秒收到几个有效值"。
// 这条存在的理由:车速那一路值不值得加,靠的就是这几个数(见 obd_source.h)。
void test_obd_rate_meter(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 40));
  fake.feed("41 00 BE 3E B8 13\r");
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 60));

  // 1 秒内喂 3 个有效值 + 1 个坏帧 → 3.0 Hz(坏帧不算)
  //
  // ★ 先把结算窗口对齐:RateMeter 是"每满 1 秒结算一次",窗口起点是它自己的
  //   第一次 tick —— 不先对齐的话,喂数据的时刻落在窗口中间,
  //   算出来的 Hz 会是 1.8 这种数(第一次写这条用例就是这么挂的)。
  t += 1500;
  obd.tick(t);                                  // 结算一次 → 窗口起点 = t,hz 归零
  TEST_ASSERT_EQUAL_FLOAT(0.0f, obd.rpmHz());

  fake.feed("41 0C 1A F8\r");
  t += 300; obd.tick(t);
  fake.feed("41 0C 1B 00\r");
  t += 300; obd.tick(t);
  fake.feed("41 0C FF FF\r");                    // 16383 rpm → 丢弃
  t += 100; obd.tick(t);
  fake.feed("41 0C 1C 00\r");
  t += 300; obd.tick(t);                        // 距窗口起点正好 1000ms → 结算
  TEST_ASSERT_EQUAL_FLOAT(3.0f, obd.rpmHz());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, obd.speedHz()); // 没喂过车速

  // 再过 1 秒一个都没喂 → 掉到 0(不是"停在上一次的 3Hz")
  t += 1000; obd.tick(t);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, obd.rpmHz());
}

// ---- 轮询节奏:快路每轮、慢路每 N 轮 ----
//
// ★ 这两条用例是"降频"这个决定的**唯一**凭据,所以它们看的是**顺序**,
//   不能只用 fake.sent()(那是"发过没有",顺序错了也照样通过)。
//
// ★ 驱动方式很讲究:必须**按刚发出的请求**喂对应的响应。第一版是把响应提前
//   塞进去,结果请求还没发、响应先被解析掉 —— 状态机不认(不是当次请求),
//   于是每格白等 250ms 超时,测出来的节奏是假的,两条用例一起挂。
static int drive_answering(FakeSerial& fake, ObdSource& obd, uint32_t& t,
                           const char* bitmap, char out[][8], int max_out) {
  int n = 0;
  size_t seen = 0;
  int guard = 6000;
  while (n < max_out && guard-- > 0) {
    t += 10;
    obd.tick(t);
    const size_t end = fake.tx.find('\r', seen);
    if (end == std::string::npos) continue;
    const std::string req = fake.tx.substr(seen, end - seen);
    seen = end + 1;
    if (req.size() < 4 || req.compare(0, 2, "01") != 0) continue;  // AT 序列
    if (req == "0100") { fake.feed(bitmap); continue; }            // 位图不是数据请求
    snprintf(out[n], 8, "%s", req.c_str());
    if (req == "010C") fake.feed("41 0C 1A F8\r");
    else if (req == "0105") fake.feed("41 05 3C\r");
    else if (req == "010F") fake.feed("41 0F 2A\r");
    else if (req == "010D") fake.feed("41 0D 3C\r");
    ++n;
  }
  return n;
}

void test_obd_poll_schedule_fast_and_slow(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  uint32_t t = 1000;

  char req[32][8];
  const int n = drive_answering(fake, obd, t, "41 00 BE 3E B8 13\r", req, 28);
  TEST_ASSERT_TRUE(n >= 13);
  // ★ 一个周期的样子(快路 = 0C + 0D,慢路 = 05 / 0F):
  //    0C 0D 05 | 0C 0D 0F | 0C 0D | 0C 0D   ← 然后重复
  const char* expect[] = {"010C", "010D", "0105", "010C", "010D", "010F",
                          "010C", "010D", "010C", "010D",
                          "010C", "010D", "0105"};
  for (int k = 0; k < 13; ++k) {
    TEST_ASSERT_EQUAL_STRING(expect[k], req[k]);
  }
}

// 不支持车速时:快路只剩转速 —— 转速每周期的份额从"三路均分 1/3"变成 4/6
void test_obd_poll_schedule_without_speed(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  uint32_t t = 1000;

  char req[32][8];
  const int n = drive_answering(fake, obd, t, "41 00 BE 16 B8 13\r", req, 24);
  TEST_ASSERT_TRUE(n >= 8);
  // 快路 = 0C;慢路摊在周期里:0C 05 | 0C 0F | 0C | 0C
  const char* expect[] = {"010C", "0105", "010C", "010F",
                          "010C", "010C", "010C", "0105"};
  for (int k = 0; k < 8; ++k) {
    TEST_ASSERT_EQUAL_STRING(expect[k], req[k]);
  }
  TEST_ASSERT_FALSE(fake.sent("010D\r"));
}

// ★ 收到响应就该**立刻**发下一个,别等满 250ms 超时。
//   旧行为:每格固定 250(等)+间隔;新行为只有间隔(80ms)。
void test_obd_response_exits_wait_early(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();
  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0100\r", 40));
  fake.feed("41 00 BE 16 B8 13\r");               // 不支持车速:快路只有 0C
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));
  const size_t before = fake.tx.size();

  fake.feed("41 0C 1A F8\r");
  t += 10;
  obd.tick(t);                                    // 响应进来 → 立刻离开等待态
  // 只推进 100ms(间隔 80ms 已过):下一条就该发出去了
  for (int k = 0; k < 10; ++k) { t += 10; obd.tick(t); }
  TEST_ASSERT_TRUE(fake.tx.size() > before);
  TEST_ASSERT_TRUE(fake.sent("0105\r"));
  // 对照:同样 100ms 若还在等超时(250ms)就什么都不会发 ——
  // 这条断言实际上钉住了"提前离开"这个改动(旧实现这里会失败)
}

void test_obd_rpm_coolant_parse(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));

  fake.feed("41 0C 1A F8\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, obd.rpm());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastRpmMs());
  TEST_ASSERT_FALSE(obd.hasCoolant());  // 水温还没喂过

  // 单字节水温响应
  fake.feed("41 05 3C\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasCoolant());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, obd.coolant());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastCoolantMs());
}

// 进气温度(010F):与水温同形,单独喂一帧看它进没进状态。
void test_obd_intake_parse(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010F\r", 60));
  TEST_ASSERT_FALSE(obd.hasIntake());          // 还没喂数据

  fake.feed("41 0F 2A\r");                     // 0x2A = 42 → 2℃
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasIntake());
  TEST_ASSERT_EQUAL_FLOAT(2.0f, obd.intake());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastIntakeMs());
  // 三路互不干扰:喂了进气温度不等于喂了水温/转速
  TEST_ASSERT_FALSE(obd.hasCoolant());
  TEST_ASSERT_FALSE(obd.hasRpm());
}

// ★ ECU 不支持 010F 时的真实形态:ELM327 回 "NO DATA"。
//   这条必须**什么都不改**(hasIntake 保持 false),否则会出现"进气 0℃"的假读数。
void test_obd_intake_no_data_keeps_invalid(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010F\r", 60));
  fake.feed("NO DATA\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_FALSE(obd.hasIntake());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, obd.intake());
}

void test_obd_bad_rpm_rejected(void) {
  FakeSerial fake;
  FakeSerialAsTransport tr(&fake);
  ObdSource obd(&tr);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));

  fake.feed("41 0C 1A F8\r");  // 先来一帧合法值
  t += 10;
  obd.tick(t);
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, obd.rpm());

  fake.feed("41 0C FF FF\r");  // 16383.75 rpm > 9000,应丢弃
  t += 10;
  obd.tick(t);
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, obd.rpm());
}

void test_obd_disabled(void) {
  ObdSource obd(nullptr);
  obd.begin();     // 不崩
  obd.tick(5000);  // 不崩
  TEST_ASSERT_FALSE(obd.enabled());
}

void register_obd_source_tests(void) {
  RUN_TEST(test_obd_init_sequence);
  RUN_TEST(test_obd_supported_pids_bits);
  RUN_TEST(test_obd_poll_table_follows_bitmap);
  RUN_TEST(test_obd_support_query_timeout_keeps_polling);
  RUN_TEST(test_obd_speed_parse);
  RUN_TEST(test_obd_poll_schedule_fast_and_slow);
  RUN_TEST(test_obd_poll_schedule_without_speed);
  RUN_TEST(test_obd_response_exits_wait_early);
  RUN_TEST(test_obd_rate_meter);
  RUN_TEST(test_obd_rpm_coolant_parse);
  RUN_TEST(test_obd_intake_parse);
  RUN_TEST(test_obd_intake_no_data_keeps_invalid);
  RUN_TEST(test_obd_bad_rpm_rejected);
  RUN_TEST(test_obd_disabled);
}
