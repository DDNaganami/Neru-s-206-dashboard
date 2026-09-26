<#
  ble-obd-client.ps1 —— 把 BLE OBD 诊断头**当串口用**:发 ELM327 指令、收回答

  为什么要有它(2026-09-27):
    车主的 BLE 诊断头 `OBDBLE`(AABBCC122233)实测是 **BLE**(不是蓝牙经典/SPP),
    而 BLE 只有一条路能连:板子当**中心**去连它。板上固件还没写,
    先在笔记本上用这个脚本把"发指令→收回答"整条路走通 —— 笔记本上验证过的
    GATT 细节,就是板上 NimBLE 要照抄的东西(见文件尾「板上要照抄的」)。

  实测到的 GATT 结构(2026-09-27,`probe-ble-gatt.ps1` 读出):
    服务 0000fff0-...
      0000fff1-...  props=0x12(读 + **通知**)  ← 诊断头 -> 中心,**回答从这里来**
      0000fff2-...  props=0x0C(写 + 无响应写)  ← 中心 -> 诊断头,**指令往这里写**
    (其余 1800/1801/1804/180F 是标准服务,不用管)

  用法:
    powershell -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1
    powershell -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1 -Cmds ATZ,ATE0,ATSP0,0100
    powershell -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1 -Cmds 010C,010D,0105,010F -WaitMs 3000

  退出码:0 收到过回答 / 2 跑错解释器或缺参数 / 4 找不到设备 / 5 连不上 / 6 找不到 FFF0 特征

  ★ 只发标准 ELM327 指令。别拿它发任何"写"类指令(清故障码 04 之类)——
    这台车是天天开的,别乱动 ECU 状态。
#>
param(
  [string]$Name = 'OBDBLE',
  [string]$Address = 'AABBCC122233',
  [string[]]$Cmds = @('ATZ', 'ATE0', 'ATL0', 'ATH0', 'ATSP0', '0100'),
  [int]$WaitMs = 2500,
  [int]$Retries = 6
)

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

if ($PSVersionTable.PSEdition -ne 'Desktop') {
  Write-Host "★ 必须用 Windows PowerShell 5.1 跑(pwsh 里没有 WinRT 的那个 async 桥)。" -ForegroundColor Red
  Write-Host "  powershell -ExecutionPolicy Bypass -File tools\bt-obd\ble-obd-client.ps1" -ForegroundColor Yellow
  exit 2
}
try { Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction Stop } catch {
  Write-Host ("加载 System.Runtime.WindowsRuntime 失败: " + $_.Exception.Message) -ForegroundColor Red; exit 2
}
foreach ($t in @(
  'Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceService, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic, Windows.Devices.Bluetooth, ContentType=WindowsRuntime',
  'Windows.Storage.Streams.DataWriter, Windows.Storage.Streams, ContentType=WindowsRuntime'
)) { try { [void][Type]::GetType($t, $true) } catch {} }

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
  })[0]
function Await($op, $t) {
  $k = $asTask.MakeGenericMethod($t).Invoke($null, @($op))
  $k.Wait(-1) | Out-Null
  $k.Result
}
function HexDump([byte[]]$b) {
  if (-not $b -or $b.Length -eq 0) { return '(空)' }
  $s = ($b | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
  $asc = -join ($b | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { '.' } })
  return ($s + '   |' + $asc + '|')
}

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
Write-Host ("找到: " + $di.Name + "  " + $Address + "  已配对=" + $di.Pairing.IsPaired) -ForegroundColor Cyan

# ---- 2. 连接(★ 这个头空闲时会自己掉线,必须重试把它叫醒) ----
$dev = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync([Convert]::ToUInt64($Address, 16))) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
$svc = $null
for ($i = 1; $i -le $Retries; $i++) {
  if (-not $di.Pairing.IsPaired) { try { $pr = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult]); Write-Host ("  配对: " + $pr.Status) } catch {} }
  $gr = Await ($dev.GetGattServicesAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult])   # 1 = Uncached
  $list = @(); foreach ($s in $gr.Services) { $list += $s }
  Write-Host ("  第 $i 次: 连接=" + $dev.ConnectionStatus + "  服务数=" + $list.Count)
  if ($list.Count -gt 1) { $svc = $list | Where-Object { $_.Uuid -match 'fff0' } | Select-Object -First 1; if ($svc) { break } }
  Start-Sleep -Seconds 3
}
if (-not $svc) { Write-Host "连不上或找不到 FFF0 服务(可能被手机 App 占着)。" -ForegroundColor Red; exit 5 }
Write-Host ("FFF0 服务已拿到: " + $svc.Uuid) -ForegroundColor Green

# ---- 3. 拿两个特征 ----
$cr = Await ($svc.GetCharacteristicsAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult])
$cl = @(); foreach ($c in $cr.Characteristics) { $cl += $c }
$rx = $cl | Where-Object { $_.Uuid -match 'fff1' } | Select-Object -First 1
$tx = $cl | Where-Object { $_.Uuid -match 'fff2' } | Select-Object -First 1
if (-not $rx -or -not $tx) { Write-Host "FFF1/FFF2 没找齐,实际拿到:" -ForegroundColor Red; $cl | ForEach-Object { Write-Host ("  " + $_.Uuid) }; exit 6 }
Write-Host ("RX(通知)=FFF1   TX(写)=FFF2") -ForegroundColor Green

