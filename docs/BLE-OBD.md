# BLE OBD 诊断头（`OBDBLE`）实测：能当板子的 OBD 数据通道

> 2026-09-27 实测。目的：让 **S3 板子自己读 OBD**（省掉往板上插第二个 USB 有线 327）。
> **结论：整条路已经通了** —— 该诊断头是 BLE；GATT 摸清；**写入路径已打通**（PowerShell 反射）；
> **读取路径唯一可用实现是 Python + bleak**（PowerShell 收不到 WinRT 通知，原因见 §4）。
> 实测已读出真值：`010C` → 978 rpm、`010F` → 59 ℃。
> ★★ 另有一条对**整个项目**都重要的发现：**这台 206 是 ISO 14230-4 KWP FAST，不是 CAN**（见 §5.5）。

---

## 1. 先决判据：它必须是 BLE，不能是蓝牙经典 SPP

`PURCHASE.md`（91~119 行）早就写过这条：**ESP32-S3 只有 BLE**，蓝牙经典/SPP 在硬件上就没有，
连不上就是连不上，不是写代码能补的。而市面便宜的 ELM327 绝大多数恰恰是经典 SPP。

> **判据（实测好用）**：用 WinRT 的 `BluetoothLEDevice.GetDeviceSelector()` 枚举 ——
> **能被列出来的就是 BLE**；经典 SPP 设备根本不会出现在这个列表里。
> 工具即 `tools/bt-obd/probe-ble-gatt.ps1`，直接跑就能看。

**实测结果（2026-09-27）**：设备 `OBDBLE`、地址 `AABBCC122233` 出现在 BLE 枚举里
⇒ **它是 BLE，判据通过。**
（`AABBCC122233` 这种地址一看就是克隆方案的默认 MAC。）

另外：它**不会**在 Windows 里生成 COM 口 —— 实测配对后 `SerialPort.GetPortNames()` 仍是
`COM5/COM6/COM7/COM8`，**没有**新的。所以"先配对再看有没有多一个 COM 口"这个思路对 BLE 无效，
别拿它当判据。

## 2. GATT 结构（已读出，照着这个写固件）

| 角色 | UUID | 属性 | 说明 |
|---|---|---|---|
| 服务 | `0000fff0-0000-1000-8000-00805f9b34fb` | — | OBD 数据口 |
| **收**（头 → 中心） | `0000fff1-…` | `0x12` = 读 + **通知** | 回答从这里来 |
| **发**（中心 → 头） | `0000fff2-…` | `0x0C` = **写** + 无响应写 | 指令往这里写 |

其余 `1800`（设备名）/`1801`（GATT）/`1804`（电池）/`180F`（设备信息）是标准服务，不用管。

配对成功后 Windows 会把 `FFF0` 也登记成一个设备（`BTHLEDEVICE\{0000FFF0-…}_AABBCC122233\…`），
这可以作为"配对确实生效"的旁证。

## 3. ★ 板上固件必须照抄的四个行为（都是踩出来的）

1. **它会自己掉线。** 空闲时 `ConnectionStatus` 变成 `Disconnected`，此时
   `GetGattServicesAsync()` **不报错、只回 1 个服务（`0x1801`）** ——
   这是「没真连上」的签名，**不是**「设备没有 OBD 服务」。极容易误判。
   ⇒ 固件必须有**重连**逻辑，不能连一次就假设一直连着。
2. **要主动叫醒。** 实测：调一次 `PairAsync()`（返回 `AlreadyPaired`）能把它唤醒；
   之后 `GetGattServicesAsync(Uncached)` 才回全部 5 个服务。
   ⇒ 固件要**重试**（实测 2~3 秒一次、最多 4~6 次能成功），别一次不成就算完。
3. **回答是分片的。** ELM327 的回答会分成多次通知送来，**不能假设一次通知 = 一整条回答**，
   要**按 `>` 提示符收尾**（与有线 327 的读法一致）。
4. **Windows 自己的配对状态就是不一致的**：`DeviceInformation.Pairing.IsPaired` 报 `False`，
   而 `PairAsync()` 回 `AlreadyPaired`。⇒ 别拿 `IsPaired` 当判据。

## 4. 往 `FFF2` 写指令：已打通（写法与两个真凶）

**现状**：连接、配对、服务发现、订阅（`WriteClientCharacteristicConfigurationDescriptorAsync(1)` → `Success`）、
**写入**（反射拿精确重载）全部通过。读取侧见 §4.1（PowerShell 不行，用 bleak）。

试过并失败的写法：

| 写法 | 结果 |
|---|---|
| `$tx.WriteValueAsync($buf)` | `Cannot find an overload … argument count: "1"` |
| `$tx.WriteValueWithResultAsync($buf, 1)` | `Cannot find an overload … argument count: "2"` |
| `WriteValueAsync(DataWriter.DetachBuffer())` | `The parameter is incorrect.` |
| **★ 反射拿精确重载 + `Invoke`** | **✅ `Status=Success`** |

★★ **能通的唯一形式（照抄这个）**：

```powershell
$m   = @($tx.GetType().GetMethods() | Where-Object {
         $_.Name -eq 'WriteValueWithResultAsync' -and $_.GetParameters().Count -eq 2 })[0]
$buf = [Windows.Security.Cryptography.CryptographicBuffer]::CreateFromByteArray($bytes)
$op  = $m.Invoke($tx, @($buf, 1))          # 1 = GattWriteOption::WriteWithoutResponse
$res = Await $op ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult])   # Status=Success
```

**两条原因都查清了（上一版的两处推测都不对，这里更正）**：

1. `Cannot find an overload` **与连接状态无关** —— 在 `CS=1`（已连接）下复测，PS 原生的
   `WriteValueAsync($buf)` / `($buf,1)` **照样报** argument count 1/2，而
   `OverloadDefinitions` 里那两个重载**明明存在**。这是 **PowerShell 的 WinRT 重载解析失灵**，
   跟"没连上""参数不对"都没关系 ⇒ **反射拿精确重载**是正解。
2. `The parameter is incorrect.` 的**真凶不是 Windows BLE 栈，而是 IBuffer 本身是坏的**：
   `DataWriter.DetachBuffer()` 返回的 IBuffer **长度是错的** —— 送 `"ATZ\r"`（4 字节），
   它读回 `Length = 1`。而 `CryptographicBuffer.CreateFromByteArray` 反射读回 `len=4 'ATZ\r'` ✅。
   ⇒ **造 IBuffer 只能用 `CryptographicBuffer`**，别用 `DataWriter.DetachBuffer()`。
   （旁注：`.Length` 在这两种 `__ComObject` 上都被 PS 显示成 1，那是 PS 的属性覆盖，
   不是真长度 —— 想拿真长度得走 `DataReader` 反射。这也是个陷阱。）

实测 4/4 条指令写入成功，送出字节逐个核对无误：`41 54 5A 0D` / `41 54 45 30 0D` /
`41 53 50 30 0D` / `30 31 30 30 0D`。

## 4.1 ★ 读取路径：PowerShell 收不到，**用 Python + bleak**

**PowerShell 的接收侧没打通**（试过的全记下来，别重复）：根因是 `Add-Type` 的 csc
**无法引用 WinRT 元数据**（`Windows.Storage.winmd` 报 `assembly name or codebase was invalid`），
所以编译出来的代码里写不出 `IBuffer`；而 `ValueChanged` 回调交给脚本的
`args.CharacteristicValue` 是**未投影的 `System.__ComObject`**（cast `IBuffer` 失败、
`GetProperty('Length')` 为 null）。另外还纠正了一条旧结论：**"WinRT 事件订不上"是错的** ——
用 `Add-Type` 编译真 .NET 方法 + `CreateDelegate` 注册，回调**确实会触发**，但触发后仍拿不到字节。
自己声明 COM ABI（`IBufferByteAccess` vtable）能 cast 成功，但 `get_Length()` 取到错值、
`Marshal.Copy` 直接把 PowerShell 进程搞崩。轮询 `ReadValueAsync(1)` 则永远回 20 个 0 字节。

⇒ **接收用 `bleak` 已经彻底解决**（`tools/bt-obd/ble-obd-client.py`）。要在这台 Windows 上
用 C# 收通知，需要一个能引用 winmd 的正经 WinRT 工程，**不值得再往 PowerShell 里投**。

**实测真值（2026-09-27，点火到 ON，Python 客户端）**：

| 指令 | 回答 | 解读 |
|---|---|---|
| `ATZ` | `ELM327 v1.5` | 适配器活着 |
| `ATDP` | `AUTO,ISO 14230-4 (KWP FAST)` | ★★ **见 §5.5** |
| `010C` | `41 0C 3D 24` | 0x3D24/4 = **978 rpm**（怠速） |
| `010D` | `41 0D 00` | 0 km/h（车没动） |
| `0105` | `41 05 7F` | 0x7F−0x40 = **87 ℃**（水温） |
| `010F` | `41 0F 63` | 0x63−0x40 = **59 ℃**（进气） |
| `0100` | `41 00 BE 3E B0 11` + `41 00 98 18 00 00` | 支持的 PID 位图 |
| `ATRV` | `13.4V` | 电瓶电压（说明在充电/发动机在转） |

