<#
  标定跑(calibration drive)用的 OBD-II 记录仪 —— 零依赖版(只用 Windows 自带的 PowerShell)

  为什么要有它(2026-09-18):
    项目要拿**车的真实车速和转速随时间的变化**当基准(ground truth),回来跟逻辑分析仪
    抓的 VAN 总线做**时间对齐**,才能定标"总线上的车速字段 ↔ 真实 km/h"。
    VAN 那一路已经能用 tools/serial-capture/capture-van-nopy.ps1 抓了;缺的是**车自己
    报的数** —— 也就是走 K 线 OBD(ELM327 有线 USB 适配器,枚举成虚拟串口)的 010C/010D。

    为什么又只用 PowerShell:跑这趟的笔记本上**没有 Python**(和 capture-van-nopy.ps1
    同一个理由),所以不能写 .py。这台机器每台 Windows 都有 PowerShell,插上就能跑。

  ★ 波特率的坑(车上最容易白跑一条):
    ELM327 的 USB 克隆板绝大多数是 **38400**,但也有一批是 **9600**,而且**页面上不写**、
    外壳上也不印。波特率不对的表现是"打开串口成功、发什么都没反应" —— 看起来像适配器坏了。
    所以本脚本 ATZ 没应答时会**自动换另一个常见波特率再试一遍**,并明确告诉你哪个通了。
    默认先试 38400(克隆板主流),不通再试 9600;-Baud 可以指定先试哪个。
    ★ 头注释里两个东西不能写:反引号(PS 的续行符,连块注释里也生效)、以及块注释的
      收尾符号本身 —— 写出来就等于提前把注释关掉,后面所有代码都会被当成语法错误。

  ★ DTR/RTS:打开串口时**必须保持 false**(见下面开串口处那几行注释)——
    这个项目已经被 DTR/RTS 复位咬过一次(见 capture-van-nopy.ps1 的文件头)。

  用法(笔记本上,脚本放哪都行):
    powershell -ExecutionPolicy Bypass -File obd-log.ps1 -Port COM5 -Seconds 900 -Out C:\obd.csv
    powershell -ExecutionPolicy Bypass -File obd-log.ps1 -Port COM3 -Baud 9600 -Rate 3
    powershell -ExecutionPolicy Bypass -File obd-log.ps1 -Port COM5 -Pids '010C,010D,0105' # 加水温
    powershell -ExecutionPolicy Bypass -File obd-log.ps1 -SelfTest                        # 不碰串口

  ★ -Pids 的写法(这个坑踩过一次):
    逗号分隔的 PID 一定要**加引号**:-Pids '010C,010D,0105'。
    不加引号时 PowerShell 先把 010C,010D,0105 当成**数组**(逗号是数组分隔符),
    再按空格拼成字符串 → 010C,10,105:后两个 PID 就这么被悄悄改掉了,而且不报任何错 ——
    你会拿着一份"以为问了 010D 车速、其实问的是 0x10"的日志回来。
    本脚本为此把 -Pids 声明成 [string[]],并自己把数组拼回去;
    真拼不回去(已经被截断成 105 那种)就**报错退出**,绝不换一个 PID 去问。
  端口号不知道?先看:[System.IO.Ports.SerialPort]::GetPortNames()
    (ELM327 USB 通常显示为 "USB-SERIAL CH340" / "Prolific USB-to-Serial Comm Port")
    找不到口 → 装适配器自带的 CH340 / PL2303 驱动;口被手机 App / 串口助手占着也打不开。

  产出:CSV(-Out,默认写在脚本旁边),UTF-8 **无 BOM**,逐行落盘 + 每轮 flush ——
        中途拔线/脚本被杀,已经记下的行不会丢。
        列:t_s,wall_time,rpm,speed_kmh,raw_rpm,raw_speed
          t_s        从轮询开始算的**单调**秒(浮点,3 位小数;来自 Stopwatch,不受系统对时影响)
          wall_time  本机本地时间(ISO,带毫秒;机器时区自己核对,对齐主要靠 t_s)
          rpm        原始值 /4(010C:(A*256+B)/4)
          speed_kmh  010D 单字节 A,直接就是 km/h
          raw_*      原始十六进制值(如 1AF8 / 3C)—— 换算方式将来改了还能重新推
        没答上来的那一轮写**空字段**(不写 0):0 是"车停了",空是"没读到",两者不能混。
