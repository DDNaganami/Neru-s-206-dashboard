# 上车试车运行单（两块 2.8C + VAN 收发器）

> **这份单子是今晚的唯一口径。** 若笔记本上别的地方（另一个 DSH 会话、旧文档、旧副本）
> 说的与它冲突，**以本单为准**，并把冲突记下来告诉我。
> 生成于 2026-09-27 深夜（台机 HEAD = `b4e7b22`）；接线口径的正式版在 `PINOUT.md` 的
> 「装车接线」一节，本单是它的**执行版**（按今晚的具体条件：笔记本一拖二）。

---

## 0. 现在的世界状态（不用猜，都在这里）

| 项 | 状态 |
|---|---|
| 主板 | **COM9** = 右屏（车速 + 进气） |
| 从板 | **COM8** = 左屏（转速 + 水温） |
| ★ 端口号 | **可能变** —— 一律认输出里的 `role=MASTER` / `role=SLAVE`，别只认 COM 号 |
| 固件 | 今晚那版：VAN 接收脚 **GPIO44**（**只有主板**开）、`0x21` 原始帧转发（从板自己解帧）、主题 `alerts` 段支持、弹跳缓冲 20 行 |
| 素材 | **修好版**：背景 480×480、左右表情都 300×300、温度数字绿/橙。自证行 `image ok: 11 张,数据 3160800 字节` |
| 蜂鸣器 | 两块板都**静音**（`mute: 1 (loaded from NVS)`） |
| 收发器模块 | **还没接**（今晚要接的就是它） |

---

## 1. 笔记本上的版本对账（**先做这一步**，全中才算没被带歪）

在笔记本的仓库目录（`C:\Users\张九思\206Dash\Neru-s-206-dashboard`）里跑：

```powershell
Select-String lib\dashcore\van_phy_gpio.cpp -Pattern 'define VAN_RX_PIN'   # 必须 44
Select-String platformio.ini -Pattern 'DVAN_RX_PIN|DDASH_LOG_UART0'        # 44 + UART0=0
Select-String PINOUT.md -Pattern '^## 装车接线'                             # 必须有这一节
Select-String lib\link\link_frame.h -Pattern 'VanRaw = 0x21'               # 原始帧转发在
Test-Path tools\serial-capture\replay-drive.py                              # 回放工具在
```

| 看到什么 | 含义 |
|---|---|
| `VAN_RX_PIN 16` 或 `15` | ★ 副本是**旧的**（这两条口径都已作废）⇒ 先同步，**别照旧副本接线** |
| 没有「装车接线」这一节 | 同上：旧副本 |
| 五条都对得上 | 可以按本单往下做 |

★ 判断依据：`GPIO16` 电气上自由但**不在 12PIN 排针上**（要焊飞线）；`GPIO15` 是**板载 I2C**
（TCA9554/QMI8658/PCF85063 共用）；只有 `RXD`=**GPIO44** 既自由又在排针上。
这几条是 2026-09-27 定案、已烧进板子的。

---

## 2. 桌面预检（车上没有条件试错，这 4 步先在桌上做完）

1. **拆掉模块上的 120Ω 终端**（车总线两端已有终端）。
2. 断电、把模块拔下来，蜂鸣档定 `RX`/`TX` 谁是接收输出：
   - 模块 `TX` ↔ **芯片第 1 脚（`D`）** 导通 ⇒ 丝印是正的，按第 3 步接；
   - 若反倒是 `RX` 通到第 1 脚 ⇒ 厂家反标：**`RX` 接 3V3**、**`TX` 接 `RXD`**；
   - **别把 3V3 接到 `R` 上**（那是输出，两个输出会顶牛）。
3. 接四根：`3.3V`→`3V3`、`GND`→`GND`、**`RX`→`RXD`**、**`TX`→`3V3`**（★ 不能悬空）。
4. 上电，用手指磨 `CANL` 那个端子，看 `edges` 跳不跳：

