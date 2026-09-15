#pragma once
#include <Arduino.h>

// K 线 OBD 数据源(ELM327, ISO 9141-2 / KWP2000)。
// 非阻塞状态机:初始化 AT 序列后轮流请求 010C(转速)/ 0105(水温)。
// 波特率由调用方在传入的串口上配好(常见 38400)。
// 转速/水温各自记录最后更新时间,供上层按字段独立超时回退。
// 文本解析在 obd_protocol.h/.cpp(纯函数,宿主机可测)。
class ObdSource {
public:
  explicit ObdSource(HardwareSerial* serial = nullptr) : s_(serial) {}

  void begin();
  void tick(uint32_t now_ms);

  bool enabled() const { return s_ != nullptr; }
  bool hasRpm() const { return rpm_valid_; }
  bool hasCoolant() const { return coolant_valid_; }
  float rpm() const { return rpm_; }
  float coolant() const { return coolant_; }
  uint32_t lastRpmMs() const { return last_rpm_ms_; }
  uint32_t lastCoolantMs() const { return last_coolant_ms_; }

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
  uint8_t next_pid_ = 0x0C;      // 0x0C 转速 / 0x05 水温,轮流
  uint32_t last_activity_ms_ = 0;
  uint32_t last_rpm_ms_ = 0;
  uint32_t last_coolant_ms_ = 0;
  uint32_t now_ms_ = 0;
  bool rpm_valid_ = false;
  bool coolant_valid_ = false;
  float rpm_ = 0.0f;
  float coolant_ = 0.0f;
  char buf_[48];
  uint8_t buf_len_ = 0;
};