#>
param(
  [string]$Port = 'COM5',
  [int]$Baud = 38400,                 # 先试这个;不通自动换另一个常见值(见文件头)
  [int]$Seconds = 600,
  [string]$Out = '',
  [string[]]$Pids = '010C,010D',      # 默认转速+车速;要水温就加 0105(引号那条见文件头)
  [double]$Rate = 5,                  # 目标轮询轮次/秒(一轮 = 每个 PID 各问一次)
  [string]$ProbeFile = '',            # 调试:用罐头串口数据替掉真串口(没硬件也能跑全流程)
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
# 控制台按 UTF-8 输出:PS 5.1 默认用 GBK(936) 解,中文提示会变乱码
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# ============================================================
# 一、解析层(纯函数,不碰串口)—— -SelfTest 与真机走的是**同一份**代码
# ============================================================

# 轮询表:能解析哪些 PID。与固件 lib/dashcore/obd_protocol.cpp 的 kSupportedPids 对齐,
# 加一个 PID 只动这张表(PID 0x00 故意不在表里 —— 那是"支持位图",不是数据)。
$PidSpec = @{
  '0C' = @{ Bytes = 2; Name = '转速';   Unit = 'rpm';   Conv = { param($r) $r / 4.0 } }             # (A*256+B)/4
  '0D' = @{ Bytes = 1; Name = '车速';   Unit = 'km/h';  Conv = { param($r) [double]($r -band 0xFF) } }  # A = km/h
  '05' = @{ Bytes = 1; Name = '水温';   Unit = 'C';     Conv = { param($r) [double]($r -band 0xFF) - 40 } } # A-40
  '0F' = @{ Bytes = 1; Name = '进气温度'; Unit = 'C';   Conv = { param($r) [double]($r -band 0xFF) - 40 } } # A-40
}

# ELM327 吐出来的"不是数据"的话。它们**一定是整行/前缀**(不会夹在数据字节之间),
# 而且都含超出 A~F 的字母,所以按整串删不会误伤十六进制数据。
$NoiseWords = @(
  'SEARCHING...', 'SEARCHING', 'BUS INIT: ...', 'BUS INIT: OK', 'BUS INIT:', 'BUSINIT',
  'UNABLE TO CONNECT', 'NO DATA', 'CAN ERROR', 'DATA ERROR', 'STOPPED', 'BUFFER FULL',
  'ACT ALERT', 'LV RESET'
)

function Get-HexVal([char]$c) {
  if ($c -ge '0' -and $c -le '9') { return [int]$c - [int][char]'0' }
  if ($c -ge 'A' -and $c -le 'F') { return [int]$c - [int][char]'A' + 10 }
  return -1
}

# 十六进制串 → 字节数组。
#
# ★ 为什么要处理**奇数个**十六进制字符:
#   ATH0 生效时都是偶数("410C1AF8"),但克隆板不认 ATH0 时会带上 ECU 地址头,
#   写成 "7E8 04 41 0D 3C" —— 3 位头让整串变成 13 个字符。若直接判"奇数=垃圾",
#   带头的适配器就一条数据都读不出来。地址头是 3 个半字节,右对齐,所以从最左边
#   丢 1 个字符、让后面的字节重新对齐即可(丢掉的只会是 0 那个前导半字节)。
#   丢完仍然按**偶数字节边界**扫 41 —— "0C41" 里那个假 41 依旧不会被误认。
function ConvertFrom-HexString([string]$s) {
  if ($null -eq $s -or $s.Length -lt 2) { return $null }
  for ($off = 0; $off -le 1; $off++) {
    $len = $s.Length - $off
    if ($len -lt 2 -or ($len % 2) -ne 0) { continue }
    $out = New-Object 'System.Collections.Generic.List[int]'
    $ok = $true
    for ($i = 0; $i -lt $len; $i += 2) {
      $hi = Get-HexVal $s[$off + $i]; $lo = Get-HexVal $s[$off + $i + 1]
      if ($hi -lt 0 -or $lo -lt 0) { $ok = $false; break }
      [void]$out.Add(($hi -shl 4) -bor $lo)
    }
    if ($ok) { return , $out.ToArray() }
  }
  return $null
}

# 清洗:大写 + 去掉 ATS0 本该去掉的空格/换行 + 删掉上面那些噪声词,再滤掉括号等符号。
# ★ 为什么**无条件**去空格:克隆板经常不认 ATS0,照样吐 "41 0C 1A F8";
#   有的还夹 "SEARCHING..." 前缀,甚至把 "410C1AF8" 和噪声糊在一行。
#   与其对每种格式写一条正则,不如统一成"一串十六进制",解析只有一条路径。
function ConvertTo-CleanLine([string]$line) {
  if ($null -eq $line) { return '' }
  $s = $line.ToUpperInvariant()
  $s = $s -replace '\s', ''
  foreach ($w in $NoiseWords) { $s = $s.Replace($w, '') }
  $s = $s -replace '[^0-9A-F]', ''      # 剩下的非十六进制(如括号、'>' 提示符)一律丢
  return $s
}

# 解析一行普通响应。
# 返回 @{ Status='OK'|'NOPID'|'SHORT'|'BAD'|'NODATA'|'ERR'|'UNKNOWN'; Pid; Raw; Value }
#   OK    = 认出了请求的 PID,数据字节齐
#   NOPID = 认出了 41,但那是别条请求的回话(克隆板会把上一问的答案拖后吐出来)
#   SHORT = 认出 PID 但数据字节不够(残帧)
#   NODATA= 适配器明说 NO DATA;ERR = '?' / UNABLE TO CONNECT 等
function ConvertFrom-ElmLine([string]$line, [string]$wantPid) {
  $res = @{ Status = 'UNKNOWN'; Pid = ''; Raw = ''; Value = $null }
  if ($null -eq $line) { return $res }

  $up = $line.ToUpperInvariant()
  if ($up -match 'NO\s*DATA')                                    { $res.Status = 'NODATA'; return $res }
  if ($up -match 'UNABLE\s*TO\s*CONNECT' -or $up.Trim() -eq '?') { $res.Status = 'ERR';    return $res }
  if ($up -match 'CAN\s*ERROR|DATA\s*ERROR|BUFFER\s*FULL|BUS\s*INIT.*ERROR') { $res.Status = 'ERR'; return $res }
  # ★ K 线初始化失败就是这个样子的:"BUS INIT: ...ERROR"(五个点不是固定的)。
  #   不单独认它的话,这行既不是 NO DATA 也不含 ERROR 之外的锚点,会被当成"没看懂"的
  #   未知响应 —— 而它其实是**最需要报出来**的一种:说明 OBD 口上根本没建立 K 线会话。

  $clean = ConvertTo-CleanLine $line
  if ($clean.Length -lt 2) { return $res }        # 空行 / 只剩提示符
  $b = ConvertFrom-HexString $clean
  if ($null -eq $b -or $b.Count -lt 2) { return $res }

  # 扫 "41" —— 只看**偶数字节边界**:
  #   · 带 ECU 头时是 "7E8 04 41 0C .." → 41 落在偶数偏移,照样命中;
  #   · 只按字符扫会把 "0C41" 里的 41 当成应答头,那是**看起来合理**的错值 —— 必须避免。
  $n = $b.Count
  for ($i = 0; $i + 1 -lt $n; $i += 2) {
    if ($b[$i] -ne 0x41) { continue }
    $gotPid = '{0:X2}' -f $b[$i + 1]
    if (-not $PidSpec.ContainsKey($gotPid)) { continue }     # 0x00 位图走另一个函数,天然被拒
    $res.Pid = $gotPid
    $need = [int]$PidSpec[$gotPid].Bytes
    if ($i + 2 + $need -gt $n) { $res.Status = 'SHORT'; return $res }
    $raw = 0
    for ($k = 0; $k -lt $need; $k++) { $raw = ($raw -shl 8) -bor $b[$i + 2 + $k] }
    # 原样保留十六进制(2 字节 → "1AF8",1 字节 → "3C"):将来换算方式改了还能从 CSV 重推。
    # ★ .NET 格式化**不支持 '{0:X{1}}' 这种嵌套宽度**,只能两支分开写(会抛 FormatError)。
    if ($need -eq 2) { $res.Raw = '{0:X4}' -f $raw } else { $res.Raw = '{0:X2}' -f $raw }
    $res.Value = & $PidSpec[$gotPid].Conv $raw
    $res.Status = 'OK'
    if ($gotPid -ne $wantPid) { $res.Status = 'NOPID' }
    return $res
  }
  return $res
}

# 一次查询的多行响应 → 一条结论。
# 为什么要把整段读完再判:克隆板的一次回话可能跨两行(报头一行、数据一行),
# 或者把 "SEARCHING..." 单独写一行 —— 只看第一行就会误判成"没数据"。
function Resolve-ElmResponse([string[]]$lines, [string]$wantPid) {
  $sawNoData = $false; $sawErr = $false; $sawOther = $false; $lastRaw = ''
  foreach ($ln in $lines) {
    if ($null -eq $ln -or $ln.Trim() -eq '') { continue }
    $lastRaw = $ln.Trim()
    $r = ConvertFrom-ElmLine $ln $wantPid
    if ($r.Status -eq 'OK') { return @{ Status = 'OK'; Pid = $r.Pid; Raw = $r.Raw; Value = $r.Value; Line = $lastRaw } }
    if ($r.Status -eq 'NOPID') { $sawOther = $true; continue }
    if ($r.Status -eq 'NODATA') { $sawNoData = $true; continue }
    if ($r.Status -eq 'ERR') { $sawErr = $true; continue }
  }
  $st = 'NONE'
  if ($sawNoData) { $st = 'NODATA' } elseif ($sawOther) { $st = 'NOPID' } elseif ($sawErr) { $st = 'ERR' }
  return @{ Status = $st; Pid = ''; Raw = ''; Value = $null; Line = $lastRaw }
}

# ---- 0100:ECU 支持的 PID 位图(4 字节 = PID 01..20,MSB 在前)----
# 位序与固件的 pidSupported 一致:bit31 ↔ PID 0x01。写错会得到一个"看起来合理"的
# 结论(比如把 0x0D 判成不支持),所以它单独一个函数 + 自检里专门钉一条。
function ConvertFrom-SupportedBitmap([string]$line) {
  $clean = ConvertTo-CleanLine $line
  $b = ConvertFrom-HexString $clean
  if ($null -eq $b) { return $null }
  $n = $b.Count
  for ($i = 0; $i + 5 -lt $n; $i += 2) {
    if ($b[$i] -ne 0x41 -or $b[$i + 1] -ne 0x00) { continue }
    $mask = 0
    for ($k = 0; $k -lt 4; $k++) { $mask = ($mask -shl 8) -bor $b[$i + 2 + $k] }
    return $mask
  }
  return $null
}

function Test-PidSupported([int]$mask, [int]$gotPid) {
  if ($gotPid -lt 0x01 -or $gotPid -gt 0x20) { return $false }
  return (($mask -band (1 -shl (31 - ($gotPid - 1)))) -ne 0)
}

# ============================================================
# 二、串口层(可被 -ProbeFile 替掉)
# ============================================================
$script:Sp = $null          # 真串口;为 $null 时走罐头数据
$script:Pf = $null          # 罐头数据的行数组
$script:PfIx = 0
$script:PfSilent = $false   # 罐头模式下的"本次探测从此静默"(见下面 NOANSWER!)

function Read-AvailableText {
  if ($script:Pf) {
    # 调试通路:一行 = 一次"读到的字节"。
    #   'NOANSWER'  = 这一次读空了(消耗一行,下一次读下一行)
    #   'NOANSWER!' = **本次波特率探测从此一直读空**(真串口在"波特率不对"时就是这样:
    #                 1.6 秒的窗口里能读上千次空,罐头文件写不下那么多行,用哨兵代替)。
    #                 ★ 它只对**当前这一次探测**有效 —— Test-BaudAlive 每开始一个新
    #                 波特率都会清掉它;否则"先静默、换一档才有应答"的用例永远走不到第二档
    #                 (这正是当初手写罐头文件时踩到的坑)。
    if ($script:PfSilent) { return '' }
    if ($script:PfIx -ge $script:Pf.Count) { return '' }
    $ln = $script:Pf[$script:PfIx]; $script:PfIx++
    $ln = $ln -replace "`r", ''
    if ($ln -eq 'NOANSWER!') { $script:PfSilent = $true; return '' }
    if ($ln -eq 'NOANSWER') { return '' }
    return $ln
  }
  if ($script:Sp -and $script:Sp.IsOpen) {
    $avail = $script:Sp.BytesToRead
    if ($avail -le 0) { return '' }
    $buf = New-Object byte[] $avail
    $n = $script:Sp.Read($buf, 0, $avail)
    return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
  }
  return ''
}

function Send-Cmd([string]$cmd) {
  if ($script:Pf) { return }      # 罐头数据是**预置好的应答序列**,发什么都不影响它
  if ($script:Sp -and $script:Sp.IsOpen) {
    $script:Sp.Write($cmd + "`r")
  }
}

# 探测这个波特率上有没有活的 ELM327:发 ATZ,看有没有任何回应(不挑内容)。
# ATZ 同时把适配器复位到干净状态,所以"探测"和"初始化第一步"是同一发,不浪费。
function Test-BaudAlive([int]$b) {
  if ($script:Pf) {
    # 罐头模式:每次尝试消耗一行。'NOANSWER!' 的静默标志只覆盖**本次**探测,所以每次
    # 尝试都先清掉它 —— 换波特率 = 重新开始读。
    $script:PfSilent = $false
    Read-AvailableText | Out-Null
  }
  else {
    try { $script:Sp.Close() } catch { }
    $script:Sp = New-Object System.IO.Ports.SerialPort($Port, $b,
                  [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $script:Sp.DtrEnable = $false
    $script:Sp.RtsEnable = $false
    $script:Sp.ReadTimeout = 200
    try { $script:Sp.Open() } catch { $script:Sp = $null; return $false }
    $script:Sp.DiscardInBuffer()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalMilliseconds -lt 250) { Start-Sleep -Milliseconds 2 }   # 让板子稳住
    Send-Cmd 'ATZ'
  }
  $sw2 = [System.Diagnostics.Stopwatch]::StartNew()
  $acc = ''
  while ($sw2.Elapsed.TotalMilliseconds -lt 1600) {      # ATZ 要等复位,给足 1.6s
    $t = Read-AvailableText
    if ($t -ne '') { $acc += $t }
    if ($acc -match 'ELM327|OK|>') { break }
    if ($t -eq '') { Start-Sleep -Milliseconds 15 }
  }
  return ($acc.Trim().Length -gt 0)
}

# 一条 AT 命令:发出去,读回一行(带超时)。
# ★ 每条都返回文本而不是"成功/失败":兼容性靠打印原文判断 —— 克隆板可能回 '?',
#   也可能干脆不回,现场看得到才知道是脚本的问题还是适配器的问题。
function Invoke-AtCmd([string]$cmd, [int]$timeoutMs) {
  Send-Cmd $cmd
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $acc = ''
  while ($sw.Elapsed.TotalMilliseconds -lt $timeoutMs) {
    $t = Read-AvailableText
    if ($t -ne '') { $acc += $t; continue }
    if ($acc -match "[`r`n]" -or $acc -match '>') { break }
    Start-Sleep -Milliseconds 8
  }
  return $acc.Trim()
}

# 发一个 PID 请求,把这一次回话**读完整**(最多 totalMs),返回所有非空行。
# 为什么要有"总预算":K 线上没有的 PID 会一直等到适配器超时,克隆板甚至会卡住 ——
# 没有预算的读法会把整个记录拖死,而我们要的是**持续的时间序列**,宁可缺一轮也不能停。
function Invoke-ObdQuery([string]$pidHex, [int]$totalMs) {
  Send-Cmd ('01' + $pidHex)
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $lines = New-Object 'System.Collections.Generic.List[string]'
  $acc = ''
  while ($sw.Elapsed.TotalMilliseconds -lt $totalMs) {
    $t = Read-AvailableText
    if ($t -ne '') {
      $acc += $t
      if ($acc -match "[`r`n]" -or $acc -match '>') { break }   # 有一整行就可以判了
      continue
    }
    if ($acc -ne '' -and $acc -match "[`r`n]") { break }
    Start-Sleep -Milliseconds 5
  }
  foreach ($ln in ($acc -split "[`r`n]")) {
    if ($ln.Trim() -ne '') { [void]$lines.Add($ln.Trim()) }
  }
  return , $lines.ToArray()
}

# ============================================================
# 三、-SelfTest:把"难看的"真实响应喂进同一份解析代码,逐条断言
#    (不碰串口。车上回来复盘全靠这些数,解析错一个字节人会照着错数去定标。)
# ============================================================
# 断言打印。★ 必须是**脚本级**函数:PS 5.1 的词法器不接受"函数里再定义函数"
# (会报 "Missing ')' in function parameter list" 并把整个文件判成语法错误)。
$script:SelfPass = 0
$script:SelfFail = 0
function Check([string]$name, $got, $want) {
  $ok = ($got -eq $want)
  if ($ok) { $script:SelfPass++ } else { $script:SelfFail++ }
  $mk = 'OK  '; if (-not $ok) { $mk = 'FAIL' }
  Write-Host ("[{0}] {1}: 得到 {2} / 期望 {3}" -f $mk, $name, $got, $want)
}

# 跑一段代码,把异常压成 'THROW'(没抛异常就是 'NOTHROW')。
# 用于断言"乱写的参数必须报错" —— 这类路径最容易悄悄变成"猜一个值继续跑"。
function Get-ThrowMessage([scriptblock]$sb) {
  try { & $sb | Out-Null; return 'NOTHROW' } catch { return 'THROW' }
}

function Invoke-SelfTest {
  $script:SelfPass = 0; $script:SelfFail = 0

  Write-Host '-- 普通响应(含克隆板各种格式)--' -ForegroundColor Cyan
  # 标准带空格
  $r = ConvertFrom-ElmLine '41 0C 1A F8' '0C'
  Check '带空格 41 0C 1A F8' "$($r.Status)/$($r.Raw)/$($r.Value)" 'OK/1AF8/1726'
  # ATS0 生效后的连写
  $r = ConvertFrom-ElmLine '410C1AF8' '0C'
  Check '连写 410C1AF8' "$($r.Status)/$($r.Raw)" 'OK/1AF8'
  # 克隆板不认 ATS0,还带 SEARCHING... 前缀 + 换行
  $r = Resolve-ElmResponse @('SEARCHING...', '41 0C 1A F8') '0C'
  Check 'SEARCHING... 换行后才是数据' "$($r.Status)/$($r.Raw)" 'OK/1AF8'
  # 单行糊在一起
  $r = ConvertFrom-ElmLine 'SEARCHING...410C1AF8' '0C'
  Check 'SEARCHING... 与数据同一行' "$($r.Status)/$($r.Raw)" 'OK/1AF8'
  # 单字节 PID:车速 60 km/h
  $r = ConvertFrom-ElmLine '41 0D 3C' '0D'
  Check '车速 41 0D 3C' "$($r.Status)/$($r.Raw)/$($r.Value)" 'OK/3C/60'
  # 没开 ATH0(带 ECU 地址头):偶数对齐处仍要能命中
  $r = ConvertFrom-ElmLine '7E8 04 41 0D 3C' '0D'
  Check '带头 7E8 04 41 0D 3C' "$($r.Status)/$($r.Value)" 'OK/60'
  # 温度换算(留给 -Pids 0105 用)
  $r = ConvertFrom-ElmLine '41 05 7B' '05'
  Check '水温 41 05 7B' "$($r.Status)/$($r.Value)" 'OK/83'
  # 转义字符污染
  $r = ConvertFrom-ElmLine "41 0C 1A F8`r" '0C'
  Check '尾部回车' "$($r.Status)/$($r.Raw)" 'OK/1AF8'

  Write-Host '-- 失败/异常响应(必须判成"没读到",不能崩也不能编)--' -ForegroundColor Cyan
  $r = ConvertFrom-ElmLine 'NO DATA' '0C'
  Check 'NO DATA' "$($r.Status)" 'NODATA'
  $r = Resolve-ElmResponse @('NO DATA') '0C'
  Check 'NO DATA 整段' "$($r.Status)/$($r.Value)" 'NODATA/'
  $r = ConvertFrom-ElmLine '?' '0C'
  Check '问号(适配器不认这条命令)' "$($r.Status)" 'ERR'
  $r = ConvertFrom-ElmLine 'UNABLE TO CONNECT' '0C'
  Check 'UNABLE TO CONNECT' "$($r.Status)" 'ERR'
  $r = ConvertFrom-ElmLine 'BUS INIT: ...ERROR' '0C'
  Check 'BUS INIT 失败' "$($r.Status)" 'ERR'
  $r = Resolve-ElmResponse @('SEARCHING...', 'NO DATA') '0C'
  Check 'SEARCHING... 之后 NO DATA' "$($r.Status)" 'NODATA'
  # 残帧:认出了 PID 但数据字节不够 —— 绝不能当成 0
  $r = ConvertFrom-ElmLine '41 0C 1A' '0C'
  Check '残帧 41 0C 1A' "$($r.Status)/$($r.Value)" 'SHORT/'
  # 上一问的回话拖后到了:不该冒充本次结果
  $r = ConvertFrom-ElmLine '41 0C 1A F8' '0D'
  Check '错位响应(问 0D 收到 0C)' "$($r.Status)" 'NOPID'
  $r = Resolve-ElmResponse @('41 0C 1A F8') '0D'
  Check '错位响应整段' "$($r.Status)/$($r.Value)" 'NOPID/'
  # '>' 提示符本身
  $r = Resolve-ElmResponse @('>') '0C'
  Check '只有提示符 >' "$($r.Status)" 'NONE'

  Write-Host '-- 0100 支持位图(位序错了会得出"看起来合理"的错结论)--' -ForegroundColor Cyan
  # 固件单测里那张真实位图:41 00 BE 3E B8 13
  $m = ConvertFrom-SupportedBitmap '41 00 BE 3E B8 13'
  Check '位图原文' ('{0:X8}' -f $m) 'BE3EB813'
  Check '  → PID 0x0C 转速 支持' (Test-PidSupported $m 0x0C) $true
  Check '  → PID 0x0D 车速 支持' (Test-PidSupported $m 0x0D) $true
  Check '  → PID 0x05 水温 支持' (Test-PidSupported $m 0x05) $true
  # 只有最低位 = 只支持 PID 0x20(位序的另一端)
  $m2 = ConvertFrom-SupportedBitmap '4100 00 00 00 01'
  Check '只有最低位 → 支持 0x20' (Test-PidSupported $m2 0x20) $true
  Check '只有最低位 → 不支持 0x01' (Test-PidSupported $m2 0x01) $false
  # 高字节最高位 = PID 0x01
  $m3 = ConvertFrom-SupportedBitmap '410080000000'
  Check '最高位 → 支持 0x01' (Test-PidSupported $m3 0x01) $true
  Check '最高位 → 不支持 0x0C' (Test-PidSupported $m3 0x0C) $false
  Check '不完整的位图判为没拿到' (ConvertFrom-SupportedBitmap '41 00 BE 3E') $null

  Write-Host '-- 参数解析(--Pids 写法) --' -ForegroundColor Cyan
  $want = $null
  $want = ConvertTo-WantedPids '010c, 010D'
  Check '小写+空格' ($want -join ',') '0C,0D'
  $want = ConvertTo-WantedPids '0C,0D,05'
  Check '允许省略 01 前缀' ($want -join ',') '0C,0D,05'
  $want = ConvertTo-WantedPids '010C,010D,0105'
  Check '加水温' ($want -join ',') '0C,0D,05'
  # '10' 是**合法** PID(0x10 = 氧传感器),不能想当然当成 '01' 的误写:必须原样保留
  $want = ConvertTo-WantedPids '10'
  Check '两位 PID 0x10 不被误当 01' ($want -join ',') '10'
  Check '乱写要报错(不能静默换一个 PID 去问)' (Get-ThrowMessage { ConvertTo-WantedPids '010Z' }) 'THROW'
  Check '空 -Pids 要报错' (Get-ThrowMessage { ConvertTo-WantedPids '' }) 'THROW'

  Write-Host ''
  if ($script:SelfFail -gt 0) {
    Write-Host ("selftest 失败:$($script:SelfFail) 项不符(通过 $($script:SelfPass) 项)") -ForegroundColor Red
    return 1
  }
  Write-Host ("selftest 全部通过($($script:SelfPass) 项)") -ForegroundColor Green
  return 0
}

# -Pids 的宽容写法:'010C' / '0C' / '010c' / 逗号或空格分隔 都收。
# ★ 只收"01xx"或"xx"这两种写法,其它一律**报错**而不是猜 ——
#   车上没人有空去核对"我写的 PID 到底问的是哪一个"。
#   (注意 '10' 是合法的 0x10(氧传感器),不能当成 '01' 的误写。)
function ConvertTo-WantedPids([string[]]$parts) {
  # ★ 为什么收数组而不是字符串:不带引号的 -Pids 010C,010D 会被 PowerShell 拆成数组传进来。
  #   先拼回一个字符串,后面就只有一条解析路径(数组元素本身也可能再含逗号/空格)。
  $s = $parts -join ','
  $out = New-Object 'System.Collections.Generic.List[string]'
  foreach ($tok in ($s -split '[,;\s]+')) {
    $t = $tok.Trim().ToUpperInvariant()
    if ($t -eq '') { continue }
    if ($t.StartsWith('0X')) { $t = $t.Substring(2) }
    if ($t -match '^01([0-9A-F]{2})$') { $t = $Matches[1] }
    elseif ($t -match '^([0-9A-F]{2})$') { $t = $Matches[1] }
    else { throw "PID 写法看不懂:'$tok'(例:010C 或 0C)" }
    if (-not $out.Contains($t)) { [void]$out.Add($t) }
  }
  if ($out.Count -eq 0) { throw '-Pids 是空的' }
  return , $out.ToArray()
}

# ============================================================
# 四、自检入口(在任何串口动作之前返回)
# ============================================================
if ($SelfTest) {
  $code = Invoke-SelfTest
  exit $code
}

# ============================================================
# 五、真机流程
# ============================================================
# ★ 先把参数验掉再干别的:写在"上车清单"之前,是为了让 -Pids 写错时**一条提示就结束**,
#   而不是先让人对着车上清单检查半天,最后才说参数不对。
try { $wanted = ConvertTo-WantedPids $Pids } catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 2 }
$wantedCsv = ($wanted | ForEach-Object { '01' + $_ }) -join ' '

if ($ProbeFile -eq '') {
  # 端口存在性先查一次:打不开时给出**可操作**的话,而不是一句 .NET 异常
  try { $ports = [System.IO.Ports.SerialPort]::GetPortNames() } catch { $ports = @() }
  if ($ports -notcontains $Port) {
    Write-Host "找不到串口 $Port 。当前机器上的串口:" -ForegroundColor Red
    if ($ports.Count -eq 0) { Write-Host '  (一个都没有 —— 适配器没插,或者 CH340/PL2303 驱动没装)' }
    else { $ports | ForEach-Object { Write-Host "  $_" } }
    Write-Host '提示:先跑 [System.IO.Ports.SerialPort]::GetPortNames() 看名字;口被手机 App/串口助手占着也打不开。'
    exit 1
  }
}

if ($Out -eq '') { $Out = Join-Path $PSScriptRoot 'obd_log.csv' }
$outDir = Split-Path -Parent $Out
if ($outDir -ne '' -and -not (Test-Path $outDir)) {
  try { [void](New-Item -ItemType Directory -Path $outDir -Force) }
  catch { Write-Host "建不了输出目录 $outDir : $($_.Exception.Message)" -ForegroundColor Red; exit 2 }
}

Write-Host '=== 上车后先确认这 3 条(错一条就是白跑一趟) ===' -ForegroundColor Cyan
Write-Host ' 1) OBD 口在方向盘下方(206 是 16 针);钥匙拧到 ON(不用点火),仪表要亮'
Write-Host ' 2) 适配器带电源开关的先打开;开关灯亮 = 供电正常(没灯 = OBD 口 16 脚没 12V)'
Write-Host ' 3) 笔记本尽量用电池跑;车上的 USB 充电器会给 K 线引入噪声/地环流'
Write-Host ''

if ($ProbeFile -ne '') {
  $script:Pf = @(Get-Content -LiteralPath $ProbeFile -Encoding UTF8)
  $script:PfIx = 0
  Write-Host "[调试] 用罐头串口数据 $ProbeFile (共 $($script:Pf.Count) 行),不碰真串口" -ForegroundColor Yellow
}

# ---- 波特率探测:ATZ,不通就换另一个常见值 ----
$cand = New-Object 'System.Collections.Generic.List[int]'
foreach ($b in @($Baud, 38400, 9600)) { if (-not $cand.Contains($b)) { [void]$cand.Add($b) } }
$useBaud = 0
foreach ($b in $cand) {
  Write-Host ("试 {0} @ {1} ..." -f $Port, $b) -NoNewline
  if (Test-BaudAlive $b) { $useBaud = $b; Write-Host ' 有应答' -ForegroundColor Green; break }
  Write-Host ' 没应答' -ForegroundColor Yellow
}
if ($useBaud -eq 0) {
  Write-Host "在 $($cand -join ' / ') 上都问不到 ELM327。" -ForegroundColor Red
  Write-Host '依次查:口选对没(GetPortNames)? 适配器开关开了没? 被别的程序占着?'
  Write-Host '       线是"USB 能传数据"的那根吗(有些线只供电)? 少数板子是 115200,可 -Baud 115200 试。'
  if ($script:Sp -and $script:Sp.IsOpen) { $script:Sp.Close() }
  exit 1
}
if ($useBaud -ne $Baud) {
  Write-Host "★ 这个适配器实际是 $useBaud(不是默认的 $Baud)—— 下次直接 -Baud $useBaud" -ForegroundColor Yellow
} else {
  Write-Host "波特率 $useBaud 可用" -ForegroundColor Green
}
Write-Host "开始记录:$Port @ $useBaud / $Seconds 秒 / 目标 $Rate 轮每秒 / PID $wantedCsv → $Out" -ForegroundColor Cyan

# ---- 初始化序列:每条都写清"为什么" ----
# 顺序有讲究:复位必须在最前(它把波特率探测和"板子还活着吗"合成一步),
# ATSP0 必须在问 0100 之前(没定协议之前问 PID,适配器只会回 SEARCHING.../UNABLE TO CONNECT)。
$InitCmd = @(
  @{ C = 'ATZ';   T = 'reset:把适配器复位到干净状态;顺便也是"板子还活着吗"的探针' }
  @{ C = 'ATE0';  T = 'echo off:不关的话命令本身会被回显,和响应糊在一起,解析要额外剥一层' }
  @{ C = 'ATL0';  T = 'linefeeds off:响应只以 CR 结尾,一行一条,读的时候好判"这次说完了"' }
  @{ C = 'ATS0';  T = 'spaces off:字节之间的空格去掉,响应更短;★ 克隆板常不认 —— 解析照样去掉空格' }
  @{ C = 'ATH0';  T = 'headers off:不带 ECU 地址头(7E8 之类)。固件也是 ATH0;万一克隆板不认,解析按偶数字节边界扫 41,带头也能命中' }
  @{ C = 'ATSP0';  T = 'protocol auto:让它自己试 ISO9141-2/KWP 等。206 上具体是哪一种由适配器定,写死了换车就废' }
)
foreach ($c in $InitCmd) {
  $resp = Invoke-AtCmd $c.C 400
  Write-Host ("  {0,-6} → {1}   ({2})" -f $c.C, $resp, $c.T)
}

# ---- 问一次 0100,报 ECU 到底支持哪些 PID ----
$deltaLines = Invoke-ObdQuery '00' 600
$bitmapMask = $null
foreach ($ln in $deltaLines) {
  $m = ConvertFrom-SupportedBitmap $ln
  if ($null -ne $m) { $bitmapMask = $m; break }
}
Write-Host ''
if ($null -ne $bitmapMask) {
  Write-Host ('0100 ECU 位图 0x{0:X8}(PID 01~20):' -f $bitmapMask) -ForegroundColor Cyan
  $bits = @()
  for ($p = 0x01; $p -le 0x20; $p++) {
    if (Test-PidSupported $bitmapMask $p) {
      $h = '{0:X2}' -f $p
      if ($PidSpec.ContainsKey($h)) { $bits += ('01{0}({1})' -f $h, $PidSpec[$h].Name) }
      else { $bits += ('01{0}' -f $h) }
    }
  }
  if ($bits.Count -gt 0) { Write-Host ('  支持:' + ($bits -join ' ')) }
  $miss = @()
  foreach ($p in $wanted) {
    if (Test-PidSupported $bitmapMask ([Convert]::ToInt32($p, 16))) { continue }
    $miss += ('01' + $p)
  }
  if ($miss.Count -gt 0) {
    Write-Host ('  ★ 位图说这些不支持:' + ($miss -join ' ')) -ForegroundColor Yellow
    Write-Host '    仍会去问 —— 位图只有 01~20 这一段,而且克隆板/ECU 的说法未必准;问到是空的就说明真没有。'
  }
} else {
  Write-Host '拿不到 0100 位图(有些克隆板不认 0100,或协议还没锁上)。' -ForegroundColor Yellow
  Write-Host '不猜也不停:照常轮询要的那几个 PID,有没有数据看下面每轮的结果。'
}
Write-Host ''

# ---- 轮询循环 ----
$swTotal = [System.Diagnostics.Stopwatch]::StartNew()
$intervalMs = 1000.0 / [Math]::Max(0.2, $Rate)     # 一轮(每个 PID 各问一次)的目标间隔
# 每个 PID 的等待上限。★ 下限取 120ms 而不是更小:固件里那条"ECU 在 ISO 9141-2 上
# 通常 20~50ms 才回"的结论对这里同样成立(见 lib/dashcore/obd_source.cpp 的 kWaitTimeoutMs)。
# 查得太紧会表现为"丢响应 → 整轮空 → 实际刷新率反而更低",所以宁可少问几次也不要问空。
$perQueryMs = [int][Math]::Max(120, [Math]::Min(300, $intervalMs / [Math]::Max(1, $wanted.Count)))
$writer = New-Object System.IO.StreamWriter($Out, $false, (New-Object System.Text.UTF8Encoding($false)))
$writer.WriteLine('t_s,wall_time,rpm,speed_kmh,raw_rpm,raw_speed')
$writer.Flush()
Write-Host ("每轮上限 {0:N0} ms(每个 PID 最多等 {1} ms);读不到就记空字段,继续下一轮。" -f $intervalMs, $perQueryMs)
Write-Host ''

$rows = 0; $cycles = 0; $okCount = 0; $failCount = 0; $firstT = 0.0
$minRpm = $null; $maxRpm = $null; $minSpd = $null; $maxSpd = $null
$lastRpm = $null; $lastSpd = $null
$statusWanted = @{}
$swLive = [System.Diagnostics.Stopwatch]::StartNew()
$swCycle = [System.Diagnostics.Stopwatch]::StartNew()

try {
  while ($true) {
    $el = $swTotal.Elapsed.TotalSeconds
    if ($el -ge $Seconds) { break }

    # 一轮:每个想要的 PID 各问一次。同一个 PID 返回多行时,**第一行能解析的**为准。
    $sel = @{}
    foreach ($gotPid in $wanted) { $sel[$gotPid] = @{ Status = 'NONE'; Raw = ''; Value = $null } }
    foreach ($gotPid in $wanted) {
      $lines = Invoke-ObdQuery $gotPid $perQueryMs
      $r = Resolve-ElmResponse $lines $gotPid
      $sel[$gotPid] = $r
      $st = [string]$r.Status
      $statusWanted[$st] = 1 + [int]$statusWanted[$st]
      $rows++
      if ($st -eq 'OK') { $okCount++ } else { $failCount++ }
    }

    $t = $swTotal.Elapsed.TotalSeconds
    if ($firstT -eq 0.0) { $firstT = $t }
    $wall = (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss.fff')
    # t_s 保留 3 位小数:和逻辑分析仪对齐时,毫秒以下没意义,但秒级分辨率不够
    $line = '{0:F3},{1}' -f $t, $wall
    $rpmCsv = ''; $spdCsv = ''; $rawRpm = ''; $rawSpd = ''
    if ($sel.ContainsKey('0C') -and $sel['0C'].Status -eq 'OK') {
      $lastRpm = [double]$sel['0C'].Value
      $rpmCsv = '{0:F1}' -f $lastRpm
      $rawRpm = [string]$sel['0C'].Raw
      if ($null -eq $minRpm -or $lastRpm -lt $minRpm) { $minRpm = $lastRpm }
      if ($null -eq $maxRpm -or $lastRpm -gt $maxRpm) { $maxRpm = $lastRpm }
    }
    if ($sel.ContainsKey('0D') -and $sel['0D'].Status -eq 'OK') {
      $lastSpd = [double]$sel['0D'].Value
      $spdCsv = '{0:F0}' -f $lastSpd
      $rawSpd = [string]$sel['0D'].Raw
      if ($null -eq $minSpd -or $lastSpd -lt $minSpd) { $minSpd = $lastSpd }
      if ($null -eq $maxSpd -or $lastSpd -gt $maxSpd) { $maxSpd = $lastSpd }
    }
    $line += ',' + $rpmCsv + ',' + $spdCsv + ',' + $rawRpm + ',' + $rawSpd
    $writer.WriteLine($line)
    $writer.Flush()      # ★ 逐行 flush:脚本被杀/线被拔,已经写下的行必须还在盘上
    $cycles++

    if ($swLive.Elapsed.TotalMilliseconds -ge 500) {
      $swLive.Restart()
      $el2 = $swTotal.Elapsed.TotalSeconds
      $pollsHz = 0.0; if ($el2 -gt 0) { $pollsHz = $rows / $el2 }
      $sr = '--'; if ($null -ne $lastRpm) { $sr = '{0:F0}' -f $lastRpm }
      $ss = '--'; if ($null -ne $lastSpd) { $ss = '{0:F0}' -f $lastSpd }
      # 实时行**全 ASCII**:中文在部分控制台上是双宽字符,PadRight 的宽度会算歪、行会换行刷屏
      $st = '[{0,7:N1}s] cycles={1} polls={2} ({3:N1}/s) rpm={4} kmh={5}' -f $el2, $cycles, $rows, $pollsHz, $sr, $ss
      if ($st.Length -le 72) { $st = $st.PadRight(78) } else { $st = $st.Substring(0, 78) }
      Write-Host ("`r" + $st) -NoNewline
    }

    # 补足到目标节奏(问得比目标快就等一会儿,慢就直接进下一轮)
    $spent = $swCycle.Elapsed.TotalMilliseconds
    if ($spent -lt $intervalMs) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($intervalMs - $spent)) }
    $swCycle.Restart()
  }
} finally {
  Write-Host ("`r" + (' ' * 80) + "`r") -NoNewline
  try { $writer.Flush(); $writer.Close() } catch { }
  if ($script:Sp -and $script:Sp.IsOpen) { try { $script:Sp.Close() } catch { } }
}