```powershell
powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-van-nopy.ps1 `
    -Port COM9 -Seconds 12 -Out C:\206dash-scratch\pre-check.txt
# 然后在文件里搜： van: edges=      （磨之前记一个数、磨几下再记一个）
```

**不跳就先别上车** —— 车上分不清是"接线错"还是"极性错"。

---

## 3. 车上接线（今晚：**笔记本一拖二，完全不碰车电**）

| 接什么 | 怎么接 |
|---|---|
| 供电 + 日志 | 笔记本（★ **拔掉充电器、只用电池**）**两个独立 USB 口** → 两块板的**原生 USB** 口（不是 UART 口） |
| 板间 | ★ **一根线都不接**（ESP-NOW 就是无线的）。**尤其别接那根 5V** —— 今晚两个 USB 就是两个 5V 源，再接板间 5V 就顶牛 |
| UART Type-C | 两块板都**空着** |
| 模块 → 主板 12PIN | `3.3V`→`3V3`、`GND`→`GND`、`RX`→**`RXD`**、`TX`→`3V3`（按桌面那 4 根） |
| 模块 GND | ★ **还要接车地**（OBD 座 4/5 脚，或仪表侧搭铁）—— 差分参考，漏了就是"`edges` 不涨" |
| `CANH`/`CANL` | 仪表插头 **5 / 10 脚**（线号 `9004` / `9005`）；先按 `CANH` → **4V** 那根 |
| 摆放 | 模块放**板子旁边**，用一对**双绞线**去插头（长线走差分）；`RO`/`3V3`/`GND` 留短（≤50 cm），**别跟点火/线圈线束并走** |

★ 12PIN 一律**按丝印名**接（`GP0 / GND / RXD / TXD / SDA / SCL / 3V3 / GND / D+ / D- / 5V / GND`），
**不要只数针号** —— 仓库里"12PIN 第 6 脚"是**原理图 J9 的脚号**，与丝印顺序不是一回事。
接之前用蜂鸣档认一次 `5V` / `3V3` / `GND`（`5V` 误接到 `SCL` 上会打死 I2C）。

---

## 4. 车上顺序

1. **先不接** `CANH/CANL` 就上电，看主板日志里有没有这一行（证明固件这一侧是对的）：
   `van phy: gpio 就绪 RX=GPIO44(RO),空闲 70us 关帧,极性正常(VAN_RX_INVERT=0)`
2. 接 `CANH` → **4V** 那根（`9004`）、`CANL` → 1V 那根（`9005`），**并接**（不要剪线）。
3. 读主板三处：`van: edges=` 涨 → `frames=` / `fcs_ok` 涨 → `SRC speed=van rpm=van` 吗。
4. ★ **`edges` 涨、`frames=0` ⇒ 停车把这两根对调**（本车极性与标准 CAN 相反。
   实测过一次：对调前 3478 边沿/秒、0 帧；对调后 11 秒 492 帧、CRC 全过）。**别先去动固件。**
5. 看从板：`SRC speed=van`（★ 它**一根 VAN 线都没接**）+ `link: locked tick_age=…`。
6. 看屏：车速数字跟着实际车速走（跟仪表对照）。

**`edges` 一直不涨时按这个顺序查**（别乱换线）：
UART Type-C 有没有插着 → 模块供没供电（`3V3` 对 `GND` ≈3.3V）→ `RX` 是否接在 `RXD` 那一针 →
`TX` 是否接了 `3V3` → 模块 `GND` 是否接了车地。

---

## 5. 抓"真车录像"（今晚最值钱的副产品）

```powershell
powershell -ExecutionPolicy Bypass -File tools\serial-capture\capture-van-nopy.ps1 `
    -Port COM9 -Seconds 900 -Out C:\206dash-scratch\drive-real-1.txt
# 想连从板那一侧也留一份，就再开一个窗口：
#   ... -Port COM8 -Seconds 900 -Out C:\206dash-scratch\drive-real-slave.txt
```

