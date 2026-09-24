# 配色 + 图片合成一个文件(`asset-package.js`)

> 车主的需求(原话):
> "theme 和 image 这两个 bin 我觉得最好导出导入的文件可以合并,这样每次只需要拷贝一个文件就完事了。"

**现在只需要一个文件、一条命令。** 原来的"两份文件、两条 `write_flash`"办法**仍然有效,一个字都没改**
(见下面「兼容性」一节 —— 那份文档不用删,两种办法并存)。

| | 老办法(仍然有效) | 新办法(本页) |
|---|---|---|
| 文件 | `theme.json` + `image.bin` | **`assets.bin`**(一个) |
| 刷写 | 两条 `write_flash`(0x210000 + 0x254000) | **一条** `write_flash 0x210000 assets.bin` |
| 固件 / 分区表 | 不用改 | **同样不用改** |

---

## 1. 为什么一个文件能成立:两段是**连续**的

固件里这是**两块独立分区**(`partitions-s3.csv` 第 49~51 行;经典板 `partitions.csv` 第 59~61 行,
两处**偏移刻意相同**):

```
theme,    data, 0x40,    0x210000, 0x4000,      ← 配色
spiffs,   data, spiffs,  0x214000, 0x40000,     ← 本项目不用文件系统
image,    data, 0x41,    0x254000, 0x800000,    ← 图片(经典板 0x100000)
```

```
0x210000 + 0x4000  = 0x214000     theme  与 spiffs 首尾相接
0x214000 + 0x40000 = 0x254000     spiffs 与 image  首尾相接
```

⇒ 从 theme 的偏移开始,三块分区在 flash 上是**一条连续的带子**。所以"合并"不是把两份数据塞进一个壳,
而是**把这一段 flash 原样拷成一个文件**:文件本身就是可以烧的镜像,

```
python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x210000 assets.bin
```

**一条命令刷完两块分区。** esptool 只写文件覆盖到的那些字节,文件里没有的地址一个都不碰 ——
所以它**不会**动 `app0` / `app1` / `nvs`(固件与 OTA 数据都安全)。

### 为什么容器里**不能**加自定义文件头

加了头,文件开头的字节就不是"要写进 0x210000 的第一个字节",`write_flash` 会直接把头当成主题内容刷进去
—— 那正是这个工具要避免的事。所以自描述能力由**分区表 + 段内容自身**提供(第 3 节)。

### 偏移一个都不写死

容器里所有位置都从**分区表**读(哪张表由 `--table` 或 `--target` 决定)。
`partitions-s3.csv` 第 8~13 行的注释就是这条约定的出处:

> `NOTE: THEME / IMAGE OFFSETS ARE DELIBERATELY IDENTICAL TO partitions.csv`
> `0x210000 (theme) and 0x254000 (image) are the numbers the web editor prints in its esptool command.`
> `Keeping them equal means ONE set of flash commands works on both boards`

也就是说"两块板共用一套刷写命令"是**分区表早就定下的目的**;这个工具只是把它用到了极致:
连"两条命令"都不需要了。

---

## 2. 容器布局

基准 = **theme 分区的偏移**(所以文件里的 `+0` 就是 flash 的 `0x210000`):

```
+0x00000   theme.json 的内容,不足 0x4000(16384)字节处补 0x00
+0x04000   spiffs 那一整块 0x40000(262144)字节,全 0x00   ← 位置与长度都从分区表算出来
+0x44000   image.bin 的内容(长度 = 文件里 image 段的实际长度)
```

### 为什么补 `0x00` 而不是 `0xFF`

`0xFF` 是**擦除后**的空白(直接刷一份 `theme.json` 时,分区剩下的部分就是这样)。
但容器里不行,两个理由:

1. **布局**:image 段必须正好落在 image 分区的偏移上 ⇒ theme 与 spiffs 两段必须**填满**,
   不能"写过就算";
2. **设备端两种终止符都认**:`src/theme_load.cpp` 第 41~42 行的读取循环遇到 `'\0'` **或** `0xFF` 都停。
   补 `0` 走的是第一条,效果与"直接刷 theme.json"留下的 `0xFF` 等价。

### spiffs 段为什么可以整块填 0(依据)

- `partitions.csv` 第 12~13 行的原话:`This project uses no filesystem at all.`
  `spiffs is kept only so the partition table has a spare data slot`
  ⇒ 那块分区里没有任何"必须有内容"的数据;
- 固件没有挂载任何文件系统 ⇒ 里面是不是合法的文件系统镜像无所谓;
- 它的**偏移与大小仍然从分区表读**,换表就自动跟着变(代码里没有 `0x40000` 这个数)。

⚠ **边界**:如果将来真有人往那块分区放了文件系统,刷这个容器会把它**清成 0**。
那时请改用老办法分别刷 theme 与 image 两块分区。

### theme 段的长度上限是 **4095 字节**,不是 16384

