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

---

## 附二：§8（可选，笔记本冒充假诊断头）已执行 —— **失败，且它本身跑不通**

车主要求做 §8。结论：**§8 按现在的工具/固件组合不可能成功**，有两个独立原因，
**都不是板子的问题**。

### 发现 1 ★★ `tools/bt-obd/obd-ble-sim.py` 提交进来时**有语法错误，根本跑不起来**

```
File "…/obd-ble-sim.py", line 599
    print("NOTE: one connection per run -- restart this script before each board (re)boot.")
    ^^^^^
SyntaxError: expected 'except' or 'finally' block
```

第 599 行的 `print` 被放在 `else:` 块里、却按 `try:` 的层级缩进（`git show HEAD:` 里就是这样，
**不是笔记本侧改坏的**）。⇒ **桌面侧**说"§8 用 `obd-ble-sim.py` 验过"这件事需要复核 ——
**这份文件在提交状态下无法执行**（除非桌面侧另有一份未提交的版本）。
笔记本侧**只做了缩进修正**（把该行移回 `else:` 内），未改任何逻辑。

### 发现 2 ★★★ 即使修好，**固件也找不到这个假头**（sim 自己的自检就这么判）

修正缩进后跑 `--selftest`，它**自己给出了判决**：

```
advertising service 0000fff0-0000-1000-8000-00805f9b34fb  status=2
--- advertisement seen by a PASSIVE scanner (rssi=-87 addr=7DEC7BC78A99 type=0) ---
  service uuids : ['42594420-4155-544f-e0a9-e50e24dcca9e']
  VERDICT       : FFF0 NOT in the advertisement -> board would never find us
```

**Windows 把服务 UUID 放在 SCAN RESPONSE 里**，而固件是
**被动扫描**（`ObdTransportBLE`：`scan_->setActiveScan(false)`）且靠
`isAdvertisingService(FFF0)` 认设备 ⇒ **广播包里没有 FFF0 就永远匹配不上**。
（对照：真诊断头 `OBDBLE` 的广播里**是带** `0000fff0-…` 的 —— 实测
`service_uuids = ['0000fff0-0000-1000-8000-00805f9b34fb']`，所以板子能"扫到"真头。）

### 发现 3 ★ `--mode idle` **永远不回 AT 命令**，所以 §8 的期望行不可达

sim 跑 idle 时只做"无请求的通知"，**不解析 `010C`/`0105`**。而 §8 期望的是

```
SRC speed=link rpm=obd coolant=obd intake=obd | …2400rpm 90.0C 42.0C…
```

那要求 `rpm/coolant/intake` 标成 `obd` —— 也就是**必须回答 PID 请求**。
实测假头全程 `cmd=0 notify=0 write=0` ⇒ **§8 的期望结果在 `--mode idle` 下不可达**
（要复现桌面侧那一轮得用 `--mode sweep` 或 `replay`）。

### 实测（已存证）

- 假头侧（66 s）：`subs=0 cmd=0 notify=0 write=0 err=0`，`stopped: cmd=0 notify=0 write=0`
  ⇒ **板子从来没连上来过**。
- 从板侧（40 s）：`特征已配齐` **未出现**；
  `link: 启动闸门开了 —— BLE 没连上(到 15s 上限…),等了 15000ms`；
  `onConnectFail reason=0x0D`（多次）。
  ★ 一个**值得注意的细节**：板子**仍在不停地尝试连接**（`受理=1`、`rssi=-70/-73`），
  说明它的扫描**匹配到了某个带 FFF0 的设备**（`onDiscovered` 命中 ⇒ 地址被写进缓存）。
  这台机器上除了假头还有别的 BLE 设备，但**假头自己的计数器是 0** ⇒
  它连的不是假头。这一格建议桌面侧下次留意（是否匹配到了别的 FFF0 设备／缓存里的旧地址）。

证据：`tools/bt-obd/captures/bench-2026-09-28-laptop-sim-slave.log`

### 结论

**§8 无法在"被动扫描 + Windows 假头"这个组合下完成。** 要做成，至少需要其中之一：
① 固件改**主动扫描**（`setActiveScan(true)`，会多一次 scan-request/response 往返）；
② 或让假头把服务 UUID 放进**广播包**而不是 scan response（`is_discoverable` 已经是 true，
   但 Windows 的摆放位置不由我们定）；
③ 或 §8 改用 `--mode sweep`/`replay` 并且**前提是连接已经能建立**。

---

## 附三：★ B 方案（车主明确授权刷机后执行）—— **成功**

