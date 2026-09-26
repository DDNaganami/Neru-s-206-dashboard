<#
  probe-ble-gatt.ps1 —— 在笔记本上"看一眼"BLE OBD 诊断头的 GATT 结构

  为什么要有它(2026-09-27):
    车主买了个 BLE 蓝牙 OBD 诊断头,打算让 **S3 板子自己读 OBD**(省掉往板上插第二个 USB)。
    但这条路有一道**先决判据**,过不了就整个方向作废:

      ★★ **它必须是 BLE(低功耗蓝牙),不能是蓝牙经典 SPP。**
         ESP32-S3 只有 BLE —— 蓝牙经典/SPP 在硬件上就没有,**连不上就是连不上**,
         不是写代码能补的。而市面便宜的 ELM327 绝大多数恰恰是经典 SPP
         (本项目 PURCHASE.md 91~119 行早就写过这条,当时的结论就是"蓝牙版不能当车上数据通道")。

    所以先判"是不是 BLE",再谈"GATT 长什么样"。判 BLE 的办法:**能在这儿被列出来就是 BLE**
    (这个脚本走的是 WinRT 的 BluetoothLEDevice,枚举到的**只有 BLE 设备**;
     经典 SPP 设备根本不会出现在这个列表里 —— 这就是判据)。

  用法:
    # 1) 先只扫描,认清是哪个(把诊断头插到车上 OBD 座、钥匙拧到 ACC)
    powershell -ExecutionPolicy Bypass -File tools\bt-obd\probe-ble-gatt.ps1

    # 2) 认准名字后,连上去把服务/特征/UUID 全列出来
    powershell -ExecutionPolicy Bypass -File tools\bt-obd\probe-ble-gatt.ps1 -Name OBDII -Services

    # 3) 名字认不出来时,按广告里的关键字猜(默认会把带 OBD/ELM/VLINK/VGATE 的都标出来)
    powershell -ExecutionPolicy Bypass -File tools\bt-obd\probe-ble-gatt.ps1 -Match OBD

  退出码:0 正常 / 2 参数错 / 4 没找到设备 / 5 连不上 / 6 服务枚举失败

  ★ 本脚本**只读不写**:不配对、不写特征、不发任何数据。要发 AT 指令是下一步的事,
     得先知道往哪个特征写(这就是 -Services 要问出来的东西)。
#>
param(
  [string]$Name = "",
  [string]$Match = "",
  [string]$Address = "",
  [switch]$Services,
  [switch]$Pair,
  [int]$ScanSeconds = 6
)

$ErrorActionPreference = "Continue"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

# ---- ★ 必须跑在 Windows PowerShell 5.1 下,不能跑在 pwsh(7.x) 下 ----
#   WinRT 的 IAsyncOperation -> Task 桥(`System.WindowsRuntimeSystemExtensions`)只在
#   .NET Framework 里有;pwsh 是 .NET Core,**这个类型根本不存在**,症状是
#   "Unable to find type [System.WindowsRuntimeSystemExtensions]" 之后一路静默失败
#   —— 实测(2026-09-27)表头打出来了、设备列表却是空的,极容易误判成"没有 BLE 设备"。
#   (另外 `Add-Type -AssemblyName Windows.Devices.Enumeration` 在 pwsh 里也会报
#    "One or more required assemblies are missing"。)
#   ⇒ 一律用 `powershell -ExecutionPolicy Bypass -File ...` 调,**别用 pwsh -File**。
if ($PSVersionTable.PSEdition -ne 'Desktop') {
  Write-Host ("★ 必须在 Windows PowerShell 5.1 里跑(当前 " + $PSVersionTable.PSEdition + " " + $PSVersionTable.PSVersion + ")。") -ForegroundColor Red
  Write-Host "  改用:  powershell -ExecutionPolicy Bypass -File tools\bt-obd\probe-ble-gatt.ps1" -ForegroundColor Yellow
  exit 2
}
try { Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction Stop } catch {
  Write-Host ("加载 System.Runtime.WindowsRuntime 失败: " + $_.Exception.Message) -ForegroundColor Red; exit 2
}

# ---- WinRT 初始化 + IAsyncOperation -> Task 的桥 ----
foreach ($t in @(
  "Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime",
  "Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime",
  "Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceService, Windows.Devices.Bluetooth, ContentType=WindowsRuntime",
  "Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic, Windows.Devices.Bluetooth, ContentType=WindowsRuntime"
)) { try { [void][Type]::GetType($t, $true) } catch {} }

$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq "AsTask" -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq "IAsyncOperation``1" })[0]
function Await($op, $t) {
  $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op))
  $task.Wait(-1) | Out-Null
  $task.Result
}

