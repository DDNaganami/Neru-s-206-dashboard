# 双板链路 v1 —— **单板回环验证**怎么做（一块裸 S3，不需要第二块板）

**这份文档是什么**：把 `lib/link`（双板链路协议 v1 的收发层）在一**块**板子上验一遍的
操作步骤 —— 接线、命令、期望输出、每一行的含义、以及**它测不到什么**。
契约在 `ARCHITECTURE.md` 的「## 双板链路协议 v1 范围」；本文只讲怎么**动手**。

适用固件：`env:esp32s3-linkloop`（`src/link_loopback.cpp`，只为这件事存在）。

---

## 1. 接线（**只有一根线**）

```
        ┌──────────────── 裸 ESP32-S3 (N16R8 / DevKitC-1) ────────────────┐
        │                                                                │
        │   GPIO17 (UART1 TX)  ●────────────┬────────────●  GPIO18 (UART1 RX)
        │                                  │
        │                           ★ 就这一根短接线
        │                                                                │
        │   GPIO43 / GPIO44  ← ★ 一个都别碰（见下）                       │
        │   原生 USB 口      ← 日志/刷机都走这里                           │
        └────────────────────────────────────────────────────────────────┘
```

| 项 | 值 |
|---|---|
| **要短接的两个脚** | **GPIO17 ↔ GPIO18**（UART1 的 TX 与 RX） |
| 用哪根线 | 一根杜邦线（母-母）就行；临时验证用一根回形针/镊子搭一下也可以 |
| 日志/刷机 | 走**原生 USB 口**（USB-CDC），`Serial`。PIO 里是 `--upload-port COM<n>` 那个 |
| 波特率 | **115200 8N1** |

### ★ 为什么**不能**拿 GPIO43/44 回环（重要）

43/44 是 UART0 的那对脚，在这块 devkit 上接着**板载 USB-串口桥**（CH340/CH343P），
而板上有颗 **`FSUSB42UMX` 模拟开关**（`ARCHITECTURE.md` §8 的 L1）把 43/44 在
"桥"和"排针"之间**二选一** —— 它**摘不掉**桥。

后果：短接 43/44 会把桥的**推挽 TX 也并进回路**（一个推挽输出对一个推挽输出 =
对打），测出来的东西不可信。所以 `link_phy_pins.h` 把回环的默认脚定在
**UART1 + GPIO17/18**，并且在**编译期**拦着不让你改成 43/44：

```c
// lib/link/link_phy_pins.h
#define LINK_LOOPBACK_UART_PORT 1   // UART1
#define LINK_LOOPBACK_TX_PIN   17
#define LINK_LOOPBACK_RX_PIN   18
```

把它改成 43/44（或把回环端口改回 0）会**编不过**，报错信息里写着原因。

> 顺带一条：**UART0 在硬件上就做不了本机回环** —— 那两块板子的 43/44 上挂着桥，
> 这不是软件选择。所以"换一个 UART"是唯一的路，不是偷懒。

---

## 2. 刷机与看输出

```powershell
# 仓库根目录（★ 若在中文路径下，先照 README 的办法复制到纯 ASCII 路径再构建）
python -m platformio run -e esp32s3-linkloop -t upload --upload-port COM4
python -m platformio device monitor -e esp32s3-linkloop -p COM4 -b 115200
```

（`COM4` 换成你机器上那个原生 USB 口。板载 CH340 那个口也能看日志，但它不参与回环。）

### ★ 期望输出（照这个核对）

```
linkloop: 206 dash 双板链路 v1 —— 单板回环验证固件
linkloop: 芯片=ESP32-S3 rev0  编译期口径 LINK_ROLE=1
linkloop: ★ 请确认 **GPIO17 与 GPIO18 已短接**（就这一根线）
linkloop: 回环走 UART1；日志走 USB-CDC 的 Serial（不碰 43/44）
linkloop: PHY 就绪 port=1 tx=GPIO17 rx=GPIO18 @115200 8N1
linkloop: --- 心跳 ---
linkloop: 发送 37 帧 / 收到 37 帧   (期望 200)
linkloop:   HELLO  sent=14  got=14
linkloop:   TICK   sent=7   got=7
...
linkloop: 五类 × 40 帧已入队，开始收尾 3000 ms
linkloop: === 汇总 ===
linkloop: 发送 200 帧 / 收到 200 帧   (期望 200)
linkloop:   HELLO  sent=40  got=40
linkloop:   TICK   sent=40  got=40
linkloop:   DATA   sent=40  got=40
linkloop:   STATUS sent=40  got=40
linkloop:   EVENT  sent=40  got=40
linkloop: CRC 错 0 / bad_len 0 / 未知类型 0 / 重同步噪声 0 字节
linkloop: 丢帧: LinkTx 环满 0 / PHY-TX 环满 0 字节 / PHY-RX 环满 0 字节
linkloop: 载荷自校验失败 0 处
linkloop: PHY 统计 rxTotal=3460 txTotal=3460
linkloop: PASS
```

**判据就是最后那一行 `linkloop: PASS`**（它把下面这些合起来判）：