## 5.5 ★★ 顺带一个对**整个项目**都重要的发现：这台 206 是 **KWP FAST，不是 CAN**

`ATDP` 实测回：**`AUTO,ISO 14230-4 (KWP FAST)`** —— 也就是 ISO 14230-4（KWP2000 over K-line，
快速初始化），**不是** ISO 15765-4 CAN。两条后果：

1. **`ATSP0`（自动搜协议）在 KWP 上很慢**：`0100` 头几秒只回 `SEARCHING...`，之后才补
   `BUS INIT: OK` + 数据。⇒ **超时要给 10~12 秒**，别把前几秒的 `SEARCHING...` 当失败
   （这与 9/22 那条"`ATSP3` 会把 K 线卡在 `BUS INIT`"是同一类现象，都是 K 线慢的锅）。
2. 低速 **K 线**做诊断本来就慢，这解释了为什么 `live-obd`/`drive-log` 那套每次问一个 PID
   都要留几百毫秒到几秒。**别指望它跑高频**，`0105`/`010F` 这种慢量降频问是对的。

## 5. ★ 笔记本侧的两个大坑（会让判据完全失效，务必先看）

1. **必须用 Windows PowerShell 5.1 跑，不能用 pwsh(7.x)。**
   WinRT 的 `IAsyncOperation → Task` 桥（`System.WindowsRuntimeSystemExtensions`）只在
   .NET Framework 里；pwsh 是 .NET Core，**这个类型根本不存在**。
   症状极具误导性：**表头打出来了、设备列表却是空的** —— 会让人得出"没有 BLE 设备"的错结论。
   （另外 `Add-Type -AssemblyName Windows.Devices.Enumeration` 在 pwsh 里也报
   "One or more required assemblies are missing"。）
   ⇒ 一律 `powershell -ExecutionPolicy Bypass -File …`。
2. **`Register-ObjectEvent -EventName ValueChanged` 不能用** —— 报
   `Windows PowerShell cannot subscribe to Windows RT events`（PS 的事件系统挂不上 WinRT 事件）。
3. **`.ps1` 必须存成带 BOM 的 UTF-8。** `write`/`edit` 工具默认存**无 BOM**，而 PS 5.1
   会按 ANSI/GBK 去读 ⇒ 中文注释乱码后**直接导致 parse error**（实测一次 21 个）。
   每次改完 `.ps1` 都要补 BOM。
4. **`-Cmds` 传参不加引号会被吃前导零**：`-Cmds ATZ,ATE0,0100` 里的 `0100` 被 PS 当数值，
   变成 `"100"` 发出去。必须写 `-Cmds "ATZ,ATE0,0100"`（脚本内部已按逗号再切一次兜底）。

## 6. 工具

| 脚本 | 用途 | 状态 |
|---|---|---|
| `tools/bt-obd/probe-ble-gatt.ps1` | 扫描 BLE、判"是不是 BLE"、配对、列全部服务/特征/UUID | ✅ 已验证可用 |
| `tools/bt-obd/ble-obd-client.ps1` | 发 ELM327 指令（**写入**路径的参考实现 + 四处踩坑记录） | ✅ 写通；读不到（见 §4.1） |
| `tools/bt-obd/ble-obd-client.py` | **真正收数据用这个**（Python + bleak） | ✅ 已实跑出真值 |
| `tools/bt-obd/obd-ble-sim.py` | **反向**：让**桌面冒充诊断头**（GATT server + 假 ELM327），台面上就能试板子的 BLE 中心 | ✅ 2026-09-28 深夜实测（§14） |

`.ps1` 两个都必须 **Windows PowerShell 5.1** 跑（脚本里已加运行环境自检，跑错解释器直接报错退出）。
`bleak` 已装（3.0.2）。

## 7. 板子上的活有多大（好消息）

`ObdSource` 已经在做 **ELM327 的问答与解析**（`Parse-Obd`、PID 轮询表、`0105`/`010F` 的 ℃ 换算都在），
**换的只是"传输层"**：UART(Serial1) → BLE。解析那半可以原样复用。

资源也够：当前固件 `740,352` 字节 = **4MB app 分区的 17.7%**，NimBLE 加得下；
`Serial1`（GPIO17/18）那组脚与 VAN 的 GPIO44 也不冲突。

## 7.5 给板上 NimBLE 固件的五条（照抄）

1. **服务 `FFF0`**；**订阅 `FFF1`（CCCD = Notify）先做，再往 `FFF2` 写 ASCII + `\r`**。
2. **`FFF2` 是「写 + 无响应写」（`0x0C`）** ⇒ 板上也应该用 **Write Without Response**。
3. **回答分片到达** ⇒ 必须**按 `>` 提示符收尾**（实测 `ATZ` 被切成 1+13 两片），
   不能假设"一次通知 = 一整条回答"。
4. ★ **超时要给宽**：这台车是 **KWP FAST**（§5.5），`0100` 头几秒只回 `SEARCHING...`，
   之后才补 `BUS INIT: OK` + 数据 ⇒ **10~12 秒**，别当失败。
5. **重连**：这个头空闲会自己掉线，且掉线后的签名是「服务只回 1 个 `0x1801`」⇒ 必须重试。

---

## 7.6 ★ 怎么和板子接（方案 + 落地清单，2026-09-27 定方向）

### 为什么非得是 BLE：UART 那条路在这块板上**物理上没了**

2.8C 的 RGB 并口把 GPIO 吃掉了：`PCLK=41 / DE=40 / VSYNC=39 / HSYNC=38 / BL=6`
加 DATA 那 16 根（**含 GPIO17/18**）。而 `ObdSource` 的默认口正是
`OBD_RX_PIN=17 / OBD_TX_PIN=18` ⇒ **冲突**，所以现在这份固件是 `-DOBD_SERIAL=0`。
⇒ **2.8C 上 OBD 只有两条路：BLE，或继续让笔记本当真值源。**（见 README 里同一条）

### 挂在哪块板：**主板（右屏）**

理由：① 主板上**本来就有** `ObdSource` 的接线（`g_data = VehicleDataService(&Serial1)`）；
② 进气温度是**右屏**的副表，就近；③ 水温从板已经有 `link` 这一档
（实测 `coolant=link intake=link` 都在用），主板拿到 OBD 后会顺着既有的
**ESP-NOW/链路 `DATA` 帧**送过去（`link_msg.h` 的 `flags` 里
`bit3..2 = coolant、bit1..0 = intake` 两格**现成**），**不用新加协议**。

### 板上要写的东西（按顺序）

1. **NimBLE central**：扫描 → 按名字 `OBDBLE` 或地址 `AABBCC122233` 连上 → 拿到
   `FFF0` 服务 → `FFF1` 订阅通知、`FFF2` 记住写句柄。
2. **传输层抽一层**：把 `ObdSource` 现在对 `Serial1` 的读写换成"能喂字节"的接口，
   **BLE 收通知 → 喂进 `ObdSource` 的同一个解析器**。解析那半（`Parse-Obd`、
   PID 轮询表、`0105`/`010F` 的 `℃ = A − 40`）**一行都不用改** —— 这是本方案最大的便宜。
3. **问答状态机**：一次挂一个 PID；写完等 `>`；超时 10~12 秒（KWP FAST）。
4. **重连**：掉线签名是「服务只回 1 个 `0x1801`」，重试 2~3 秒一次。
5. **数据接线**：主板 `data_service` 的 `intake` 来源变 `Obd`（右屏副表就是它）；
   `coolant` 也走 `Obd`，并通过链路发给从板（从板那格 `coolant=link` 已经在等）。

### ✅ 2026-09-27 深夜：**板上已经写完了**（这一节原先是"动手前的待确认"）

| 项 | 状态 |
|---|---|
| `ObdTransport` 传输层 | ✅ 抽出来了（`lib/dashcore/obd_transport.h`）—— 问答/解析**一行没改** |
| `ObdTransportBle` | ✅ 写好（`obd_transport_ble.h/.cpp`，NimBLE-Arduino **2.5.1**） |
| 主板固件 `esp32s3-rgb-master-now` | ✅ **编译 SUCCESS**；对象文件 388,988 字节 |
| NimBLE 与 IDF5.5 + LVGL + ESP-NOW 共存 | ✅ **能共存**（这是当初最大的未知） |
| 固件容量 | **1,704,144 字节 = 2MB app 槽的 81.3%**（加 NimBLE 前 71.3%）⇒ 余量约 380KB |
| native 单测 | ✅ 385 例 / 383 通过 / 2 跳过 / **0 失败** |

★ **`lib_deps` 是覆盖不是合并**：只写 NimBLE 会把继承来的 `lvgl/lvgl@^9.3.0` 顶掉、
整个 UI 编不过 ⇒ 两个都要写（已在 env 注释里写明）。

★ 顺带被这轮测试抓出一个**会上车的真 bug**（`fe17c7f`）：`sendRequest` 里那句
"PID < 0x10 就补个 `0`"是按 **Arduino `print(v, HEX)` 不补零**写的，而新抽的
`writeHex()` **总是两位** ⇒ 叠加成 `0100C` / `01000` / `01005` / `0100F` ——
**ECU 一个都不认**，而现象只是"OBD 一直没数据"，和"没插头"长得一样。
⇒ 教训：**"测试跑不起来"本身就是风险**（这个 native 构建此前根本没编 `dashcore`，
是 `lib/dashcore/library.json` 让它第一次真正跑起来）。

