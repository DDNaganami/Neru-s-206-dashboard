#include "image_load.h"
#include <string.h>

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

bool image_role_has_alpha(ImageRole role) {
  const ImageBlobHeader* h = image_blob_header();
  if (!h) return false;
  ImageView v;
  if (!imageBlobGet(image_blob(), image_blob_len(), *h, 0, &v)) return false;
  // 按角色找第一张,看它的颜色格式
  ImageView found[kImageMaxCount];
  const uint8_t n = imageBlobFindRole(image_blob(), image_blob_len(), *h, role,
                                      found, kImageMaxCount);
  if (n == 0) return false;
  return found[0].cf == LV_COLOR_FORMAT_RGB565A8;
}

bool image_dsc_for_role(ImageRole role, lv_image_dsc_t* out) {
  if (!out) return false;
  const ImageBlobHeader* h = image_blob_header();
  if (!h) return false;

  ImageView found[kImageMaxCount];
  const uint8_t n = imageBlobFindRole(image_blob(), image_blob_len(), *h, role,
                                      found, kImageMaxCount);
  if (n == 0) return false;
  const ImageView& v = found[0];      // 已按 order 升序,取第一张

  memset(out, 0, sizeof(*out));
  // lv_image_dsc_t 的 header 是 lv_image_header_t(位域,小端字节序见
  // lib/themetool/image_blob.h 的说明)。逐字段赋值,不要整体 memcpy ——
  // 位域布局依赖编译器,逐字段更稳。
  out->header.magic  = LV_IMAGE_HEADER_MAGIC;   // 0x19,填错 LVGL 当旧格式画乱码
  out->header.cf     = (lv_color_format_t)v.cf;
  out->header.w      = v.w;
  out->header.h      = v.h;
  out->header.stride = (uint16_t)v.strideBytes();
  // ★ data 直接指向 mmap 的像素,零拷贝。
  //   LV_IMAGE_SRC_VARIABLE 路径下 LVGL 不会复制它(见 image_blob.h)。
  out->data      = (const uint8_t*)v.pixels;
  out->data_size = v.imageBytes();
  return true;
}
