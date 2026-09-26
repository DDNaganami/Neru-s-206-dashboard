#pragma once
#include <stdint.h>
#include "ui_theme.h"

// 前向声明：告警配置住在数据层（lib/dashcore/alerts.h），本层只拿它的**引用**，
// 不 include 那个头 —— 依赖方向见下面 theme_parse_alerts_json 的说明。
struct AlertsConfig;

// ============================================================
// 主题的读取与解析（运行时主题）
//
// 流程:
//   1. dash_ui_init() 先 theme_reset_to_defaults() —— 保证一定有可用主题
//   2. main 的 setup() 调 theme_load() 尝试从 flash 的 theme 分区读主题文件
//      成功 → 覆盖 g_theme;失败（没刷主题/文件坏了）→ 保持默认值,照常跑
//
// ★ 固件永远不因"没有主题文件"而跑不起来 —— 这是刻意的设计:
//   新手最容易卡在"刷了主题反而黑屏",所以降级路径必须是默认主题。
//
// 主题文件格式:JSON（见 tools/theme-editor/，编辑器的"导出"就是这个格式）
//   - 字段可缺失:缺失的用默认值
//   - 未知字段忽略（向前兼容）
//   - 越界值在 theme_after_load() 里被钳制
//
// 为什么用 JSON 而不是二进制:主题文件是人会手改、会进版本库的东西,
// 出问题时能直接打开看。代价是设备端要多一个几百行的小解析器,
// 但它没有依赖、可在宿主机单测。
// ============================================================

// 尝试加载主题。返回 true 表示成功覆盖了 g_theme。
// 失败时 g_theme 保持调用前的值（通常是默认值）。
bool theme_load();

// ★ 同上，但读的是**同一份文件里的 `alerts` 段**（见下面 theme_parse_alerts_json）。
//   返回 false = 没有分区 / 没刷过 / 文件坏了 / 文件里没有这一段 ⇒ cfg 一个字都没动。
//   ★ 与 `theme_load()` 各读一次分区（各 ≤4 KB，只在 setup 里跑一次）。
bool theme_load_alerts(AlertsConfig& cfg);

// 从内存里的 JSON 文本解析主题（不含 IO）——便于宿主机单测。
// 成功时把解析结果写进 t，并调用 theme_after_load() 做派生与钳制。
bool theme_parse_json(const char* json, uint32_t len, Theme& t);

// ------------------------------------------------------------
// ★★ 2026-09-27：主题文件里除配色之外还能带一段**告警/蜂鸣器参数**
//    （`"alerts": { ... }`，在编辑器里拖控件改；见 alerts.h 的 AlertsConfig）。
//
//    为什么要单独一个入口、而不是塞进上面的 Theme：
//      `Theme`（ui_theme.h）是**界面层**的数据模型，而告警阈值属于**数据层**
//      （lib/dashcore/alerts.h）—— 让界面层的结构体去 include 数据层的头，
//      是为了省一次解析而把两层的依赖方向拨反。这里只做"多扫一遍同一段文本"：
//      文件 ≤4 KB，启动时多走一遍扫描器的代价可以忽略。
//    ★ `AlertsConfig` 用**前向声明**，本头文件因此不依赖 lib/dashcore。
//
//    位置：`"alerts"` 既可以在根上（`{"alerts":{...}}`），也可以在
//      `"theme"` 里面（`{"theme":{"alerts":{...}}}`）—— 编辑器的导出是后者
//      （它把整份主题当一个对象存/传，见 tools/theme-editor/theme-json.js）。
//      两种写法都认：扫描器按"第一个 `"alerts"` 键 + 冒号"定位，不关心它在第几层。
//
//    语义（与主题配色**完全一样**，这是"字段可缺失"那条规矩的延续）：
//      · 文件里**没写**的字段 ⇒ cfg 里保持原值（调用方从固件默认值出发即可）；
//      · 文件里写了但越界 ⇒ 由 `alerts_config_clamp()` 钳到安全范围（见 alerts.h）；
//      · 返回 false = 文件里**没有**这一段（或这一段不是对象）⇒ cfg 一个字都没动。
bool theme_parse_alerts_json(const char* json, uint32_t len, AlertsConfig& cfg);

// 主题文件的预期存放位置（供文档/工具引用）
//   ESP32: theme 分区（partitions.csv 里的 `theme`，subtype 0x40）
//   宿主机: 环境变量 THEME_FILE 指定的路径，默认 ./theme.json
#define THEME_PARTITION_LABEL "theme"

// 主题文件大小上限。
//
// ★ 这个数不是为了"保险",它直接决定 DRAM 占用:设备端用一块**静态**
//   缓冲把分区读进来,缓冲整个计进 .bss。曾经设成 16KB,在 esp32dev
//   (320KB DRAM)上把 dram0_0_seg 顶爆了 9KB —— 而主题本身只有约 500 字节。
//   所以按"主题 JSON 实际大小 + 余量"定,不要随手放大。
//   编辑器导出的主题 JSON 约 1.2~2KB(含两屏弧参数)。
#define THEME_MAX_BYTES       (4 * 1024)
