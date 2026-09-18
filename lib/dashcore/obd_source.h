#pragma once
#include <Arduino.h>

// K 线 OBD 数据源(ELM327, ISO 9141-2 / KWP2000)。
// 非阻塞状态机:初始化 AT 序列 → 问一次 0100(支持的 PID 位图)→
// 按"快路每轮、慢路每 N 轮"的节奏轮询转速/车速/水温/进气温度(见下面的节奏说明)。
// 波特率由调用方在传入的串口上配好(常见 38400)。
// 每路各自记录最后更新时间,供上层按字段独立超时回退。
// 文本解析在 obd_protocol.h/.cpp(纯函数,宿主机可测)。
//
// ★ 为什么加 010F(进气温度):
//   2026-09 用户用蓝牙 ELM327 + EOBD 实测:206 CC 的 ECU 支持这一项,
//   于是把它做成速度表上的副表(与转速表上的水温表左右对称)。
//   同一个 01 服务、同样单字节 A-40,代价只是**多占一个轮询时隙**。
//
// ★ 轮询节奏:快路每轮都问,慢路每 kSlowEveryNTurns 轮问一次
//   (2026-09-18 用户确认:"进气温度和水温确实可以先降频,这两参数确实不需要
//    太快速的更新温度")。
//     快路 = 0x0C 转速 (+ 0x0D 车速,ECU 支持时)
//     慢路 = 0x05 水温、0x0F 进气温度
//   一个"周期"的请求序列(kSlowEveryNTurns=4、快路两路时):
//     0C 0D 05 | 0C 0D 0F | 0C 0D | 0C 0D
//   —— 快路每周期各 4 次,慢路各 1 次;K 线那条管子的预算就是这样分配的。
//
// ★ 顺带修掉一个更大的浪费:原先"等响应"那一格只会等满 250ms 超时才走,
//   **收到响应也不提前离开** → 每个请求实际花 250(等)+200(间隔)=450ms,
//   只有 ~2.2 请求/秒,比降频本身省下的还多。现在收到**当次请求的**响应当即
//   回到间隔态(靠 waiting_pid_ 认领,避免把上一轮的残留响应当成这一轮的);
//   没响应(ECU 不支持)才走超时那一条 —— 这正是超时值必须留着的原因。
//   间隔也从 200ms 收到 80ms(实测值等车上 SRC-Hz 出来再定;调坏了会表现为
//   丢响应→走超时→刷新率不升反降,所以它是"用 SRC-Hz 说话"的旋钮之一)。
//
// ★ 实测刷新率(Hz):每个字段各自统计,**每 1 秒结算一次**。
//   为什么必须实测:`kPollIntervalMs`/`kSlowEveryNTurns` 只是我们这一侧的名义值,
//   ELM327 自己的转换开销、ECU 的响应快慢、超时重试都在里面,算不出来。
//   到了车上(或接上仿真器)看日志里那四个数就能定"还要不要再调"。
//
// ★ 010D 车速:**先问 0100 位图,ECU 说支持才排进快路**(见下面 onSupportedPids)。
//   拿不到位图(某些 clone 不认这条)就只跑 0C 一路快路,不猜、不试。
//   `-DOBD_SPEED_PID=0` 可以强制关掉车速那一路(用来对比"加/不加"的实测刷新率)。
//   上层优先级见 data_service.h:车速 Van > Obd > Sim。
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

  // 慢路降频倍数:快路每轮都问,慢路每 N 轮问一次。
  // 公开出来是为了让用例能直接算"一个周期应该有几个请求"(见 test_obd_source.cpp)。
  static constexpr uint8_t kSlowEveryNTurns = 4;

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
  uint8_t slot_ = 0;             // 当前在周期序列里的第几格
  uint8_t waiting_pid_ = 0;      // 这一格问的是谁(用来认领响应、提前离开等待态)
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

  // 一个周期的请求序列(快路每轮各一次 + 慢路每周期各一次),见文件头部的图。
  // 最大长度:快路 2 路 × N 轮 + 慢路 2 个。
  static constexpr uint8_t kScheduleMax = 2 * kSlowEveryNTurns + 2;
  uint8_t schedule_[kScheduleMax] = {0x0C, 0x05, 0x0F, 0};
  uint8_t schedule_count_ = 3;
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
