#include "image_load.h"

// ============================================================
// 见 image_load.h 的说明。这里只做"把字节能拿到手"这件事。
// 真正吃 DRAM 的解析与校验在 lib/themetool/image_blob.cpp(纯逻辑,有单测)。
//
// ★ 一个容易被忽略的点:设备上像素**不进 DRAM**。
//   走的是 esp_partition_mmap 只读映射,LVGL 在 LV_IMAGE_SRC_VARIABLE
//   路径下不会复制像素(见 image_blob.h)。所以这里的常驻开销只有
//   一个 716 字节的索引结构 + 一个指针。
// ============================================================

namespace {
const uint8_t* g_blob = nullptr;
uint32_t g_blob_len = 0;
ImageBlobHeader g_hdr;      // 716 字节,常驻 .bss
bool g_hdr_ok = false;
}  // namespace

const uint8_t* image_blob() { return g_blob; }
uint32_t image_blob_len() { return g_blob_len; }

const ImageBlobHeader* image_blob_header() {
  return (g_blob && g_hdr_ok) ? &g_hdr : nullptr;
}

bool image_load() {
  uint32_t len = 0;
  const uint8_t* blob = imageBlobLoad(&len);   // 内部已经打印了失败原因
  if (!blob || len < sizeof(ImageBlobHeader)) {
    g_blob = nullptr;
    g_blob_len = 0;
    g_hdr_ok = false;
    return false;
  }

  // 再解析一次拿出索引:上层(ui)拿 header 就能按 role 找图。
  // imageBlobLoad 里已经解析过一次,这里 716 字节的 memcpy 不值得为省它
  // 去改 imageBlobLoad 的接口(那会让宿主机预览那边也要跟着动)。
  if (!imageBlobParse(blob, len, &g_hdr)) {
    g_blob = nullptr;
    g_blob_len = 0;
    g_hdr_ok = false;
    return false;
  }

  g_blob = blob;
  g_blob_len = len;
  g_hdr_ok = true;
  return true;
}
