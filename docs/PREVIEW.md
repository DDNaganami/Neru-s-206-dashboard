# 宿主机预览（`env:pcpreview`）—— 怎么跑、每个键是什么、与真机的对应关系

这份文档回答四件事：

1. **怎么跑**（可复制命令，含那条"必须在纯 ASCII 副本里跑"的前提）；
2. **每个键 / 每个注入项是什么**（键盘 + `preview/inject.txt`）；
3. **预览与真机的对应关系**（画布 px ↔ 面板 mm、四角为什么不可见、
   哪一层代码是同一份、哪一层是预览专有）；
4. **预览看不到什么**（★ 这一节最重要 —— 不写清楚就会误导：
   撕裂/残留那类问题**在这个预览里结构上不可能出现**）。

> 口径来源：本文所有面板数字都取自仓库里**已有的**记录，没有新发明的数：
> `PURCHASE.md` 第六节（Ø 有效区 70.13 mm）、`tools/theme-editor/asset-spec.js`
> （内切正方形 336 / `circleSafeSide()`）、`lib/dashcore/panel_view.h`
> （固件/预览侧同一套算式）、`docs/RGB-PANEL-2.8C.md`（真机侧判据）。

---

## 1. 怎么跑

### 1.1 前提（三条，缺一条就会报看起来像"代码坏了"的错）

| 前提 | 为什么 | 症状（不满足时） |
|---|---|---|
| **在纯 ASCII 路径的副本里跑** | 中文路径会让工具链/`ld` 写 map 失败 | 构建报路径相关错误（与代码无关） |
| `C:\206dash-scratch\zigbin` 挂进 `PATH` | 那是一组 `gcc`/`g++` → zig 的**转发桩** | `'gcc' is not recognized as an internal or external command` |
| `PYTHONPATH` / `PLATFORMIO_CORE_DIR` | 本机的 PlatformIO Core 与工作区依赖不在系统 python 里 | `No module named platformio` |

### 1.2 命令（复制即用）

```powershell
# ① 同步到纯 ASCII 副本（退出码 1/3 = 成功，不是错误）
$repo='C:\Users\张九思\206Dash\Neru-s-206-dashboard'
$copy='C:\206dash-scratch\Neru-pcp'
robocopy $repo $copy /E /XD .git .pio /NJH /NJS /NP /NFL /NDL

# ② 三个环境变量（只对当前这个 shell 有效）
$env:PYTHONPATH='C:\Users\张九思\206Dash\.pio-pylibs'
$env:PLATFORMIO_CORE_DIR='C:\Users\Public\206dash\.pio-core'
$env:PATH='C:\206dash-scratch\zigbin;' + $env:PATH

# ③ 编 + 跑（program.exe 自己不会退出：跑够了 Ctrl+C，帧与退出方式无关）
cd $copy
python -m platformio run -e pcpreview -t exec

# ④ 看：浏览器打开两个页面里的任意一个（都是相对 preview/ 的静态页）
#    老页面（纯帧播放）              preview/preview.html
#    2.8C 档（带真机圆边标注）        preview/preview-28c.html
```

只编译不跑（只要"编得过"这个结论时）：

```powershell
python -m platformio run -e pcpreview      # SUCCESS 即可
```

### 1.3 视频之外的两种"记录"

* **帧**：`preview/frames/l_0000.bmp` … `l_0149.bmp`（与 `r_*`）—— 480×480 的
  24bpp BMP，每 200 ms 一对，**固定 150 对 = 30 秒**，跑第二轮会覆盖同一批文件名
  （`dash_display_poll()` 里 `frame_no >= 150` 就返回）。
* **终端日志**：`BEEP pattern=…`、`inject: …`、`alert: …`、
  `preview: panel 2.8C: 480x480 px = 70.13 mm active dia | …`。
  ★ 预览/测试输出**一律纯 ASCII**：README 那条纪律 —— 中文在 GBK 控制台上会抛
  `UnicodeEncodeError`，把统计打乱。

---

## 2. 「2.8C（最终板）」档是什么

**它是 `env:pcpreview` 的默认行为**，不是另一个 env、也不是另一套几何：

