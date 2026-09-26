<#
  ble-obd-client.ps1 —— 把 BLE OBD 诊断头当串口用:发 ELM327 指令、收回答

  ============================================================================
  ★★ 2026-09-27 实测结论(先看这段,能省你几小时)★★
  ============================================================================

  【一、写入这条路,通了 —— 精确写法】

    能通的形式 = **反射拿精确重载 + Invoke**,一条不多一条不少:

        $m = @($tx.GetType().GetMethods() | Where-Object {
                 $_.Name -eq 'WriteValueWithResultAsync' -and $_.GetParameters().Count -eq 2 })[0]
        $buf = [Windows.Security.Cryptography.CryptographicBuffer]::CreateFromByteArray($bytes)
        $op  = $m.Invoke($tx, @($buf, 1))       # 1 = GattWriteOption::WriteWithoutResponse
        $res = Await $op ([...GattWriteResult]) # Status=Success

    前提(缺一不可):
      · 必须真的连上:`$dev.ConnectionStatus -eq 1`(Connected);
      · `WriteValueWithResultAsync` 比 `WriteValueAsync` 好,因为它回一个
        GattWriteResult(能读到 Status 和 ProtocolError),而 WriteValueAsync 只回状态。

  【二、为什么前面那几种写法不行 —— 逐条实测记录】

    先说清一个**误导性极强的现象**:上一版注释里写"`WriteValueAsync($buf)` 报
    Cannot find an overload"。这次重测发现:

      · 同样一行代码,在 **Disconnected 状态**下 → `Cannot find an overload ... argument count: "1"`
      · 在 **Connected 状态**下       → 同样的 `Cannot find an overload`(仍然报!)

    所以"cannot find an overload"**不是**"没连上"的锅(两版注释都猜错了),它是
    PowerShell 调用 WinRT 重载解析的真实毛病,只是**偶发**。逐条记录:

    (1) `$tx.WriteValueAsync($buf)`(PS 原生调用,1 参)
        → `Cannot find an overload for "WriteValueAsync" and the argument count: "1".`
        ★ 即使 CS=1 也照样报。这是 PS 的 WinRT 重载解析器在这个类型上失灵。
        (注释可见 OverloadDefinitions 里那个 (IBuffer) 重载明明存在 —— 这就是"欺骗性"。)

    (2) `$tx.WriteValueAsync($buf, 1)`(PS 原生调用,2 参)
        → `Cannot find an overload ... argument count: "2".`
        ★ 同上。可见"加参数"根本不解决问题。

    (3) `[Windows.Security.Cryptography.CryptographicBuffer]::CreateFromByteArray(...)`
        + PS 原生 `WriteValueAsync($ib)`
        → 重载**匹配上了**,但 Windows 回 `The parameter is incorrect.`
        ★ 这次是 Windows BLE 栈拒绝,不是 PS 语法问题 —— 上一版注释推测"卡在 Windows
          这一层"是对的,但**原因猜错了**。真正原因是那个 IBuffer 实际是不合格的(见下)。

    (4) ★★ **反射拿精确重载 + Invoke** → **成功,Status=Success** ★★
        这才是通的写法。同一时刻(CS=1)用 (1)/(2) 都失败、(4) 成功,
        所以结论很硬:**问题在 PowerShell 的 WinRT 重载解析,不在连接状态、不在参数。**

  【三、IBuffer 的两条造法 —— 哪条真能用】

    ★ 真正的坑在 **DataWriter.DetachBuffer()**:它返回的 IBuffer **长度是坏的**。
      实测(送 "ATZ\r" = 4 字节):
        · `DataWriter.WriteBytes($bytes)` → `UnstoredBufferLength = 4`(写入前是对的)
        · `$dw.DetachBuffer()` → 读回 `Length = 1` ✗ **只有 1 字节**,
          用 DataReader 反射读出来也是错的。⇒ **别用这条路造 IBuffer!**
        · `CryptographicBuffer.CreateFromByteArray($bytes)` → 反射读回
          `len=4 ReadString='ATZ\r'` ✓ **正确**。⇒ 用这条。

      (旁注:`.Length` 在 PS 里对这两种 __ComObject 都显示 1,那是 PS 内置属性覆盖,
       不是真长度;要拿真长度必须走 DataReader 反射。这也是个陷阱。)

  【四、读取这一侧:PowerShell 收不到通知(实测卡死在这)】

    这一侧**没打通**,而且不是没试。把试过的全记下来,免得重复:

    · `Register-ObjectEvent -EventName ValueChanged` → `Windows PowerShell cannot
      subscribe to Windows RT events`。(上一版结论正确)
    · **轮询 `$rx.ReadValueAsync(1)`** → `Status=Success`,但**永远回 20 个 0 字节**,
      连发完 ATZ 之后也一样。所以轮询对这个头**拿不到数据**(不是"比对去重"写错了)。
    · `ValueChanged` 事件本身 —— ★ 上一版说"注册不上"其实**是可以注册的**,但有个大坑:
        `$sb -as $delegateType` 注册会**返回 OK**,但回调**永远不会被调用**(hits=0);
        用 `Add-Type` 编译一个**真 .NET 方法**再 `CreateDelegate` → 回调**真的会触发**。
      ★ 但触发了也白搭:`args.CharacteristicValue` 拿到的是一个**未投影的
        `System.__ComObject`**,`GetType().GetProperty('Length')` 为 null,
        任何 cast 到 `Windows.Storage.Streams.IBuffer` 都失败
        (`Cannot convert the "System.__ComObject" value ... to type "...IBuffer"`)。
      · 试过把它 `Marshal.GetIUnknownForObject` 再 `GetObjectForIUnknown` 重新投影
        → 还是 `__ComObject`,依旧不能 cast。
      · 试过用 **COM ABI**(自己声明 `IBufferByteAccess` + `IBuffer` vtable)直接读字节
        → cast 成功了,但 `get_Length()` 取到错值,`Marshal.Copy` 读飞,**整个 PowerShell
        进程直接崩掉**(没得 catch)。这条别再试了。
      · 根因:`Add-Type` 的 csc **无法引用 WinRT 元数据**(`Windows.Storage.winmd`
        用 `-ReferencedAssemblies` 会报 `assembly name or codebase was invalid`),
        所以编译代码里写不出 `IBuffer`;而 PS 脚本层又投影不了那个 __ComObject。
        ⇒ **在 PowerShell 里收这个头的通知,是不划算的**(除非上 C#/WinRT 原生工程)。

    ★ 结论:**读取请用 Python + bleak**(同目录 `ble-obd-client.py`),它开箱即通。

  【五、连接管理(实测)】

    · `BluetoothLEDevice.FromBluetoothAddressAsync` 之后 `ConnectionStatus` 可能是 0;
    · **不一定非要 PairAsync**:直接 `GetGattServicesAsync(1)` 有时能直接到 CS=1、5 个服务;
    · 但**掉线时 PairAsync 是有效的叫醒手段**(回 AlreadyPaired 也能把链路叫起来);
    · ★ 判据:服务数 **5** = 真连上;只回 **1** 个(0x1801)= **没真连上**,别下别的结论;
    · 这个头**空闲会自己掉线**,所以脚本里是"重试 6 次、每次 2 秒"的循环。

  ============================================================================
  用法:
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1 -NoWait
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1 -Cmds "ATZ,ATE0,ATSP0,0100"

  ★★ 传 -Cmds 一定要**加引号**,写成整个字符串 -Cmds "ATZ,ATE0,0100"。
     实测踩到的坑:不加引号写成 -Cmds ATZ,ATE0,0100 时,PowerShell 的
     `-File` 参数解析会把**像数字的那一项 0100 当数值处理,前导零被吃掉**,
     变成 "100" 发出去(诊断头当然不认)。
     脚本内部已按逗号/空格切分,所以加引号传一条字符串最稳。

  ★ 本脚本只发标准 ELM327 **读**指令。绝不发清故障码/写类指令(04、2F 等)——
    这台车天天开,不许乱动 ECU 状态。
  退出码:0 / 2 解释器错 / 4 找不到设备 / 5 连不上 / 6 找不到 FFF0 特征 / 7 写失败
#>
param(
  [string]$Name = 'OBDBLE',
  [string]$Address = 'AABBCC122233',
  [string[]]$Cmds = @('ATZ', 'ATE0', 'ATL0', 'ATH0', 'ATSP0'),
  [int]$WaitMs = 2500,
  [int]$Retries = 6,
  # 本脚本只负责"写进去"这一侧(读取那侧 WinRT 投影不通,见头部【四】)。
  # -NoWait 只用来看"写入是否成功",不假装能收到回答。
  [switch]$NoWait
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ★ -Cmds 的两种传法都要能用。踩过的两个坑:
#   (a) `-File ... -Cmds ATZ,ATE0` 传进来是**一个**字符串("ATZ,ATE0"),不是数组,
#       不切分就会拼成一条畸形指令;
#   (b) 不加引号时,像数字的 0100 会被 PowerShell 参数解析当数值、**前导零被吃掉**
#       变成 "100"。⇒ 用法上请加引号:-Cmds "ATZ,ATE0,0100"(见文件头)。
#   这里统一按逗号/空格/分号切分,并去掉空白,两种写法都能正常工作。
$Cmds = @($Cmds | ForEach-Object { $_ -split '[,;\s]+' } | Where-Object { $_ -ne '' })

if ($PSVersionTable.PSEdition -ne 'Desktop') {
  Write-Host "★ 必须用 Windows PowerShell 5.1 跑(pwsh 里没有 WinRT 的 async 桥)。" -ForegroundColor Red
  Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1" -ForegroundColor Yellow
  exit 2
}
try { Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction Stop } catch {
  Write-Host ("加载 System.Runtime.WindowsRuntime 失败: " + $_.Exception.Message) -ForegroundColor Red; exit 2
}
foreach ($t in @(
  'Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattClientCharacteristicConfigurationDescriptorValue, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Security.Cryptography.CryptographicBuffer, Windows.Security.Cryptography, ContentType=WindowsRuntime'
)) { try { [void][Type]::GetType($t, $true) } catch {} }

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $t) {
  if ($null -eq $op) { throw "Await: null async op" }
  $k = $asTask.MakeGenericMethod($t).Invoke($null, @($op))
  $k.Wait(-1) | Out-Null
  $k.Result
}
$TStatus = [Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]
$TWriteRes = [Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult]
$TCB = [Windows.Security.Cryptography.CryptographicBuffer]

function HexOf([byte[]]$b) { if (-not $b -or $b.Length -eq 0) { return '(空)' }; return (($b | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') }

# ---- 1. 找设备 ----
$sel = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelector()
$res = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($sel)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
$di = $null
foreach ($d in $res) {
  try {
    $a = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromIdAsync($d.Id)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
    if ($a.BluetoothAddress.ToString('X12') -eq $Address) { $di = $d; break }
  } catch {}
}
if (-not $di) { Write-Host ("找不到 BLE 设备 " + $Name + " / " + $Address + " —— 诊断头插好了吗?") -ForegroundColor Red; exit 4 }
Write-Host ("找到: " + $di.Name + "  " + $Address + "  IsPaired=" + $di.Pairing.IsPaired) -ForegroundColor Cyan

# ---- 2. 连接(★ 空闲会掉线,必须重试) ----
$uaddr = [Convert]::ToUInt64($Address, 16)
$dev = $null; $svc = $null; $rx = $null; $tx = $null
for ($i = 1; $i -le $Retries; $i++) {
  if ($null -eq $dev) {
    $dev = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync($uaddr)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
  }
  # 掉线时 PairAsync 是有效的叫醒手段(回 AlreadyPaired 也能把链路叫起来)
  try { $pr = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult]) } catch {}
  Start-Sleep -Seconds 2
  $gr = $null
  try { $gr = Await ($dev.GetGattServicesAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult]) } catch {}
  if ($gr) {
    $list = @($gr.Services)
    Write-Host ("  第 $i 次: CS=" + [int]$dev.ConnectionStatus + "  服务数=" + $list.Count + "  Status=" + $gr.Status)
    # ★ 服务数 > 1 才是真连上;只回 1 个(0x1801)= 没真连上
    if ($list.Count -gt 1) {
      $cand = $list | Where-Object { $_.Uuid -match 'fff0' } | Select-Object -First 1
      if ($cand) {
        $cr = Await ($cand.GetCharacteristicsAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult])
        $ch = @($cr.Characteristics)
        $r0 = $ch | Where-Object { $_.Uuid -match 'fff1' } | Select-Object -First 1
        $t0 = $ch | Where-Object { $_.Uuid -match 'fff2' } | Select-Object -First 1
        if ($r0 -and $t0) { $svc = $cand; $rx = $r0; $tx = $t0; break }
      }
    }
  }
  Start-Sleep -Seconds 2
}
if (-not $tx) { Write-Host "连不上或找不到 FFF0/FFF1/FFF2(可能被手机 App 占着)。" -ForegroundColor Red; exit 5 }
Write-Host ("连上: CS=" + [int]$dev.ConnectionStatus + "  FFF0/FFF1(通知)/FFF2(写) 齐了") -ForegroundColor Green

