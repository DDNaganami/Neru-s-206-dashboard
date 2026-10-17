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

// ============================================================
// ★★ 2026-10-16：两个入口（`theme_load()` / `theme_load_alerts()`）**共用这一块**缓冲。
//
//   原来两处各写一个函数内 `static char buf[THEME_MAX_BYTES]` ⇒ `.bss` 里
//   2 × 4 KB = **8 KB**，而 `lib/themetool/theme_store.h` 第 72 行一直写的是
//   "设备端用**一块**静态缓冲" —— 那个头第 74 行自己还记着"原来设 16 KB 就把
//   esp32dev 的 dram0_0_seg 顶爆了 9 KB" ⇒ 这 4 KB 的量级在本项目里是**动过
//   DRAM 生死线**的数，白占一倍没有任何理由。
//
//   ★ 为什么可以共用（安全性只有这一条）：这两个入口**都只在 setup() 里串行调用**
//     —— `main.cpp` 的顺序是 `theme_load();` 紧接着 `theme_load_alerts(...)`。
//     而且每次调用的形状都是"读完字节 → 当场解析完 → 返回"，解析结果落在
//     `Theme` / `AlertsConfig` 里；没有任何地方留下指向这块缓冲的指针，
//     所以第二个入口开始时，第一个入口的字节已经没人看了。
//   ★★ 因此：**这两个入口不许并发、不许在 loop() 里调用**。谁要加第三个读者
//     （例如运行时热重载主题），要么同样落在 setup 的串行序列里，要么自己再拿
//     一块 —— 这一条写在这里，就是为了让"加第三个读者的人"先看到它。
// ============================================================
static char s_buf[THEME_MAX_BYTES];   // 两入口共用一块,仅限 setup 串行调用

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
  // ★ 缓冲在文件顶上（`s_buf`，两入口共用一块，见那里的说明）。THEME_MAX_BYTES
  //   的取值直接决定 DRAM 占用，别随手放大 —— 见 lib/themetool/theme_store.h。
  const uint32_t len = read_theme_blob(s_buf, sizeof(s_buf));
  if (len == 0) {
    dash_logf("theme: 没有可读的主题文件(无分区/空/读失败),用默认主题\n");
    return false;
  }

  Theme* slot = nullptr;
  theme_loaded_slot(&slot);
  if (!theme_parse_json(s_buf, len, *slot)) {
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
  const uint32_t len = read_theme_blob(s_buf, sizeof(s_buf));   // 同一块，见文件头
  if (len == 0) return false;
  return theme_parse_alerts_json(s_buf, len, cfg);
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
  const uint32_t n = read_theme_blob(s_buf, sizeof(s_buf));   // 与设备端同一块，见文件头
  if (n == 0) return false;

  Theme* slot = nullptr;
  theme_loaded_slot(&slot);
  if (!theme_parse_json(s_buf, n, *slot)) {
    fprintf(stderr, "theme: 解析失败,用默认主题\n");
    return false;
  }
  theme_use_loaded();
  fprintf(stderr, "theme: 已加载 (%u 字节)\n", (unsigned)n);
  return true;
}

bool theme_load_alerts(AlertsConfig& cfg) {
  const uint32_t n = read_theme_blob(s_buf, sizeof(s_buf));
  if (n == 0) return false;
  return theme_parse_alerts_json(s_buf, n, cfg);
}

#endif
