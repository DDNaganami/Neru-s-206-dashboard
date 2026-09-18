#include "theme_store.h"
#include "dash_log.h"   // 日志同时打到 USB-CDC 与 UART0(见文件头说明)

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

bool theme_load() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40,
      THEME_PARTITION_LABEL);
  if (!part) {
    dash_logf("theme: 没有 theme 分区,用默认主题\n");
    return false;
  }

  // 静态缓冲:整块计进 .bss。THEME_MAX_BYTES 的取值直接决定 DRAM 占用,
  // 别随手放大 —— 见 lib/themetool/theme_store.h 的说明。
  static char buf[THEME_MAX_BYTES];
  const esp_err_t err = esp_partition_read(part, 0, buf, sizeof(buf) - 1);
  if (err != ESP_OK) {
    dash_logf("theme: 读分区失败 (%d),用默认主题\n", (int)err);
    return false;
  }

  // 分区里是 0xFF 说明从没刷过主题
  uint32_t len = 0;
  while (len < sizeof(buf) - 1 && buf[len] != '\0' &&
         (uint8_t)buf[len] != 0xFF) {
    ++len;
  }
  buf[len] = '\0';
  if (len == 0) {
    dash_logf("theme: 分区为空,用默认主题\n");
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

#else  // 宿主机(pcpreview):从文件读,便于不上板就预览主题

#include <stdio.h>
#include <stdlib.h>

bool theme_load() {
  const char* path = getenv("THEME_FILE");
  if (!path || !*path) path = "theme.json";

  FILE* f = fopen(path, "rb");
  if (!f) return false;   // 没有主题文件是正常情况,用默认值

  static char buf[THEME_MAX_BYTES];
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  Theme* slot = nullptr;
  theme_loaded_slot(&slot);
  if (!theme_parse_json(buf, (uint32_t)n, *slot)) {
    fprintf(stderr, "theme: %s 解析失败,用默认主题\n", path);
    return false;
  }
  theme_use_loaded();
  fprintf(stderr, "theme: 已从 %s 加载 (%zu 字节)\n", path, n);
  return true;
}

#endif
