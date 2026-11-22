# 高德"画中画"地图（阶段 B2）方案与施工单

车主 2026-11-22：*"能看见路线，但是太短了，效果也很差，能不能做出来高德地图画中画那种效果"*，
并确认**地图要跟着车头旋转（heading-up）**。

现状（已提交）：左屏只画**矢量路线**（外描边 26px + 亮线 16px + 终点圆点），
范围 600 m / 24 点。黑底 + 一条线，注定不像地图 ⇒ 真正的地图必须是**图片**。

---

## 1. 总体链路

```
高德 SDK MapView（App 内离屏） --截图--> Bitmap 240×240
        --JPEG(质量 60, 垂直翻转?)--> ~15~25 KB
        --BLE 分片(0xA9)--> 从板
        从板：分片重组到 PSRAM --> LVGL(tjpgd) 解码 --> 背景图
        叠加：车标（底部中央）+ 高亮路线（车头朝上 = 截图时就按 bearing 旋转）
```

★ **heading-up 在哪做**：在 **App 端**。高德 MapView 支持
`aMap.setMapStatus(MapStatusUpdateFactory.newCameraPosition(new CameraPosition(pos, zoom, bearing, tilt)))`
⇒ 截图前把 bearing 设成当前车头角、位置设成当前坐标、zoom 约 17。
板子只负责"把这张图当背景贴上去"，**不做旋转**（省算力、也省得被 PSRAM 带宽拖累）。

## 2. 图片分片协议（magic 0xA9）

```
+0  magic 0xA9
+1  ver   1
+2  frame_id   (每张新图 +1；同一个 frame_id 的分片属于同一张图)
+3  idx        (0..chunk_total-1)
+4  total      (总片数，<= 200)
+5..6  len     (本片字节数 u16 LE，<= 4096)
+7..   data[len]
+...   crc16   (覆盖整个分片，复用 nav_crc16)
```

- 重组缓冲：`img[32*1024]`（PSRAM 优先），最多 200 片
- 收满 `total` 片 → 交给解码；**丢掉不完整的上一帧**（frame_id 变了就重来）
- 丢片处理：v1 先不做重传；App 每 0.5~1 s 发一张新图，下一张自然覆盖
- 与现有 0xA7（快照）/0xA8（路线）**共用同一条字节流**，`nav_source` 里按魔数分派

## 3. 固件侧步骤（先做，能独立验证）

1. `include/lv_conf.h`：`#define LV_USE_TJPGD 1`（现在没开；`lvgl/src/libs/tjpgd` 已在 libdeps 里）
2. `lib/dashcore/nav_img.h/.cpp`：分片编解码 + 重组（纯逻辑，**native 可测**：坏 CRC/丢片/乱序/跨帧）
3. `lib/dashcore/nav_source.h`：`0xA9` 分派 → 重组器
4. `src/nav_ui.cpp`：
   - 新增一个 `lv_image` 对象铺在路线下面（240×240 居中，或铺满 480 靠缩放）
   - 解码：`lv_image_decoder_open()` 得到 RGB565 像素缓冲 → **拷贝到 PSRAM 自有缓冲**（LVGL 的 cache
     按 src 指针缓存，指针不变就不会重解 ⇒ 每帧必须拷到自己的 buffer 并让 dsc 指向它）
   - 验证判据：串口打 `img: ok %u bytes %ux%u decode=%ums`
5. 先用**一张固定测试 JPEG**（可以是 App 生成的黑底彩色网格）跑通"传→重组→解码→上屏"

## 4. App 侧步骤

1. `MapView` 离屏：`new AMap`（`mapView.onCreate(null)`，尺寸固定 240×240，不加入布局）
2. 导航中每次更新（0.5~1 s）：
   - `cameraPosition = new CameraPosition(当前坐标, 17f, bearingDeg, 0f)`
   - `aMap.moveCamera(MapStatusUpdateFactory.newCameraPosition(cameraPosition))`
   - 等一帧：`aMap.getMapScreenShot(callback)`（SDK 异步回调，要注意节流，别排队）
   - `bitmap.compress(JPEG, 60, out)` → `NavLink.sendImage(jpegBytes)`
3. `NavLink.sendImage`：按 ~180 B 分片（MTU 185 已谈好），每片一个 `0xA9` 帧

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| `getMapScreenShot` 在无显示时返回黑图/慢 | 先做"固定测试图"验证链路；黑图就换成 `Bitmap` 手动绘制（矢量路网 + 路名）作为退路 |
| BLE 带宽不够（25 KB @1 Hz ≈ 200 kbit/s） | 降频到 0.5 Hz、JPEG 质量 45、尺寸降到 200×200；必要时只在转向变化时发图 |
| 解码时 PSRAM 峰值 | 240×240 RGB565 = 115 KB；PSRAM 余量 7.2 MB，够；解码缓冲用完即释放 |
| 与横纹复发 | 已验证的抗横纹配置（停广播/60~90ms 间隔/-12dBm/弹跳40）必须保留；图片流会增加 BLE 负载，需重测横纹 |

## 6. 验收判据

1. 稳定刷出测试图（串口 `img: ok`，屏上是那张图）
2. 换成真实地图截图后：**地图随车头旋转**、车标在底部中央、路线高亮叠加
3. 横纹不复发（`swap_wait_max` 保持 < 10 ms、车主目视确认）

---

## 7. 车主已定（2026-11-22）

- **地图尺寸与位置：居中 360×360**（240×240 的截图放大 1.5 倍；不铺满 480，
  因为放大 2 倍太糊，而 360 覆盖面积与清晰度平衡最好）
- **heading-up 跟随车头旋转**（在 App 端截图前设 bearing）

⇒ 固件布局：地图 `lv_image` 360×360 居中（x=(480-360)/2=60, y=60），
路线/车标画在它上面（同一坐标系，地图是 1.5x 放大 ⇒ 路线的 px/米 比例也要按
"地图上 1 米 = ? px"反推，或用固定 1.2 px/米 并对齐视觉，先在测试图上看效果再定）。
