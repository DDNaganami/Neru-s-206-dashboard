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

工具链本身也搬到了 `C:\mingw64`（winget 装的那份在带中文的 WinGet 路径下）。

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
```

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