Write-Host "=== 扫描 BLE 设备(只列 BLE;经典 SPP 不会出现,这正是判据)===" -ForegroundColor Cyan
Write-Host "(诊断头要先插车上 OBD 座、钥匙拧到 ACC,它才会广播)" -ForegroundColor DarkGray
$sel = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelector()
$res = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($sel)) ([Windows.Devices.Enumeration.DeviceInformationCollection])

$rows = foreach ($d in $res) {
  $addr = ""
  try {
    $dev = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromIdAsync($d.Id)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
    $addr = $dev.BluetoothAddress.ToString("X12")
  } catch {}
  [PSCustomObject]@{
    Name  = if ($d.Name) { $d.Name } else { "(无名)" }
    Addr  = $addr
    Paired= [bool]$d.Pairing.IsPaired
  }
}

"{0,-34} {1,-16} {2}" -f "名称", "地址", "已配对"
"-" * 62
foreach ($r in ($rows | Sort-Object Name)) { "{0,-34} {1,-16} {2}" -f $r.Name, $r.Addr, $(if ($r.Paired) { "是" } else { "" }) }
Write-Host ""

# 关键字提示:便宜的 OBD BLE 头常见广告名
$guess = $rows | Where-Object { $_.Name -match "OBD|ELM|VLink|VGATE|Vgate|KONNWEI|IOS|BLE|Car" }
if ($guess) {
  Write-Host "★ 名字像 OBD 诊断头的:" -ForegroundColor Yellow
  $guess | ForEach-Object { Write-Host ("    " + $_.Name + "   " + $_.Addr + $(if ($_.Paired) { "  (已配对)" } else { "  (未配对 —— 要先在 Windows 设置里配对)" })) }
  Write-Host ""
} else {
  Write-Host "★ 没看到名字像 OBD 诊断头的设备。" -ForegroundColor Yellow
  Write-Host "  · 若你那个头**根本没出现在上面这张表里** ⇒ 它是蓝牙经典 SPP,**S3 连不上**,这条路到此为止;" -ForegroundColor Yellow
  Write-Host "  · 出现了但没配对 ⇒ 先去 Windows 设置 → 蓝牙 里配对,再回来加 -Services。" -ForegroundColor Yellow
  Write-Host ""
}

# ---- 选定目标 ----
$target = $null
if ($Address) { $target = $rows | Where-Object { $_.Addr -eq $Address.ToUpper().Replace(":", "") } }
elseif ($Name) { $target = $rows | Where-Object { $_.Name -eq $Name } }
elseif ($Match) { $target = $rows | Where-Object { $_.Name -match $Match } }
elseif ($guess -and $guess.Count -eq 1) { $target = $guess }

if (-not $Services) {
  Write-Host "下一步:认准名字后加 -Name <名字> -Services 去读 GATT 结构。" -ForegroundColor Green
  exit 0
}

if (-not $target) {
  if ($rows.Count -eq 0) { Write-Host "没找到任何 BLE 设备。" -ForegroundColor Red }
  else { Write-Host "没能唯一确定目标:请显式给 -Name <名字> 或 -Address <地址>。" -ForegroundColor Red }
  exit 4
}
if (@($target).Count -gt 1) { Write-Host "匹配到多个,请用 -Address 指定:" -ForegroundColor Red; $target | ForEach-Object { Write-Host ("  " + $_.Name + "  " + $_.Addr) }; exit 4 }
$target = @($target)[0]

Write-Host ("=== 连 " + $target.Name + " / " + $target.Addr + " ===") -ForegroundColor Cyan

# 先拿到设备对象(配对那一段也要用它,所以放在前面)
$dev = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync([Convert]::ToUInt64($target.Addr, 16))) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
if (-not $dev) { Write-Host "拿不到设备对象(可能没通电/不在范围)。" -ForegroundColor Red; exit 5 }

# ★ 配对(可选):没配对时 Windows 不给 GATT 服务(实测:状态 Disconnected、
#   GetGattServicesAsync 直接回 Success 但 0 个服务 —— 极容易误判成"设备不支持")。
#   这一步与"设置 → 蓝牙 → 添加设备"是同一个操作;BLE OBD 头基本都是 Just Works
#   (无 PIN),所以通常不弹窗。★ 它会**写入系统状态**(这台机器以后一直记得这个设备)。
if ($Pair) {
  $di = $null
  foreach ($d in $res) { try { $a = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromIdAsync($d.Id)) ([Windows.Devices.Bluetooth.BluetoothLEDevice]); if ($a.BluetoothAddress.ToString("X12") -eq $target.Addr) { $di = $d; break } } catch {} }
  if ($di) {
    if ($di.Pairing.IsPaired) {
      Write-Host "已经是配对状态,不用再配对。" -ForegroundColor Green
    } else {
      Write-Host "正在配对(Just Works 的话不弹窗)..." -ForegroundColor Yellow
      try {
        $pr = Await ($di.Pairing.PairAsync()) ([Windows.Devices.Enumeration.DevicePairingResult])
        Write-Host ("配对结果: Status=" + $pr.Status + "  ProtectionLevelUsed=" + $pr.ProtectionLevelUsed) -ForegroundColor $(if ($pr.Status -eq 2) { 'Green' } else { 'Red' })
      } catch { Write-Host ("配对异常: " + $_.Exception.Message) -ForegroundColor Red }
    }
  } else { Write-Host "没在扫描结果里找到该设备,跳过配对。" -ForegroundColor Yellow }
  Start-Sleep -Seconds 2
}