两个数管两件不同的事,别混:

| 数 | 出处 | 管什么 |
|---|---|---|
| `0x4000` = 16384 | 分区表 theme 分区大小 | **布局**(image 段的起点由它决定) |
| `4095` | `lib/themetool/theme_store.h` 第 46 行 `THEME_MAX_BYTES (4 * 1024)`,由 `src/theme_load.cpp` 第 32~33 行 `static char buf[THEME_MAX_BYTES]` + `sizeof(buf) - 1` 决定 | **固件能读进来多少** |

超过 4095 字节的主题在设备上会被**截断**,截断的 JSON 解析必然失败 ⇒ 表现是
"刷进去了,但屏上还是默认配色"而且**不报错**。所以 `pack` 直接拒绝,并把这两个数都写进报错里。
(正常一份主题 JSON 约 1.2~2 KB —— 车主那份是 1388 字节。)

---

## 3. `unpack` 怎么知道每段多长(自描述)

只凭**分区表 + 容器本身**,不靠任何额外记录,而且**两者不一致时报错、不猜**:

| 段 | 长度怎么来的 | 不成立时 |
|---|---|---|
| theme | 末尾的 0 是填充 ⇒ 长度 = **最后一个非 0 字节 + 1**;再要求这段**通过 JSON 合法性**(`theme-json.js`,与网页/固件同一套规则)、末尾没有空白 | 填充里混了非 0 / JSON 不合法 / 根不是对象 ⇒ 报错并指出第几个字节 |
| image | **读 image 自己的清单**:头 12 字节的 `data_bytes`(`lib/themetool/image_blob.h` 第 191~195 行的 `ImageBlobHeader`,魔数 0x44363032 / 版本 1),总长 = 包头 + `data_bytes`(包头大小按同一份表算:12 + 32×44 = 1420,见 `image-blob-build.js` 的 `HEADER_SIZE`) | 魔数/版本/数量不对,或容器实际长度与清单**不一致** ⇒ 报错 |

★ image 段的长度**绝不**用"文件剩下的字节数"当长度 —— 那正是会悄悄出错的地方
(多拷了几个字节、少拷了几个字节,都看不出来)。

---

## 4. 两条命令

### 打包(两份 → 一个文件)

```
node tools/theme-editor/asset-package.js pack --theme theme.json --image image.bin --out assets.bin
```

真实输出(车主那两份文件):

```
已生成 assets.bin  (2123148 字节)
  分区表 C:\Users\张九思\206Dash\Neru-s-206-dashboard\partitions-s3.csv
  容器基准 = theme 分区偏移 0x210000(文件里 +0 就是这里)
  +0x0      theme.json(不足 16384 = 0x4000 字节处补 0)
  +0x40000       …中间 262144 字节(spiffs,全 0 填充)
  +0x44000   image.bin(最长 8388608 字节 = 0x800000)
  theme 段 : 1388 字节内容 + 14996 字节 0 填充
  image 段 : 1844620 字节(包头 1420 + 数据 1843200,共 9 张图)
  刷写(端口换成自己的): python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x210000 assets.bin
```

### 拆开(一个文件 → 两份原样)

```
node tools/theme-editor/asset-package.js unpack --in assets.bin --theme-out theme.json --image-out image.bin
```

真实输出:

```
已拆开 assets.bin  (2123148 字节)
  theme.json  (1388 字节)
  image.bin  (1844620 字节,包头 1420 + 数据 1843200,共 9 张图)
  分区表 …\partitions-s3.csv(容器基准 0x210000)
```

### 参数

| 参数 | 说明 |
|---|---|
| `--table <csv>` | 分区表。只给文件名时按仓库根找(`partitions.csv` / `partitions-s3.csv`),也可以给完整路径 |
| `--target <id>` | 不给 `--table` 时按目标板取分区表与刷写命令的 `--chip`:`classic` / `s3` / `s3_240`(默认 `s3`,与编辑器一致) |
| `--port <COMx>` | 只影响**印出来的刷写命令**,默认 `COM6` —— **换成自己那块板的口** |
| `--help` | 用法 |

退出码:`0` 成功 / `2` 用法错(少参数、不认识的子命令)/ `1` 输入不对(报错文字以 `失败: ` 开头)。

---

## 5. 刷写

```
python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x210000 assets.bin
```

- **端口换成自己的**(`COM6` 只是车主这台机器上那块 S3 的口);`--chip` 跟着目标板走,`--target` 会印对。
- 偏移 **0x210000** = theme 分区的偏移。**只有这一条命令**,不再需要第二条 `0x254000`。
- ⚠ 不要改成 `0x254000` —— 那是 image 分区的偏移,容器里的 image 段在 `+0x44000`,刷错位置等于
  把 spiffs 那块内容写到图片分区里去。

---

## 6. 兼容性:老办法**仍然有效**