车主要求"刷机走一次 B 方案"，即落实上面 ① ：把从板改成**主动扫描**，
从而判定"**从板的 BLE 中央设备究竟能不能连上任何外设**"。

### 改动（只有一行，且是实验性的）

```cpp
// lib/dashcore/obd_transport_ble.cpp
scan_->setActiveScan(true);   // 原为 false（被动）
```

镜像验证：`firmware.bin` → **1706752**，编译输出里确实编了 `obd_transport_ble.cpp.o`
（**不是只看 `[SUCCESS]`**）。

### 结果：**连上了，而且数据、订阅、共存全通**

| 判据 | 实测（原样） |
|---|---|
| 特征/订阅 | `obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready` |
| 闸门 | `link: 启动闸门开了 —— BLE 已连上(ready),等了 14759ms ⇒ …` |
| 连接维持 | `obd-ble: state=ready peer=40:ab:3d:ef:3b:df conn=1 drop=0 connects=1` |
| 数据来源 | `SRC speed=van rpm=obd coolant=obd intake=obd \| … 90.0C 42.0C`（假头设的 90/42）|
| **共存** | `link: locked tick_age=38ms seen=250 seq_gap=0 miss=0`、`espnow: done_fail=0 overflow=0` |
| 假头侧 | `client subscribed`、`cmd=186 notify=234 write=744 err=0`，命令/应答闭环 |

⇒ **从板的 BLE 中央设备工作正常，且与 ESP-NOW 接收共存无碍**（零丢帧）。
**被动扫描就是那道锁** —— 它让板子收不到 scan response，于是匹配不上任何把服务 UUID
放在 scan response 里的对端。

★ 这条**不影响真头**：真头 `OBDBLE` 的 FFF0 **在广播包里**，主动扫描对它只会更容易匹配。

### 一条留给下一单的观察（本次只记录、不追）

假头侧看到的 AT 请求**是逐字节串行的**，且字符交错：

```
< '01'  < '0'  < 'C'  < '\r'        →  > '41 0C 51 05 \r'
< '0'   < '01' < 'C'  < '\r'        →  > '?\r\r>'
< '01'  < '0'  < '\r'               →  > '?\r\r>'
< '01'  < 'C'  < '0'  < '\r'        →  > 'NO DATA\r\r>'
```

也就是说**一个请求被拆成了多次 write，而且两次请求的字节会交错**。
`err=0`（假头侧没报错）且数据能用，所以不影响本单结论；
但它解释了为什么有些 PID 回 `?`/`NO DATA` —— 建议下一单查
"`sendRequest` 是否应该把整条命令**一次** `write()` 写完"。
证据：`tools/bt-obd/captures/bench-2026-09-28-bplan-active-scan.log`

### 现在的状态与建议

- 从板跑的是**带主动扫描的实验固件**（`esp32s3-rgb-slave-obdtest` + `setActiveScan(true)`）。
- ★ **这一行是去是留需要桌面侧决定**（我按车主授权只做实验，未擅自把它当结论固化）：
  留 ⇒ 兼容"服务 UUID 只在 scan response"的对端，代价是每次命中多一次
  SCAN_REQ/SCAN_RSP 往返；不留 ⇒ 恢复被动，但永远匹配不上那类对端。
- ★ 更值得下一单回答的是：**为什么真头连不上，而假头一连就上** —— 现在这一格被
  收窄到"真头特有的属性"（广播内容/地址类型/连接参数/是否已被别的中心占用）。

---

## 附四：★ 用刚刷好的主动扫描固件去连**真头** —— 失败；但拿到了决定性对照

车主授权后，在**同一块板、同一个固件**上先连假头（成功），再连真头（失败）。
真头地址 `AA:BB:CC:12:22:33`，车主还重插了一次真头。

### 结果：真头 **54~69 次尝试全部失败**，`lastFail=0x0D`

```
obd-ble: state=connecting peer=aa:bb:cc:12:22:33 conn=0 drop=0 connects=0 cs=54/54 … fails=54 lastFail=0x0D
link: 启动闸门开了 —— BLE 没连上(到 15s 上限:仪表优先,不再等 OBD),等了 15000ms
```

同一份 60 s 日志里**两个对端都出现过**（这是最干净的一次对照）：

| 对端 | 出现次数 | 结果 |
|---|---|---|
| `40:ab:3d:ef:3b:df`（**笔记本假头**）| 4 | ✅ `state=ready conn=1 connects=1`（**连上并维持**）|
| `aa:bb:cc:12:22:33`（**真头**）| 69 | ❌ 全部 `0x0D` |