# ---- 3. 订阅通知(FFF1,CCCD=Notify) ----
#   ★ 注意:这一步会成功,但**收不到数据**(原因见文件头【四】)。
#     留着它是为了让诊断头把 FFF1 当成"已使能",行为更接近手机 App。
$TCCCDVal = [Windows.Devices.Bluetooth.GenericAttributeProfile.GattClientCharacteristicConfigurationDescriptorValue]
try {
  $cccd = Await ($rx.WriteClientCharacteristicConfigurationDescriptorAsync($TCCCDVal::Notify)) ($TStatus)
  Write-Host ("订阅 FFF1 通知(CCCD=Notify): " + $cccd) -ForegroundColor Green
} catch { Write-Host ("订阅失败: " + $_.Exception.Message) -ForegroundColor Yellow }

# ---- 4. ★ 写入:反射拿精确重载 ----
#   这是唯一实测能通的形式。不要改成 PS 原生调用(会报 cannot find an overload)。
$mWrite = @($tx.GetType().GetMethods() | Where-Object {
    $_.Name -eq 'WriteValueWithResultAsync' -and $_.GetParameters().Count -eq 2 })[0]
if (-not $mWrite) { Write-Host "找不到 WriteValueWithResultAsync(IBuffer, GattWriteOption)" -ForegroundColor Red; exit 6 }
Write-Host ("写入用的精确重载: " + $mWrite.Name + "(" + (($mWrite.GetParameters() | ForEach-Object { $_.ParameterType.Name }) -join ', ') + ")") -ForegroundColor DarkGray

