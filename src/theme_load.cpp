#include "theme_store.h"
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

// ============================================================
// 主题的"取文件"部分:设备端从 flash 主题分区读,宿主机从 THEME_FILE 读。
//
// 解析/钳制/默认值都在 lib/themetool/(那份是纯逻辑,可在宿主机单测)。
// 这里只负责把字节拿到手 —— 它依赖平台(esp_partition / stdio),
// 不适合放进要单测的库里。
//
// ★ 降级路径是刻意的:任何失败(分区不存在、没刷过、JSON 坏了)都只打印
//   一行日志并**继续用默认主题**。新手最容易卡在"刷了主题反而黑屏",
//   所以固件永远不因主题缺失而跑不起来。
// ============================================================

#if defined(ARDUINO)

#include <Arduino.h>
#include "esp_partition.h"

// 把主题文件那一段字节读进 buf（NUL 结尾），返回长度。
// ★ theme_load() 与 theme_load_alerts() 共用它 —— 两处各写一遍读分区/判空/
//   截断的逻辑，迟早只改一处（这正是 2026-09-27 那单里"两个入口绕过关系不同"
//   那类事故的成因）。返回 0 = 没有分区 / 读失败 / 从没刷过。
static uint32_t read_theme_blob(char* buf, uint32_t cap) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40,
      THEME_PARTITION_LABEL);
  if (!part) return 0;

  const esp_err_t err = esp_partition_read(part, 0, buf, cap - 1);
  if (err != ESP_OK) return 0;

  // 分区里是 0xFF 说明从没刷过主题
  uint32_t len = 0;
  while (len < cap - 1 && buf[len] != '\0' && (uint8_t)buf[len] != 0xFF) ++len;
  buf[len] = '\0';
  return len;
}

bool theme_load() {
  // 静态缓冲:整块计进 .bss。THEME_MAX_BYTES 的取值直接决定 DRAM 占用,
  // 别随手放大 —— 见 lib/themetool/theme_store.h 的说明。
  static char buf[THEME_MAX_BYTES];
  const uint32_t len = read_theme_blob(buf, sizeof(buf));
  if (len == 0) {
    dash_logf("theme: 没有可读的主题文件(无分区/空/读失败),用默认主题\n");
    return false;
  }

  Theme* slot = nullptr;
  theme_loaded_slot(&slot);
  if (!theme_parse_json(buf, len, *slot)) {
    dash_logf("theme: 解析失败,用默认主题\n");
    return false;
  }
  theme_use_loaded();
  dash_logf("theme: 已加载 (%u 字节, bg=0x%06X, face=%d)\n",
                (unsigned)len, (unsigned)g_theme.bg_color,
                (int)g_theme.face_size);
  return true;
}

// 同一份文件里的 `alerts` 段（2026-09-27）。返回 false = 没有分区/空/坏了/没这一段。
bool theme_load_alerts(AlertsConfig& cfg) {
  static char buf[THEME_MAX_BYTES];
  const uint32_t len = read_theme_blob(buf, sizeof(buf));
  if (len == 0) return false;
  return theme_parse_alerts_json(buf, len, cfg);
}

#else  // 宿主机(pcpreview):从文件读,便于不上板就预览主题

#include <stdio.h>
#include <stdlib.h>

// 与设备端同名同形的那一个（见上面 ARDUINO 分支里的说明）。
static uint32_t read_theme_blob(char* buf, uint32_t cap) {
  const char* path = getenv("THEME_FILE");
  if (!path || !*path) path = "theme.json";
  FILE* f = fopen(path, "rb");
  if (!f) return 0;   // 没有主题文件是正常情况,用默认值
  const size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = '\0';
  return (uint32_t)n;
}

bool theme_load() {
  static char buf[THEME_MAX_BYTES];
  const uint32_t n = read_theme_blob(buf, sizeof(buf));
  if (n == 0) return false;

  Theme* slot = nullptr;
  theme_loaded_slot(&slot);
  if (!theme_parse_json(buf, n, *slot)) {
    fprintf(stderr, "theme: 解析失败,用默认主题\n");
    return false;
  }
  theme_use_loaded();
  fprintf(stderr, "theme: 已加载 (%u 字节)\n", (unsigned)n);
  return true;
}

bool theme_load_alerts(AlertsConfig& cfg) {
  static char buf[THEME_MAX_BYTES];
  const uint32_t n = read_theme_blob(buf, sizeof(buf));
  if (n == 0) return false;
  return theme_parse_alerts_json(buf, n, cfg);
}

#endif
