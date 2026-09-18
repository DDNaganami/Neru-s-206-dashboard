#pragma once
#include <Arduino.h>

// K 线 OBD 数据源(ELM327, ISO 9141-2 / KWP2000)。
// 非阻塞状态机:初始化 AT 序列后轮流请求 010C(转速)/ 0105(水温)/ 010F(进气温度)。
// 波特率由调用方在传入的串口上配好(常见 38400)。
// 每路各自记录最后更新时间,供上层按字段独立超时回退。
// 文本解析在 obd_protocol.h/.cpp(纯函数,宿主机可测)。
//
// ★ 为什么加 010F(进气温度):
//   2026-09 用户用蓝牙 ELM327 + EOBD 实测:206 CC 的 ECU 支持这一项,
//   于是把它做成速度表上的副表(与转速表上的水温表左右对称)。
//   同一个 01 服务、同样单字节 A-40,代价只是**多占一个轮询时隙**。
//
// ★ 轮询时隙的账(为什么三路转一圈会拖慢转速):
//   一路一个时隙:间隔 200ms + 等待 250ms;ELM327 在 ISO 9141-2 上一次
//   问答本身还要 50~100ms,所以三路轮一圈约 0.7~1.4s,
//   即**转速刷新率从 ≈1.4Hz 降到 ≈0.7~1.4Hz**。
//   当下可接受:转速要的是"当前值",读数有缓动;水温/进气是慢变量。
//   真要提速,正确做法不是砍掉进气温度,而是把两条慢弧**降频**
//   (每 N 圈才问一次)——kSlowEveryNTurns 就是留给这件事的旋钮。
//
// ★ 010D 车速(2026-09-18 加):**先问 0100 位图,ECU 说支持才占时隙**。
//   为什么不是"直接加进轮询表":
//     · 每多一个时隙,转速/水温/进气各慢 ~25%(见上面那笔账);
//     · 而"ECU 不支持"的代价更大 —— ELM327 要等到超时才回 "NO DATA",
//       等于白扔一个时隙。
//   所以开机(AT 序列之后)先问一次 0100:位图里 0x0D 置位才轮询它。
//   拿不到位图(某些 clone 不认这条)就按原来的三路继续跑,不猜。
//   `-DOBD_SPEED_PID=0` 可以强制关掉车速那一路(用来对比"加/不加"的实测刷新率)。
//   上层优先级见 data_service.h:车速 Van > Obd > Sim。
//
// ★ 实测刷新率(Hz):每个字段各自统计,**每 1 秒结算一次**。
//   为什么必须实测:`kPollIntervalMs` 只是我们这一侧的名义间隔,
//   ELM327 自己的转换开销、ECU 的响应快慢、超时重试都在里面,算不出来。
//   到了车上(或接上仿真器)看日志里那四个数就能定"要不要降频/加时隙"。
class ObdSource {
public:
  explicit ObdSource(HardwareSerial* serial = nullptr) : s_(serial) {}

  void begin();
  void tick(uint32_t now_ms);

  bool enabled() const { return s_ != nullptr; }
  bool hasRpm() const { return rpm_valid_; }
  bool hasCoolant() const { return coolant_valid_; }
  bool hasIntake() const { return intake_valid_; }
  bool hasSpeed() const { return speed_valid_; }
  float rpm() const { return rpm_; }
  float coolant() const { return coolant_; }
  float intake() const { return intake_; }
  float speed() const { return speed_; }
  uint32_t lastRpmMs() const { return last_rpm_ms_; }
  uint32_t lastCoolantMs() const { return last_coolant_ms_; }
  uint32_t lastIntakeMs() const { return last_intake_ms_; }
  uint32_t lastSpeedMs() const { return last_speed_ms_; }

  // 0100 位图的结果。known = 问到了;supported = 那一位是 1。
  // ★ 注意区分三件事,别混:
  //   known_      = 有没有拿到位图(没拿到 → 下面这些都没意义)
  //   supported_  = **ECU 自己说**支不支持(如实报告,不管我们开不开)
  //   polled_     = 我们**实际**有没有把它排进轮询表(受 -DOBD_SPEED_PID 影响)
  bool speedSupportKnown() const { return support_known_; }
  bool speedSupported() const { return speed_supported_; }
  bool speedPolled() const { return speed_polled_; }
  uint32_t supportMask() const { return support_mask_; }

  // 实测刷新率(Hz),每 1 秒结算
  float rpmHz() const { return rpm_rate_.hz; }
  float coolantHz() const { return coolant_rate_.hz; }
  float intakeHz() const { return intake_rate_.hz; }
  float speedHz() const { return speed_rate_.hz; }

private:
  enum class Phase : uint8_t { Init, QuerySupport, PollIdle, PollWait };

  // 每字段一个:累计收到的有效值,每秒把"这一秒的数"换算成 Hz
  struct RateMeter {
    uint32_t count = 0;
    uint32_t window_start_ms = 0;
    uint32_t window_count = 0;
    float hz = 0.0f;
    void tick(uint32_t now_ms) {
      if (window_start_ms == 0) { window_start_ms = now_ms; return; }
      const uint32_t dt = now_ms - window_start_ms;
      if (dt >= 1000u) {
        hz = (float)(count - window_count) * 1000.0f / (float)dt;
        window_start_ms = now_ms;
        window_count = count;
      }
    }
  };

  void drain();
  void parseLine();
  void sendCmd(const char* cmd);
  void sendRequest(uint8_t pid);
  void onPidValue(uint8_t pid, uint16_t raw);
  void onSupportedPids(uint32_t mask);
  void buildPollTable(bool with_speed);

  HardwareSerial* s_;
  Phase phase_ = Phase::Init;
  uint8_t init_step_ = 0;
  uint8_t pid_slot_ = 0;
  uint32_t last_activity_ms_ = 0;
  uint32_t last_rpm_ms_ = 0;
  uint32_t last_coolant_ms_ = 0;
  uint32_t last_intake_ms_ = 0;
  uint32_t last_speed_ms_ = 0;
  uint32_t now_ms_ = 0;
  bool rpm_valid_ = false;
  bool coolant_valid_ = false;
  bool intake_valid_ = false;
  bool speed_valid_ = false;
  float rpm_ = 0.0f;
  float coolant_ = 0.0f;
  float intake_ = 0.0f;
  float speed_ = 0.0f;

  // 轮询表:开机问过 0100 之后才定下来(见头部的说明)
  uint8_t poll_pids_[4] = {0x0C, 0x05, 0x0F, 0x00};
  uint8_t poll_count_ = 3;
  bool support_sent_ = false;
  bool support_known_ = false;
  bool speed_supported_ = false;
  bool speed_polled_ = false;
  uint32_t support_mask_ = 0;

  RateMeter rpm_rate_;
  RateMeter coolant_rate_;
  RateMeter intake_rate_;
  RateMeter speed_rate_;

  char buf_[48];
  uint8_t buf_len_ = 0;
};
