# BLE-OBD v2 车上验证结果（2026-09-28，笔记本侧）

树版本：`9ad3ed4`（`git pull` 后；执行单要求的 `d66d743` 是它的祖先 ✓）

> **执行范围说明（按 §3 的顺序，如实写）**：
> 本单只走到 **§3.2**，判据命中 **❌ 失败分支** ⇒ 按 §3.2 第 4 条的强制跳转
> （"失败 → 直接跳 §7"），**§3.3（发动+行驶）没有执行**，因此 §3/§4/§5 三节
> 按模板保留为"未执行"。§7 第 1 条要求的"先交证据"即本文件 + 三份抓包。

## 1. 台面自检

- **主板**：COM7 `role=MASTER` 行（原样）：
  ```
  boot: reason=UNKNOWN n=9 (raw=11) up=239ms | prev=UNKNOWN (raw=11) prev_up=100min | hb=17 | role=MASTER
  obd: 本机未启用(-DOBD_SERIAL=0,-DOBD_BLE=0) —— 分工 v2 下这正是主板该有的样子(进气由从板经链路回传)
  link: 主板侧就绪 ESP-NOW ch=6 (no pins) @1000000—— 真 PHY(ESP-NOW)
  ```
- **从板**：COM8 `role=SLAVE` 行（原样）：
  ```
  boot: reason=UNKNOWN n=83 (raw=11) up=242ms | prev=UNKNOWN (raw=11) prev_up=100min | hb=22 | role=SLAVE
  obd: BLE 那条路已挂上(目标服务 FFF0 / 通知 FFF1 / 写 FFF2) —— start() 推迟到显示初始化之后
  obd: BLE start() = 1  heap=82KB(面板之后)
  obd: heap_caps 内部 free 144→82 KB(掉 61) largest 91→41 KB | PSRAM free 7287→7287 KB(掉 0)
  obd: 从板角色 ⇒ 射频占用上限 4000ms（压久了左屏会掉进"数据不可信"）
  ```
  ⇒ **两张对上 §3.1 的表** ⇒ 继续。

★ 两处**如实记下**的环境差异（都不影响判据，但别当成"执行单写错了"）：

1. **端口号是 COM7/COM8，不是执行单里的 COM8/COM9**。按 §5 纪律"认板子只看 `role=`"，
   已用 `role=` 认板，与此前一致（COM7=MASTER、COM8=SLAVE，MAC 也对上）。
2. **两块板都读不到 `obd:` / `role=` 那两行** —— 因为它们是**开机时打一次**，
   而板子从桌面侧刷完起已连续跑 ~100 分钟（`prev_up=100min`）。
   按 §3.1 的注解（"抓不到就按一下 RST 再抓"）补了一次复位后即抓到。
   ★ 手法上有一处值得写回执行单：**只有 `esptool --after hard-reset` 能可靠复位这两块板**，
   我在 pyserial 里单独拉 RTS **无效**；而同时拉 DTR+RTS 会把板子留在 ROM 下载模式
   （串口静默，但 esptool 仍认得到 MAC `80:45:6b:35:a3:c4`）。

## 2. 只拧到 ON（关键判据）

- **闸门那一行（原样）**：
  ```
  link: 启动闸门开了 —— BLE 没连上(到 15s 上限:仪表优先,不再等 OBD),等了 15000ms ⇒ 现在启链路 PHY(射频让出来了)
  ```
- **结果：❌ 失败**（命中 §3.2 判据表的第 2 行，逐字一致；`XXXX` = `15000`）
- `特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready`：**未出现**（失败分支，符合预期）
- **失败整行（§3.2 第 3 条要求"必须原样抄下这一整行"）**：
  ```
  obd-ble: state=connecting peer=aa:bb:cc:12:22:33 conn=0 drop=0 connects=0 cs=73/73 radio=balance win=5 cap=5 fails=73 lastFail=0x0D
  ```
- 其它 `obd-ble:` 行（前 10 行，原样）：
  ```
  obd-ble: ← connect() 受理=1 立即返回耗时=0ms rssi=-70 内部free=81KB（★ 异步：结果看 onConnect / onConnectFail）
  obd-ble: onConnectFail reason=0x0D ← ★ 这才是真错误码
  obd-ble: state=connecting peer=aa:bb:cc:12:22:33 conn=0 drop=0 connects=0 cs=5/5 radio=balance win=1 cap=1 fails=5 lastFail=0x0D
  obd-ble: ← connect() 受理=1 立即返回耗时=1ms rssi=-70 内部free=29KB（★ 异步：结果看 onConnect / onConnectFail）
  obd-ble: onConnectFail reason=0x0D ← ★ 这才是真错误码
  obd-ble: ← connect() 受理=1 立即返回耗时=1ms rssi=-70 内部free=29KB（★ 异步：结果看 onConnect / onConnectFail）
  obd-ble: onConnectFail reason=0x0D ← ★ 这才是真错误码
  ```

### ★ 这一抓里最有信息量的一点（供桌面侧定位，不改方案）

