<#
  车上抓 VAN —— 零依赖版(只用 Windows 自带的 PowerShell)

  为什么要有它(2026-09-18):抓帧是在**另一台笔记本**上做的,那台机器上不一定有
  Python + pyserial;而用串口终端(PuTTY / Tera Term / 各种"串口助手")有一条暗坑:
  它们打开串口时会**拉 DTR/RTS**,而那两根线在板上接的正是 EN / IO0 ——
  等于把芯片按住复位(或按进下载模式),屏幕上**一个字都不会有**,看起来像固件坏了。
  (这就是当初写 capture.py 的原因,见它的文件头。)
  .NET 的 SerialPort 默认 `DtrEnable = RtsEnable = false` ✓ 与 capture.py 行为一致,
  而且 PowerShell 每台 Windows 都有 —— 所以这个脚本**不用装任何东西**。

  用法(在笔记本上,脚本放哪都行):
    powershell -ExecutionPolicy Bypass -File capture-van-nopy.ps1 -Port COM5
    powershell -ExecutionPolicy Bypass -File capture-van-nopy.ps1 -Port COM5 -Seconds 60 -UntilFrames 20
    powershell -ExecutionPolicy Bypass -File capture-van-nopy.ps1 -SelfTest
    powershell -ExecutionPolicy Bypass -File capture-van-nopy.ps1 -ReplayFile 样本.log   # 调试:不开串口

  端口号不知道?先看:`[System.IO.Ports.SerialPort]::GetPortNames()`
  (板上是 CH340,通常显示为"USB-SERIAL CH340";插板子的 **UART 口**,不是原生 USB 口)

  产出:脚本旁边的 `van_capture_1.txt`(UTF-8 无 BOM),收尾打一张小结。
  格式与 capture.py 写出来的**完全一致**,拿回主力机器上照样能分析/回放。
