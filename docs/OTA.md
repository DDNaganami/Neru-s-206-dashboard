# 无线刷写（WiFi OTA）—— 手机连板子的热点就能升级固件

> 2026-09-29 新增。车主问："能否进行 wifi 下的无线固件写入？"
> **答案：现在可以了**，但**第一次必须用 USB 刷一次**（把分区表换成 A/B 两槽的那张），
> 之后每一次升级都可以在车库/车里用手机完成，不需要笔记本、不需要数据线。
>
> ★★ **硬件验证状态：未验**。桌面侧完成了：分区表 + 固件 + 两个 OTA env 都**编过**、
> 分区表产物**逐项核过**（见 §5）。**真机上传/重启这一环还没跑过**（板子当时不在桌面侧）
> ⇒ 按 §5 的清单验一次再上车。

---

## 1. 为什么原来不行、现在怎么行

| | 原来 | 现在 |
|---|---|---|
| 分区表 | `partitions-s3-now.csv`：app **只有一个 2MB 的 `factory` 槽**（那张表的头里就写着 "NO OTA HERE"） | `partitions-s3-now-ota.csv`：app 是 **A/B 两槽** `ota_0` @0x10000 + `ota_1` @0xa70000，各 2MB |
| 无线刷写 | **结构上不可能**（没地方写"另一个镜像"） | 可以：写进**另一个**槽，重启后由 bootloader 切过去 |
| 代价 | —— | 一次性：新分区表要**用 USB 刷一次**；之后升级都走 WiFi |

★ **没动的东西**（这是硬约束，网页工具和文档都在印这些偏移）：
`nvs @0x9000`、`theme @0x210000`、`spiffs @0x214000`、`image @0x254000`、`coredump @0xa54000`
⇒ 换成新分区表之后，**主题、图片、静音标志全都还在**，不用重刷。

## 2. 一次性准备（USB，5 分钟）

前提：`origin/main` 已拉到含本功能的提交。

```powershell
# 从板（带 BLE 的那块，OBD 网关）
python -m platformio run -e esp32s3-rgb-slave-ota -t upload --upload-port COM8
# 主板（VAN + 链路主发）
python -m platformio run -e esp32s3-rgb-master-ota -t upload --upload-port COM7
```

★ 两条纪律照旧：
- **认板子看 `role=`，不要靠 COM 号**（上面只是示例）；
- **改完/换完镜像要验证它真的重编了**（看 `firmware.bin` 大小/时间戳变化 + 开机日志里
  出现下面那一行），别只看 `[SUCCESS]`。

刷完开机应当看到（这是"这一档支持无线刷写"的自证）：

```
ota: 这一档支持无线刷写 —— 串口敲 `u` 即重启进 OTA 模式（见 docs/OTA.md）
```

## 3. 每次升级（手机，约 1 分钟）

1. 板子上电（车里点火到 ON，或台面供电）。
2. **串口敲一个小写 `u`**（或串口面板里发一个 `u` + 回车）。
   板子会打印一行、然后**自己重启**：
   ```
   ota: 收到 `u` ⇒ 重启进 OTA 模式（只起 AP，链路/OBD 都不启动）
   ```
3. 重启后（约 2 秒）应当看到：
   ```
   ota: 当前槽 app0 @0x010000 | 目标槽 app1 @0xa70000 (2048 KB)
   ota: AP `206dash-SLAVE` 开放(无密码) 起来了=1  IP=http://192.168.4.1/  heap 163→114 KB
   ota: 等上传…（10 分钟没人来就自己退回正常固件）
   ota: 手机连上 `206dash-SLAVE` → 浏览器开 http://192.168.4.1/ → 选 firmware.bin
   ```
   ★ 上面打的是**分区标签**（`partitions-s3-now-ota.csv` 里那两行就叫 `app0` / `app1`），
   不是 `ota_0` / `ota_1` —— 文档以前写错了，上板核验时以板子打的这行为准。