### 仍未验（下一步就是它）

- **射频共存**：★ **2026-09-27 车上已实测，结论比预想的硬** —— 链路 PHY 在跑时 BLE
  **根本连不上**（永远 `status=13(BLE_HS_ETIMEOUT)`），把链路 PHY 关掉就**一次连上**
  ⇒ 不是"互相让"，是"BLE 拿不到建连要的那几毫秒"。
  ⇒ 修法已落地（**§9**）：**开机先让 BLE 建连、再启链路** + 建连窗口内临时把共存偏好
  切给 BT（一次 ≤8s、让出后冷却 ≥30s）。
  ★ **仍未验**：两者稳态共存时的**链路质量**（主板 `espnow: tx_fail/done_fail`、
  从板 `rx_overflow`/`tick_age`、`meas rx:` 的丢包与 p99）—— 下一单上车盯这几行。
- **真机连通**：`obd-ble: state=ready` + `SRC … intake=obd`。
- 从 BLE 拿到的数据与**有线**那条（`Serial1`）是否一致。
- 这个头在车上长期通电时的花样（点火瞬间掉线、ECU 睡眠后不应答）。

### 其余前置（已解决，留档）

- **NimBLE 在这套构建里能不能加**：✅ 能 —— `h2zero/NimBLE-Arduino@^2.1.0`，
  实测与 LVGL/ESP-NOW/IDF5.5 不打架。
- **flash 预算**：⚠️ 不再是"17.7% 随便加"了 —— 现在是 **81.3%**，加新东西前先看这个数。
- **`0x1801` 那条判据**：板上枚举服务时，"只回 1 个服务"= 没真连上，
  **不是**设备没有 OBD 服务。

### 车上还要定的一条：**这个头插着不拔，耗的是常电**

OBD 座 16 脚是常电，诊断头自己会一直吃电（还会一直广播/等连接）。
两条路：① 主板走 ACC 取电 ⇒ 熄火后主板不跑、BLE 自然不连（推荐）；
② 头一直插着不拔 ⇒ 长期停车要留意电瓶（这与之前给板子定的"别用常电"是同一条纪律）。

## 8. 仍未验

- 从 BLE 拿到的数据与**有线**那条（`Serial1`）是否一致（两边都是同一颗 ECU，理论上应一致）；
- 这个头在**车上长期通电**时会不会有别的花样（点火瞬间掉线、ECU 睡眠后不应答）；
- **板上 NimBLE 的实际表现**（★ 2026-09-27 深夜更正：**代码已经写完并编过**，见 §7.6；
  "上板实测"这一格仍未打勾 —— 车上那次拿到真值用的是 `-DOBD_BLE_ONLY_TEST=1` 的诊断
  构建，链路 PHY 是关的，所以它**不等于**"共存场景下也成"）。

---

## 9. ★★ 射频共存：车上的结论 + 已落地的两条策略（2026-09-27 深夜）

### 9.1 结论（硬证据，来自车上那两次）

| 条件 | 现象 |
|---|---|
| 链路 PHY **不启动**（`-DOBD_BLE_ONLY_TEST=1`） | BLE **一次就连上**：`obd-ble: state=ready conn=1 connects=1`，且拿到真值 `2666rpm / 93.0C / 62.0C` |
| 链路 PHY 在跑 | 永远 `status=13`(`BLE_HS_ETIMEOUT`) —— 八个候选原因全排掉（地址类型 / 客户端状态 / 扫描残留 / 连接参数 / 共存偏好 / 配对 / 内存 / 发射功率） |

⇒ 根因是 **ESP-NOW 把射频占住**（ESP32-S3 只有**一套** 2.4G 射频，由 IDF 的共存仲裁分时）。
"笔记本一连就上、板子死活连不上"也在这里解释清楚了：**笔记本不跑 ESP-NOW**。

框架侧确认（不是猜）：`framework-arduinoespressif32-libs/esp32s3/sdkconfig` 里
`CONFIG_ESP_COEX_ENABLED=y`、`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`、`CONFIG_SW_COEXIST_ENABLE=y`
⇒ **软件共存是编进固件里的**，所以问题在**调度**，不在"没编进去"。

### 9.2 已落地的两条策略（策略全在 `lib/dashcore/radio_arbiter.h`，native 用例钉住）

1. **开机顺序**（`LinkStartGate`；执行点 `src/main.cpp` 的 `link_start_gate_tick()`）：
   **先让 BLE 建连，再启链路 PHY**。理由就是 9.1 那张表 —— BLE 建连失败的那几秒
   *恰好是*链路还没起来的那几秒，把两者错开就够了（不需要动任何射频参数）。
   ★ **硬上限 15 s**：到点一律开闸 —— 链路是仪表的**主命脉**，绝不允许"OBD 连不上
   ⇒ 从板永远没数据"。代价如实记：这 15 s 里从板是模拟数据状态
   （`tick_age > 3s` → Sim + 挂"数据不可信"角标）。
   ★ 只在**主板 + `OBD_BLE`** 的构建里生效：从板没有 BLE，有线档/pcpreview 进不来
   ⇒ 那些构建的启动序列**一个字没变**。
2. **运行期优先权**（`RadioArbiter`）：BLE 真要建连时，把共存偏好从 `BALANCE` 临时切到
   `ESP_COEX_PREFER_BT`，**一次最多 8 s**，让出后**至少冷却 30 s**；连上之后再压
   `grace 2 s`（服务发现 + 订阅 CCCD + 第一条指令要几个来回），然后还给链路。
   ★ 为什么必须有上限与冷却：`busy` 可以是**分钟级**的（头没电/没插时状态机一直在
   扫 + 退避重连）⇒ 没有上限就成了"拿主命脉换一个可选功能"，方向反了。
   ★ `busy` 的定义里带 `peer_valid_`：**没扫到过对端就一次都不抢**（台面上没插头时不白抢）。

### 9.3 日志怎么读（下次上车就看这几行）

| 行 | 判据 |
|---|---|
| `link: 启动闸门开了 —— BLE 已连上(ready),等了 Nms` | 顺序策略生效且**没等到上限**（最好的情况） |
| `… BLE 没连上(到 15s 上限:仪表优先,不再等 OBD)` | 兜底生效：从板恢复收数据，OBD 回头再试 |
| `obd-ble: … radio=BT win=N cap=M` | `win` 涨而 `conn` 恒 0、`cap` 也涨 ⇒ **共存这招不够**，要上"让出 VANRAW / 拉长连接间隔"那几条 |
| 主板 `link:`/`meas rx:` + 从板 `espnow: rx_overflow`、`tick_age` | 共存对**链路质量**的代价（本单没量，下一单要量） |

### 9.4 两个临时宏已删（`platformio.ini`）

`-DOBD_BLE_ONLY_TEST=1`（诊断构建；注意它**同时**关掉链路 PHY 与 VAN）与
`-DCONFIG_NIMBLE_CPP_LOG_LEVEL=4`（NimBLE DEBUG 日志）都已去掉，注释里留了"怎么加回来"。
⇒ 现在这份 env 编出来就是**可用固件**（链路 + VAN + BLE 都在）。

### 9.5 仍未验

- **共存下的链路质量**：0 丢帧 / `gap_max < 100ms` / `p99 < 20ms` 这套指标有没有被吃掉；
- **上车后 BLE 到底连上没有**：本单只写了策略、编过，**没上过板**；
- 主板 `RGB_BOUNCE_LINES` 20→10 之后**横纹有没有回来**（那 10 行是给 BLE 让内部 RAM 的）。

### 9.6 ★ 桌面首次上板实测（2026-09-27 深夜，两块板都在电脑上）

**这一趟验的是"兜底分支"**（桌上没有诊断头 ⇒ BLE 扫不到对端），日志逐行如下：

| 行 | 读数 |
|---|---|
| `obd: BLE start() = 1  heap=82KB(面板之后)` | BLE 协议栈起来了 |
| `obd-ble: 射频优先权 -> BALANCE(还给链路) (esp_coex=0)` | ★ 共存 API **真的生效且返回 0**（不是 `ESP_ERR_NOT_SUPPORTED`） |
| `obd-ble: state=scanning peer=- conn=0 … radio=balance win=0 cap=0` | ★★ **没扫到对端就一次都不抢**（`win=0`）—— 设计里的"防白抢"在真机上成立 |
| `link: 启动闸门开了 —— BLE 没连上(到 15s 上限:仪表优先,不再等 OBD),等了 15000ms ⇒ 现在启链路 PHY` | ★ 闸门**正好 15.000s** 开（`waited=15000`），随后 `espnow: sta_mac=… ch=6 ps=off` + `link: 主板侧就绪 ESP-NOW` |
| 从板 `link: locked tick_age=2ms seen=4078 seq_gap=0 miss=0 offset=2046ms` / `espnow: rx_frames=10522 gap_rx=0~29ms overflow=0` | 链路恢复，无序号缺口、无溢出 |

**链路正式指标（同一晚，闸门 + 仲裁都在跑的固件上重测）**：