# ---- 收尾小结 ----
$dur = $swTotal.Elapsed.TotalSeconds
$win = $dur - $firstT
$cycleHz = 0.0; $pollHz = 0.0
if ($win -gt 0) { $cycleHz = $cycles / $win; $pollHz = $rows / $win }
Write-Host ''
Write-Host ("==== OBD 记录小结($("{0:N1}" -f $dur) 秒)====") -ForegroundColor Cyan
Write-Host ("数据行 {0}(OK {1} / 空 {2})· 轮次 {3} · 实际 {4:N2} 轮/秒 · {5:N2} 次查询/秒(目标 {6:N1})" -f `
  $rows, $okCount, $failCount, $cycles, $cycleHz, $pollHz, $Rate)
$rng = '--'
if ($null -ne $minRpm) { $rng = ('{0:N0} ~ {1:N0} rpm' -f $minRpm, $maxRpm) }
$rngS = '--'
if ($null -ne $minSpd) { $rngS = ('{0:N0} ~ {1:N0} km/h' -f $minSpd, $maxSpd) }
Write-Host ("转速 {0} · 车速 {1}" -f $rng, $rngS)
if ($statusWanted.Count -gt 0) {
  $parts = @()
  foreach ($k in ($statusWanted.Keys | Sort-Object)) { $parts += ('{0}={1}' -f $k, $statusWanted[$k]) }
  Write-Host ('每次查询的结果分布:' + ($parts -join ' '))
}
if ($okCount -eq 0) {
  Write-Host '一条数据都没读到。依次查:' -ForegroundColor Yellow
  Write-Host '  · 钥匙在 ON 位?(ACC 不给 ECU 供电,只会一直 NO DATA)'
  Write-Host '  · 全程 SEARCHING... → 协议没锁上/OBD 口线序不对(206 上有些针脚是 VAN,不是 K 线)'
  Write-Host '  · 全是 ? → 适配器不认这条命令(多见于山寨板);试 -Pids 010C 单问一个'
  Write-Host '  · 全是 NODATA 且位图说这些 PID 不支持 → 这台 ECU 真的不报(206 上 010C/010D 是验过的)'
}
if (Test-Path $Out) {
  $f = Get-Item $Out
  Write-Host ''
  Write-Host ('CSV:{0}({1:N0} 字节)' -f $f.FullName, $f.Length) -ForegroundColor Green
  Write-Host ('列:t_s,wall_time,rpm,speed_kmh,raw_rpm,raw_speed  ·  t_s 是单调秒(轮询起点=0),对齐以它为准')
}
exit 0
