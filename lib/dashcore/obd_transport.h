#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ============================================================================
// ObdTransport —— `ObdSource` 与"物理链路"之间那一层（2026-09-27 抽出来）
// ============================================================================
//
// 为什么要这一层：
//   原来 `ObdSource` 直接持有 `HardwareSerial*`，8 处调用把它焊死在串口上。
//   而 **2.8C 这块板上 UART 那条路物理上没了** —— RGB 并口把 GPIO17/18 占了
//   （正是 `OBD_RX_PIN`/`OBD_TX_PIN` 的默认值），所以 OBD 只能走 **BLE**
//   （诊断头 `OBDBLE`，GATT 实测结构见 `docs/BLE-OBD.md`）。
//   把传输抽出来之后：**问答状态机、PID 轮询表、应答解析一个字都不用改**，
//   换的只是"字节从哪儿来、往哪儿去"。
//
// ★ 这一层刻意做得极小 —— 只有 4 个方法，因为 `ObdSource` 实际就用了这么多：
//     · 写：`print(cmd)` / `print('\\r')` / `print(pid, HEX)`
//     · 读：`available()` / `read()`
//   `start()` 只是把"打开端口/BLE 连接"这件事挪进来（原来的 `Serial1.begin()`）。
//
// ★ 为什么用虚基类而不是模板：
//   主板同一份固件里可能**两个传输都要**（UART 兜底 + BLE 主用），模板会把
//   `ObdSource` 变成两个类型、两个状态机;而虚函数在这里的开销是每次请求几次
//   间接调用 —— 对一个"一问一答几百毫秒"的 K 线设备来说完全可以忽略。
//
// ★ 实现必须遵守的三条（否则上层会出难查的错）：
//   1. `available()`/`read()` **绝不阻塞**（上层在主循环里轮询它）;
//   2. `write()` 只负责"交出去"，可以内部排队（BLE 写是异步的），但**不能丢**;
//   3. `start()` 返回 false 表示"这条链路现在没戏"（串口打不开 / BLE 连不上）——
//      上层据此把 `enabled()` 判成 false，从而整条 OBD 路回退（照旧由
//      `data_service` 按字段超时兜底），**不会假装有数据**。
class ObdTransport {
public:
  virtual ~ObdTransport() {}

  // 打开/连接。幂等：重复调用应当安全。返回 false = 这条链路用不了。
  virtual bool start() = 0;

  // 链路当前是否可用（BLE 断开后就该变 false，上层据此回退）
  virtual bool connected() const = 0;

  // 写。只要求"最终会送出去"，允许内部排队。
  virtual void write(const char* s) = 0;
  virtual void write(char c) = 0;

  // 读。`available()` 不阻塞；`read()` 只在 `available() > 0` 时才该被调用。
  virtual int  available() = 0;
  virtual int  read() = 0;

  // 便捷：写一个字节的十六进制（大写两位）。原来是 Arduino 的 `print(pid, HEX)`，
  // 但那个重载是 HardwareSerial 特有的，传输层不能依赖 Arduino 的 Print 类
  // —— 所以在这儿补一个，语义与 `print(v, HEX)` 一致（**不带** 0x 前缀、
  // **不做**两位补零）。
  void writeHex(uint8_t v) {
    static const char* k = "0123456789ABCDEF";
    char b[2] = { k[(v >> 4) & 0x0F], k[v & 0x0F] };
    write(b[0]);
    write(b[1]);
  }
};