⇒ **主动扫描对真头无效** ⇒ 真头不是"扫描匹配"的问题（地址一直在、`rssi` 也够）。

### 同刻笔记本对照（排除"头不接受连接"）

真头重插后，笔记本侧实测：

```
广播: rssi=-81  name='OBDBLE'  uuids=['0000fff0-…']  manufacturer_data={}  tx_power=0
笔记本连接(第一次) = True
```

**笔记本在 −81 一连就上；板子在 −70~−73 却一次都上不去** ⇒

- **不是信号强度**（板子的读数反而更强）
- **不是头不接受连接**（重插后立刻能连）
- **不是距离/遮挡**（头没动，笔记本连得上）
- ⇒ **问题在板子发出的那个连接请求本身**，与射频环境无关。

### ★★ 首要嫌疑：**地址类型**（能解释全部现象，且从未被排除）

固件日志一直打的是：

```
obd-ble: → connectNow begin (peer=aa:bb:cc:12:22:33 type=0)
```

`type=0` = `BLE_ADDR_PUBLIC`。而真头地址 `AA:BB:CC:12:22:33` 的**最高两 bit 是 `10`**
（`0xAA` = `10101010`）—— 那是**随机地址**（RPA/随机静态）的编码，**不是 public**。

若控制器按 `public` 去发 `LE Create Connection`，对端不会应答 ⇒
**`LE Create Connection` 超时 ⇒ 正好就是 `0x0D`（`BLE_HS_ETIMEOUT`）**。

| 观察到的现象 | 用"地址类型不对"解释 |
|---|---|
| 板子恒定 `0x0D`、一次不成 | 类型不对 ⇒ 对端永不应答 |
| 笔记本一连就上 | bleak/Windows 正确带随机地址类型 |
| **假头一连就上** | Windows 生成的地址 `40:ab:…`（高两 bit `01`）**恰好与 public 兼容** |
| 扫得到、地址缓存正确、重扫也命中 | **扫描不看地址类型，建连才看** |
| 主动扫描没帮助 | 类型与扫描模式无关 |
| 与 ESP-NOW 是否在跑无关（81KB 时空闲也失败）| 类型与射频占用无关 |

★ 注意：`NimBLEAddress` 的实现在本库**没有公开头**（`NimBLEAddress.h` 里没有
`getBase()`/`BLE_ADDR_*`），所以**没法在编译期确认** `getAddress().getType()` 的返回口径；
而 `onDiscovered` 里存的正是这个对象。⇒ 下一步就是**把它打出来看清楚**。

证据：`tools/bt-obd/captures/car-2026-09-28-slave-vs-real-dongle.log`

---

## 附五：★ 验证地址类型（车主说"1-2都做"）—— **假设被推翻**；同时排除"被笔记本占用"

上面附四把"地址类型"列为首要嫌疑。**这一节是它的实测判决**，以及一个**必须纠正的读数错误**。

### ⑤.1 先把地址类型打出来（新增日志，不动方案）

在 `onDiscovered()` 里加了**两个来源**的打印，并加了 `-DOBD_BLE_FORCE_RANDOM_ADDR_TYPE=1`
这个**唯一的行为改动**（建连前把地址对象按 `BLE_ADDR_RANDOM` 重建）。实测：

```
obd-ble: 扫到对端 aa:bb:cc:12:22:33 rssi=-75 地址类型 adv=0 obj=0（0=public 1=random 2=public-id 3=random-id）
obd-ble: ★ 地址类型强制为 random（obj 0 ⇒ 1）后再建连
obd-ble: → connectNow begin (peer=aa:bb:cc:12:22:33 type=1)
```

⇒ 库与控制器**两个来源都报 `0`**，而且**强制成 `1` 之后确实按 `type=1` 发出去了**。

### ⑤.2 判决：**仍然全败，`0x0D` 一次没变**

同一抓（55 秒，`bench-2026-09-28-force-random.log`）：

| 量 | 值 |
|---|---|
| `→ connectNow begin (… type=1)` | 33 次 |
| `connect() 受理=1` | 55 次（**全部受理**，即前置检查全过） |
| `onConnect ✓` | **0** |
| `特征已配齐 … state=ready` | **0** |
| `onConnectFail` | 118 次，**全部 `0x0D`** |

⇒ **地址类型不是根因。** 附四那张"用类型不对解释全部现象"的表**成立但非唯一**，
真实根因另在别处。`-DOBD_BLE_FORCE_RANDOM_ADDR_TYPE` **改回默认 0**（代码里默认就是 0）。