$WRITE_WITHOUT_RESPONSE = 1
$okCount = 0
foreach ($c in $Cmds) {
  # ★ ELM327 每条指令都要以回车 \r 收尾
  $bytes = [Text.Encoding]::ASCII.GetBytes($c + "`r")
  # ★ 必须用 CryptographicBuffer 造 IBuffer;DataWriter.DetachBuffer() 造出来的长度是坏的
  $buf = $TCB::CreateFromByteArray($bytes)
  $st = ''
  try {
    $op = $mWrite.Invoke($tx, @($buf, $WRITE_WITHOUT_RESPONSE))
    $r = Await $op ($TWriteRes)
    $st = [string]$r.Status
    $pe = ''
    try { $pe = [string]$r.ProtocolError } catch {}
    Write-Host ("  " + $c.PadRight(7) + " 送出=" + (HexOf $bytes) + "  CS=" + [int]$dev.ConnectionStatus + "  Status=" + $st + $(if ($pe) { "  ProtocolError=" + $pe } else { "" })) -ForegroundColor $(if ($st -eq 'Success') { 'Green' } else { 'Red' })
    if ($st -eq 'Success') { $okCount++ }
  } catch {
    Write-Host ("  " + $c.PadRight(7) + " 写失败: " + $_.Exception.Message) -ForegroundColor Red
  }
  if (-not $NoWait) { Start-Sleep -Milliseconds 300 }
}