| 项 | 值 | 说明 |
|---|---|---|
| 画布 | 480×480 | `THEME_DISPLAY_RES`（默认 480，= 2.8C 的像素矩阵） |
| 可视区 | 内切圆，**Ø70.13 mm** | 真机只有这个圆里的内容看得见 |
| 1 像素 | ≈ **0.1461 mm** | 70.13 / 480 |
| 内切正方形 | **336 × 336** | 矩形内容想整块落在圆内就不能超过它 |
| 落盘帧 | 480×480 BMP，**圆外那层标注烧在图里** | 圆内 = 真机画面（逐像素未改） |
| 遮罩开关 | 键 `V` / 控制文件 `mask=0` | 关掉后落盘的是裸画布 |

几何只有**一份**：`lib/dashcore/panel_view.h`（纯头、不依赖 LVGL/Arduino，
所以 native 用例能直接测）。它做三件事：

1. **判断**某个像素在真机上可不可见（`panelPixelVisible`）；
2. 给遮罩的**着色规则**（圆内不动、圆外参考圈带半边暗、更外的四角打点）；
3. 把规则落到缓冲上（`panelApplyOverlayRgb565`）。

着色规则的三段（`panelShadeAt`）：

```
圆内              → 256（= 原样，一个字节都不碰）
圆外 3 px 的圈带  → 128（半边暗：把"可视圆"这条边界描出来）
更外的四角        → 32 / 64 交替（斑点：一眼看出这是标注层，不是画面内容）
```

★ 两个刻意的选择：

* **圈带画在圆外**（不是压在圆上）—— 压在圆上会改掉真机可见的那一圈像素，
  而那正是"素材有没有被切掉"要看的地方。
* **不涂黑**——涂黑会让人分不清"这是遮罩"还是"固件真的画黑了"。

★ 预览**不动 LVGL 的帧缓冲**：遮罩是在 `write_bmp_panel()` 里对"落盘的那一份"
现算的，所以帧里的圆内像素就是真机像素，而按 `V` 关掉遮罩也不会留下"已经压暗过"
的痕迹。

---

## 3. 每个键 / 每个注入项是什么

两条路（**都只存在于 `env:pcpreview`**：三个固件目标里连 `src/preview_input.cpp`
都不编）：

### 3.1 键盘（预览窗口一开就能用，改完当帧生效）

| 键 | 作用 | 说明 |
|---|---|---|
| `←` / `→` | 左 / 右转向灯 | **开关**（按一下开、再按一下关）。屏上六格灯的相应格会**闪**（半周期 375 ms） |
| `空格` | 双闪 | 左右箭头 + 双闪格一起闪（与数据层"两位同时置位"同一口径） |
| `L` | 近光 | 稳态灯：不闪 |
| `P` | 仪表盘灯 | 稳态灯：不闪。**红区告警借这一格闪**（超速/红区没有专属槽位） |
| `D` | 门 | 是"**动过**"（`door_activity`），不是"门开着"（那个字段还没解出来） |
| `O` | 超速 | 车速 = **130**（> 告警阈值 120）⇒ 触发超速告警 |
| `R` | 红区 | 转速 = **6000**（> 阈值 5800）⇒ 触发红区告警 |
| `M` | 静音 | 只掐蜂鸣器；**屏上的指示灯照旧报**（"静音"≠"假装没事"） |
| `V` | 圆屏遮罩开关 | 只影响**落盘的那张图**（LVGL 缓冲不动） |
| `X` / `Esc` | 全部复位 | 回到假数据（并把遮罩复位成"开"） |

### 3.2 控制文件 `preview/inject.txt`（每帧重读 ⇒ 保存即生效）

```ini
# 复制 preview/inject.example.txt 改名即可
left=1          # 左转向
right=0         # 右转向
hazard=1        # 双闪
low_beam=1      # 近光
position=0      # 仪表盘灯
door=1          # 门"动过"
speed=140       # 车速（km/h，浮点也行）
rpm=6000        # 转速
mute=1          # 静音
mask=0          # 关掉圆屏遮罩（等价于按 V）
clear=1         # 全部复位（见下面第 ① 条）
```

三条必须记住的口径：

1. **值是绝对值**：删掉一行**不会**把那一格退回假数据 —— 要退就写 `clear=1`。
2. **只读前 1024 字节**，超了会打一行 `WARNING … longer than 1024 bytes`。
   别把带长注释的例子文件直接当控制文件用（实测踩过：注释占满缓冲 ⇒
   真正那几行一个字都没读进来，而**日志上什么异常都没有**）。
3. 注入**只覆写 main 的快照**：不造 VAN 帧、不碰 `kSpeedScale`、
   不改 `data_service` 的优先级（纯函数在 `lib/dashcore/preview_input.h`，
   native 用例逐条钉住）。