### ⑤.3 ★★★ 读数纠错：`0x0D` 这个数**本身是假的**，而且它指向了**主机自己**

`%02X` 会把宽值截断，所以先把原始整数打出来。实测（`bench-2026-09-28-fail-latency.log`）：

```
obd-ble: onConnectFail #1 reason=13(0xD) 距发起 17ms ← ★ 这才是真错误码
obd-ble: onConnectFail #2 reason=13(0xD) 距发起 17ms ← ★ 这才是真错误码
```

`reason` **就是十进制的 13**，不是 `0x23E` 被截断。所以它确实是 `BLE_HS_ETIMEOUT`。
**但这次能说清它是谁给的**（读库源码，不是猜）——主机的建连路径里只有两条会走到 13：

| 出处 | 触发条件 |
|---|---|
| `ble_gap.c:1131 ble_gap_master_connect_cancelled()` | 主机**取消了建连** ⇒ `event.connect.status = BLE_HS_ETIMEOUT` |
| `ble_gap.c:1146` / `NimBLEClient.cpp:304` | 同步版 `taskWait` 超时后补写（**异步改造后这条路已不参与**） |

而第一条只由 `ble_gap.c:2632` 的 `BLE_ERR_UNK_CONN_ID` 触发，即：
**主机的"建连计时器"到点 ⇒ 发 `LE Create Connection Cancel` ⇒ 控制器回 `UNKNOWN_CONN_ID` ⇒ 报 13。**

⇒ **13 是"主机把这次建连取消了"，不是"对端不应答"。**
⇒ 于是真问题变成一个**很具体、可证伪**的问题：

> `m_connectTimeout` 明明是 30000 ms（`NimBLEClient.cpp:69`），
> **为什么这个计时器 17 ms 就到点了？**

★ 一条极强的旁证：`docs/BLE-OBD.md:725-730` 记录**同步版也是"16~17ms 返回 false"**，
当时列出的候选第一条正是"**连接被取消**"，标注为"尚未验证"。
现在两条独立的路（同步的 `taskWait` / 异步的回调）量到**同一个 16~18ms**，
说明**不是 `taskWait` 的问题，而是那个计时器/取消动作本身**。

★ 于是 `docs/BLE-OBD.md` §12.1 里"基于 `status=13` 的推断全部作废"这句要**再收窄一格**：
作废的是"13 = 对端超时/内存不够"这类推断；**13 本身是有意义的**，它= **主机侧取消**。

### ⑤.4 ★★ 另一个必须补的量：失败是"当场"来的还是"超时"来的

（已由 ⑤.3 回答：**16~18ms，当场**。留这段是为了记住判据怎么定的。）
现有日志里 `connect()` **1ms 就受理返回**，而 45 秒内失败计数涨到 134～249
⇒ 平均每个回合不到 0.2 秒。**真超时不可能是这个节奏**（15 s 量级）。
⇒ 已在 `onConnectFail` / `onConnect` 里加了 `millis()` 时间差（"距发起 N ms"）。
判据很干净：

- **≥10000ms** ⇒ 控制器真的等了对端，对端不应答；
- **几毫秒～几十毫秒** ⇒ 请求**根本没发出去**，问题在库/控制器这一侧。

实测答案是后者：**16 / 17 / 18 ms**。

### ⑤.5 排除"真头被笔记本占着"（A/B 实测）

之前用"笔记本同时能连"来排除占用，**那个推理不成立**——ELM327 类设备普遍只允许 1 条连接，
"笔记本能连"与"板子能连"并不互斥。所以补了一个干净的 A/B：

1. 板子在跑（持续重试），笔记本侧 `bleak` 连上真头并**持有 18 秒**，然后**干净断开**；
2. 全程 45 秒抓板子串口。

结果（`bench-2026-09-28-board-vs-dongle-occupancy.log`）：

| 时刻 | 事实 |
|---|---|
| 笔记本侧 | `扫到 rssi=-71` ⇒ **2.6 秒连上**（FFF0 全在）⇒ 头是活着、可连的 |
| 笔记本持有 18s 期间 | 板子 **0 次成功** |
| 笔记本**断开后**的 20+ 秒 | 板子**仍然 0 次成功**（51 次全 `0x0D`）|

⇒ **"被占用"排除**。头能被别的中心连上、占用解除后板子照样连不上。

### ⑤.6 同时排除的因素（同一批抓里的数）

