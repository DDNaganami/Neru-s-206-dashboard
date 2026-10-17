# BUILD-ENV.md —— 笔记本这台机器的构建/刷机环境坑（`tools/build/pio.ps1` 的详解）

> 这份文档是 `tools/build/pio.ps1` 里那段英文注释的**中文详解**。
> 脚本本体**刻意保持纯 ASCII** —— 原因见「坑 7」，那不是洁癖，是它能不能跑起来的问题。

**为什么要有这个脚本**：2026-09-27 一天之内踩了四个坑，每个都能浪费半小时；
2026-09-28 又加了三个，其中一个**把一块板刷黑过**。全部固化在脚本里，别再手工敲 pio。

---

## 坑 1 — 必须用 PlatformIO 自己的 venv python

```
C:\.platformio\penv\Scripts\python.exe
```

这台机器上还有另一个 python（`...\pythoncore-3.14-64\python.exe`），它装了
platformio 6.2.0 但**没装 esptool / intelhex**。编 `esp32s3` 时看不出来（不打包 bin），
一到要生成 `bootloader.bin` 就报：

```
ModuleNotFoundError: No module named 'intelhex'
```

★ `tool-esptoolpy/esptool.py` 的 shebang 是裸的 `#!/usr/bin/env python`，它会**去 PATH 上找 python**
⇒ 把 `penv\Scripts` 放在 PATH **最前**也同样重要（两件事都要做）。

## 坑 2 — 中文用户名会把宿主机工具链打死（GCC/ld/as）

症状**看着像链接失败、其实是连中间 `.o` 都写不出来**：

```
Fatal error: can't create C:\Users\<中文>\AppData\Local\Temp\ccXXXX.o
ld.exe: cannot find .../crt2.o / -lstdc++ / -lmingw32 ...   （一连串）
```

**修法**：只给编译器一个 ASCII 的 `TMP`（GCC 优先读 `TMP`，其次 `TEMP`）：
`$env:TMP='C:\temp'`，并且**不要再设 `$env:TEMP`**（见坑 3）。

工具链本身在 **`C:\206dash-scratch\zigbin`**（里面是 `gcc.cmd` / `g++.cmd` / `cc.cmd` / `c++.cmd`，每个都转发到 `C:\ziglang\zig.exe cc`）。

★ 旧写法说"工具链也搬到了 `C:\mingw64`"，**那是错的**：`C:\mingw64` 在本机**根本不存在**（`Test-Path` = False，实测）。照它把编译器路径钉进 `PATH`，等于整台机器**没有宿主机编译器** —— 每个 `.o` 都报一遍
`'gcc' is not recognized as an internal or external command`，最后收在 `[FAILED]`，
看起来像代码错，其实只是路径错（`tools/build/pio.ps1` 现在**先查目录存在再设 `PATH`**，缺了就直接 `exit 2` 并说明缺什么）。

## 坑 3 — 别改用户级的 `TEMP`

曾经把它指到 `C:\temp` 求 ASCII，结果 `intelhex` 直接 import 不到
（用户 site-packages 的解析跟着 TEMP 走）。**只改 `TMP` 就够，GCC 认它。**

## 坑 4 — 构建目录要 ASCII

`C:\206dash-build`（仓库本身在 `C:\Users\张九思\...` 里，路径带中文）。

## 坑 5 — pioarduino 那几档还得从 ASCII 镜像编

pioarduino 平台自己的构建脚本（`platforms/espressif32@src-*/builder/frameworks/
arduino.py` → `pioarduino-build.py`）在**中文路径**下解不出 `FRAMEWORK_DIR`：

```
TypeError: argument should be a str or an os.PathLike object where
 __fspath__ returns a str, not <class 'NoneType'>
```

**修法**：用 pioarduino 的档（`esp32s3-rgb*`）要从纯 ASCII 的镜像编：

```powershell
robocopy C:\Users\张九思\206Dash\Neru-s-206-dashboard C:\206dash-repo /MIR /XD .git .pio
# 然后在 C:\206dash-repo 里编（实测：编译 SUCCESS，128 秒）
```

（官方 `espressif32` 的档 —— `esp32s3` / `esp32dev` —— 在原路径能编，不用镜像。）

---

## 坑 6 ★★★ — `PYTHONIOENCODING=utf-8`：不加就会**把板子刷黑**

**2026-09-28 实测，代价：一块板黑屏十几分钟 + 一次 10 分钟超时。**

**症状**（认准这一条，它看起来完全不像编码问题）：
`pio run -t upload` **已经连上、stub 也跑了、正在写 `bootloader.bin`**，然后：

