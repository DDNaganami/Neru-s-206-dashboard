#include "image_blob.h"
#include <string.h>
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

// ============================================================
// 图片镜像的解析与加载。设计与格式见 image_blob.h。
//
// 核心安全要求:**任何坏镜像都不能让上层拿到越界指针**。
// 分区内容是外部工具写进去的,可能被写坏、被截断、或版本对不上;
// 一旦 offset/size 越界,LVGL 就会去读非法地址。
// 所以这里对每一项都做范围校验,校验不过就整份拒收。
// ============================================================

uint8_t ImageView::cfBytesPerPixel(uint8_t cf) {
  switch (cf) {
    case LV_COLOR_FORMAT_L8:         return 1;
    case LV_COLOR_FORMAT_A8:         return 1;
    case LV_COLOR_FORMAT_I8:         return 1;
    case LV_COLOR_FORMAT_RGB565:     return 2;
    case LV_COLOR_FORMAT_RGB565A8:   return 2;   // 另有 alpha 平面,这里不支持
    case LV_COLOR_FORMAT_RGB888:     return 3;
    case LV_COLOR_FORMAT_ARGB8888:   return 4;
    case LV_COLOR_FORMAT_XRGB8888:   return 4;
    default:                         return 0;   // 未知格式
  }
}

bool imageBlobParse(const uint8_t* blob, uint32_t len, ImageBlobHeader* out) {
  if (!blob || !out || len < sizeof(ImageBlobHeader)) return false;

  ImageBlobHeader h;
  memcpy(&h, blob, sizeof(h));

  if (h.magic != kImageBlobMagic) return false;
  // 版本只接受当前版本:宁可拒收也不用可能改过布局的数据
  if (h.version != kImageBlobVersion) return false;
  if (h.count > kImageMaxCount) return false;
  if (h.count == 0) return false;

  // 像素数据区从头部之后开始
  const uint32_t data_begin = (uint32_t)sizeof(ImageBlobHeader);
  if (data_begin > len) return false;
  const uint32_t data_avail = len - data_begin;
  if (h.data_bytes > data_avail) return false;

  // 逐项校验:每张图必须完整落在数据区内
  for (uint8_t i = 0; i < h.count; ++i) {
    const ImageEntry& e = h.entries[i];
    if (e.w == 0 || e.h == 0) return false;

    const uint8_t bpp = ImageView::cfBytesPerPixel(e.cf);
    if (bpp == 0) return false;                 // 未知/不支持的格式

    // 期望字节数(带行尾填充)。**必须含 RGB565A8 追加的 A8 平面** ——
    // 用 strideBytes()*h 会少算 1/3,把合法镜像拒收(实测踩过)。
    ImageView probe;
    probe.w = e.w;
    probe.h = e.h;
    probe.cf = e.cf;
    probe.stride_pad = e.stride_pad;
    const uint64_t expect = (uint64_t)probe.packedBytes();
    if (expect == 0 || expect > 0xFFFFFFFFull) return false;
    // size 必须与 w/h/cf 自洽:不让"声明 10 字节实际按 480x480 读"
    if ((uint64_t)e.size != expect) return false;

    // 必须落在数据区内
    const uint64_t end = (uint64_t)e.offset + e.size;
    if (end > h.data_bytes) return false;

    // 名字必须是合法 C 字符串(首字节不保证有终止符时按满长算)
    if (e.name[kImageNameMax - 1] != '\0') return false;
    bool has_nul = false;
    for (uint8_t k = 0; k < kImageNameMax; ++k) {
      if (e.name[k] == '\0') { has_nul = true; break; }
    }
    if (!has_nul) return false;
  }

  *out = h;
  return true;
}

bool imageBlobGet(const uint8_t* blob, uint32_t len,
                  const ImageBlobHeader& hdr, uint8_t i, ImageView* out) {
  if (!blob || !out) return false;
  if (i >= hdr.count) return false;
  if (i >= kImageMaxCount) return false;

  const ImageEntry& e = hdr.entries[i];
  const uint32_t data_begin = (uint32_t)sizeof(ImageBlobHeader);
  const uint64_t abs = (uint64_t)data_begin + e.offset;
  if (abs + e.size > len) return false;         // 双保险

  out->pixels     = blob + abs;
  out->w          = e.w;
  out->h          = e.h;
  out->cf         = e.cf;
  out->stride_pad = e.stride_pad;
  out->role       = (ImageRole)e.role;
  out->order      = e.order;
  out->name       = e.name;
  return true;
}