| 嫌疑 | 实测 | 判 |
|---|---|---|
| 内存不够 | `内部free=29~81KB`，`BLE start()` 后 82KB | 排除 |
| 射频被 ESP-NOW 抢 | `tx_fail=0 done_fail=0 overflow=0 gap_rx=11~34ms` | 排除（链路健康） |
| 扫不到对端 | 反复扫到，rssi −71～−82 | 排除 |
| 现场有第二个 FFF0 设备（连错目标） | 车库扫描：**只有 1 个**设备广播 FFF0 | 排除 |
| 板子 BLE 中心整体不工作 | 假头 `40:ab:…` 能连上、能订阅、`err=0` | 排除 |

### ⑤.7 关于"真头是哪一个"的一个发现（需车主确认）

笔记本的蓝牙设备表里有一个已配对的 **`OBDII`，MAC 是 `00:1D:A5:0B:33:4B`**
（`00:1D:A5` 是真实 OUI）；而现场唯一在广播 FFF0 的是 **`AA:BB:CC:12:22:33` / `OBDBLE`**，
**两者不是同一个设备**，且 `00:1D:A5:…` 此刻根本没在广播。

⇒ 现场这个头是 `AA:BB:CC:12:22:33`。这个地址**不是**合法的公开 IEEE 地址
（首字节 `0xAA` 高两 bit = `10`，属随机地址域），是**典型克隆模块**的固定地址。
**这一条只作为事实记录**，不作为根因。

### ⑤.8 这台头自己的广播特征（新增，都是笔记本侧量的硬事实）

用 WinRT `BluetoothLEAdvertisementWatcher` 看 10 秒（脚本是临时件，不在仓库里）：

| 项 | 实测 |
|---|---|
| 广播类型 | **`CONNECTABLE_UNDIRECTED`（可连）**，`is_connectable=True` |
| FFF0 在**广播包**里还是只在 SCAN_RSP | **在广播包里**（`uuids=['0000fff0-…']`、`local_name='OBDBLE'`），另有独立的 `SCAN_RESPONSE` |
| 广播稀疏度 | **约 2~5 秒一次**（10 秒只看到 5 组 adv+rsp） |

⇒ 头两条**排除了"头不可连 / UUID 只在 SCAN_RSP 里"**这两种最容易误判的情况。
⇒ 第三条是新线索：**这个头广播很稀疏**（省电型/慢广播）。它**单独解释不了 17ms 的失败**
（17ms 太短，来不及等任何广播窗口），但记下来 —— "对端广播稀 ⇒ 建连窗口里抓不到包"
本来是最能解释 `BLE_ERR_CONN_ESTABLISHMENT` 的机制。

★ 一条来自交接单 §4 第 1 条的纪律值得记：**桌面假头一个进程只接一次连接**
（客户端一走开就再也接不上第二个，一直失败而广播还在）⇒
台面那组 277/277 **是受污染的，不能用来类比车上**。
真头不是这个毛病（它是**物理重插**之后恢复的），但"连过一次就卡住"的脾气很像 ⇒
下次台面测真头前，**先把头断电重插**，并把这个动作记进日志。

### ⑤.9 下一步：一个**可证伪**的决定性实验（只改一个数）

既然 ⑤.3 把机制收窄到"**主机那个 30 秒计时器 17ms 就到点**"，下一步只需验一件事：

> **把 `m_connectTimeout` 改掉，看 17ms 跟不跟着变。**

- 若失败延迟**跟着变**（设 5000ms ⇒ ~5ms、设 60000ms ⇒ ~34ms 之类，按比例）
  ⇒ 计时器被喂了一个**按比例缩小**的值（单位/换算问题）⇒ 在库这一侧，与对端无关；
- 若延迟**恒定 16~18ms** ⇒ 不是那个计时器，是**别的东西在 17ms 时取消了建连**
  （那时再查"谁在 17ms 取消"）。

判据是一对数：`onConnectFail … 距发起 N ms` + 我们设进去的 `duration_ms`。
★ 仍然只改一个变量，不动扫描/GATT/共存。

### ⑤.10 结论一句话（给车主）

**地址类型不是原因**（强制 random 也没用）、**头没被笔记本占着**（笔记本断开后板子照样连不上）、
**头和板子各自的射频都正常**。失败是**板子主机侧在 16~18ms 时自己把建连取消了**
（`reason=13` = 主机取消，**不是**对端超时）—— 下一步只改一个连接超时值来验证它。

★ 附带记录（不是本次目标）：同一抓里 `espnow: ★PHY 未上线 stage=0`，
但主板的 `rx_frames` 在涨、且 `trust: sim-fallback` ⇒ 走的是**屏上模拟数据回退**。
这条与 BLE 无关，但它解释了从板上"看起来有数据"的来源。