`内部free` 在**闸门开前 = 81KB、闸门开后 = 29KB**，而 `lastFail` 前后**都是 `0x0D`**：

```
内部free=81KB  → onConnectFail reason=0x0D      （链路 PHY 还没起）
link: 启动闸门开了 —— BLE 没连上(到 15s 上限…)
内部free=29KB  → onConnectFail reason=0x0D      （链路 PHY 起来之后）
```

⇒ **失败与"链路在不在跑"无关**：PHY 起来前（81KB 空闲）也同样失败、
同样是 `0x0D`。这一条把"内存不够"和"ESP-NOW 抢射频"两个解释**同时排除**了
（至少在本车这台诊断头上）。`0x0D` = `BLE_HS_ETIMEOUT` = 控制器报"建立连接超时"。

## 3. 发动 + 行驶

**未执行** —— §3.2 命中失败分支，按该节第 4 条强制跳 §7。

## 4. 现象（车主视角，可选）

**未采集** —— 本次执行只到 §3.2，未上车发动。

## 5. 结论（只填一句）

- **建连失败**（`lastFail=0x0D`，73 次尝试无一成功；闸门按 15 s 上限开，
  链路 PHY 随后正常启动）

---

## 证据文件（§6.2）

| 文件 | 内容 |
|---|---|
| `tools/bt-obd/captures/car-2026-09-28-pre-master.log` | §3.1 主板自检判据行（复位后启动序列） |
| `tools/bt-obd/captures/car-2026-09-28-pre-slave.log` | §3.1 从板自检判据行（复位后启动序列） |
| `tools/bt-obd/captures/car-2026-09-28-on-slave.log` | §3.2 判据行（60 s 窗口，含失败整行与上下文） |

每份文件头两行 ASCII 注释写明来源、端口、固件、判定；正文均为板子原始输出（UTF-8），
**未改写**。90 KB 原始日志未提交（按 §6.2 第 3 条，只留判据行 + 关键上下文）。

---

## 附：§7 第 2 条（切回"主板读 OBD"）已按要求执行，但**在这块板上不可行**

车主明确要求走 §7 第 2 条，我按该条的两条硬纪律执行，结果如下 —— **只交证据，未再现场折腾。**

### 第 ① 条纪律：镜像真的重编了吗 —— ✅ 通过

| | `firmware.bin` | `firmware.factory.bin` |
|---|---|---|
| 改宏前（不带 BLE） | 1497072 | 1562608 |
| **改宏后（带 BLE）** | **1709984**（+206 KB） | **1775520** |
| 编译输出 | `Compiling … \src\main.cpp.o` 确实编了 | — |

⇒ 大小与时间戳都变了 ⇒ **不是"只看 `[SUCCESS]`"**，这次镜像确实带 BLE。

### 第 ② 条纪律：先刷主板、确认能连上 —— ❌ **主板起不来**

```
ESP-ROM:esp32s3-20210327
rst:0x15 (USB_UART_CHIP_RESET),boot:0xb (SPI_FAST_FLASH_BOOT)
Saved PC:0x4037f5fe
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fce2820,len:0x10cc
load:0x403c8700,len:0xc2c
load:0x403cb700,len:0x30c0
entry 0x403c88b8          ← 进了应用入口，然后一行都不打
```

**主板在 ROM 阶段之后彻底静默**：没有 `boot: reason=`、没有 `psram :`、没有 `obd:`、没有 `obd-ble:`。
**`obd: BLE 那条路已挂上(…)` 这一行从来没有出现过** —— 而它正是第 ① 条纪律要求确认的那一行。

**复现两次**，包括一次**整片烧**（`write-flash 0x0`，`Hash of data verified`）——
所以**不是普通刷机的残留**。对照：**同一块板刷 `-DOBD_BLE=0` 就正常启动并打出完整横幅**
（见 `car-2026-09-28-pre-master.log`）。

### ★ 这一条为什么重要（供桌面侧）

执行单 §1 说"两块板镜像已都是 `d66d743` 编出来的"，
但 `d66d743` 那一版主板是**不带 BLE** 的（§1 的第 2 项自己也写了"主板：**不带 BLE**"）。
⇒ **"主板 + BLE"这一组合在本单固件上没有被验证过**，而实测它**起不来**。
桌面侧 §0 说的"两条路同时活着 80+ 秒"是在**从板**上验的（从板 `obdtest` 带 BLE），
与主板这条组合不是同一件事。

证据文件：`tools/bt-obd/captures/car-2026-09-28-master-ble-silent.log`

### 我做的收尾（未继续折腾）

主板已刷回 `-DOBD_BLE=0`（能起、链路与 VAN 正常），`platformio.ini` 也回到 `-DOBD_BLE=0`，
以免把"起不来的配置"留在仓库里。**从板未刷**（仍是 `obdtest`，带 BLE）；因此
§7.2 要求的"两块板同源同批刷"**没有完成** —— 因为第 ② 条在第一步就断了。

