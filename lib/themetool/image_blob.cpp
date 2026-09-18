#include "image_blob.h"
#include <string.h>
#include "dash_log.h"   // 日志同时打到 USB-CDC 与 UART0(见文件头说明)

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
  // ★ 常量名是 SPI_FLASH_MMAP_DATA(esp_spi_flash.h),不是 ESP_PARTITION_MMAP_*。
  // ★ handle 存成 static:映射在我们这里活到重启为止,绝不能让它被回收 ——
  //   否则那块虚拟地址会被别的 mmap 复用,LVGL 读到的就是别人的数据。
  static spi_flash_mmap_handle_t s_handle = 0;
  const void* mapped = nullptr;
  const esp_err_t err = esp_partition_mmap(
      part, 0, part->size, SPI_FLASH_MMAP_DATA, &mapped, &s_handle);
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

const uint8_t* imageBlobLoad(uint32_t* blob_len) {
  if (blob_len) *blob_len = 0;

  const char* path = getenv("IMAGE_BLOB");
  if (!path || !*path) return nullptr;    // 没设就是"不用图片",正常

  FILE* f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "image: 打不开 %s\n", path);
    return nullptr;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || (unsigned long)n > IMAGE_BLOB_MAX_BYTES) {
    fclose(f);
    fprintf(stderr, "image: %s 大小不合理 (%ld)\n", path, n);
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