# ★ 状态枚举的取值要打成人话:0=未连接 1=已连接 2=已配对但没连上
$cs = $dev.ConnectionStatus
Write-Host ("连接状态: " + $cs + "  " + $(switch ([int]$cs) { 0 { "(未连接)" } 1 { "(已连接)" } 2 { "(已配对,未连接)" } default { "" } }))

# ★ 连不上的两种情形要分清(2026-09-27 实测):
#   ① **配对**没做        ⇒ 用 -Pair(或设置里手动加设备);
#   ② 配对了但**没建立连接** ⇒ 本脚本实测遇到过:状态 `Disconnected`,而
#      `GetGattServicesAsync()` **不报错、只回 1 个服务(0x1801 Generic Attribute)** ——
#      这是"没真连上"的签名,**不是**"设备没有 OBD 服务",别据此下结论。
#   处置:CreateSession 强制把链路拉起来,并用 **Uncached** 再枚举(缓存里只有配对时那张表)。
$gr = $null
for ($try = 1; $try -le 3; $try++) {
  if ([int]$dev.ConnectionStatus -ne 1) { Start-Sleep -Seconds 2 }
  $gr = Await ($dev.GetGattServicesAsync(1)) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult])   # 1 = Uncached:强制真连一次
  $n = 0; if ($gr.Services) { $n = @($gr.Services).Count }
  Write-Host ("  第 " + $try + " 次: 连接状态=" + $dev.ConnectionStatus + "  Status=" + $gr.Status + "  服务数=" + $n)
  if ([int]$dev.ConnectionStatus -eq 1 -and $n -gt 1) { break }
  Start-Sleep -Seconds 2
}
if ($null -eq $gr -or -not $gr.Services -or @($gr.Services).Count -eq 0) {
  Write-Host "服务枚举拿不到任何服务。" -ForegroundColor Red
  Write-Host "  · 若连接状态一直是 Disconnected ⇒ 它可能**正被手机 App 占着**(BLE 从机通常只接一个中心)," -ForegroundColor Yellow
  Write-Host "    或它要求先由中心发一帧才开始工作 ⇒ 先断开手机、再跑一次;" -ForegroundColor Yellow
  Write-Host "  · 若只回 1 个服务(0x1801)⇒ 就是「没真连上」,**不是**设备没有 OBD 服务。" -ForegroundColor Yellow
  exit 6
}

Write-Host ("共 " + $gr.Services.Count + " 个服务:") -ForegroundColor Green
foreach ($s in $gr.Services) {
  Write-Host ""
  Write-Host ("服务 " + $s.Uuid) -ForegroundColor Yellow
  try {
    $cr = Await ($s.GetCharacteristicsAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult])
    if ($cr.Status -ne 1) { Write-Host ("   特征读取失败 Status=" + $cr.Status) -ForegroundColor DarkYellow; continue }
    foreach ($c in $cr.Characteristics) {
      $props = @()
      if ($c.CharacteristicProperties -band 0x08) { $props += "写入" }
      if ($c.CharacteristicProperties -band 0x04) { $props += "无响应写" }
      if ($c.CharacteristicProperties -band 0x10) { $props += "通知" }
      if ($c.CharacteristicProperties -band 0x20) { $props += "指示" }
      if ($c.CharacteristicProperties -band 0x02) { $props += "可读" }
      Write-Host ("   特征 " + $c.Uuid + "   [" + ($props -join "/") + "]")
    }
  } catch { Write-Host ("   特征枚举异常: " + $_.Exception.Message) -ForegroundColor DarkYellow }
}

Write-Host ""
Write-Host "=== 怎么读这份结果 ===" -ForegroundColor Cyan
Write-Host "  · 找**既「写入」又「通知」**的那一对特征 —— 那就是 ELM327 的命令口/数据口;"
Write-Host "  · 常见的成对 UUID:FFE0/FFE1、FFF0/FFF1、E7810A71-73AE-499D-8C15-FAA9AEF0C3F2(读写同一特征);"
Write-Host "  · 把这两行(UUID + 属性)发回给开发侧,下一步才是写 AT 指令、把 ELM327 那套问答搬上板子。"
Write-Host "  · ★ 板上已有 ObdSource 在做 ELM327 的问答与解析,换的只是**传输层**(UART -> BLE),解析那半可以复用。"