4. 手机：连 WiFi **`206dash-SLAVE`**（主板是 `206dash-MASTER`）→ 浏览器开
   **http://192.168.4.1/** → 选 `firmware.bin` → 上传。
   - 选**对的那块板**的固件：从板 = `esp32s3-rgb-slave-ota`，主板 = `esp32s3-rgb-master-ota`；
     文件在 `.pio/build/<env>/firmware.bin`。
   - **不要**选 `firmware.factory.bin`（那是整片镜像，给 USB 用的）。
5. 上传中串口每 64KB 报一次进度：`ota: 已收 512 KB / heap=…KB`；
   收完打印 `ota: 收完 … B ⇒ 写入成功,准备重启`，手机页面显示 `OK: 固件已写入,正在重启…`。
6. 板子自动重启回**正常固件**（新的这一版）。串口里那行 `ota:` 提示会再次出现。

★ 进 OTA 模式之后**没有上传也没关系**：10 分钟自己重启回正常固件，不会卡住。
★ 上传失败也不会把板子变砖：**旧固件还在另一个槽里**，超时/断电后照旧启动它。

## 4. 已知代价与注意

- **OTA 期间屏幕是黑的**（v1 刻意不初始化显示/链路，为了射频干净、RAM 最多）。
  这是"正在升级"的信号；升完自动回来。想要"屏上显示升级进度"是下一步的打磨项。
- **AP 是开放的（无密码）**，但只在 OTA 模式的 10 分钟内存在。要加密码：
  给那一档加 `-DDASH_OTA_AP_PASS=\"你的密码\"`（≥8 位）。
- **上传约 1.7MB / 二三十秒**（从板）。慢一点是正常的（SoftAP + 手机）。
- **OTA 期间 ESP-NOW 链路与 OBD 都不工作**（就是为此才重启进来的）⇒ 两块屏显示旧值/模拟值，
  升完自动恢复。
- **不要在上传中断电**：写了一半的那个槽是坏镜像，但 bootloader 不会选它
  （`Update.end()` 才写 otadata），所以最坏情况是"这次没升成"，不是变砖。
- **两个槽都会用**：这次写 ota_1，下次就写 ota_0（A/B 轮换）——日志里那行
  `当前槽 …| 目标槽 …` 每次都会告诉你。

## 5. 上板核验清单（★ 2026-10-10 台面走通；判据都在下面）

**第 0 步就是判据行**：正常固件开机时会打一行
`boot: slot=app0 @0x010000`（= 当前运行槽的**分区标签 + 起始地址**）。
⇒ 无线刷成功之后这一行会变成 `app1 @0xa70000`。
**不用连 WiFi、不用读 `/status`：插上 USB 看这一行就知道"板子上跑的到底是哪个槽"。**
（`fw: build …` 那行只说"哪一版源码"，不说"哪个槽" —— 两行要一起看。）

1. **USB 刷 `esp32s3-rgb-slave-ota`** → 开机日志必须有
   `ota: 这一档支持无线刷写 …`（有 = 这一档真的带 OTA 代码）。✅ 已验
2. 串口敲 `u` → 重启进 OTA 模式，横幅按顺序出现：
   `开机检查标志 magic=0x20607A00 req=1` → `当前槽 app0 @0x010000 | 目标槽 app1 @0xa70000 (2048 KB)`
   → `AP \`206dash-SLAVE\` 起来了=1 IP=http://192.168.4.1/`。✅ 已验
3. 分区表真的进去了：横幅里的两个槽必须是 `app0 @0x010000` / `app1 @0xa70000`（各 2048 KB）。
   - ★ 如果打的是 `目标槽 (无)` 或 `★ 这张分区表没有第二个 OTA 槽`，
     说明**只刷了 app、没刷分区表** ⇒ 重刷一次，并确认 `partitions.bin` 也跟着写了。✅ 已验（USB 首次刷入）
4. 电脑/手机连 `206dash-SLAVE`、开 `http://192.168.4.1/` → 页面出得来，
   `GET /status` 回 JSON 且带 `Access-Control-Allow-Origin: *`。✅ 已验
5. 传 `esp32s3-rgb-slave-ota` 的 `firmware.bin`（**不是** `firmware.factory.bin`）→
   串口出进度、板子自己重启 ⇒ **`boot: slot=app1 @0xa70000`** ⇒ 全链路通。
