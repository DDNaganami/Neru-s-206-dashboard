#include "nav_source_ble.h"

#if defined(DASH_NAV_BLE)

#include <NimBLEDevice.h>

#include "dash_log.h"
#include "nav_source.h"
#include "nav_img.h"
#include <esp_heap_caps.h>

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
NavImgRx g_img_rx;                 // 地图图片重组（缓冲放 PSRAM）
uint8_t* g_img_buf = nullptr;
uint32_t g_img_seen = 0;
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
    // 实验 2（2026-11-22）：横纹与射频事件密度相关 =>
    //   ① 连上就停广播（少一类射频活动）；
    //   ② 连接间隔 60~90ms、latency 2（事件少 3~4 倍；导航 1 Hz 推送不敏感）。
    if (s) {
      NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
      if (a) a->stop();
      s->updateConnParams(info.getConnHandle(), 48, 72, 2, 500);   // void 返回
      dash_logf("nav: connected; adv stopped; conn 60~90ms/lat2\n");
    }
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
  NimBLEDevice::setPower(ESP_PWR_LVL_N12);   // 实验 2：-12 dBm

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

  // 地图图片缓冲：32 KB 放 PSRAM（内部 RAM 只剩 ~50KB，别抢）
  g_img_buf = (uint8_t*)heap_caps_malloc(kNavImgBufferMax, MALLOC_CAP_SPIRAM);
  if (g_img_buf) {
    nav_img_rx_init(g_img_rx, g_img_buf, kNavImgBufferMax);
    g_src.img = &g_img_rx;
    dash_logf("img: 重组缓冲 %u B @PSRAM\n", (unsigned)kNavImgBufferMax);
  } else {
    dash_logf("img: PSRAM 分配失败，图片通道关闭\n");
  }

  g_started = true;
  dash_logf("nav: BLE peripheral 起来了（name=%s）\\n", kDevName);
  return true;
}

size_t nav_source_ble_tick(uint32_t now_ms) {
  if (!g_started) return 0;
  // 把环里的字节搬到栈缓冲再出临界区解析（临界区内只做 memcpy）
  uint8_t chunk[64];
  size_t frames = 0;
  for (;;) {
    size_t n = 0;
    portENTER_CRITICAL(&g_mux);
    while (g_tail != g_head && n < sizeof(chunk)) {
      chunk[n++] = g_ring[g_tail];
      g_tail = (g_tail + 1u) % kRingSize;
    }
    portEXIT_CRITICAL(&g_mux);
    if (n == 0) break;
    frames += nav_source_push(g_src, chunk, n, now_ms, g_state);   // 返回"上账的帧数"
  }
  // ★★ 2026-11-22 实测补上：超时降级必须每拍都跑，否则手机停发后屏上会**永远停在最后一帧**
  //   （那正是 nav_state.h 要避免的事：宁可不显示，也不显示过期指令）。
  {
    static NavTimeouts t;
    nav_state_tick(g_state, now_ms, t);
  }

  // 图片整图收齐就报一行（大小/尺寸由解码那一步再打）
  if (g_src.img_ready != g_img_seen) {
    g_img_seen = g_src.img_ready;
    dash_logf("img: ok %u bytes (jpg), total=%u\n",
              (unsigned)(g_img_rx.len), (unsigned)g_img_seen);
  }

  // 每秒最多一行：能一眼看出"帧到底到没到板子上"（BLE 连上 ≠ 帧能解析）
  {
    static uint32_t last_log_ms = 0;
    if ((uint32_t)(now_ms - last_log_ms) >= 1000u) {
      last_log_ms = now_ms;
      dash_logf("nav: rx frames=%u img=%u bad=%u ready=%u last(turn=%u dist=%u spd=%u hdg=%u) crc_err=%u\n",
                (unsigned)g_src.frames, (unsigned)g_src.img_chunks, (unsigned)g_src.img_bad,
                (unsigned)g_src.img_ready, (unsigned)g_state.turn, (unsigned)g_state.dist_m,
                (unsigned)g_state.speed_kph, (unsigned)g_state.heading_cdeg,
                (unsigned)g_src.crc_errors);
    }
  }
  return frames;
}

const uint8_t* nav_source_ble_image(size_t* len, uint32_t* gen) {
  if (!g_img_buf || !g_img_rx.complete) return nullptr;
  if (len) *len = g_img_rx.len;
  if (gen) *gen = g_src.img_ready;
  return g_img_buf;
}

const NavState& nav_source_ble_state() { return g_state; }

#endif  // DASH_NAV_BLE
