# BLE OBD 诊断头（`OBDBLE`）实测：能当板子的 OBD 数据通道

> 2026-09-27 实测。目的：让 **S3 板子自己读 OBD**（省掉往板上插第二个 USB 有线 327）。
> 结论：**这条路成立** —— 该诊断头是 BLE，ESP32-S3 能连；GATT 结构已摸清。
> **尚未打通**：从中心往它写指令（见 §4），卡在笔记本的 Windows BLE 栈上。

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

## 4. 还没打通的一格：往 `FFF2` 写指令

**现状**：连接、配对、服务发现、订阅（`WriteClientCharacteristicConfigurationDescriptorAsync(1)` → `Success`）、
以及**读** `FFF1`（`ReadValueAsync`）全部通过；**只有"写"过不去**。

试过并失败的写法：

| 写法 | 结果 |
|---|---|
| `$tx.WriteValueAsync($buf)` | `Cannot find an overload … argument count: "1"` |
| `$tx.WriteValueWithResultAsync($buf, 1)` | `Cannot find an overload … argument count: "2"` |
| `WriteValueAsync(CryptographicBuffer.CreateFromByteArray(...))`（**已连接**时） | `The parameter is incorrect.` |

★ **重要更正**：上表前两条是在 `ConnectionStatus = Disconnected` 的链路上试的，**结论不作数**；
只有第三条是在已连接状态下试的、才是真失败。所以"写不通"这件事**还没有被充分证明**。

**下一步该试的**（按优先级）：
1. **用反射拿精确重载再 `Invoke`** —— "Cannot find an overload" 是 PowerShell 调 WinRT
   重载解析的典型毛病，反射最可靠；
2. 写之前**先确认 `ConnectionStatus -eq 1`**，不然全是在断链上白试；
3. 该特征是「写 + 无响应写」，注意 `GattWriteOption` 要显式给；
4. PowerShell 这条路若攻不下来，**换 Python 的 `bleak`**（它把 BLE 写包好了）——
   vivid 的对照是：`bleak` 里就是 `await client.write_gatt_char(FFF2, b"ATZ\r")` 一句话。

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
   ⇒ 改成**轮询** `ReadValueAsync(Uncached)`，比对上次的十六进制串（变了才算新数据），
   60~80ms 一次对这个数据量完全够。

## 6. 工具

| 脚本 | 用途 | 状态 |
|---|---|---|
| `tools/bt-obd/probe-ble-gatt.ps1` | 扫描 BLE、判"是不是 BLE"、配对、列全部服务/特征/UUID | ✅ 已验证可用 |
| `tools/bt-obd/ble-obd-client.ps1` | 当串口用发 ELM327 指令（订阅 FFF1 / 写 FFF2） | ⚠️ 读通、写卡住（见 §4） |

两个都必须 **Windows PowerShell 5.1** 跑（脚本里已加运行环境自检，跑错解释器会直接报错并退出）。

## 7. 板子上的活有多大（好消息）

`ObdSource` 已经在做 **ELM327 的问答与解析**（`Parse-Obd`、PID 轮询表、`0105`/`010F` 的 ℃ 换算都在），
**换的只是"传输层"**：UART(Serial1) → BLE。解析那半可以原样复用。

资源也够：当前固件 `740,352` 字节 = **4MB app 分区的 17.7%**，NimBLE 加得下；
`Serial1`（GPIO17/18）那组脚与 VAN 的 GPIO44 也不冲突。

## 8. 仍未验

- 写通了没有、`0100`/`010C`/`0105`/`010F` 能不能真的读出来（§4）；
- 从 BLE 拿到的数据与**有线**那条（`Serial1`）是否一致；
- 这个头在**车上长期通电**时会不会有别的花样（比如点火瞬间掉线、ECU 睡眠后不应答）。