```
UnicodeEncodeError: 'gbk' codec can't encode character '\u2591' in position 23
```

**根因**（不是板子、也不是 USB）：

- 本机 ANSI 代码页 = **936（GBK）**；
- esptool 的写入进度条是用 `░`（U+2591，light shade）画的；
- Python 按 GBK 编码 stdout 时当场抛异常 ⇒ **上传线程被打断在写 bootloader 的中途**；
- ⇒ 启动区**被擦了却没写全** ⇒ **板子黑屏、串口全静默**
  （芯片还活着、USB 还在枚举，只是没有固件可跑）。

**为什么 `[Console]::OutputEncoding = UTF8` 救不了它**：
那句只改 **.NET 侧**的编码；**Python 子进程走的是 `PYTHONIOENCODING` 和本机 ANSI 代码页**。
在这个坑里两者是两回事 —— 脚本里那句一直都在，照样中招。

**修法**：在**派生子进程之前**把它设进环境（脚本第 4.5 步）：

```powershell
$env:PYTHONIOENCODING = 'utf-8'
$env:PYTHONUTF8       = '1'      # ★ 也是"不能省"，两个一起设才算完整
```

★★ **只设 `PYTHONIOENCODING` 还不够，`PYTHONUTF8=1` 必须一起设**（2026-10-10 实测，各烧掉一次构建）。
`PYTHONIOENCODING` 只管 stdout/stderr 这一层；`PYTHONUTF8` 管其余（**文件 / 文件系统的默认编码**），
不管 ANSI 代码页是多少都强制 utf-8。不设它，构建会在**几秒内**以两种面目之一失败，而两种都**长得不像编码问题**：

1. `[checkprogsize] UnicodeDecodeError: 'gbk' codec can't decode byte 0x85 ...`
   —— 镜像**其实已经生成好了**，只是末尾的尺寸检查拿 GBK 去解码产物里的 UTF-8 中文；
2. `TypeError: argument should be a str or an os.PathLike object where
   __fspath__ returns a str, not 'NoneType'` —— 同一个病在**配置阶段**发作，7 秒就失败。

⇒ 补一条判据：**看 `[SUCCESS]` 那一行，别只看退出码**（成功时 pio 也可能返回 1）。

**中招之后怎么救**：不用短接、不用手动进 download 模式 —— **重刷一次就好**。
芯片没坏，重刷会写到 `0x00000000` 把启动区补全（实测：59 秒，四个镜像全部
`Hash of data verified`）。

## 坑 7 ★★★ — `.ps1` 没有 BOM，Windows PowerShell 5.1 会**执行注释里的内容**

**2026-09-28 实测**：一个编辑工具把 `pio.ps1` 按**无 BOM 的 UTF-8** 回写之后：

```
Missing ')' in function parameter list
The term '<中文>' is not recognized as a cmdlet ...
```

指的行号落在**块注释内部**。

**根因**：Windows PowerShell **5.1**（本 harness 用 `powershell -File` 调的就是它）
**没有 BOM 就按本机 ANSI（936/GBK）读 `.ps1`** ⇒ 中文注释逐字变乱码 ⇒ 连块注释的
`#>` 也认不出来 ⇒ **整个脚本结构崩掉、正文被当代码执行**。

★ 这不是"注释乱码不好看"，是**脚本根本跑不起来**。

**两条规矩**：

1. **给 `.ps1` 加非 ASCII 文本，就必须存成带 BOM 的 UTF-8**，并存完验证：

   ```powershell
   [IO.File]::ReadAllBytes($p)[0..2]   # 必须是 EF BB BF
   ```
2. ★ **更好的做法：让脚本保持纯 ASCII**，编码问题就不存在了。
   `tools/build/pio.ps1` 现在就是这个状态（非 ASCII 字节数 = 0），
   中文详解就是本文。

★ 同一个坑对**别的 `.ps1` 也成立**：`tools/serial-capture/*.ps1`、
`tools/sync/*.ps1`、`tools/bt-obd/*.ps1` 里都带中文注释 —— 那些文件现在**都带 BOM**，
改它们的时候别把 BOM 弄丢。

---

## 用法

在仓库里的任意 PowerShell 里（**pio 的参数要整串用引号包起来**）：

```powershell
.\tools\build\pio.ps1 -Cmd 'test -e native'
.\tools\build\pio.ps1 -Cmd 'run -e esp32s3-rgb-master-now'
.\tools\build\pio.ps1 -Cmd 'run -e esp32s3-rgb-slave-b10ble -t upload --upload-port COM8'
```

