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
class ObdSource {
public:
  explicit ObdSource(HardwareSerial* serial = nullptr) : s_(serial) {}

  void begin();
  void tick(uint32_t now_ms);

  bool enabled() const { return s_ != nullptr; }
  bool hasRpm() const { return rpm_valid_; }
  bool hasCoolant() const { return coolant_valid_; }
  bool hasIntake() const { return intake_valid_; }
  float rpm() const { return rpm_; }
  float coolant() const { return coolant_; }
  float intake() const { return intake_; }
  uint32_t lastRpmMs() const { return last_rpm_ms_; }
  uint32_t lastCoolantMs() const { return last_coolant_ms_; }
  uint32_t lastIntakeMs() const { return last_intake_ms_; }

private:
  enum class Phase : uint8_t { Init, PollIdle, PollWait };

  void drain();
  void parseLine();
  void sendCmd(const char* cmd);
  void sendRequest(uint8_t pid);
  void onPidValue(uint8_t pid, uint16_t raw);

  HardwareSerial* s_;
  Phase phase_ = Phase::Init;
  uint8_t init_step_ = 0;
  uint8_t pid_slot_ = 0;         // 0=010C 转速 / 1=0105 水温 / 2=010F 进气温度
  uint32_t last_activity_ms_ = 0;
  uint32_t last_rpm_ms_ = 0;
  uint32_t last_coolant_ms_ = 0;
  uint32_t last_intake_ms_ = 0;
  uint32_t now_ms_ = 0;
  bool rpm_valid_ = false;
  bool coolant_valid_ = false;
  bool intake_valid_ = false;
  float rpm_ = 0.0f;
  float coolant_ = 0.0f;
  float intake_ = 0.0f;
  char buf_[48];
  uint8_t buf_len_ = 0;
};
