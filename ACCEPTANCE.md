# 验收

- [x] 工程能编译（esp32dev 已实测；构建路径需纯 ASCII，见 ARCHITECTURE.md）
- [x] 宿主机单元测试全绿（python -m platformio test -e native，当前 23 例）
- [ ] 刷到板子：串口每秒打印 `206 dash ok`、每 5 秒打印 `SRC speed/rpm/coolant` 源状态
- [ ] 假数据弧 0→210 走满（sim 正弦扫全量程）：Wokwi 或真屏可见；
      桩驱动丢弃画面，只能靠串口确认数值在动
- [ ] 真屏到货：锁分辨率（改 ui_theme.h 的 THEME_DISPLAY_RES）→ 写实驱动
      （dash_display.cpp，#error 保护）→ 删 DASH_DISPLAY_STUB；板子换 S3+PSRAM
- [ ] 实车 K 线 OBD：010C/0105 持续更新，断线 3 秒回退假数据
- [ ] 实车 VAN：0x824 车速帧格式验证（不符先用 configureSpeedFrame 现场改）