**为什么是 `-Cmd '整串'` 而不是直接 `pio.ps1 run -e esp32s3`**：
PowerShell 会把 `-e` 当成**脚本自己的参数名**（报 "parameter name 'e' is ambiguous"，
匹配到 `-ErrorAction`），而 `--` 分隔符在 `-File` 调用下也不吃。整串传参绕开这一切。

## 两块 2.8C 真正跑的是哪一档

| 板 | 档 |
|---|---|
| 主板（右屏） | `esp32s3-rgb-master-now` |
| 从板（左屏） | `esp32s3-rgb-slave-now` |

这两个是**无线档**（ESP-NOW，`LINK_PHY_UART=0`）+ 自己的分区表 `partitions-s3-now.csv`。
而 `esp32s3` 是**有线链路**档 ⇒ 它与 `VAN_RX_PIN=44` 不能共存
（`lib/dashcore/van_phy_gpio.cpp` 里那道 `#error` 就是拦这个）。

★ **认板不要看 COM 号**（主副板的端口会互换）：读启动行的 `role=MASTER` / `role=SLAVE`。

## 改 `platformio.ini` 的宏时要验一遍解析结果

`extends` + 重复 `-D` 的语义是"**后写的生效**"，但这件事**必须验、不能假设**
（本仓库踩过一次重复 `RGB_BOUNCE_LINES` 的坑）。用 PlatformIO 自己的解析器打出来看：

```powershell
$py = 'C:\.platformio\penv\Scripts\python.exe'
& $py -c "from platformio.project.config import ProjectConfig; c=ProjectConfig(r'<repo>\platformio.ini'); c.validate(); print(c.get('env:esp32s3-rgb-slave-b10ble','build_flags'))"
```

## 坑 8 ★★ — 系统 `python` 是**商店假 python**：退出码 0、零输出

**2026-10-14 实测**（代价：一整轮 A/B 白跑）：

```powershell
(Get-Command python).Source
# C:\Users\<用户名>\AppData\Local\Microsoft\WindowsApps\python.exe   ← 不是真解释器
```

那是 Windows 的"应用执行别名"占位：**不打印、不报错、退出码 0**。
于是 `python -m platformio run -e pcpreview -t exec` 会"成功"返回，而**一个字节都没产出**
—— 落的帧还是上一次的旧文件。

★ 后果特别阴：**"前后两轮产物完全一样"会被读成"A/B 没有差异"**（本轮就差点这么报）。
⇒ 两条纪律：

1. **只用** `tools/build/pio.ps1 -Cmd '…'`（写死了 venv python 全路径），或
   `& C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe -m platformio …`。
   **别用**裸 `python` —— 它是"应用执行别名"占位（`python --version` 退出码 **9009**、**零输出**）。
   `README.md` §构建 里那几行 `python -m platformio` 已按此**改成** `.\tools\build\pio.ps1 -Cmd '…'`。
2. **判据看产物，不看退出码**：跑完先确认落下来/编出来的文件**时间戳或哈希变了**
   （帧看 `preview/frames/l_0030.bmp` 的 mtime；镜像看 `[SUCCESS]` 行）。
   ★ 这与"pio 成功时也可能返回 1"是同一个病的两面。

## 环境现状（2026-10-14 实测，与本文旧写法有出入）

| 路径 | 在不在 | 说明 |
|---|---|---|
| `C:\.platformio\penv\Scripts\python.exe` | ✓ | `tools/build/pio.ps1` 用的那套 |
| `C:\206dash-scratch\pio-core-mix\penv\Scripts\python.exe` | ✓ | `docs/LINK-TWO-BOARD.md` §构建 用的那套 |
| `C:\206dash-scratch\now-vanraw` | ✓ | **在用的 ASCII 镜像**（`-now` 两档；`ota-cycle.ps1` 里写死了它的固件路径） |
| `C:\206dash-scratch\now` | ✓ | 另一份镜像（`README.md` §构建 里的 `$copy` 例子） |
| `C:\206dash-repo` | ？ **可能不存在，取决于哪一轮建的** | 坑 5 里那个镜像名 —— 2026-10-14 记的是"已不存在"，之后**打包代理又重建过**（现在 `Test-Path` = True）⇒ **判据：用前先 `Test-Path`**，别把某一轮的结论写死 |
| `C:\206dash-build` | ？ **可能不存在，取决于哪一轮建的** | 坑 4 里的 `PLATFORMIO_BUILD_DIR`，现在是**按需创建**（现在 `Test-Path` = True）⇒ **判据：用前先 `Test-Path`** |
| `C:\206dash-scratch\zigbin` | ✓ | ★ **宿主机工具链**（`gcc.cmd`/`g++.cmd`/`cc.cmd`/`c++.cmd` → `C:\ziglang\zig.exe cc`）；`tools/build/pio.ps1` 设进 `PATH` 的就是它 |
| `C:\mingw64\bin` | ✗ **不存在** | 旧注释/旧文档里的工具链位置，**是错的** —— 钉在这里等于这台机器没有宿主机编译器（症状：每个 `.o` 都报 `'gcc' is not recognized`） |

