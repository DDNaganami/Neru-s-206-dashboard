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

## 8. 仍未验

- 从 BLE 拿到的数据与**有线**那条（`Serial1`）是否一致（两边都是同一颗 ECU，理论上应一致）；
- 这个头在**车上长期通电**时会不会有别的花样（点火瞬间掉线、ECU 睡眠后不应答）；
- **板上 NimBLE 的实际表现**（还没写一行代码）—— 上面 §7.5 五条全是"从笔记本侧推出来的要求"，
  不是板上实测。
