#include <Arduino.h>
#include "data_service.h"
#include "dash_display.h"
#include "dash_ui.h"
#include "ui_model.h"
#include "van_phy.h"
#include "van_replay.h"

// OBD(K 线)串口:板子和引脚定了以后在这里接上。
//   例: Serial1.begin(38400, SERIAL_8N1, OBD_RX_PIN, OBD_TX_PIN);
//       static VehicleDataService g_data(&Serial1);
// 没接 OBD 时传 nullptr,只跑假数据。
static VehicleDataService g_data(nullptr);

// VAN 物理层:现在是桩(无硬件);SN65HVD230 + VanBus 库到货后
// 换成 VanPhyRmt 实现(见 van_phy.h),数据层不用动。
static VanPhyStub g_van_phy;

static uint32_t last_ui_ms = 0;
static uint32_t last_status_ms = 0;

// 串口离线回放 VAN 帧:一行 "VAN 824 18F82710000000" 喂一帧(见 van_replay.h),
// 实车接收发器前先用抓到的帧联调,不用先焊板。
static void van_replay_poll(uint32_t now) {
  static char line[80];
  static uint8_t len = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len) {
        line[len] = '\0';
        VanPacket p{};
        if (parseVanReplayLine(line, &p, now)) {
          g_data.onVanPacket(p);
        } else {
          Serial.printf("VAN? %s\n", line);   // 解析失败回显,方便排错
        }
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  g_data.begin();
  g_van_phy.setSink(&g_data.vanSource());
  g_van_phy.begin();
  dash_ui_init();
}

void loop() {
  const uint32_t now = millis();
  g_van_phy.tick(now);   // VAN 物理层解帧 → 喂给 data_service(当前为桩)
  van_replay_poll(now);  // 串口贴帧离线回放(和物理层等价,先到的先写)
  const VehicleState st = g_data.update(now);
  dash_ui_tick(now);   // LVGL 心跳,每个循环都跑
  dash_display_poll(); // 设备上为空;pcpreview 落 BMP 帧

  if (now - last_ui_ms >= 200) {
    last_ui_ms = now;
    dash_ui_render(make_view(st, now), now);
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