```
meas tx: sent=2000/2000 period=10ms span=19990ms
meas rx: frames=332 seq=0..331 expected=332 lost=0 (0.00%) gap_max=12ms
         p50=10ms p95=11ms p99=12ms over95=0 n=331 wire_gap_max=10ms
         -> loss<0.1% / gap_max<100ms / p99<20ms / n>=300  **全过**
```
⇒ 与改动前的记录（0 丢帧 / `gap_max` 13~20ms / p99 13~14ms）**同档** ⇒ 这次改动
没有把链路指标弄坏。
★ 两个如实记下、**尚未解释**的观察：① 同一行末尾 `link rx … crc=2`（21,218 帧里 2 帧
CRC 错，0.009%）；② 收端汇总窗口只覆盖 332 帧 / 3.31s（发端发了 2000 帧 / 20s，
`coalesced=426` 可能与它有关）—— 判据里的 `n>=300` 就是照这个口径定的，但这一格下次要弄清楚。

**仍然没验的**（差异要说清）：上表全是**兜底分支**（BLE 没连上）。**"BLE 先连上 ⇒ 闸门
提前开"这条 happy path 一次都没在真机上走过** —— 桌上诊断头不在广播范围（`peer=-`）。
要验它：把诊断头供电（车上，或桌面给它 12V：OBD 16 脚 + 4/5 脚地），预期看到
`obd-ble: state=ready conn=1` → `link: 启动闸门开了 —— BLE 已连上(ready),等了 Nms`（**N < 15000**）。

★ 顺带一个工具 bug（不是固件）：`rf-measure.ps1` 原来**裸发一个 `w`**，而单字符命令要求
**行首**（`serial_cmd.h`）⇒ 那个 `w` 落进日志流中间被当回放数据吃掉，**发端不打 `meas: 开跑`、
收端也没有 `meas rx:`，脚本却看起来跑完了**。已补前导 `\r\n`（脚本不在仓库里，在 scratch 目录）。

---

## 10. ★★ 2026-09-27：**VAN 上就有水温** —— 这条改变了"从板要不要背 BLE"

**动机**：从板 BLE 反复连不上（见 §9），而"从板当 OBD 网关"这个分工 v2 的**全部理由**
就是右屏要的进气温度 + 各处温度。如果温度本来就在 VAN 上广播，那从板这份
~61 KB 的内部 RAM 代价就值得重新算一遍。

**核实结果（两条独立来源，详见 `VAN-PROTOCOL.md` §4.8）**：

| 量 | VAN 上有吗 | 位置 / 公式 |
|---|---|---|
| **水温** | ✅ 有，**广播**（`0x8A4`，周期 500 ms，不要 ACK） | `data[2]`，`raw − 39` |
| **外界温度** | ✅ 有，广播 | `0x8A4.data[6]`，`(raw − 0x50) / 2` |
| **机油温度** | ✅ 有，广播 | `0x4FC.data[7]`，`raw − 40` |
| 蒸发器温度 | ✅ 有 | `0x4DC`（A/C 系统） |
| **进气温度** | ❌ **没有** | 外部库 **64 个包体定义逐个搜过**，`intake`/`air temperature`/`manifold` **零命中** |

**⇒ 结论（对分工 v2 的影响）**：

1. **从板不再"必须"背 BLE**。它要显示的水温，`0x8A4` 上就有；而它是挂在
   **VAN 舒适段**上的板子（右屏），收得到这一族。
2. 唯一真正只有 OBD 能给的量是 **进气温度（`010F`）**。于是取舍变成：
   - 要进气 ⇒ 从板得连上 BLE（目前**连不上**，且是为它单独付 61 KB 内部 RAM）；
   - 不要进气 ⇒ 从板可以完全不装 BLE，把内部 RAM 还给弹跳缓冲（**横纹问题一起解决**）。
3. ★ **别把这条读成"从板永远不需要 BLE"**：TPMS、故障码、油耗那些仍未解，
   将来若要，还是得有一条 OBD 通路。本条只否掉**"为了让从板显示水温而付 BLE 的代价"**。

**状态**：VAN 侧的水温/外界/油温解析**已接进 `VanSource` + `data_service`**
（优先级 `OBD > VAN > Link > Sim`，native 用例未变红），公式是 307 的逆向结果，
**本车标定待上车一次**（判据见 `VAN-PROTOCOL.md` §4.8.5）。

---

## 11. ★★ 2026-09-28 晚：两块板都在机旁的一轮实测（含两条**推翻旧结论**的读数）

### 11.1 ★ 10 行弹跳缓冲在 12 MHz 下**没有横纹** —— 副板不必在"横纹 vs BLE"之间二选一

旧结论（写在 `platformio.ini` 里）是"30 行才不横纹"，但那**是 18 MHz 时代做的**，
而 `RGB_PIXEL_CLOCK_HZ` 早已降到 **12000000u**，之后**没人回头重测过**。
专门开了一档 `[env:esp32s3-rgb-slave-b10ble]`（12 MHz + 10 行 + BLE）刷上去，
**车主实屏确认：没有横纹**。

⇒ 10 行在 12 MHz 下是够的；30 行那 55 KB 可以还回去。

### 11.2 ★ 10 行 + BLE **同时装得下**（把"只剩 16 KB"那条账作废）

同一版固件、**重新开机之后**的启动行（★ 口径：只有重新开机的读数可复现）：

```
rgb: bounce=10行/块(2块共18KB内部SRAM)
rgb: 双fb 之后 空闲 PSRAM=7282KB heap=96KB(内部)
obd: BLE start() = 1  heap=35KB(面板之后)
obd: heap_caps 内部 free 96→35 KB(掉 61) largest 55→25 KB | PSRAM free 7282→7282 KB(掉 0)
```

- 面板 + 双 fb 之后 **96 KB**（不是估的 77 KB）；
- `BLE start()` 吃 **61 KB**（这条与旧账一致，是同一个数）；
- 之后还剩 **35 KB**。

★ **一条要如实记下的反复**：同一块板在**刷机前那版固件**上曾读到
"面板后 167 KB / BLE 后 104 KB"，而新固件重新开机得到的是 96/35。
104 这个数**不可复现**，所以**别再引用它当基准**。差 9 KB（167 vs 96 那一段）
的成因**未查明** —— 可能与当时板子所处的运行阶段有关（那条 104 是在**没有复位**的
运行态读到的，而这次是复位后的启动序列）。**下次要引用内存数字，一律用"复位后启动行"。**

### 11.3 ★ BLE 建连失败的**量出来的**新嫌疑：最大连续块只剩 25 KB

```
obd-ble: state=connecting peer=aa:bb:cc:12:22:33 conn=0 cs=3/3 radio=balance win=1 cap=1
obd-ble: ← client->connect() = 0  isConnected=0  耗时=17ms  rssi=-69
```

`start()` 之后 **`largest`（最大连续块）只有 25 KB**。若建立连接需要一块连续的
控制器状态，25 KB 不够 ⇒ 这能解释"16~17 ms 就当场返回"（不是 15 s 超时）。

★ **但这仍只是嫌疑，不是定论** —— 我们**没有**直接证据说明建连要多少连续内存。
两条边界都要守住：
- 别把它写成"已确认原因是碎片"；
- 也别回到"**总量**不够"那个说法（总量 35 KB，而昨天主板只剩 36 KB 时**连上过**）。

### 11.4 下标与哨兵：油温 `data[6]`、外界温度哨兵是 `0x00`（不是 `0xFF`）

同一轮把两个字段钉死了，两处都改了固件（详见 `VAN-PROTOCOL.md` §4.8.1b / §4.8.1c）：

- **机油温度下标是 `0x4FC.data[6]`**（我第一版写成 `data[7]`，**那是错的** ——
  `data[7]` 是油量百分比）。★ 本车这一格**恒为 `0x00`**（两份切片共 9 帧无一例外，
  而同帧的 `data[5]` 灯光位域是活的：读到过 `0x08` = 近光）⇒ **本车拿不到油温数值**。
- **外界温度的哨兵值是 `0x00`，与水温的 `0xFF` 不是同一套**。依据是同一份 JSON 的
  **示例帧**（`data[2]=3C=21℃` / `data[6]=7D=22.5℃`）与**实测帧**
  （未到点火档时 `data[2]=FF` / `data[6]=00`）对照。只挡 `0xFF` 的实现会把 `0x00`
  算成 **−40 ℃** 当有效值显示出去。

### 11.5 这一轮最有价值的东西可能不是上面任何一条，而是**两个环境坑**

都在 `tools/build/pio.ps1` 上，且其中一个**把副板刷黑过**，详见 `docs/BUILD-ENV.md`：

1. **`PYTHONIOENCODING=utf-8` 不加，刷机会崩在写 bootloader 的中途** ——
   本机 ANSI 代码页 936(GBK)，而 esptool 的进度条用 `░`(U+2591) 画 ⇒
   `UnicodeEncodeError` ⇒ 上传线程死掉 ⇒ **启动区擦了没写全 ⇒ 板子黑屏、串口全静默**。
   （`[Console]::OutputEncoding` 救不了，那只改 .NET 侧。）