# ---- 5. 收(说明为什么这里收不到) ----
if (-not $NoWait) {
  Write-Host ""
  Write-Host "★ 关于"收不到回答":本脚本这一侧只验证写入。" -ForegroundColor Yellow
  Write-Host "  PowerShell 收不到这个头的通知(实测:ValueChanged 回调拿到的是未投影的" -ForegroundColor Yellow
  Write-Host "  System.__ComObject,转不成 IBuffer;轮询 ReadValueAsync 永远回 20 个 0)。" -ForegroundColor Yellow
  Write-Host "  要真收数据,用同目录的 ble-obd-client.py(Python + bleak),它开箱即通。" -ForegroundColor Yellow
}

try { [void](Await ($rx.WriteClientCharacteristicConfigurationDescriptorAsync([Windows.Devices.Bluetooth.GenericAttributeProfile.GattClientCharacteristicConfigurationDescriptorValue]::None)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus])) } catch {}
try { $svc.Dispose() } catch {}

Write-Host ""
if ($okCount -eq $Cmds.Count) {
  Write-Host ("完成:$okCount/$($Cmds.Count) 条全部写入成功(Status=Success)。") -ForegroundColor Green
  Write-Host "板上 NimBLE 要照抄的:" -ForegroundColor Green
  Write-Host "  · 服务 FFF0;通知 FFF1(0x12 读+通知);写 FFF2(0x0C 写+无响应写);" -ForegroundColor Green
  Write-Host "  · 先 CCCD=Notify 订阅 FFF1,再往 FFF2 写 ASCII+CR;" -ForegroundColor Green
  Write-Host "  · 回答**分片**来(实测 ATZ 被切成 1+13 两片),必须按 '>' 提示符收尾,不能假设一帧一答;" -ForegroundColor Green
  Write-Host "  · '0100' 前几次可能只回 'SEARCHING...',要等,**别当失败**。" -ForegroundColor Green
  exit 0
}
Write-Host ("只有 $okCount/$($Cmds.Count) 条写成功。") -ForegroundColor Red
exit 7