# ---- 4. 订阅通知 ----
# ★ 两个 PowerShell↔WinRT 的坑(2026-09-27 实测踩到,写下来免得再试一遍):
#   ① `Register-ObjectEvent -EventName ValueChanged` **不能用** —— 报
#      "Windows PowerShell cannot subscribe to Windows RT events"(PS 的事件系统挂不上 WinRT 事件)。
#      ⇒ 改成**轮询**:反复 `ReadValueAsync()` 读特征值。FFF1 带 Read 属性(props=0x12),这条路通;
#        60ms 的轮询对这个数据量完全够(ELM327 一问一答本来就是百毫秒级)。
#   ② `WriteValueWithResultAsync($buffer)` **找不到重载**(PS 把单个 IBuffer 匹配不上那个签名)。
#      ⇒ 改用更老的 `WriteValueAsync($buffer)`,它收 IBuffer,返回 GattCommunicationStatus。
$global:rxBytes = @()
$global:lastHex = ''
$cccd = Await ($rx.WriteClientCharacteristicConfigurationDescriptorAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus])
Write-Host ("订阅通知(1=Notify): " + $cccd) -ForegroundColor Green

function Read-Pending {
  # ReadValueAsync 回的是**缓存值、会重复**,所以跟上次比:不一样才算新数据
  try {
    $vr = Await ($rx.ReadValueAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult])   # 1 = Uncached
    if ([int]$vr.Status -ne 1) { return }
    $buf = $vr.Value
    if (-not $buf -or $buf.Length -eq 0) { return }
    $b = New-Object byte[] $buf.Length
    [Windows.Storage.Streams.DataReader]::FromBuffer($buf).ReadBytes($b)
    $s = ($b | ForEach-Object { '{0:X2}' -f $_ }) -join ''
    if ($s -eq $global:lastHex) { return }
    $global:lastHex = $s
    $script:rxAcc += $b
  } catch {}
}

# ---- 5. 逐条发指令、等回答 ----
$got = 0
foreach ($c in $Cmds) {
  $script:rxAcc = @()
  $global:lastHex = ''
  $st = 'n/a'
  # ★ 写指令这一格**还没打通**(2026-09-27,写到这儿收工),两种写法都试过:
  #   ① `$tx.WriteValueAsync($w.DetachBuffer())` → PS 说 "cannot find an overload";
  #   ② 显式造 IBuffer(`Windows.Security.Cryptography.CryptographicBuffer.CreateFromByteArray`,
  #      该类型要先 [Type]::GetType 加载)→ 这次重载匹配上了、但 **Windows 回
  #      "The parameter is incorrect."** —— 也就是这一格卡在 Windows BLE 栈这一层,
  #      不是脚本语法问题。
  #   下一步该试的(留给下次,别重复踩):改用 `WriteValueWithResultAsync` 并**显式给
  #     两个参数**(IBuffer + GattWriteOption::WriteWithoutResponse,因为这个特征的
  #     属性是 0x0C = 写 + **无响应写**);或者干脆换到 Python 的 bleak(它把 BLE 写
  #     包好了,比 WinRT 好使)。
  $ib = $null
  try {
    [void][Type]::GetType('Windows.Security.Cryptography.CryptographicBuffer, Windows.Security.Cryptography, ContentType=WindowsRuntime', $true)
    $ib = [Windows.Security.Cryptography.CryptographicBuffer]::CreateFromByteArray([Text.Encoding]::ASCII.GetBytes($c + "`r"))
  } catch { $ib = $null }
  try {
    if ($ib) { $st = Await ($tx.WriteValueAsync($ib)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus]) }
    else { $st = 'IBuffer 造不出来' }
  } catch { $st = 'ERR:' + $_.Exception.Message }
  if ($null -eq $script:rxAcc) { $script:rxAcc = @() }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.ElapsedMilliseconds -lt $WaitMs) {
    Read-Pending
    if ($null -eq $script:rxAcc) { $script:rxAcc = @() }
    $txt = [Text.Encoding]::ASCII.GetString([byte[]]$script:rxAcc)
    if ($txt -match '>') { break }
    Start-Sleep -Milliseconds 60
  }
  Read-Pending
  if ($null -eq $script:rxAcc) { $script:rxAcc = @() }
  $raw = [Text.Encoding]::ASCII.GetString([byte[]]$script:rxAcc)
  $txt = ($raw -replace "`r", ' ' -replace "`n", ' ').Trim()
  $ok = ($txt.Length -gt 0)
  if ($ok) { $got++ }
  Write-Host ("  " + $c.PadRight(7) + " 写=" + $st + "  " + $(if ($ok) { '收到: ' + $txt } else { '★ 没回答' })) -ForegroundColor $(if ($ok) { 'Gray' } else { 'Red' })
  Write-Host ("          原始: " + (HexDump ([byte[]]$script:rxAcc))) -ForegroundColor DarkGray
  Start-Sleep -Milliseconds 200
}

try { [void](Await ($rx.WriteClientCharacteristicConfigurationDescriptorAsync(0)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCommunicationStatus])) } catch {}
try { $svc.Dispose() } catch {}

Write-Host ""
if ($got -gt 0) {
  Write-Host ("完成:$($Cmds.Count) 条里收到 $got 条回答 —— BLE 这条路通了。") -ForegroundColor Green
  Write-Host "板上要照抄的:服务 FFF0 / 通知 FFF1 / 写 FFF2;先订阅 FFF1 再往 FFF2 写 ASCII+CR;" -ForegroundColor Green
  Write-Host "回答是**分片**来的(通知一次一小段),要按 '>' 提示符收尾,不能假设一帧就是一整条回答。" -ForegroundColor Green
  exit 0
}
Write-Host "一条回答都没收到。排查顺序:" -ForegroundColor Red
Write-Host "  · 诊断头插在 OBD 座上吗?钥匙至少拧到 ACC?" -ForegroundColor Yellow
Write-Host "  · 它是不是正被手机 App 连着(BLE 从机通常只接一个中心)?" -ForegroundColor Yellow
Write-Host "  · 换个 App 确认它能出数据,再回来跑本脚本。" -ForegroundColor Yellow
exit 5