2. **`.ps1` 没 BOM，Windows PowerShell 5.1 会把注释当代码执行** ——
   中文注释按 GBK 解读成乱码 ⇒ 块注释的 `#>` 认不出来 ⇒ 脚本结构崩掉。
   ⇒ 定案：**`pio.ps1` 保持纯 ASCII**，中文详解放 `docs/BUILD-ENV.md`。

### 11.6 ★★ 一条**没验成**的事（别当成已验）

刷完之后副板出现：

```
SRC speed=van rpm=van coolant=van intake=link | v=0.0km/h 910rpm 87.0C 35.0C
```

`coolant=van` 是**第一次**有来源真的落到 `Van`。**但这不能当作"VAN 水温解对了"的证据**，
原因是**两个源在同时供数**：

| 板 | 那一行 |
|---|---|
| 副板 | `SRC speed=van rpm=van coolant=van intake=link` |
| 主板 | `SRC speed=van rpm=van coolant=sim intake=sim` |

主板那两格是 **`sim`（模拟数据）**，而它**也有水温弧**，值还和副板的读数吻合
（副板 87~91 ℃、主板 90.5 ℃）。⇒ **分不清**那个 90 ℃ 是 VAN 真值还是主板模拟值经链路来的。

**⇒ 本车标定仍未完成。** 要定标必须让两个源**可分辨**，条件至少是：
主板也刷上带 VAN 温度解析的固件，且真车 VAN 在校准源（`vanraw: ok` 在涨）。
判据仍按 `VAN-PROTOCOL.md` §4.8.5（冷启动看 `data[2]` 单调升到稳态 85~95 ℃、
与 OBD `0105` 对照），**别用现在这个 90 ℃ 当已验**。

---

## 12. ★★ 2026-09-28 深夜：BLE 建连失败**查到了哪一步**（含一条推翻前面推断的读数）

### 12.1 ★★★ `status=13` **不是控制器给的错误码** —— 前面基于它的推断全部作废

**怎么查出来的**（读库源码，不是猜）：

`NimBLEClient::connect()` 里那个 `rc` 来自 `taskData.m_flags`：

```cpp
if (!NimBLEUtils::taskWait(taskData, (m_connectTimeout + …) * (retries + 1))) {
    if (m_connStatus != CONNECTED) {
        ble_gap_conn_cancel();
        taskData.m_flags = BLE_HS_ETIMEOUT;      // ← 兜底写的 13
    }
}
rc = taskData.m_flags;
if (rc != 0) NIMBLE_LOGE(LOG_TAG, "Connection failed; status=%d %s", rc, …);
```

而 `NimBLEUtils::taskWait()`：

```cpp
xTaskNotifyWait(0, TASK_BLOCK_BIT, &notificationValue, 0);   // ★ 超时 = 0 tick（不阻塞）
if (notificationValue & TASK_BLOCK_BIT) return true;         // ← 走这条就**不写 m_flags**
return xTaskNotifyWait(0, TASK_BLOCK_BIT, nullptr, ticks) == pdTRUE;
```

⇒ **快速路径从不设置 `taskData.m_flags`** ⇒ `rc` 可能是 `NimBLETaskData` 的构造值。
⇒ 我们看到的 **`status=13` 极可能是未初始化/兜底值，不是控制器真实错误码。**

**因此作废的推断（两条，都别再引用）**：

| 作废的说法 | 为什么作废 |
|---|---|
| "内存不够导致建连失败（largest 只剩 25KB）" | 15 秒超时窗口里 17ms 就返回 ⇒ 根本没等到控制器结果 |
| "地址类型不对（`type=0` public，而 `AA:BB:CC…` 是 random static）" | 同上；而且这条从来没有直接证据 |

★ **教训（写给下一个查 BLE 的人）**：`status=13` 在 NimBLE-Arduino 2.5.1 上
**不能当作"控制器报超时"来读**。要拿真错误码必须自己挂 `onConnectFail` 打印 `reason`，
或读 `client->getLastError()` —— 而 `connect()` 只返回 bool，**这两条路现在都没走**。

### 12.1b ★★★ 2026-09-29 凌晨：异步版**挂上了 `onConnectFail`，13 依然出现** —— 但这次知道了它是谁

`onConnectFail(reason)` 已接（`lib/dashcore/obd_transport_ble.cpp`，含 `millis()` 时间差）。
实测（`tools/bt-obd/captures/bench-2026-09-28-fail-latency.log`）：

```
obd-ble: onConnectFail #1 reason=13(0xD) 距发起 17ms
obd-ble: onConnectFail #2 reason=13(0xD) 距发起 17ms
```

**`reason` 就是十进制 13**（不是宽值被 `%02X` 截断 —— 我们改印原始整数后确认）。
所以 13 本身有效，只是**它的含义不是"对端超时"**：

| 出处 | 触发 |
|---|---|
| `ble_gap.c:1131 ble_gap_master_connect_cancelled()` | 主机**取消建连** ⇒ `event.connect.status = BLE_HS_ETIMEOUT` |
| `ble_gap.c:2632` → 上面的调用 | 只由 `BLE_ERR_UNK_CONN_ID` 触发，即**主机发了 `LE Create Connection Cancel`** |

⇒ **13 = "主机把这次建连取消了"**，而主机只会在自己的建连计时器到点时取消。
而 `m_connectTimeout` 是 **30000 ms**（`NimBLEClient.cpp:69`）⇒ 计时器不可能 17ms 到点。

★ 与 §12.1 的账要对上：那里写"同步版 16~17ms 返回、随后库补写 13"，
候选中第一条就是"**连接被取消**"（标注"尚未验证"）。
现在**异步版（没有 `taskWait`）量到同一个 16~18ms** ⇒
**不是 `taskWait` 的问题，而是那个取消动作本身**。§12.1 的结论要按这一格收窄。

★ 同期**排除**（都在 `docs/CAR-TEST-BLE-V2-RESULT.md` 附五，有证据文件）：
地址类型（强制 random 无效）、头被占用（笔记本断开后仍失败）、内存、射频共存、
信号强度、现场有第二个 FFF0 设备。**头本身可连**（`CONNECTABLE_UNDIRECTED`、
FFF0 就在广播包里、笔记本 2.6s 连上）。

⇒ 下一步只验一个数：**改 `m_connectTimeout`，看 17ms 跟不跟着变**（见附五 ⑤.9）。

### 12.1c ★★★★★ 2026-09-29 凌晨：**根因 = `setConnectTimeout` 的单位写错了（毫秒被当成秒）**

上一节那个"只验一个数"的实验做完了，答案就是根因本身：

```cpp
client_->setConnectTimeout(15);             // ← 原文的注释写"秒"
```

**这个 API 收的是毫秒**：
- `NimBLEClient.h`：`void setConnectTimeout(uint32_t timeout);`
- `NimBLEClient.cpp:584`：`m_connectTimeout = time;`
- `NimBLEClient.cpp:69`：`m_connectTimeout{30000}` —— 注释原文
  *"The number of milliseconds before timeout, default is 30 seconds."*
- 用法：`ble_gap_connect(..., m_connectTimeout, ...)`，
  内部 `ble_npl_time_ms_to_ticks(duration_ms, …)`

⇒ **真正的建连超时一直是 15 毫秒**（比库默认小 2000 倍）
⇒ 主机计时器 15ms 到点 ⇒ `LE Create Connection Cancel` ⇒ `UNKNOWN_CONN_ID`
⇒ `ble_gap_master_connect_cancelled()` ⇒ `status = BLE_HS_ETIMEOUT` = **13**。

| 实测档位 | 结果 |
|---|---|
| `15`（线上那版） | 恒定 **16~18ms** 失败，130 次全败 |
| `3000` | **连上**（433ms 完成，150 秒零掉线）|
| `30000`（库默认） | **连上**（350ms / 969ms）|

**修后实测**（真头、台面、默认档）：
```
obd-ble: onConnect ✓（异步连接完成，距发起 969ms，接着去拿特征）
obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready
SRC speed=van rpm=obd coolant=obd intake=obd | v=0.0km/h 894rpm 89.0C 64.0C
```

⇒ §12.1 / §12.1b 里"13 是主机取消"那条判断**是对的**，只是当时没往下追
"主机为什么取消"；答案是我们自己**传了个 15**。
⇒ 之前所有"假头能连、真头不能"的解释（射频/地址类型/占用/内存）**全部作废**：
  真头广播间隔 **2~5 秒**（WinRT 实测），15 毫秒的建连窗里几乎抓不到；
  假头广播密，碰巧抓得到。

★ **教训**：给库传数字的地方，注释必须写单位，并且要能对到库的声明。
   `// 秒` 这五个字让后面每一轮排查都按"15 秒"推理，白花了一整天。
★ 现场应急：串口 **`t`** 可以切建连超时档 {3000, 30000, 1000, 10000}，
   不必重刷固件（`main.cpp` / `serial_cmd.h`，口径与 `o` 相同）。

### 12.2 本轮**排除**掉的假设（都有实测依据）