**原来的"两份文件、两条 `write_flash`"一个字都没改,继续能用**:

```
python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x210000 theme.json
python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x254000 image.bin
```

- 这个工具**只读** `theme.json` / `image.bin`,不产出它们、也不改它们 ⇒ 两条老命令的输入完全不受影响;
- 两个偏移(`0x210000` / `0x254000`)仍然与分区表一致(有单测盯着);
- 设备端读的还是那两块分区、同样的位置(`src/theme_load.cpp` 读 theme 分区;
  `lib/themetool/image_blob.cpp` mmap image 分区)⇒ **固件与分区表一个字都不用改**。

---

## 7. 已知边界 / 什么时候还用老办法

| 情况 | 建议 |
|---|---|
| **只改配色**(图片一个字没动) | 老办法更快:直接刷 1.4KB 的 `theme.json` 就行,不必重写整份容器(容器里有 262KB 的 spiffs 填充 + 整份 image)。用容器**也能刷**,只是 921600 波特率下要多花几十秒 |
| 只想改图片 | 同理,老办法只刷 `image.bin` 那一块 |
| 配色与图片**一起换**(最常见) | **用容器**:拷一个文件、刷一条命令 |
| 将来往 spiffs 放了文件系统 | **必须**用老办法(容器会把那块清成 0,见第 2 节) |
| 主题 JSON 末尾有空格/换行 | `pack` 会拒绝 ⇒ 去掉末尾空白再打包。理由:那些空白在设备端无所谓,但会让"导出→导入"不再逐字节相同,而往返一致是这个工具的核心判据 |
| 换了分区表(移动了 theme/image 偏移) | 不用改代码:`--table` 指过去就行。但**必须**仍然连续、`theme` 仍然恰好 0x4000,否则直接报错(不猜) |
| 经典板(4MB) | image 分区只有 1MB ⇒ 同一套命令、同样的偏移,只有"能放多大"不同。`--target classic` 会印正确的 `--chip esp32` |

---

## 8. 自测

```
node tools/theme-editor/test-asset-package.js
```

纯 node、无依赖,全部用临时目录里自造的输入,不碰仓库里的任何文件。
覆盖:布局算术、分区表解析(坏行/重复/不连续)、theme 长度与 JSON 校验、image 包头长度、
往返逐字节一致、命令行真跑(退出码 0/2/1)、以及"宁可报错不猜"的那一批负例。

★ 这个文件**不在** `syntax-check-pages.js` 里(那份只管 HTML 里的内联脚本,没被改过)。

### 验收记录(车主那两份真文件,只读)

| 文件 | 大小 | sha256(打包前) | sha256(打包 → 拆开后) |
|---|---|---|---|
| `theme.json` | 1,388 B | `3AC5F92EE198CC6EC57F6BD9B1BF8682DC7821B389147C3210F598ABB684C0C8` | **相同** |
| `image.bin` | 1,844,620 B | `6FBE3B1A948CFFCC0A36072AF0D92101D6D4518A60AEB2AEEEF72C54B4F16510` | **相同** |

容器:`assets.bin` = 2,123,148 字节 = `0x44000`(278,528)+ 1,844,620;
容器 `+0` 的 1,388 字节 == 原 `theme.json`;`+0x44000` 的 1,844,620 字节 == 原 `image.bin`;
`+0x4000`~`+0x43FFF` 全 0。

复现(在任意 scratch 目录里,先把真文件**拷**过去,别在原地写):

```
node tools/theme-editor/asset-package.js pack   --theme theme.json --image image.bin --out assets.bin
node tools/theme-editor/asset-package.js unpack --in assets.bin --theme-out theme.copy.json --image-out image.copy.bin
node -e "const c=require('crypto'),f=require('fs'),h=p=>c.createHash('sha256').update(f.readFileSync(p)).digest('hex');console.log(h('theme.json')===h('theme.copy.json'), h('image.bin')===h('image.copy.bin'))"
```

### 页面 UI 还没接

**这一轮没有改任何页面**(`index.html` / `image-editor.html` 一个字节都没动 —— 它们当时正被别人改)。
所以现在这条能力**只能用命令行**:

- 编辑器页面上的"导出"仍然导出 `theme.json` 与 `image.bin` 两份文件;
- 想让页面直接给出**一个** `assets.bin`(以及那条刷写命令),需要下一轮在页面上接线
  (`index.html` 加一个"导出合并文件"的按钮 / 勾选框,调 `asset-package.js` 的 `pack()`);
- `asset-package.js` 同时给浏览器与 Node 用,但**注意**它用了 `fs` / `path` / `Buffer`
  ⇒ 页面里要么走 Node 侧(如果将来有),要么把 `pack()` 的**纯字节部分**抽出来给浏览器
  (现在的 `pack()` 直接落盘,不是纯函数)。这一条留给接线那一轮决定,本页先记在这里。
