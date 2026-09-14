#include <Arduino.h>
#include "data_service.h"
#include "dash_ui.h"
#include "ui_model.h"

// OBD(K 线)串口:板子和引脚定了以后在这里接上。
//   例: Serial1.begin(38400, SERIAL_8N1, OBD_RX_PIN, OBD_TX_PIN);
//       static VehicleDataService g_data(&Serial1);
// 没接 OBD 时传 nullptr,只跑假数据。
static VehicleDataService g_data(nullptr);
static uint32_t last_ui_ms = 0;
static uint32_t last_status_ms = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  g_data.begin();
  dash_ui_init();
}

void loop() {
  const uint32_t now = millis();
  const VehicleState st = g_data.update(now);
  dash_ui_tick(now);   // LVGL 心跳,每个循环都跑

  if (now - last_ui_ms >= 200) {
    last_ui_ms = now;
    dash_ui_render(make_view(st, now));
  }

  // 每 5 秒打印各字段当前由哪个源供给(调试用)
  if (now - last_status_ms >= 5000) {
    last_status_ms = now;
    const DataSourceStatus& s = g_data.status();
    Serial.printf("SRC speed=%s rpm=%s coolant=%s\n",
                  fieldSourceName(s.speed),
                  fieldSourceName(s.rpm),
                  fieldSourceName(s.coolant));
  }
}