★ 同步命令（本轮实测可用）：`robocopy $repo $copy /E /XD .git .pio /NJH /NJS /NP`
—— 退出码 **1/3 = 成功**、**≥8 = 失败**；`/E` 不删目标里多余的文件（要镜像式删才用 `/MIR`）。

★★ **CJK 路径 + 无 BOM 的 `.ps1` 会把路径本身也解析坏**（坑 7 的延伸）：本轮
`robocopy` 报 **退出码 16**（"没有复制任何文件"）就是这么来的 —— 脚本里的
`C:\Users\张九思\…` 被 Windows PowerShell 5.1 按 GBK 读成了乱码路径。
⇒ 脚本里**不要写字面 CJK 路径**，用 `Join-Path $env:USERPROFILE '206Dash\Neru-s-206-dashboard'`；
脚本正文**保持纯 ASCII**（与 `tools/build/pio.ps1` 同一条规矩）。

## 坑 9 ★★ — `PLATFORMIO_BUILD_DIR` 也必须指到**纯 ASCII** 路径

坑 5 只写了“仓库路径要 ASCII”。2026-10-15 实测：**只把仓库镜像放 ASCII 还不够** ——
`esp32s3-rgb-master-now` 在 pioarduino 自己的 `arduino.py:531` 抛
`TypeError: argument should be a str or an os.PathLike object where __fspath__ returns a str,
not 'NoneType'`。

⇒ 与坑 5 同源（CJK 路径让 pioarduino 解析不出路径），解法也一样：
```powershell
$env:PLATFORMIO_BUILD_DIR = 'C:\206dash-scratch\final-builds\master'   # 每块板一个子目录
```
（同一天用这套拿到了 master/slave 双 `[SUCCESS]`，见 `ACCEPTANCE.md` 的 2026-10-15 一节。）

## 坑 10 ★★★ — `platformio.ini` 的 `-D` 会**覆盖源码默认值**：改源文件可能等于白刷

**2026-10-15 实测**（代价：一整轮刷机白跑）：

**现象**：把 `src/dash_display_rgb.cpp` 里 `#ifndef RGB_BOUNCE_LINES` 的默认值 10 改成 20，
native / pcpreview / esp32s3-rgb-slave-now 三项全 `[SUCCESS]`、上传也 `Hash of data verified` ✓，
**但开机日志里 `rgb: bounce=10行/块(2块共18KB内部SRAM)` 一个字都没变 ✗** —— 板上跑的还是 10 行。

**根因**：那个默认值在 `#ifndef` 守卫里，而 `platformio.ini` 的**每个 env 都显式带了**
`-DRGB_BOUNCE_LINES=<N>` ⇒ `-D` 覆盖默认值，**源文件里那一行根本轮不到生效** ✗。
（同一个宏的值被写在两个地方 ⇒ 只改一份就是白刷。）

⇒ 两条纪律：

1. **改档位要两处一起改**：源文件默认值 **+** 对应 env 的 `-D`（以及 `extends` 链上可能又被覆盖的
   那一层，见上面「改 `platformio.ini` 的宏时要验一遍解析结果」那一节）。
2. ★ **判据永远认开机日志里的那一行，不是看你改了哪个文件**：改 bounce 就看
   `rgb: bounce=<N>行/块`；改绘制缓冲就看 `rgb: 编译期事实 … draw_buf=`；改 pclk 就看
   `rgb: vsync=` 的每秒值。**日志里那个数没变 = 改动没生效**，别拿 `[SUCCESS]` 当"生效"的证明。

★ 本条的直接出处 —— 对「**主板从不犯、从板反复犯**」的直接解释：

| env | 板 | 2026-10-15 之前的 `-D` | 改档之后 |
|---|---|---|---|
| `[env:esp32s3-rgb-master-now]` | 主板（右屏） | **30**（`platformio.ini:461`） | 30 |
| `[env:esp32s3-rgb-slave-now]` | 从板（左屏） | **10**（当时在 `platformio.ini:547`，现 `:558`） | **30** |

⇒ 两块板跑的**一直不是同一个档**：主板 30 行、从板 10 行。同一份源码、同一个"横纹"问题，
从板反复犯而主板从不犯 —— 差异就在这一行 `-D` 上，不在源码里。

