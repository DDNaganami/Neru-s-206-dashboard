# feat/nav-cluster —— 阶段 A 现场记录（2026-11-21 夜）

改装后新形态：**左屏导航 / 右屏转速+进气**（车速数字在导航页上）。双板保留。
详细研判见 `docs/BRANCH-NAV-FEASIBILITY.md`；这里只记"已经落地了什么、怎么验、还差什么"。

## 已落地（本分支 5+2 个提交）

| 模块 | 文件 | 验法 |
|---|---|---|
| 导航短报文（39 B 上限，CRC16） | `lib/dashcore/nav_proto.{h,cpp}` | native 黄金字节 `A7 01 07 03 … 73 CA` |
| 导航状态（超时降级/昼夜新鲜度/距离分档） | `lib/dashcore/nav_state.h` | native 9 条用例 |
| 字节流解析（半包/粘包/噪音/坏 CRC 重同步） | `lib/dashcore/nav_source.h` | native 5 条用例 |
| 左屏导航页（箭头/距离/车速/车图标旋转/NO NAV） | `src/nav_ui.{h,cpp}` + `dash_ui` 钩子 | pcpreview 落帧（`preview/frames/*.bmp`） |
| 夜间模式改由高德 10019 驱动 | `lib/dashcore/night_mode.h` | native 4 条用例（立即采信 / 掉线回退 / 数据不可信不动） |
| 右屏主题 = 转速 + 进气 | `lib/themetool/ui_theme.h`（默认值） | pcpreview 右屏落帧：6950 rpm + 28 ℃ |
| Android 桥接骨架 | `android/amap-bridge/` | **未编译**（本机无 Android SDK） |

判据：native **411 例 / 2 跳过 / 409 通过**；pcpreview 编译 SUCCESS。

## 还差（下一轮）

1. **BLE 传输胶水**：`nav_source_ble.{h,cpp}`（NimBLE central，写 `nav_source_push()` 一行），
   并把 `nav_ui_update(g_nav)` 接进主循环 —— 目前设备端只会显示 `NO NAV`（正确但空）。
2. **设备固件编译未验**：沙箱仍不允许写 `C:\.platformio`。车主侧：
   `icacls "C:\.platformio" /setintegritylevel (OI)(CI)low` + 对 `C:\206dash-build` 同样处理，
   然后 `.\tools\build\pio.ps1 -Cmd 'run -e esp32s3-rgb-slave-now'`。
3. **两件上车前验证**（决定项目是否成立）：小米 14 上高德 `KEY_TYPE=10001` 广播是否还在；
   BLE ELM327 读 `0100/010C/010D/010F`。

## 预览怎么看

```
# 工作区内自举（沙箱只给写工作区时用）
export PLATFORMIO_CORE_DIR=.../.pio-core PLATFORMIO_BUILD_DIR=.../.pio-build
export PATH=.../.zigbin:$PATH
python -m platformio run -e pcpreview
./.pio-build/pcpreview/program.exe   # 落帧到 ./preview/frames/{l,r}_*.bmp（每 200 ms 一帧，共 150）
```