| 假设 | 判据 | 结论 |
|---|---|---|
| 扫描没扫到对端 | `onDiscovered()` 命中、`rssi=-69~-71` | ✗ 排除，扫得到 |
| 停扫后立刻连被控制器拒（.cpp 里那条老注释） | 加了 600ms 闸门后**仍是 17ms 失败** | ✗ 排除 |
| 存下来的地址过期（换头/对端重启） | 加"连败 5 次丢地址重扫" ⇒ 重扫**又扫到同一个** `aa:bb:cc:12:22:33` | ✗ 排除，地址新鲜、头就在那儿 |
| 内存不够 | 见 12.1：17ms 返回，没等到控制器结果 | ✗ 作废 |
| 手机/笔记本占着这个头 | 笔记本用 bleak 连同一个头**一连连妥**（2026-09-27 实测） | ✗ 排除 |

### 12.3 本轮**落地的两个修复**（都是真 bug，与根因无关也该修）

1. **`kPostScanSettleMs = 600`**（`obd_transport_ble.{h,cpp}`）——
   把 `.cpp` 注释里承诺过、却被整段删掉的"停扫之后等够再连"闸门补回来，
   ★ 并且**给它自己的时间戳 `scan_stop_ms_`**：绝不能借 `last_try_ms_`
   （它在函数开头刚被赋值，差值恒 0 ⇒ 闸门永远成立 ⇒ connect() 一次都发不出去，
   2026-09-27 就是这么栽的，然后闸门被删、原始问题又回来了 —— **同一处栽过两次**）。
2. **`kRescanAfterFails = 5`**（同上）—— `peer_addr_` 原本是**开机扫一次就再也不刷新**
   （`onDiscovered()` 里 `if (want_peer_) return;`）；换头/对端重启后地址一变就是
   **拿过期地址盲撞**，而那个错误形状与"地址类型不对"完全一样，日志分不出来。
   现在连败 5 次就丢掉地址重扫。

### 12.4 下一步（**要真错误码，不要再从 `status` 推**）

1. ~~`onConnectFail` 里已经打了 `reason`~~ —— ★ 它**从来没出现过**，说明
   `onConnectFail` 根本没被调用，这与 12.1 自洽：快速路径直接返回、不走回调。
2. ✅ **已做（2026-09-28）：读 `client_->getLastError()` 打进 `connectNow()` 那行日志。**
   **结果：`lastErr=13`，与日志里的 `status` 是同一个数。**
   ⇒ 这条也走不通：失败路径**本身就是设 13 的那条**（`error:` 标号处 `m_lastErr = rc`，
   而 `rc` 就是那个补写的 `BLE_HS_ETIMEOUT`）。**`getLastError()` 拿不到真错误码。**
   ★ 同一行顺带打了 `内部free=30KB` ⇒ **"内存不够"这条可以彻底划掉了**
     （30KB 空闲，而 17ms 就返回，根本没等到控制器分配连接状态）。
3. ⇒ **只剩这一条路：改 `asyncConnect = true` + 在 `onConnect`/`onConnectFail`
   回调里驱动状态机**（结构性绕开 `taskWait` 那条可疑的快速路径）。

---

## 13. ★★★ 2026-09-28：异步连接改造完成 —— **真错误码拿到了**

### 13.1 改动（`obd_transport_ble.{h,cpp}`）

- `connect(peer_addr_, /*deleteAttributes=*/true, /*asyncConnect=*/true, /*exchangeMTU=*/true)`
  —— **不再阻塞**。实测发起即返回：`← connect() 受理=1 立即返回耗时=1ms`。
- 回调**只记事实**：`onConnect` 置 `conn_`；`onConnectFail(reason)` 记
  `connect_fails_` / `last_fail_reason_` 并打日志。
- 连上之后的"拿服务/特征/订阅"抽成 `attachCharacteristics()`，**从 `tick()` 里调**，
  不放回调里 —— `getService()`/`subscribe()` 要等对端应答（走 `taskWait` 那一族），
  在 NimBLE 任务上下文里同步等容易把自己等死。
- ★ **补上"发起后一直没回调"的超时收尾**（`kAsyncConnectTimeoutMs = 15000`）：
  同步版由 `taskWait` 的超时负责这一格，异步把它绕开了 ⇒ 必须自己发现，
  否则会**永久卡在 connecting**。走到这条超时本身就是判据（见 13.3）。
- ★★ **`connectNow()` 返回值语义变了**：旧 = "连上了"，新 = "**请求已受理**"。
  调用方不能再拿它 `++connects_`（那会变成"发一次请求算一次成功"）。
- 1 Hz 状态行加上 `fails=` 与 `lastFail=0x%02X` —— 判据落在同一行。

### 13.2 ★ 结果：真错误码 = `0x0D`

```
obd-ble: ← connect() 受理=1 立即返回耗时=1ms rssi=-81 内部free=30KB
obd-ble: onConnectFail reason=0x0D ← ★ 这才是真错误码
obd-ble: state=connecting … cs=87/87 fails=87 lastFail=0x0D
```

★★ **一处必须纠正的说法（我一开始也读错了）**：`0x0D` **就是 13** ——
`ble_hs.h` 的权威定义：

```
#define BLE_HS_ETIMEOUT       13   (0x0D)   ← 控制器报告"建立连接超时"
#define BLE_HS_ETIMEOUT_HCI   19   (0x13)   ← HCI 命令超时（另一个码）
```

所以**不是"换了一个新码"，而是同一个码、这次是真的**：
- 改造前那个 13 是库在 `taskWait` 提前返回后**补写的兜底值**（§12.1）；
- 现在这个 `0x0D` 是控制器**通过 `onConnectFail` 报上来的真值**。

⇒ 异步改造**达到了目的**：`getLastError()` 拿不到的东西，回调拿到了；
而且这条路本身**走通了**（受理 → 回调被调用 → 状态行可见）。

### 13.3 ★ 由此得到的结论（方向变了）

`BLE_HS_ETIMEOUT(0x0D)` 的含义是"**建立连接的过程超时**"，即
**连接请求发出去了、对端没有在时限内完成连接** —— 这与之前"当场被拒 17ms"是
**两个不同的故障**，而后者已被证明是库的假象。

★ 所以现在的问题**从"库/时序"转到了"对端/射频"**：
- 同一时刻 `rssi=-81`（开机扫描读到的最弱一档；先前是 -69~-71）⇒ **链路很弱**；
- 弱链路下 `LE Create Connection` 超时是最典型的症状。

**下一步（按代价排序）**：
1. **把诊断头挪近板子**（或把板子挪出机舱金属遮挡）⇒ 看 `rssi` 是否回到 -70 以内、
   `fails` 是否还在涨。这是**零代码**的一步，先做它。
2. 若信号好了仍失败 ⇒ 查对端是不是**已被别的中心占着**（手机/笔记本还连着它）。
3. 若都不是 ⇒ 试 `setConnectTimeout()` 放大、或改用**白名单式定向广播**连接
   （`NimBLEDevice::setScanFilterMode` / 直接连白名单地址），减少扫描-连接切换。

### 13.4 ★★ 现场对照（2026-09-28）：**诊断头本身完全正常** —— 问题在板子这一侧

车主确认："插着，基本不用考虑信号屏蔽，板子离的比笔记本电脑还近点。"
⇒ 于是同一时刻用**笔记本**做独立对照（`tools/bt-obd/ble-obd-client.py`）：

```
找到 AA:BB:CC:12:22:33  OBDBLE
已连接 = True
已在 FFF1 上订阅通知(CCCD=Notify)
  --scan-only -> 'ELM327 v1.5'
```

**⇒ 诊断头在广播、接受连接、还会答 AT。** 这一条把范围钉死：

| 已排除 | 依据 |
|---|---|
| 头不在/坏了/没插 | 笔记本现场连上并拿到 `ELM327 v1.5` |
| 信号弱 / 被屏蔽 | 车主确认板子比笔记本更近；笔记本 -81dBm 都能一连就上 |
| 地址过期 | 重扫又命中同一个 `aa:bb:cc:12:22:33`（§12.2） |
| 内存不够 | 内部 free 29~30KB，同行打印（§12.4） |
| 库/时序假象 | 已改用异步，`reason` 是控制器真值（§13.1） |

**⇒ 剩下两个方向**（都还没验）：
1. **射频共存**：副板此刻正以约 14 帧/秒收 ESP-NOW（`rx_frames=7725`/10s），
   与 BLE 抢同一个 2.4GHz。**判别很便宜**：敲 `o` 把射频抑制打开、
   让 BLE 独占一小段，看 `fails` 是否停止增长。
2. **地址类型/连接参数**：`type=0`(public) 而地址是随机静态；
   或头的广播间隔长，控制器在时限内没抓到它的广播包。
   ⇒ 可试"先扫到、立刻连"（缩短扫描→连接的间隔）或放大 `setConnectTimeout()`。

### 13.5 ★ 异步改造的后续：修掉一个**自伤 bug**（已修，但还有残留）

改成异步之后，这一行**必须改**（否则是自伤）：

```cpp
// 旧（同步语义下正确）：true = 连上了 ⇒ 清零退避
if (connectNow()) { ++connects_; backoff_ms_ = 0; return; }
// 新：true 只表示"请求已受理" ⇒ 清零退避等于"每次请求都把退避打回起点"
if (connectNow()) return;
```

