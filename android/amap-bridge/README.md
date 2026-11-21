# 206dash 导航桥（Android 最小工程，阶段 A）

把**高德手机版**的导航广播翻译成 206dash 的 39 字节短报文，经 BLE 写给左屏 ESP32。

- 手机：小米 14（Android 14/HyperOS）——**必须**前台服务 + 电池白名单，否则几分钟就被杀
- 高德：手机版，`KEY_TYPE=10001`（导航）/ `10019`（昼夜）；字段含义见
  `docs/BRANCH-NAV-FEASIBILITY.md` §12
- 固件：`lib/dashcore/nav_proto.h`（魔数 0xA7 / 版本 1 / 小端 / CRC-16-CCITT-FALSE）
- 传输：BLE **central**（手机主动扫/连板子），板子做 peripheral 广播
  `206dash-nav` + 服务 `0xNUS` 风格自定义 UUID（见 `NavLink.kt` 常量）

## 目录

```
android/amap-bridge/
  README.md
  settings.gradle.kts
  build.gradle.kts
  app/build.gradle.kts
  app/src/main/AndroidManifest.xml
  app/src/main/java/com/dash206/navbridge/MainActivity.kt
  app/src/main/java/com/dash206/navbridge/NavLink.kt   # 广播解析 + 组帧 + BLE + 前台服务
```

## 用法（Android Studio）

1. 打开本目录（Gradle 同步）
2. 装到小米 14：`Run`
3. 授予"通知"与"附近设备"权限；在系统设置里给本应用**允许后台运行**（HyperOS 必做）
4. 打开高德 → 开始导航 → 本应用状态变绿即已连上板子并开始推送

## 已知限制（阶段 A）

- 只做"广播":没有路线几何 ⇒ 屏上只显示箭头 + 距离 + 车速，车图标原地旋转
- 高德字段随版本可能变；解析全部做成"有就用、没有就跳过"，绝不崩
- BLE 断开会自动重连（3 s 重试），导航结束（`KEY_TYPE=10001` 停发）时 3 s 后推一帧"无效"