★ 主板把**每一帧的每个字节**都打出来，格式 `VAN 824 18 F8 27 10 00 00 00` —— **正好是回放格式**。
所以这一趟抓下来，回来可以：① 用 `replay-drive.py` 把**真车数据**重放并逐条对数；
② 顺便解那句历史悬案「VAN 上有没有水温/进气温度」（旧脚本只落 `data[0..2]`，这份是全字节）。

---

## 6. ELM327 那一趟（车边 5 分钟，与本项目固件无关，纯体检）

1. 插 OBD 座，看它在笔记本/手机上**怎么出现**：
   ```powershell
   Get-PnpDevice -Class Bluetooth | Format-Table FriendlyName, InstanceId -AutoSize
   ```
   **`BTHLE\…` = BLE** / **`BTHENUM\…` = 经典蓝牙（SPP）**；★ 纯 BLE **不会**多出 COM 口。
2. **有 COM 口**（USB 或 SPP）⇒ 直接问 PID 位图：
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\obd-log\live-obd.ps1 -Port COMx -Seconds 30
   # 或： obd-log.ps1 -Port COMx -Seconds 60 -Pids '010C,010D,0105,010F'
   ```
3. **是 BLE** ⇒ 用手机 app（Car Scanner 之类）看能不能连、读不读得出水温；顺手记下有没有
   `FFF0`/`FFF1`/`FFF2` 这类服务（Windows 的 *Bluetooth LE Explorer* 也能看 GATT）。
4. ★ 回来要三个答案：**型号丝印**、**配对后的 InstanceId**、**有没有 COM 口**。
5. ELM327 插在 OBD 座是**常电**，试完拔掉。

**为什么值得问 `0100`**：它决定这台 ECU 到底**支不支持 `0105`（水温）/`010F`（进气）** ——
而 2.8C 上 UART 版 OBD **用不了**（`RX=17`/`TX=18` 正是 RGB 面板的 `DATA15/DATA14`），
所以那两个副表有没有真值，只能靠 BLE 那条路（或继续用笔记本当真值源）。

---

## 7. 红线（碰了会烧东西或白跑）

1. **UART Type-C 绝不插**（车上的时候）：① 两个 5V 源**直接并联**（这条路上没有二极管）；
   ② 它会把 43/44 从排针切给 CH343P ⇒ **VAN 接收（GPIO44）当场失效**。
2. **同一块板上不要同时接两个 5V 源**（车上那一路 + 任何 USB 线）。
3. 笔记本**只用电池**（别一边插市电一边接车）；要插市电就得加 USB 隔离器。
4. 模块 `TX` **不能悬空**（悬空被噪声拉低 = 显性 = **主动干扰总线**）。
5. `CANH`/`CANL` **并接取信号，不要剪线**。
6. **别用 5V 排针乱接**：只认丝印上的 `5V` / `3V3` / `GND` / `RXD`（`5V` 误接 `SCL` = 打死 I2C）。

---

## 8. 回来交什么

- `drive-real-1.txt`（有从板那份就一起）
- 串口里这几行（截图或文本都行）：`van phy:` / `van: edges=` / `SRC speed=` /
  从板 `SRC speed=` / `link: locked`
- ELM327 的三个答案
- 一句话现象：**屏上的车速跟不跟实际车速**

---

## 9. 以后（正式装车）与今晚的差别

- 今晚：笔记本一拖二（两个 USB 口，板间零线）。
- 正式：**OBD 降压线一拖二** —— 两种做法（详见 `PINOUT.md`「装车接线」）：
  **A** 一路 USB 进主板 + 主板 12PIN 的 `5V`/`GND` 两根线到从板；
  **B** 一个车充两个口 + 两块板各一根 USB 线（板间零线）。
  两种都要遵守：车上**不插 UART Type-C**、要看日志先**关掉车电**、一块板**不要两个 5V 源**。