**后果实测**：`backoff_ms_` 永远停在初始的 500ms ⇒ 每秒猛撞约 2 次 ⇒
`fails` 打到 **200+**，而且每个请求都活不到超时（连接过程被自己打断）。
顺带把 `connects_` 改成"**上升沿**计数"（`ready()` 每拍都成立，
直接 `++` 会把它数成几百次）—— 用 `was_ready_` 记住上一拍。

★ **仍未解决**：修完之后尝试速率**还是不退避**（`cs` 每 60 秒涨约 5，
但 `connectNow begin` 已经不再刷屏）。两次尝试之间约 200ms，
比 500ms 的初始退避还短 ⇒ **另有东西在清 `backoff_ms_`**。
下一个要查的就是它（候选：`connect_pending_` 那条早退绕过了退避闸门、
或 `onDiscover` 每命中一次就把某个计时复位）。**别把这轮当成"退避已修好"。**

### 13.6 ★ 2026-09-28 深夜：退避已修好；并发现**抑制开关有个逻辑漏洞**

**① 退避修好了**（13.5 那个自伤 bug 修完之后实测）：

| 时间 | `cs` | 相邻间隔 |
|---|---|---|
| 4s | 4 | — |
| 9s | 9 | ~3.3 s |
| 14s | 14 | ~2.5 s |
| 19s | 19 | ~2.5 s |

从"每秒猛撞约 5 次"变成 **~2.5 秒一次**、且在按 500→1000→2000 的梯度走。
⇒ `connectNow()` 的"受理 ≠ 成功"语义修正是生效的。

**② 也顺手修掉 `wantsRadio()` 漏判抑制**：抑制打开时它仍然返回 true
⇒ 仲裁器继续替 BLE 抢射频 ⇒ 实测状态行里抑制期间还是 `radio=BT` 且 `win/cap` 在涨。
**那个开关做了两件自相矛盾的事**：不扫不连（所以不会连上），却又一直抢射频
（所以链路/显示照样被压）。修法：`wantsRadio()` 加 `!inhibited_`，
并在抑制分支里显式 `applyRadioArbitration()` 让已占的窗口**立刻**释放
（否则打开抑制后最多还要等 `kHoldMaxMs` 4 秒，容易被误读成"没生效"）。

**③ ★★ 但发现抑制开关有个致命的逻辑漏洞（我自己设计的，得认）**：

`inhibited_` 的语义是"**不再扫 / 不再连**"⇒ 打开它之后 **BLE 根本不发起连接**。
所以它**不能**用来验证"BLE 独占射频时能不能连上"——
那个实验需要的是"**关掉 ESP-NOW、但保留 BLE**"，而本开关做不到。
★ 之前两轮我把抑制当成"BLE 独占"的判别用过，**那两次的结论都不成立**
（抑制期间 `cs`/`fails` 冻住只证明"没在尝试"，不证明"能连上"）。

**⇒ 要做真正的独占实验，需要第三种状态**（尚未实现）：
`inhibit` 拆成两档，或加一个"只停 ESP-NOW、不动 BLE"的开关。
在它落地之前，**射频共存这条假设仍未验**。

### 13.7 当前仍成立的结论（别丢）

- 真错误码 = `0x0D` = `BLE_HS_ETIMEOUT`（控制器报"建立连接超时"），**不是**那个补写的假 13。
- **诊断头完全正常**：笔记本现场连上并答 `ELM327 v1.5`（13.4）。
- **不是**内存（29~30KB free）、**不是**地址过期（重扫命中同一地址）、
  **不是**信号（板子比笔记本更近；`rssi=-83` 是只在 `onDiscovered` 记一次的残影）。
- 待验：**射频共存**（需上面那个第三种状态）、地址类型/连接参数。




★ 【历史：下面这段描述的是**同步 `connect()`** 那条路，而 §13.1 已经把它整段换成异步、
  `taskWait` 不再参与建连 ⇒ "谁在 16ms 唤醒 `taskWait`" 这个问题**已经不存在**。
  保留原文只为对账。】

★ **当前对失败机制的准确描述**（别再说"超时"）：`taskWait` 声称等
`(m_connectTimeout + itvl_max*7) * (retries+1)`（15 秒量级），
**实际 16~17ms 就返回 false**，随后库补写 `BLE_HS_ETIMEOUT`。
所以真问题是"**谁在 16ms 时提前唤醒了 `taskWait`**"，不是"等超时了"。
候选（都尚未验证，别再当结论用）：连接被取消 / 主机复位 / 控制器立刻报错 /
库内部有别的路径释放了任务通知。

---

## 14. ★★★ 2026-09-28 深夜：**桌面当假诊断头**（BLE 外设模拟器）—— 车上那个失败在台面上复现了

### 14.1 为什么做它

诊断头只存在于车上 ⇒ 每一个"连不上"的判断都要跑一趟车。而**桌面本来就能当 BLE 外设**
（Windows 的 GATT server）。这条打通之后，"能不能连上 / 为什么是 `0x0D`"这类问题
可以**在机旁几分钟一轮**地试，不用动车。

### 14.2 能力先验（这台机器，实测）

| 项 | 结果 |
|---|---|
| 适配器 | `A0:B3:39:5C:6B:23`（Intel，Win11 26200） |
| `IsPeripheralRoleSupported` | **True**（`IsCentralRoleSupported` 也 True） |
| 建 GATT 服务 `FFF0` + `FFF1`(read+notify) + `FFF2`(write + write-no-resp) | ✅ 三个都成功 |
| 广播 | `AdvertisementStatus = STARTED`（会先闪一下 `CREATED`/`ABORTED` 再稳住 `STARTED`，**别在那一瞬间下结论**） |

### 14.3 ★★ 两条**做不到**的路（写下来省下一个人半天）

1. **Windows PowerShell 5.1 做不了 GATT server** —— 卡在**收写请求**这一步：
   `Register-ObjectEvent` 直接报
   `Windows PowerShell cannot subscribe to Windows RT events.`；
   而 GATT server 没有 `WriteRequested` 处理就没法接指令（对方写了也没人答）。
2. **`Add-Type` 也做不了** —— PS 5.1 的 csc 要 WinRT 元数据，而这台机器上
   `C:\Windows\System32\WinMetadata\Windows.winmd` **根本不存在**
   （`Add-Type` 报 "The given assembly name or codebase was invalid"）。
   ★ 这与 §4.1「PowerShell 收不到通知」是同一条根因。

⇒ **顺手的路只有 Python + pywinrt**：`winrt-runtime 3.2.1` **有 cp314 轮子**，
  本机 Python 3.14 直接 `pip install` 即可（**不用装 .NET SDK、不用装第二个 Python**）：
  `winrt-runtime` / `winrt-Windows.Devices.Bluetooth` /
  `winrt-Windows.Devices.Bluetooth.GenericAttributeProfile` /
  `winrt-Windows.Devices.Bluetooth.Advertisement` / `winrt-Windows.Storage.Streams` /
  `winrt-Windows.Foundation` / `winrt-Windows.Foundation.Collections`。
  三个必须知道的 API 细节：`start_advertising()` 与
  `start_advertising_with_parameters()` 是**分开的两个名字**（不是重载）；
  `init_apartment(runtime.ApartmentType.MULTI_THREADED)` **要带参数**；
  `IAsyncOperation` 直接 `await`（事件回调在**别的线程**上，要
  `loop.call_soon_threadsafe` 交给事件循环，见工具头部的说明）。

### 14.4 工具

`tools/bt-obd/obd-ble-sim.py` —— 桌面侧的假 ELM327：

- 服务/特征与真头逐字节相同（`FFF0` / `FFF1` notify / `FFF2` write-no-resp），
  并**照抄真头的脾气**：回答分片（`--chunk 13`）、以 `\r` 结尾、末尾跟 `>` 提示符、
  按 `--latency-ms` 慢答。
- 值来源三档：`--mode idle`（常量）、`--mode sweep`（针来回扫）、
  `--mode replay --csv tools/serial-capture/drive-2026-09-22-obd.csv`（**回放真实车上记录**）。
- 故障注入：`--drop-after`（到点停广播 = 拔头）、`--kill-after`（不答 = 头哑了）、
  `--fail-pids 0105`（回 `NO DATA`）、`--search-first`（`0100` 先只回 `SEARCHING...`）。
- `--control sim-control.txt`：**不重启**改值（往里写 `rpm=3000` 这类行即可）。
- `--selftest`：自己广播 + 自己被动扫描 —— ★ 实测**扫不到自己**（Windows 会滤掉本机
  广播），所以"广播里到底有没有 `FFF0`"**不能**靠它判；判据是**板子那边**
  `isAdvertisingService()` 命中（14.5a 已证）。

### 14.5 ★★★ 台面实测（两块板都在机旁，2026-09-28 深夜）

> ★★ **读这一节之前先看 §14.9 的第 1 条**：桌面假头**一个进程只接一次连接**。
> 下面 (a) 那一轮（277/277 全败）**对端的那个模拟器实例当时已经"用过了"**
> ⇒ 它**不能**单独用来证明"射频是根因"。真正的判据是 §14.7 那一轮
> （重启模拟器 + 闸门修好之后，同一块板、同一个假头，上电 6~10 秒连上）。
> (a)/(b) 保留为过程记录：它至少证明了"广播能被被动扫描认出来"和
> "整条 PID 链在台面上能通"。