### 3.3 键盘与文件同时用时的优先关系

文件里**写了**的那一项，每帧都会盖回文件里的值（文件是每帧重新施加的"绝对值"）；
文件里**没写**的那一项，只受键盘控制。`mask=` 也照这条走（判据是那个 `_set` 旗标，
不是"值等于默认值"）—— 少了它，键盘按 `V` 关掉遮罩后会被下一帧的文件重读按默认值盖回去。

### 3.4 蜂鸣器

预览档挂的是 `BuzzerHost`（`lib/dashcore/buzzer.h`）：

* 默认**打印一行** —— `BEEP pattern=urgent ms=120`（纯 ASCII，格式固定，便于 grep）；
* 想真出声：把 `-DBUZZER_HOST_SOUND=1` 加进这次构建（`platformio` 没有
  `--project-option` 这个开关，实测报 `No such option` ⇒ 用环境变量）：

  ```powershell
  $env:PLATFORMIO_BUILD_FLAGS='-DBUZZER_HOST_SOUND=1'   # Windows 上会调 MessageBeep
  python -m platformio run -e pcpreview -t exec
  ```

  ★ 这是**临时**的：下次构建前清掉它（`Remove-Item Env:PLATFORMIO_BUILD_FLAGS`），
  否则"带声音"会一直粘在那份构建缓存上（`platformio.ini` 里**没有**这一条）。
* **什么时候该响**的判据完全不在这一层（在 `lib/dashcore/alerts.*`，与真机同一份）。

### 3.5 「告警那一拍」为什么在预览里也看得见（一个采样问题）

真机上告警的"屏上闪"跟着蜂鸣器走：`Alerts::beeping()` 只持续 `beep_ms`（120 ms）。
真机 60 fps 下每一拍都看得见；而**预览是每 200 ms 落一帧** ⇒ 有一半的落帧时刻
那一拍已经过去了，于是"按了 `O` 却没看见闪"会时不时出现。

修法只动**预览这一侧**（`src/main.cpp` 的 pcpreview 段）：脉冲开始时记住当时的帧号，
**一直保持到显示侧真的落了新的一帧**（`dash_display_preview_frames()`）为止，
另有 1500 ms 的兜底上限。判据/去抖/蜂鸣器一个字都没改。

---

## 4. ★ 预览看不到什么

**这一节是"别被预览误导"的清单。** 下面这些**不是"还没做"，而是"预览里结构上不可能出现"**：

| 看不到的 | 为什么 |
|---|---|
| **撕裂（tearing）** | 预览的 flush 是**即时**的：`preview_flush_cb()` 把区域像素 `memcpy` 进帧缓冲就 `lv_display_flush_ready()` 了。没有双 framebuffer、没有 vsync、没有扫描出图 —— 一块缓冲永远不可能"被读的同时被写"。 |
| **图像残留（ghosting）** | 同上：帧缓冲是普通内存，一帧覆盖一帧，没有面板的像素保持特性。 |
| **横纹 / 抖动** | 那是**带宽争用**的指纹（PSRAM/DMA 与面板抢带宽，见 `docs/RGB-PANEL-2.8C.md` 第 11/12 节）。宿主机上没有 PSRAM、没有 DMA、没有那 18 MHz 的 pclk。 |
| **真实帧率与时序** | 预览固定 200 ms 一帧、固定 150 对；`dash_display_poll()` 里的节流与真机的 `rgb: frames=` 毫无关系。 |
| **面板初始化 / 时序参数** | 复位序列、`pclk_hz`、porch、极性那些只在真机上有效（`src/dash_display_rgb.cpp`）。预览一行都不碰。 |
| **字体墨迹（字宽）** | 预览用的字体与设备端不同：**位置与字号可信，字宽只是近似**。要卡像素请用 `tools/theme-editor/check-preview-frame.js` 读落下来的 BMP。 |
| **圆屏的物理边框内缩** | 预览按**内切圆**（Ø70.13 = 有效区）算。真机面板还有自己的边框/黑边，实际可视角比它略小。 |
| **触摸 / 音频 / 真蜂鸣器** | 2.8C 是非触控版；蜂鸣器在预览里是打印（可选系统提示音），真机那一路（`§8 L14` 由哪块板发声）**仍未裁决**。 |

