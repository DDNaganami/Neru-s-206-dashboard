#include <Arduino.h>
#include "vehicle_state.h"
#include "sim_source.h"
#include "dash_ui.h"
#include "ui_model.h"

static VehicleState g_state;
static uint32_t last_ui_ms = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  dash_ui_init();
}

void loop() {
  const uint32_t now = millis();
  sim_update(g_state, now);

  if (now - last_ui_ms >= 200) {
    last_ui_ms = now;
    dash_ui_render(make_view(g_state, now));
  }
}
