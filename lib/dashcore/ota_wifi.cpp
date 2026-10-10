#include "ota_wifi.h"

#if defined(ARDUINO) && defined(DASH_OTA)

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "dash_log.h"

namespace dashcore {
namespace {

// ★ 标志放 RTC_NOINIT：软件复位保留、**断电丢失**（理由见头文件那段）。
//   `magic` 是必需的：冷启动时这块内存是**垃圾值**，只判 `flag == 1` 会在
//   上电瞬间随机进 OTA 模式（这类 bug 极难查，所以第一版就带上）。
RTC_NOINIT_ATTR uint32_t s_magic;
RTC_NOINIT_ATTR uint32_t s_request;
constexpr uint32_t kMagic  = 0x20607A00u;   // "206 OTA 00"
constexpr uint32_t kReqOn  = 1u;
constexpr uint32_t kReqOff = 0u;

// 不舒服就自己退出的时限（默认 10 分钟）。可被 `-DDASH_OTA_TIMEOUT_MS=` 覆盖。
#ifndef DASH_OTA_TIMEOUT_MS
#define DASH_OTA_TIMEOUT_MS (10u * 60u * 1000u)
#endif

WebServer* s_server  = nullptr;
bool       s_started = false;   // 收到过 UPLOAD_FILE_START
bool       s_ok      = false;   // Update.end() 成功
uint32_t   s_written = 0;

// 上传页：故意做到最小 —— 一个 file input + 一个按钮，手机浏览器就够，
// 不依赖网络（板子自己是 AP），也不依赖任何 App。
const char kPage[] PROGMEM = R"HTML(<!doctype html><html lang="zh"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>206Dash 无线刷写</title><style>
body{font-family:system-ui,sans-serif;margin:1.6rem;max-width:40rem;line-height:1.6}
code{background:#eee;padding:.1rem .3rem;border-radius:3px}
button{padding:.6rem 1.2rem;font-size:1.05rem}
</style></head><body>
<h2>206Dash 固件无线写入</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
  <p><input type="file" name="firmware" accept=".bin" required></p>
  <p><button type="submit">上传并重启</button></p>
</form>
<p>选 <code>firmware.bin</code>（<code>.pio/build/&lt;env&gt;/firmware.bin</code>），
<strong>不要</strong>选 <code>firmware.factory.bin</code> 或 theme/image 的分区文件。</p>
<p>上传约 1.7 MB、二三十秒。<strong>期间不要断电</strong>；写完板子自己重启。</p>
</body></html>)HTML";

void handleRoot() {
  if (s_server != nullptr) s_server->send_P(200, "text/html; charset=utf-8", kPage);
}

// ★ 2026-09-29：`GET /status` —— 给"206Dash 开发台"那个本地应用看的。
//   为什么要它：应用要能一眼说清"连上的是哪块板、跑的是哪个槽、还剩多少 RAM"，
//   而不是让用户去数串口日志。返回 JSON，字段都是**叶子值**（不序列化任何活对象）。
//   ★ 这条只在 OTA 模式下存在（AP 只在 OTA 模式起）⇒ 平时不占任何东西。
void handleStatus() {
  if (s_server == nullptr) return;
  const esp_partition_t* run  = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
#if defined(LINK_ROLE) && (LINK_ROLE == 1)
  const char* role = "MASTER";
#else
  const char* role = "SLAVE";
#endif
  char body[320];
  snprintf(body, sizeof(body),
           "{\"fw\":\"%s %s\",\"role\":\"%s\",\"running\":\"%s\",\"target\":\"%s\","
           "\"ota_ready\":%s,\"heap_kb\":%u,\"uptime_ms\":%lu,\"ota_mode\":true}",
           __DATE__, __TIME__, role,
           run ? run->label : "?", next ? next->label : "(none)",
           (next && next != run) ? "true" : "false",
           (unsigned)(ESP.getFreeHeap() / 1024u), (unsigned long)millis());
  s_server->send(200, "application/json; charset=utf-8", body);
}

// 上传回调（分片进来；每片都要写进另一个 OTA 槽）。
void handleUpload() {
  if (s_server == nullptr) return;
  HTTPUpload& up = s_server->upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      s_started = true;
      s_ok      = false;
      s_written = 0;
      dash_logf("ota: 开始接收 %s\n", up.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        dash_logf("ota: ★ Update.begin 失败 —— 多半是分区表没有第二个槽\n");
        Update.printError(Serial);
      }
      break;
    case UPLOAD_FILE_WRITE:
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        dash_logf("ota: ★ 写入失败（已收 %u B）\n", (unsigned)s_written);
        Update.printError(Serial);
      }
      // 每 64KB 报一次进度（对数友好，也不至于把串口刷爆）
      if ((s_written / 65536u) != ((s_written + up.currentSize) / 65536u)) {
        dash_logf("ota: 已收 %u KB / heap=%uKB\n",
                  (unsigned)((s_written + up.currentSize) / 1024u),
                  (unsigned)(ESP.getFreeHeap() / 1024u));
      }
      s_written += up.currentSize;
      break;
    case UPLOAD_FILE_END:
      s_ok = (!Update.hasError()) && Update.end(true);
      dash_logf("ota: 收完 %u B ⇒ %s\n", (unsigned)s_written,
                s_ok ? "写入成功,准备重启" : "★ 写入失败(见上面)");
      break;
    default:
      break;
  }
}