**所以"预览看着没问题"≠"真机没问题"**：预览能证明的是"**该画的东西画出来了、
位置/配色/亮灭对不对、有没有伸到圆外**"；帧率、撕裂、残留、横纹这四类
只能上真机看，判据与打点都在 `docs/RGB-PANEL-2.8C.md` 与 `ACCEPTANCE.md`。

---

## 5. 预览与真机的对应关系（哪一层是同一份代码）

```
                 ┌──────────────────────── 同一份 ────────────────────────┐
真机 (esp32s3-rgb)│  src/dash_ui.cpp · boot_anim.cpp · theme_load.cpp       │ 预览 (pcpreview)
  ↓              │  lib/themetool/ui_theme.*（主题/几何/字号/配色）           │   ↓
  ↓              │  lib/dashcore/*（数据层 / 告警 / 六格灯映射）              │   ↓
  └──────────────┤  lib/dashcore/panel_view.h（圆屏可视区几何）              ├───┘
                 └────────────────────────────────────────────────────────┘
       ↓                                                        ↓
  esp_lcd RGB 面板驱动                              write_bmp() + 遮罩标注
  （撕裂/残留/带宽那一切都在这一层）                  （每 200ms 一对 BMP）
```

* **同一份**：UI、主题、数据层、告警判据、六格灯映射、圆屏几何。
* **只有预览有**：`src/preview_input.cpp`（键盘 + 控制文件）、
  落帧与遮罩标注（`write_bmp_panel()`）、`BuzzerHost` 的打印出口。
* **只有真机有**：`src/dash_display_rgb.cpp` 的整条面板链路
  （复位序列、双 framebuffer、bounce buffer、vsync 回调、带宽打点）。

### 5.1 画布 px ↔ 面板 mm

| 画布（px） | 面板（mm） | 备注 |
|---|---|---|
| 0 / 479 | 屏的最左 / 最右 | 但这两个点**只在 y=240 附近可见**（圆） |
| 240 | 圆心的水平位置 | 几何基准的圆心就是 (240, 240) |
| 240（半径） | 35.065 mm | 圆的半径 = 有效区半径 |
| 336 | 49.1 mm | 内切正方形边长（矩形内容的"绝对安全"上限） |
| 480 | 70.13 mm | 整块像素矩阵的对角线方向**看不到** |

### 5.2 四角为什么不可见

屏是 **480×480 的方形像素矩阵**，外面套了一个 **Ø70.13 的圆窗口**。
像素矩阵的对角线一半是 `480·√2/2 ≈ 339.4 px`，而圆的半径只有 `240 px` ——
所以**四个角（以及沿对角线的各约 9 mm）在物理上被圆边挡住**，不存在"画上去了但看不见"
以外的问题：它们真的不出现在人眼里。

对素材的约束因此是**一条几何**：矩形内容的对角线一半 ≤ 240 px（≈ 内切正方形 336）。
`tools/theme-editor` 的上传检查（`asset-spec.js` 的 ⑤ 圆形可视区）会对每张图算这条，
超了就给出人话警告并建议缩到 336 以内。

---

## 6. 常用核查动作（本仓库现成工具）

```powershell
# 逐像素核对某一帧的六格灯（能指名道姓说哪一格没亮）
node tools/theme-editor/check-preview-frame.js preview/frames/l_0030.bmp idle yes left yes `
     "lamps=L,Low" "alert=none"

# 两个编辑器的内联脚本语法（改 HTML 之后必跑）
node tools/theme-editor/syntax-check-pages.js
```

`lamps=` 用 `L,R,Haz,Low,Pos,D` 逗号分隔；`alert=none` 时**没点名的格子必须灭**，
有告警时那些格会闪、脚本就不断言它们。

---

## 7. 还没做 / 拿不准的（别把它们当结论用）

* **真机上"告警闪与蜂鸣器同一拍"没有对着眼睛核过** —— 那一拍在真机
  (`#else` 支) 用的是 `Alerts::beeping()`，逻辑上一致，但**没有实拍验证**。
* **圆屏的物理边框内缩量没有实测** —— 本文与预览都用"内切圆"这一条理想口径；
  真机可视角会比 Ø70.13 略小，具体差多少要等对屏实测。
* **240 档（历史 DualEye）没有独立量过有效区直径** —— 所以
  `tools/theme-editor/asset-spec.js` 的 `ROUND_PANEL.res240.activeAreaMm10`
  故意留空（不编一个数）。它的**像素**口径（内切正方形 168）仍然有效。
* **预览不能替代真机回归** —— 见第 4 节。