**（a）射频在跑 ⇒ 复现车上那个失败。** 从板刷 `esp32s3-rgb-slave-obdtest`
（BLE + ESP-NOW 都在），桌面开着模拟器：

```
obd-ble: → connectNow begin (peer=64:d7:6d:0c:f5:55 type=1)   ← 广播**扫到了**（服务 UUID 命中）
obd-ble: ← connect() 受理=1 立即返回耗时=1ms rssi=-71
obd-ble: onConnectFail reason=0x0D
obd-ble: state=connecting … conn=0 connects=0 cs=277/277 fails=277 lastFail=0x0D
```

140 秒里 **277 次**尝试、**一次都没连上**，错误码与车上**逐字相同**（`0x0D`）。
⇒ 现象能在台面上复现，而且对端是一个**我们自己写的** GATT server
（排掉了"诊断头脾气怪"这一整类解释）。
★ 顺带证实（这一条**成立**）：从板用**被动扫描**（`setActiveScan(false)`）也能认出
Windows 的广播 ⇒ `FFF0` 确实在 `ADV_IND` 里（以前只是推测）。

**（b）把从板的射频腾空 ⇒ 几秒内连上，整条 PID 链路都通了。**
同一块板、同一个模拟器，只换 PHY
（`PLATFORMIO_BUILD_FLAGS='-DLINK_PHY_ESP_NOW=0'`，即"链路 PHY 不启动"）：

```
23:18:46 client subscribed  (subscribed clients now 1)      ← 桌面侧：连上并订阅了 FFF1
23:18:46 < 'ATZ' …                                          ← 板子在发 ELM327 指令
obd-ble: state=ready peer=… conn=1 connects=1
SRC speed=sim rpm=obd coolant=obd intake=obd | v=18.6km/h 2500rpm 91.0C 45.0C
```

`2500rpm / 91.0C / 45.0C` 正是模拟器被要求发的值（`--rpm 2500 --coolant 91 --intake 45`）
⇒ **桌面假头 → 板上 NimBLE 中心 → `ObdSource` 解析 → 数据服务/UI** 整条链在台面上跑通。
（`speed=sim` 也对：分工 v2 里车速走链路，台面上没有主板。）

### 14.6 ★★ 读代码查实的一个真缺口（当轮就修了，见 §14.7）

1. `src/main.cpp` 的 `link_start_gate_tick()` 条件是 `#if LINK_ROLE == 1 && OBD_BLE`
   ⇒ **闸门只装在主板**；
2. 分工 v2 把 BLE 挪到了**从板**（`LINK_ROLE=0`）⇒ 从板**根本没有安静窗口**；
3. 主板那一支当时还被 2026-09-28 的"ESP-NOW 先起"实验**绕过**了
   （`setup()` 里直接 `g_link_phy.begin(false)`，闸门那次 `begin()` 被幂等守卫吃掉）。
⇒ **两块板都不给 BLE 建连留那几秒空窗**；`radio_arbiter.h` 的策略实际只走了
  "运行期优先权"这一半。
★ 而"运行期优先权"这一半**够不够**，这一轮**没有**干净的实验能回答（见 §14.5 顶上那段
  与 §14.9 第 1 条）；不要再拿 (a) 那组 277/277 当"射频是根因"的证据。

### 14.7 ★★★ 修好并验证（2026-09-28 深夜，车主点头之后）

**落地（`src/main.cpp`，四处，都是"让谁背 BLE 谁就让路"）：**

1. `link_start_gate_tick()` 的条件 `#if LINK_ROLE == 1 && OBD_BLE` → **`#if OBD_BLE`**；
2. 从板 `setup()` 里那行 `g_link_phy.begin(false)` **挪进闸门**（没编 BLE 的档一字未变）；
3. `"link: 从板侧就绪 …"` 那一行跟着搬（新增 `link_log_slave_ready()`）——
   否则会打出"就绪"而 PHY 还没起，那正是这条日志存在的意义；
4. 主循环里 `link_tx_task_start_once()` 的守卫 `!(LINK_ROLE == 1 && OBD_BLE)` → **`!OBD_BLE`**。

**台面实测（从板 = `esp32s3-rgb-slave-obdtest` + 桌面假头，两块板都在机旁）：**

```
obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready          ← 上电 ~6s / ~10s 连上
link: 启动闸门开了 —— BLE 已连上(ready),等了 5726ms ⇒ 现在启链路 PHY   ← 闸门**提前开**，没等满 15s
espnow: tx_frames=24…33 rx_frames=63…183 | link: locked tick_age=67ms ← 闸门开之后 ESP-NOW 照常起
SRC speed=link rpm=obd coolant=obd intake=obd | v=161.3km/h 2400rpm 90.0C 42.0C
```

- `2400rpm / 90.0C / 42.0C` = 假头被要求发的值；`speed=link` 来自主板
  ⇒ **OBD（BLE）与链路（ESP-NOW）同时活着，数据各走各的**。
- ★ **"先 BLE、后 ESP-NOW"这个顺序在从板上没有复现主板那次 `esp_now_init` 失败**
  （`stage` 直接到成功、`tx_frames` 正常涨）—— 对主板那个仍挂着的顺序实验是个有用的旁证。
- 稳定态：连上之后 **80+ 秒**（链路 PHY 已起）持续 `state=ready conn=1 drop=0`，
  假头侧 `subs=1 cmd=280 notify=426 write=1112 err=0`（≈20 Hz 轮询没停）
  ⇒ **"连上之后能不能维持"这一条，台面上至少 80 秒是维持住的**（车上仍未验）。
- 代价如实记：这 6~10 秒里左屏是模拟值 + "数据不可信"；上限 15s 到点一律开闸。

### 14.8 还没解决的（如实记，别当已验）

- **车上没验**：台面 80 秒维持 ≠ 车上长时间 + 真头 + 点火/ECU 睡眠那些花样。
- **稳态共存的量化没做**：丢包 / p99 / `gap_rx` 在"BLE 连着"与"BLE 没连"两种情况下
  的各是多少，这一轮**没量**。
- **静默窗口给从板是有代价的**（左屏最长 15s 不可信）：这一版按"先连 BLE"实现，
  如果车主觉得上电那几秒不能忍，就得换 ② 号方案（从板要建连时**通过链路请主板安静几秒**）。
- **桌面假头有一个硬限制**（下一节）：它一个进程只接一次连接 ⇒
  "连不上"的现场先要排掉"这个模拟器实例已经用过了"。
- 板子把一条指令拆成**好几个小 ATT 写**（`"01"` + `"0C"` + `\r`），而 Windows 的外设角色
  **不保证 write-without-response 的顺序** ⇒ 假头按 `\r` 拼行后仍会偶尔收到错乱的行
  （表现为 `?` / `NO DATA`）。真头背后是 UART，会按顺序拼回去，所以**这不一定**是板子的 bug；
  但板子这侧"整条指令攒起来一次写"是更稳的做法。
- 桌面广播用**随机地址**（每次跑都不一样：`64:d7:…` / `65:9c:…` / `46:ea:…`）
  ⇒ **不要按地址匹配**，按服务 UUID 匹配（现有固件就是这么做的，是对的）。

### 14.9 ★★ 桌面假头的两条坑（会让人误判成"板子连不上"）

1. **一个进程只接一次连接**（实测，Windows 11 + Intel）：客户端一走开（板子复位、
   重刷、断电），**这个实例就再也接不上第二个** —— 每次 `onConnectFail 0x0D`，
   而广播还在（板子照样扫得到）。
   ★ 试过 `stop_advertising()` + `start_advertising()`（"re-arm"）**救不回来**
     （日志里能看到 `client unsubscribed → status 3 → 2 → re-armed`，之后依旧连不上）。
   ⇒ **每次重启板子之前，先重启这个脚本**。"连不上"的第一条排查就是它。
   （2026-09-28 晚上的几轮里，正因为不知道这条，白追了一轮"是不是射频"。）
2. **分片/顺序**：见上一节最后一条。

### 14.10 怎么跑一遍

```powershell
$py = "$env:LOCALAPPDATA\Python\pythoncore-3.14-64\python.exe"   # 本机 Python 3.14
# 1) 桌面当假头（另开一个窗口；★ 每次板子重启前先把它重启）
& $py tools\bt-obd\obd-ble-sim.py --mode idle --rpm 2400 --coolant 90 --intake 42 --speed 55
# 2) 板子：刷 -DOBD_BLE=1 的那一档（从板 obdtest），串口应看到：
#    obd-ble: 特征已配齐（FFF1 已订阅 / FFF2 可写）⇒ state=ready
#    link: 启动闸门开了 —— BLE 已连上(ready),等了 XXXXms ⇒ 现在启链路 PHY
#    SRC speed=link rpm=obd coolant=obd intake=obd | …          ← 两条路都在工作
# 3) A/B 对照（"没有闸门会怎样"）：PLATFORMIO_BUILD_FLAGS='-DLINK_PHY_ESP_NOW=0'
#    刷一版（链路 PHY 不进 begin），或者用 git 回退这一版改动再看 277/277 那组。
```