uint8_t imageBlobFindRole(const uint8_t* blob, uint32_t len,
                          const ImageBlobHeader& hdr, ImageRole role,
                          ImageView* out, uint8_t out_cap) {
  if (!blob || !out || out_cap == 0) return 0;

  // 先按 order 收集(简单选择排序:数量上限只有 16,不值得引入排序)
  uint8_t found[kImageMaxCount];
  uint16_t found_order[kImageMaxCount];
  uint8_t n = 0;
  for (uint8_t i = 0; i < hdr.count; ++i) {
    if ((ImageRole)hdr.entries[i].role != role) continue;
    found[n] = i;
    found_order[n] = hdr.entries[i].order;
    ++n;
    if (n >= kImageMaxCount) break;
  }
  if (n == 0) return 0;

  for (uint8_t a = 0; a + 1 < n; ++a) {
    for (uint8_t b = a + 1; b < n; ++b) {
      if (found_order[b] < found_order[a]) {
        const uint8_t ti = found[a];       found[a] = found[b];       found[b] = ti;
        const uint16_t to = found_order[a]; found_order[a] = found_order[b]; found_order[b] = to;
      }
    }
  }

  uint8_t written = 0;
  for (uint8_t k = 0; k < n && written < out_cap; ++k) {
    if (imageBlobGet(blob, len, hdr, found[k], &out[written])) ++written;
  }
  return written;
}

// ---------------- 加载（平台相关） ----------------
#if defined(ARDUINO)

#include <Arduino.h>
#include "esp_partition.h"
#include "esp_idf_version.h"   // ESP_IDF_VERSION:只为下面那处 memory 枚举的类型适配

const uint8_t* imageBlobLoad(uint32_t* blob_len) {
  if (blob_len) *blob_len = 0;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41,
      IMAGE_PARTITION_LABEL);
  if (!part) {
    dash_logf("image: 没有 image 分区,不用图片资源\n");
    return nullptr;
  }

  // 只读映射:像素直接从 flash 取,不占 DRAM。
  // LVGL 在 LV_IMAGE_SRC_VARIABLE 路径不会复制像素(见 image_blob.h),
  // 所以这里指过去的指针在整个运行期都有效。
  // ★ handle 存成 static:映射在我们这里活到重启为止,绝不能让它被回收 ——
  //   否则那块虚拟地址会被别的 mmap 复用,LVGL 读到的就是别人的数据。
  const void* mapped = nullptr;
  // ★★ 2026-09-24(换栈到 ESP-IDF 5.5 时唯一的**类型**适配,值与语义都没变):
  //   IDF 5.x 把这一对类型/常量从 `spi_flash_*` 挪到了 `esp_partition_*` 名下:
  //     · 句柄:`spi_flash_mmap_handle_t` → `esp_partition_mmap_handle_t`(都是 uint32_t)
  //     · memory 参数:`spi_flash_mmap_memory_t`/`SPI_FLASH_MMAP_DATA`
  //       → `esp_partition_mmap_memory_t`/`ESP_PARTITION_MMAP_DATA`(都是 0 = data 区)
  //   5.x 里 `spi_flash_mmap_handle_t` **已经不存在**(`esp_partition.h` 不再带它)
  //   ⇒ 直接用旧名字会 `does not name a type`。两个枚举还是**不同类型**,
  //   C++ 不允许互相隐式转换 ⇒ 名字也要跟着换。
  //   映射的还是同一块只读 data 区,行为一个字没变。
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  static esp_partition_mmap_handle_t s_handle = 0;
  const esp_partition_mmap_memory_t kMmapMemory = ESP_PARTITION_MMAP_DATA;
#else
  static spi_flash_mmap_handle_t s_handle = 0;
  const spi_flash_mmap_memory_t kMmapMemory = SPI_FLASH_MMAP_DATA;
#endif
  const esp_err_t err = esp_partition_mmap(
      part, 0, part->size, kMmapMemory, &mapped, &s_handle);
  if (err != ESP_OK) {
    dash_logf("image: mmap 失败 (%d)\n", (int)err);
    return nullptr;
  }

  ImageBlobHeader hdr;
  if (!imageBlobParse((const uint8_t*)mapped, part->size, &hdr)) {
    // 分区是空的(全 0xFF)或没刷过 —— 都属于正常情况,不算错误
    dash_logf("image: 镜像无效或未刷入,不用图片资源\n");
    return nullptr;
  }

  if (blob_len) *blob_len = part->size;
  dash_logf("image: 已加载 %u 张图 (%u 字节数据)\n",
                (unsigned)hdr.count, (unsigned)hdr.data_bytes);
  return (const uint8_t*)mapped;
}

#else  // 宿主机（pcpreview / native 测试）

#include <stdio.h>
#include <stdlib.h>