6. **轮换验证**：再敲一次 `u`，横幅里的目标槽应当翻成 `app0` ⇒ 再传一次又翻回去。
   这一条验的是"下次升级还有地方写"。
7. 主板照做一遍（`206dash-MASTER`）。

### 5.1 ★ 从电脑上做同一件事：`tools/serial-capture/ota-cycle.ps1`

台面上踩到的两个坑**都不在板子上，在电脑这一侧**（脚本里已修掉，别再踩）：

1. **"连上了" ≠ "能通"**：Windows 会**立刻**报 `SSID=206dash-SLAVE`，而网卡还没拿到 IP
   （板子的 DHCP 慢半拍）。这时 `/status` 超时、POST 连 `/update` 都到不了 ——
   `app1` 一个字节都没写（读回来全是 `0xFF`），可客户端只给一句
   `Unable to connect to the remote server`，看着像板子的错。
   ⇒ **判据要等真的地址（`192.168.4.x`），不是等 SSID。**
2. **Windows 会自己"回家"**：家里那张 profile 是 `auto` ⇒ 它随时可能切回有网的那张，
   上传就断在半路。⇒ 上传期间把家里那张改成 `connection_mode=manual`，最后再改回 `auto`。
3. **HTTP 响应丢了 ≠ 上传失败**：`Update` 收完立刻 `ESP.restart()`，
   回复可能死在路上（板子重启会掐掉连接）。⇒ 判据是重启后的 `boot: slot=app1`
   （或 OTA 横幅里"当前槽/目标槽"翻过来），**不是** curl / 浏览器的返回码。

一条命令走完全程（进 OTA → 连 AP → 上传 → 验槽 → 恢复网络）。
★ **必须是一个进程**：板子的 SoftAP 没有外网，agent 分步做的话自己会断网
（那次 `DeepSeek API request failed` 就是这么来的）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\serial-capture\ota-cycle.ps1 -Port COM8
#   可选：-Bin <firmware.bin>  -Ap 206dash-SLAVE  -HomeAp MikamoNeru
```


## 6. 相关文件

| 文件 | 作用 |
|---|---|
| `partitions-s3-now-ota.csv` | A/B 两槽分区表（★ 里面记着两个踩过的坑：文件顺序必须升序、app 偏移要 64KB 对齐）|
| `lib/dashcore/ota_wifi.h/.cpp` | OTA 服务：AP + 上传页 + `Update` 写入；`u` 的入口与"取一次"标志 |
| `lib/dashcore/serial_cmd.h` | `u` → `SerialCmd::Ota`（口径与 `o`/`t` 相同：没编 OTA 的构建里恒 false）|
| `src/main.cpp` | `setup()` 开头读标志 → `ota_wifi_run()`（在显示/VAN/链路/BLE **之前**，所以射频干净、RAM 最多）；`boot: slot=…` 判据行（哪个槽在跑）|
| `platformio.ini` | `[env:esp32s3-rgb-slave-ota]` / `[env:esp32s3-rgb-master-ota]` |
| `tools/serial-capture/ota-cycle.ps1` | 电脑侧一条命令走完 OTA 全程（见 §5.1）；★ 这个文件必须保持**纯 ASCII** |

## 7. 为什么是"按需重启进入"，而不是常驻一个网页服务

写在这里，免得下一个人"顺手把它改成常驻"：

1. **稳态零代价**：AP + HTTP + `Update` 要几十 KB 内部 RAM，而从板带 BLE 时稳态只剩 ~29KB
   （还要喂 RGB 面板的弹跳缓冲）⇒ 常驻会直接吃掉余量，甚至把面板要的连续块挤碎。
2. **不碰射频共存**：ESP-NOW 在 STA 上、频道 6，SoftAP 会改频道；重启进 OTA 之后
   链路/OBD **根本不启动**，比"让它们互相让"干净得多。
3. **状态干净**：成功或超时都重启 ⇒ 不存在"半个初始化的状态"要收拾。
4. **断电安全**：标志放在 `RTC_NOINIT`（软件复位保留、断电丢失）—— 断电后不会把板子
   永远留在 OTA 模式里。
