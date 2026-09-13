#include "dash_ui.h"
#include <Arduino.h>

static const char* gear_name(Gear g) {
  switch (g) {
    case Gear::P: return "P";
    case Gear::R: return "R";
    case Gear::N: return "N";
    case Gear::D: return "D";
    case Gear::M3: return "3";
    case Gear::M2: return "2";
    case Gear::M1: return "1";
  }
  return "?";
}

void dash_ui_init() {}

void dash_ui_render(const ArcDashView& v) {
  Serial.printf("ARC spd=%3.0f%% rpm=%3.0f%%  %s  center=%3.0f%%\n",
                v.speed_t * 100.0f,
                v.rpm_t * 100.0f,
                gear_name(v.gear),
                v.center_phase * 100.0f);
}