// ============================================================
// ★★ 2026-09-26：**把"静默退回"变成"喊出来"**（这一处是本单修的）
//
// 起因：车主问"是不是模拟页面导出的 bin 文件本来就有问题"，而排查时先踩到的
//   其实是**预览侧**这条降级路径：
//     · 宿主机预算默认按**经典板的 1MB**（image_blob.h 的 IMAGE_PARTITION_BYTES）；
//     · 车主真素材 **1,844,620 B** ⇒ 超预算 ⇒ 一张图都不加载；
//     · 而当时的日志只有一句 `image: … 大小不合理 (1844620)`（**没有**预算数、
//       **没有**怎么办、还在 stderr），stdout 那行只说
//       `image none: 无图片资源,背景用主题纯色`。
//   ⇒ 谁读到这两行都会以为"是素材/页面导出的文件不对"，而实际上只要给预览
//     加上 8MB 口径就一切正常（见 docs/PREVIEW.md）。这正是"静默退回"的害处。
//
// 所以判定挪成**纯函数**（宿主机可测，见 test_image_blob.cpp），并由调用点
// 把整句话打到**主日志流**（dash_logf = stdout）上：数字齐全 + 出路齐全。
// ============================================================
bool imageBlobSizeVerdict(unsigned long bytes, unsigned long budget,
                          char* msg, unsigned msg_cap) {
  if (msg && msg_cap) msg[0] = '\0';
  if (bytes > 0 && bytes <= budget) return true;
  if (!msg || !msg_cap) return false;

  if (bytes == 0) {
    snprintf(msg, msg_cap, "image: 文件是空的(0 字节) ⇒ 一张都不加载");
    return false;
  }
  // 一句人话，三个数 + 两条出路（数字一律给字节与 KB 两种口径，便于与页面/分区表对照）
  snprintf(msg, msg_cap,
           "image: ** 图片预算不够，一张都不加载 ** "
           "文件 %lu 字节(%lu KB) > 预算 %lu 字节(%lu KB) ⇒ "
           "表情会退回程序化形状（**不是素材/页面导出的问题**）。"
           "出路：编译时加 -DIMAGE_PARTITION_BYTES=(8u*1024u*1024u) "
           "（S3 那块板/双 2.8C 的 image 分区就是 8MB），"
           "或改用按目标板取预算的 env。见 docs/PREVIEW.md。",
           bytes, (bytes + 1023ul) / 1024ul, budget, (budget + 1023ul) / 1024ul);
  return false;
}

const uint8_t* imageBlobLoad(uint32_t* blob_len) {
  if (blob_len) *blob_len = 0;

  const char* path = getenv("IMAGE_BLOB");
  if (!path || !*path) return nullptr;    // 没设就是"不用图片",正常

  FILE* f = fopen(path, "rb");
  if (!f) {
    // ★ 这条也**喊出来**（2026-09-26）：它与"预算不够"是**两件不同的事** ——
    //   加 -DIMAGE_PARTITION_BYTES 对这种一点用都没有，所以必须说清是哪一种，
    //   否则读日志的人会去改一个改了也没用的地方。
    //   ★ 诚实记一笔：本单排查时我一度把这条归因成"路径里有中文 ⇒ fopen 失败"
    //     （因为 IMAGE_BLOB 指到 `C:\Users\张九思\…` 时确实打不开）。**那个归因是错的** ——
    //     实测中文路径能正常打开（`theme:` 那行一直是同一条中文路径且成功），
    //     当初打不开是**我那个 PowerShell 夹具脚本自身**把中文默认值按 GBK 解坏了
    //     （无 BOM 的 .ps1 在 Windows PowerShell 5.1 下按 ANSI 读）。脚本已改成全 ASCII。
    dash_logf("image: ** 打不开 %s ** ⇒ 一张都不加载。"
              "通常是路径写错、或文件被移走/删掉了"
              "（这种情况加 -DIMAGE_PARTITION_BYTES 没用）。\n", path);
    fprintf(stderr, "image: 打不开 %s\n", path);
    return nullptr;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);

  char verdict[512];
  if (!imageBlobSizeVerdict((unsigned long)(n > 0 ? n : 0),
                            (unsigned long)IMAGE_BLOB_MAX_BYTES,
                            verdict, (unsigned)sizeof(verdict))) {
    fclose(f);
    dash_logf("%s\n", verdict);       // ← 主日志流（stdout）：与其它 image: 行同一条流
    fprintf(stderr, "%s\n", verdict); // ← 也留一份在 stderr：只看 stderr 的人也能看到
    return nullptr;
  }

  static uint8_t buf[IMAGE_BLOB_MAX_BYTES];
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) return nullptr;

  if (blob_len) *blob_len = (uint32_t)n;
  return buf;
}

#endif