| 行 | 期望 | 它在证明什么 |
|---|---|---|
| `发送 200 帧 / 收到 200 帧` | 两边都是 **200** | 五类消息各 40 帧，一帧都没丢 —— 帧编解码 + CRC + 重同步在**真实 UART 字节流**上站得住 |
| `CRC 错 0` | **0** | 没有误码；有的话说明线上真有问题（或波特率/接线不对） |
| `bad_len 0` / `未知类型 0` | **0** | 帧长判据与 TYPE 表一致 |
| `重同步噪声 0 字节` | **0** | 没有半截帧被丢掉再重新找 SYNC |
| `LinkTx 环满 0` | **0** | 契约 §1.2 ② 的"整帧丢"没发生（发送节奏淹不掉环） |
| `PHY-TX / PHY-RX 环满 0 字节` | **0** | UART 的两级环都没溢出 |
| `载荷自校验失败 0 处` | **0** | **大端字节序**：把解出来的字段与载荷原始字节逐字段对了一遍 |
| `rxTotal == txTotal` | 相等 | 发出去多少字节、收回来多少字节（回环里两者必然相等） |

`发送 N 帧 / 收到 N 帧` 这两个数**在心跳行里也应该同步在涨**（我这边是每 2 ms 一帧
⇒ 心跳那一秒里大概各涨 30~40）。如果**发送在涨、收到不动**，按下面排查。

---

## 3. 结果不对怎么查（按这个顺序，别跳）

1. **17/18 真的短接了没有** —— 万用表通断档量一下（最常见的原因就是这个）。
2. **串口 115200 8N1**；日志走的是原生 USB 口，别插错成板载 CH340 那个口就看错设备了。
3. **有没有别的外设占着 17/18**：`env:esp32s3-linkloop` 里 `-DVAN_PHY_GPIO=1` 仍然生效，
   而 VAN 默认用的是 **GPIO16**（不是 17/18），不冲突 —— 但如果你之前改过 `VAN_RX_PIN`
   或自己加过外设，先确认 17/18 是空的。
4. ★ **UART1 会不会被 OBD 抢走**：`src/main.cpp` 里 OBD 用的是 **UART1 + GPIO17/18**！
   本固件把 `main.cpp` 整个摘掉了（`-DLINK_LOOPBACK_FIRMWARE=1`），所以 OBD 那段**不在**，
   UART1 是干净的。但你要是拿别的固件来试回环，这条一定先查。
5. **`CRC 错` 在涨但收发数都到 200** ⇒ 线上有误码（线太长/接触不良/共地不好），
   回环里基本只会是"杜邦线没插实"。
6. **`PHY-RX 环满` 在涨** ⇒ 主循环被别的东西拖住了（本固件里不该发生；这是给你改代码后用的）。

---

## 4. ★ 这个回环**测不到**什么（别把结论读过头）

回环把**同一块板**的 TX 直接接到自己的 RX，所以它覆盖的范围是：

**测到了**：
- 帧编解码（§2 的偏移表、LEN 判据、CRC-15 覆盖）在真实 UART 字节流上往返正确；
- **大端字节序**（多字节字段拆/装一致，写反了会当场露出来）；
- 非阻塞 PHY 真的能发能收：TX 环 → `pumpTx()` → FIFO，UART 的 RX 任务 → PHY 环；
- 五类消息的**长度**都合法（11/12/12/13/23 B），且 `EVENT` 的 4 B 载荷不会被
  `bad_len` 误杀（§2 那处"下界 4 不是 5"的契约修正）；
- 两个环的溢出计数机制本身是通的。

**测不到**（要两块板 / 要最终硬件）：
- **43/44 的电气**：`ARCHITECTURE.md` §8 L1 里剩下的 ⓐ（`SEL` 默认电平）
  ⓑ（不插 UART Type-C 时 44 对地电平）ⓒ（插上 Type-C 时链路按预期失效）；
- **跨板电平/共地**、`5V` 支路能不能带得动从板（§8 L6）；
- **两块板的角色对账**（§5：`HELLO` 交换、`role_conflict`）—— 回环里
  收发是同一角色，`role_conflict` 那条路径**不会**被走到；
- **中断密度对 VAN 采集的影响**（§8 L2）、**临界区允许多长**（§8 L3）；
- **跨屏扫表对齐**（§8 L5）。

⇒ 回环 PASS 只说明"协议层 + PHY 层是通的"，**不等于**"两块板能跑"。

---

## 5. 这条路径在代码里落在哪（改东西时看这几处）

| 文件 | 里面是什么 |
|---|---|
| `src/link_loopback.cpp` | 回环固件本体（发 5×40 帧 → 收 → 汇总）。**只在 `LINK_LOOPBACK_FIRMWARE` 里编** |
| `lib/link/link_phy_uart.h/.cpp` | 真实 UART 的 PHY（非阻塞：自有环 + `pumpTx()`，见文件头） |
| `lib/link/link_phy_pins.h` | 接线常量 + **编译期守卫**（回环不许用 43/44、回环 UART ≠ 链路 UART） |
| `platformio.ini` 的 `[env:esp32s3-linkloop]` | 那四个 `-D`（含**为什么**这么定） |
| `test/test_dashcore/test_link_phy_uart.cpp` | 同一套接线口径在**宿主机**上的用例（改坏了会红） |

### 想改成"两块板对接"的同款自检？

把 `link_loopback.cpp` 里 `g_phy.begin(true)` 的 `true` 改回 `false`（走 §0 的
UART0 + GPIO43/44），去掉 `-DLINK_PHY_LOOPBACK=1`，两块板各自刷一套、43→44 交叉接线，
再各自把 `LINK_ROLE` 分别设成 1 / 0。**本轮没做这一步** —— 它要等最终 2.8" 板到货
（§5：两个角色 env 要等它落地），而且 §8 L1 的 ⓐⓑⓒ 三条实测还没做。