// POST 收尾（应答 + 成功就重启）。★ 这一步必须**先回包再重启**，否则手机那边
// 只会看到"连接被重置"，用户以为失败了。
void handleUpdateDone() {
  if (s_server == nullptr) return;
  const bool ok = s_ok;
  s_server->sendHeader("Connection", "close");
  s_server->send(200, "text/plain; charset=utf-8",
                 ok ? "OK: 固件已写入,正在重启…" : "FAILED: 见串口日志");
  if (ok) {
    delay(400);
    ESP.restart();
  }
}

}  // namespace

bool ota_wifi_supported() { return true; }

void ota_wifi_request() {
  s_magic   = kMagic;
  s_request = kReqOn;
  dash_logf("ota: 收到 `u` ⇒ 重启进 OTA 模式（只起 AP，链路/OBD 都不启动）\n");
  delay(200);
  ESP.restart();
}

bool ota_wifi_take_request() {
  if (s_magic != kMagic) return false;          // 冷启动：RTC 内存是垃圾值
  if (s_request != kReqOn) return false;
  s_request = kReqOff;                          // 只认一次，见头文件那段说明
  return true;
}

void ota_wifi_run() {
  // ① 先问"这张分区表到底支不支持 A/B"。不支持就立刻退回正常固件 ——
  //    把板子留在 AP 模式里等一个永远写不进去的镜像，比直接说"不行"糟得多。
  const esp_partition_t* run  = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  dash_logf("ota: 当前槽 %s @0x%06x | 目标槽 %s @0x%06x (%u KB)\n",
            run ? run->label : "?", (unsigned)(run ? run->address : 0u),
            next ? next->label : "(无)", (unsigned)(next ? next->address : 0u),
            (unsigned)(next ? (next->size / 1024u) : 0u));
  if (next == nullptr || next == run) {
    dash_logf("ota: ★ 这张分区表没有第二个 OTA 槽 ⇒ 无线刷写不可用。"
              "先用 USB 刷一次带 OTA 的分区表（见 docs/OTA.md）\n");
    delay(1200);
    ESP.restart();
  }

  // ② 起 AP + 上传页。SSID 带角色，车里两块板同时进 OTA 也分得清。
  char ssid[32];
#if defined(LINK_ROLE) && (LINK_ROLE == 1)
  snprintf(ssid, sizeof(ssid), "206dash-MASTER");
#else
  snprintf(ssid, sizeof(ssid), "206dash-SLAVE");
#endif
  const uint32_t heap_before = (uint32_t)ESP.getFreeHeap();
  WiFi.persistent(false);            // ★ 别把 AP 配置写进 NVS（那是用户数据区）
  WiFi.mode(WIFI_AP);
  bool ap_ok = false;
#if defined(DASH_OTA_AP_PASS)
  ap_ok = WiFi.softAP(ssid, DASH_OTA_AP_PASS);
  const char* ap_note = "WPA2（-DDASH_OTA_AP_PASS）";
#else
  ap_ok = WiFi.softAP(ssid);         // 开放 AP：车库/车内短时间用，10 分钟自己退
  const char* ap_note = "开放(无密码)";
#endif
  const IPAddress ip = WiFi.softAPIP();
  dash_logf("ota: AP `%s` %s 起来了=%d  IP=http://%s/  heap %u→%u KB\n",
            ssid, ap_note, (int)ap_ok, ip.toString().c_str(),
            (unsigned)(heap_before / 1024u), (unsigned)(ESP.getFreeHeap() / 1024u));
  if (!ap_ok) {
    dash_logf("ota: ★ AP 没起来 ⇒ 重启回正常固件\n");
    delay(800);
    ESP.restart();
  }

  s_server = new WebServer(80);
  // ★★ 2026-09-29：**必须开 CORS** —— 否则"206Dash 开发台"（跑在 http://127.0.0.1）
  //   没法把固件 POST 到 http://192.168.4.1/update（跨源被浏览器拦下），
  //   也没法读 /status。`enableCORS(true)` 会带上 Access-Control-Allow-Origin: *
  //   并处理 OPTIONS 预检。
  //   ★ 安全口径（写清楚）：这条 AP 只在 OTA 模式下存在、最长 10 分钟、无外网出口，
  //     所以"*"在这里的暴露面就是"同一个热点里的人能刷这次固件" —— 与板子自带的
  //     上传页等价（那个页面本来也是开放的）。要更紧就加 `-DDASH_OTA_AP_PASS`。
  s_server->enableCORS(true);
  s_server->on("/", HTTP_GET, handleRoot);
  s_server->on("/status", HTTP_GET, handleStatus);
  s_server->on("/update", HTTP_POST, handleUpdateDone, handleUpload);
  s_server->onNotFound(handleRoot);
  s_server->begin();
  dash_logf("ota: 等上传…（%u 分钟没人来就自己退回正常固件）\n",
            (unsigned)(DASH_OTA_TIMEOUT_MS / 60000u));
  dash_logf("ota: 手机连上 `%s` → 浏览器开 http://%s/ → 选 firmware.bin\n",
            ssid, ip.toString().c_str());

  // ③ 非阻塞地服务请求，直到超时。★ `delay(2)` 是必需的：它让出 CPU 给空闲任务，
  //    否则任务看门狗会把这块板子当死机复位（我们此刻还没进 loop()）。
  const uint32_t deadline = millis() + (uint32_t)DASH_OTA_TIMEOUT_MS;
  for (;;) {
    s_server->handleClient();
    delay(2);
    if ((int32_t)(millis() - deadline) >= 0) {
      dash_logf("ota: 超时（%u 分钟没有任何上传）⇒ 重启回正常固件\n",
                (unsigned)(DASH_OTA_TIMEOUT_MS / 60000u));
      delay(300);
      ESP.restart();
    }
    // 上传失败之后别立刻重启：让手机上的页面把 FAILED 收到，也让人有时间看串口。
    if (s_started && !s_ok && Update.hasError()) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        dash_logf("ota: ★ 这次上传失败；板子会继续等（可以重试上传），"
                  "超时后自动退回正常固件\n");
      }
    }
  }
}

}  // namespace dashcore

#endif  // ARDUINO && DASH_OTA
