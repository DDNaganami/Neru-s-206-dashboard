#include "obd_source.h"

static const uint32_t kStepTimeoutMs  = 300;  // 每步 AT 命令等待
static const uint32_t kPollIntervalMs = 200;  // 两次 PID 请求间隔
static const uint32_t kWaitTimeoutMs  = 250;  // 请求发出后的响应等待
static const char* const kInitCmds[]  = {"ATZ", "ATE0", "ATL0", "ATH0"};

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 从 idx 处解析一个十六进制字节(跳过空格),返回 -1 表示失败
static int hexByte(const char* s, uint8_t& idx) {
  while (s[idx] == ' ' || s[idx] == '\t') ++idx;
  const int hi = hexVal(s[idx]);
  const int lo = (s[idx + 1] != '\0') ? hexVal(s[idx + 1]) : -1;
  if (hi < 0 || lo < 0) return -1;
  idx += 2;
  return (hi << 4) | lo;
}

void ObdSource::begin() {
  if (!s_) return;
  phase_ = Phase::Init;
  init_step_ = 0;
  sendCmd(kInitCmds[0]);
  last_activity_ms_ = millis();
}

void ObdSource::sendCmd(const char* cmd) {
  s_->print(cmd);
  s_->print('\r');
}

void ObdSource::sendRequest(uint8_t pid) {
  s_->print("01");
  if (pid < 0x10) s_->print('0');
  s_->print(pid, HEX);
  s_->print('\r');
}

void ObdSource::tick(uint32_t now_ms) {
  if (!s_) return;
  now_ms_ = now_ms;
  drain();

  switch (phase_) {
    case Phase::Init:
      if (now_ms - last_activity_ms_ < kStepTimeoutMs) break;
      if (++init_step_ < 4) {
        sendCmd(kInitCmds[init_step_]);
      } else {
        phase_ = Phase::PollIdle;
      }
      last_activity_ms_ = now_ms;
      break;

    case Phase::PollIdle:
      if (now_ms - last_activity_ms_ >= kPollIntervalMs) {
        sendRequest(next_pid_);
        next_pid_ = (next_pid_ == 0x0C) ? 0x05 : 0x0C;
        phase_ = Phase::PollWait;
        last_activity_ms_ = now_ms;
      }
      break;

    case Phase::PollWait:
      if (now_ms - last_activity_ms_ >= kWaitTimeoutMs) {
        phase_ = Phase::PollIdle;
        last_activity_ms_ = now_ms;
      }
      break;
  }
}

void ObdSource::drain() {
  while (s_->available()) {
    const char c = (char)s_->read();
    if (c == '\r' || c == '\n') {
      if (buf_len_) {
        buf_[buf_len_] = '\0';
        parseLine();
        buf_len_ = 0;
      }
    } else if (c >= 32 && buf_len_ < sizeof(buf_) - 1) {
      buf_[buf_len_++] = toupper(c);
    }
  }
}

// 找 "41 0C" / "41 05" 响应(ATH0 之后没有 ECU 地址头)
void ObdSource::parseLine() {
  for (uint8_t i = 0; i + 4 <= buf_len_; ++i) {
    if (buf_[i] != '4' || buf_[i + 1] != '1' || buf_[i + 2] != '0') continue;
    if (buf_[i + 3] != 'C' && buf_[i + 3] != '5') continue;

    const uint8_t pid = (buf_[i + 3] == 'C') ? 0x0C : 0x05;
    uint8_t j = i + 4;
    const int a = hexByte(buf_, j);
    const int b = hexByte(buf_, j);
    if (a < 0 || b < 0) return;
    onPidValue(pid, (uint16_t)(a * 256u + b));
    return;
  }
}

void ObdSource::onPidValue(uint8_t pid, uint16_t raw) {
  if (pid == 0x0C) {
    rpm_ = raw / 4.0f;              // 010C: (A*256+B)/4 rpm
    rpm_valid_ = true;
  } else if (pid == 0x05) {
    coolant_ = (float)raw - 40.0f;  // 0105: A-40 °C
    coolant_valid_ = true;
  }
  last_update_ms_ = now_ms_;
}
