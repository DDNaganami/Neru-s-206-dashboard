#include "nav_source_ble.h"

#if defined(DASH_NAV_BLE)

#include <NimBLEDevice.h>

#include "dash_log.h"
#include "nav_source.h"

namespace {

const char* kDevName = "206dash-nav";
const char* kSvcUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const char* kRxUuid  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";

// 手机 write-no-response ⇒ 可能连续来很多包：环形缓冲 512 B（够十几帧）+ 两个游标。
constexpr size_t kRingSize = 512u;
uint8_t  g_ring[kRingSize];
size_t   g_head = 0;   // 写（BLE 任务）
size_t   g_tail = 0;   // 读（主循环）
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

NavSource g_src;
NavState  g_state;
uint32_t  g_drops = 0;   // 环满丢弃的字节数（只记数，不刷日志）

// ★ NimBLE 2.x 回调风格：NimBLEServerCallbacks::onConnect/onDisconnect、
//   NimBLECharacteristicCallbacks::onWrite(NimBLECharacteristic*, NimBLEAttValue)。
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    (void)info;
    auto v = c->getValue();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
    const size_t n = v.size();
    portENTER_CRITICAL(&g_mux);
    for (size_t i = 0; i < n; ++i) {
      const size_t next = (g_head + 1u) % kRingSize;
      if (next == g_tail) { ++g_drops; break; }   // 环满：丢这一包剩下的
      g_ring[g_head] = p[i];
      g_head = next;
    }
    portEXIT_CRITICAL(&g_mux);
  }
};

class SrvCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    (void)s; (void)info;
    dash_logf("nav: ble central 已连接\\n");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    (void)s; (void)info; (void)reason;
    dash_logf("nav: ble 断开，重新广播\\n");
    NimBLEDevice::startAdvertising();
  }
};

RxCallbacks g_rx_cb;
SrvCallbacks g_srv_cb;
bool g_started = false;

}  // namespace

bool nav_source_ble_start() {
  if (g_started) return true;
  NimBLEDevice::init(kDevName);
  NimBLEDevice::setMTU(185);

  NimBLEServer* srv = NimBLEDevice::createServer();
  if (!srv) return false;
  srv->setCallbacks(&g_srv_cb);

  NimBLEService* svc = srv->createService(kSvcUuid);
  if (!svc) return false;
  NimBLECharacteristic* rx = svc->createCharacteristic(
      kRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  if (!rx) return false;
  rx->setCallbacks(&g_rx_cb);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(kSvcUuid);
  adv->setName(kDevName);
  adv->enableScanResponse(true);
  adv->start();

  g_started = true;
  dash_logf("nav: BLE peripheral 起来了（name=%s）\\n", kDevName);
  return true;
}

size_t nav_source_ble_tick(uint32_t now_ms) {
  if (!g_started) return 0;
  // 把环里的字节搬到栈缓冲再出临界区解析（临界区内只做 memcpy）
  uint8_t chunk[64];
  size_t total = 0;
  for (;;) {
    size_t n = 0;
    portENTER_CRITICAL(&g_mux);
    while (g_tail != g_head && n < sizeof(chunk)) {
      chunk[n++] = g_ring[g_tail];
      g_tail = (g_tail + 1u) % kRingSize;
    }
    portEXIT_CRITICAL(&g_mux);
    if (n == 0) break;
    nav_source_push(g_src, chunk, n, now_ms, g_state);
    total += n;
  }
  return total;
}

const NavState& nav_source_ble_state() { return g_state; }

#endif  // DASH_NAV_BLE