#>
param(
  [string]$Port = 'COM4',
  [int]$Seconds = 600,
  [string]$Out = '',
  [int]$Baud = 115200,
  [int]$UntilFrames = 0,
  [string]$ReplayFile = '',
  [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# ---- 日志行解析(与 capture.py 的 Stats.feed 一一对应) ----
# 只用**纯 ASCII 锚点**匹配:设备端的中文标签在串口分片时可能被切成两半,
# 但 `VAN ` / `# ` / `iden=` / `edges=` 永远是 ASCII,靠它们就够稳。
$reVan    = '^VAN\s+([0-9A-Fa-f]{3,4})(?:\s+(.*))?$'
$reVanBad = '^#\s*VAN\b.*?iden=([0-9A-Fa-f]+)'
$reDiag   = '^van:\s*(.+)$'
$reEdges  = 'edges=(\d+)'
$reSrc    = '^SRC\s+(.+)$'

function New-Stats {
  return @{
    Bytes = 0; Lines = 0; Good = 0; Bad = 0
    GoodByIden = @{}; BadByIden = @{}; Example = @{}
    Diag = ''; Src = ''; Edges = $null
    Tail = ''      # 跨块残留的半行:一次读到的数据可能正好切断一行
  }
}

# 处理"刚收到的一块文本" —— 串口和 -ReplayFile 走的是**同一条**流水线,
# 这样"分块切断/续行/落盘/统计"这几件事在桌面上就能用回放验完,
# 不用等到车上才发现少了一行(2026-09-18:加这个函数就是为了能自测)。
function Process-Chunk([string]$text, $st, $sw, [int]$nbytes) {
  if ($nbytes -lt 0) { $nbytes = [System.Text.Encoding]::UTF8.GetByteCount($text) }
  $st.Bytes += $nbytes
  if ($sw) { $sw.Write($text) }              # 逐块落盘:中途拔线也不会丢已经收到的
  $st.Tail += $text
  $parts = $st.Tail -split "`n"
  $st.Tail = $parts[-1]                      # 最后一段可能还没结束,留到下一轮
  for ($i = 0; $i -lt $parts.Count - 1; $i++) { Feed-Line $parts[$i] $st }
}

# 哈希表是引用类型:函数里改 $st.Good 会直接改到外面那个对象(不需要 [ref])
function Feed-Line([string]$line, $st) {
  $line = $line.TrimEnd("`r", "`n")
  if ($line -eq '') { return }
  $st.Lines++

  if ($line -match $reVan) {
    $iden = [Convert]::ToInt32($Matches[1], 16)
    $st.Good++
    $st.GoodByIden[$iden] = 1 + [int]$st.GoodByIden[$iden]
    # 只留第一条样例:同一 IDEN 的样例几乎一样,留多了只会淹掉别的 IDEN
    if (-not $st.Example.ContainsKey($iden)) {
      $st.Example[$iden] = $line.Substring(0, [Math]::Min(72, $line.Length))
    }
    return
  }
  if ($line -match $reVanBad) {
    $iden = [Convert]::ToInt32($Matches[1], 16)
    $st.Bad++
    $st.BadByIden[$iden] = 1 + [int]$st.BadByIden[$iden]
    return
  }
  if ($line -match $reDiag) {
    $st.Diag = $line
    if ($line -match $reEdges) { $st.Edges = [int]$Matches[1] }
    return
  }
  if ($line -match $reSrc) { $st.Src = $line }
}

function Write-Summary($st, [double]$sec) {
  Write-Host ''
  Write-Host ("==== 抓帧小结({0:g} 秒)====" -f $sec) -ForegroundColor Cyan
  Write-Host ("字节 {0} · 行 {1} · 好帧 {2} · 坏帧 {3}" -f $st.Bytes, $st.Lines, $st.Good, $st.Bad)
  $idens = @($st.GoodByIden.Keys) + @($st.BadByIden.Keys) | Sort-Object -Unique
  if ($idens.Count -gt 0) {
    Write-Host ('{0,-6}{1,6}{2,6}   样例' -f 'IDEN', '好帧', '坏帧')
    foreach ($id in $idens) {
      $g = [int]$st.GoodByIden[$id]; $b = [int]$st.BadByIden[$id]
      $ex = ''
      if ($st.Example.ContainsKey($id)) { $ex = $st.Example[$id] }
      Write-Host ('{0,-6}{1,6}{2,6}   {3}' -f ('{0:X3}' -f $id), $g, $b, $ex)
    }
  } else {
    Write-Host '一条 VAN 帧都没有。桌面(没接总线)这样是正常的;' -ForegroundColor Yellow
    Write-Host '车上出现就是 PINOUT.md「C. 上车步骤」那张表的哪一种,照着办:' -ForegroundColor Yellow
    Write-Host '  edges 在涨 → 极性反了,对调 VAN_H/VAN_L'
    Write-Host '  edges 恒 0 → 接错脚/供电/RS 接错;先在桌面用蜂鸣档认脚位(见 PINOUT.md「抓帧盒」A 节)'
  }
  if ($st.Diag) { Write-Host "最近诊断: $($st.Diag)" }
  if ($st.Src)  { Write-Host "最近数据源: $($st.Src)" }
}

# ---- -SelfTest:不碰串口,只验"这趟抓到了什么"的统计对不对 ----
# 车上抓完回来,结论全靠这个统计;它要是把坏帧算成好帧、或把 IDEN 解析错,
# 人会照着错数字去改协议代码 —— 所以它必须有自检。
$sample = @(
  'I (123) BOOT',
  'van phy: gpio 就绪 RX=GPIO16(RO),空闲 300us 关帧',
  'van: edges=15 frames=0 fcs_ok=0 dropped=0(队列0) 待收=0',
  'VAN 824 18 F8 27 10 00 00 00   # cmd=1 ack=0',
  'VAN 824 18 F8 28 10 00 00 00   # cmd=1 ack=0',
  'VAN 8A1 01 02 03   # cmd=0 ack=1',
  '# VAN 校验失败 iden=824 len=3',
  'van: edges=99 frames=2 fcs_ok=2 dropped=1(队列0) 待收=0',
  'SRC speed=van rpm=sim coolant=sim intake=sim | v=57.3km/h 2100rpm 88.0C 21.0C',
  ''
)

if ($SelfTest) {
  $st = New-Stats
  foreach ($ln in $sample) { Feed-Line $ln $st }
  $checks = @(
    @('行数(空行不计)', $st.Lines, 9),
    @('好帧数', $st.Good, 3),
    @('坏帧数', $st.Bad, 1),
    @('IDEN 824 好帧', [int]$st.GoodByIden[0x824], 2),
    @('IDEN 824 坏帧', [int]$st.BadByIden[0x824], 1),
    @('IDEN 8A1 好帧', [int]$st.GoodByIden[0x8A1], 1),
    @('edges 取最后一次', $st.Edges, 99),
    @('数据源识别', $st.Src.StartsWith('SRC speed=van'), $true),
    @('样例保留第一行', $st.Example[0x824].StartsWith('VAN 824 18 F8 27'), $true)
  )
  $bad = 0
  foreach ($c in $checks) {
    $ok = ($c[1] -eq $c[2]); if (-not $ok) { $bad++ }
    $mark = 'OK '; if (-not $ok) { $mark = 'FAIL' }
    Write-Host ("[{0}] {1}: {2}" -f $mark, $c[0], $c[1])
  }
  Write-Summary $st 600
  if ($bad -gt 0) { Write-Host "$bad 项不符" -ForegroundColor Red; exit 1 }
  Write-Host 'selftest 全部通过' -ForegroundColor Green
  exit 0
}

if ($Out -eq '') { $Out = Join-Path $PSScriptRoot 'van_capture_1.txt' }

if ($ReplayFile -ne '') {
  # 调试通路:不开串口,把文件按 100 字符一块喂进**同一条**流水线,
  # 块边界故意切在行中间 —— 验的就是"跨块续行/统计/落盘"这几件事。
  # (真板不在手边时,这是唯一能把这条路径跑通的办法。)
  $all = [System.IO.File]::ReadAllText($ReplayFile, [System.Text.Encoding]::UTF8)
  $st = New-Stats
  $sw = New-Object System.IO.StreamWriter($Out, $false, (New-Object System.Text.UTF8Encoding($false)))
  $t0 = Get-Date
  for ($i = 0; $i -lt $all.Length; $i += 100) {
    $len = [Math]::Min(100, $all.Length - $i)
    Process-Chunk $all.Substring($i, $len) $st $sw -1
    Start-Sleep -Milliseconds 5
  }
  if ($st.Tail -ne '') { Feed-Line $st.Tail $st }
  $sw.Flush(); $sw.Close()
  Write-Summary $st ((Get-Date) - $t0).TotalSeconds
  Write-Host ('[调试] 回放 {0} 字节 → 写出 {1}' -f [System.Text.Encoding]::UTF8.GetByteCount($all), $Out) -ForegroundColor Cyan
  exit 0
}

Write-Host '=== 上车后先确认这 4 条(错一条就是白跑一趟) ===' -ForegroundColor Cyan
Write-Host ' 1) 笔记本用电池:充电器必须拔掉(USB 地 + 车 12V 地 = 地环流)'
Write-Host ' 2) 模块 3V3/GND 接板子;模块 GND 再单独一根到车地(OBD 4/5 脚或仪表侧搭铁)'
Write-Host ' 3) 120Ω 终端电阻已拆;GPIO16 接模块 RX;模块 TX 接 3V3(★ 不能悬空);RS 接 GND(6 脚模块无此脚=板内已接 GND)'
Write-Host ' 4) VAN_H/VAN_L 接仪表连接器 5/10 脚:先量对地 2~3V 且两根不相等,是 0V/12V 就停手'
Write-Host ''
Write-Host "开始抓帧:$Port @ $Baud / $Seconds 秒 → $Out" -ForegroundColor Cyan

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud,
        [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
# ★ 打开前就设好:这两根线在板上接 EN/IO0,拉高 = 按住复位,一个字都收不到
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.ReadTimeout = 200

try { $sp.Open() } catch {
  Write-Host "打不开 $Port : $($_.Exception.Message)" -ForegroundColor Red
  Write-Host '检查:口选对没(GetPortNames)?线插的是板子的 UART 口吗?被别的程序占着吗?'
  exit 1
}

$st = New-Stats
$sw = New-Object System.IO.StreamWriter($Out, $false, (New-Object System.Text.UTF8Encoding($false)))
$t0 = Get-Date
$lastLive = Get-Date
try {
  while ($true) {
    $el = ((Get-Date) - $t0).TotalSeconds
    if ($el -ge $Seconds) { break }
    if ($UntilFrames -gt 0 -and $st.Good -ge $UntilFrames) {
      Write-Host ''
      Write-Host "[抓帧] 已抓到 $($st.Good) 个好帧,提前收工" -ForegroundColor Green
      break
    }
    $avail = $sp.BytesToRead
    if ($avail -gt 0) {
      $buf = New-Object byte[] $avail
      $n = $sp.Read($buf, 0, $avail)
      $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
      Process-Chunk $text $st $sw $n
    } else {
      Start-Sleep -Milliseconds 40
    }
    # 实时进度最多 2Hz:太快只是闪屏,人眼也读不过来
    if (((Get-Date) - $lastLive).TotalMilliseconds -ge 500) {
      $lastLive = Get-Date
      $line = "[{0,7:N1}s] 字节={1} 行={2} 好帧={3} 坏帧={4}" -f $el, $st.Bytes, $st.Lines, $st.Good, $st.Bad
      if ($st.GoodByIden.Count -gt 0) {
        $line += '  IDEN ' + (($st.GoodByIden.GetEnumerator() | Sort-Object Value -Descending |
                 Select-Object -First 4 | ForEach-Object { '{0:X3}x{1}' -f $_.Key, $_.Value }) -join ' ')
      }
      if ($st.Diag) { $line += '  | ' + $st.Diag }
      Write-Host ("`r" + $line.PadRight(110)) -NoNewline
    }
  }
} finally {
  if ($st.Tail -ne '') { Feed-Line $st.Tail $st }
  $sw.Flush(); $sw.Close()
  if ($sp -and $sp.IsOpen) { $sp.Close() }
}

Write-Host ("`r" + (' ' * 112) + "`r") -NoNewline
Write-Summary $st ((Get-Date) - $t0).TotalSeconds
if (Test-Path $Out) {
  $f = Get-Item $Out
  Write-Host ''
  Write-Host ('原始日志:{0} ({1:N0} 字节)' -f $f.FullName, $f.Length) -ForegroundColor Green
}
exit 0
