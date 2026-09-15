// 图片镜像（image blob）解析测试（宿主机）
//
// 为什么值得测:分区内容是外部工具写进去的,可能被写坏、被截断、版本对不上。
// 一旦 offset/size 越界,LVGL 会拿着这个指针去读非法地址 —— 那是硬件异常,
// 不是"图不显示"这种小事。所以这里重点测**坏镜像必须被拒**。
//
// 测试自己构造字节流(不依赖任何 .bin 文件),因此这些用例同时也是
// 文件格式的**可执行文档**:改格式时这里会红。
#include <unity.h>
#include <stddef.h>
#include <string.h>
#include "image_blob.h"

// 构造一个镜像的最小工具。像素用固定模式填充,便于校验解出的位置对不对。
namespace {

// ★ 缓冲大小必须 >= sizeof(ImageBlobHeader) + 像素数据。
//   ImageBlobHeader 含 32 个 ImageEntry,实测 1548 字节 —— 早期这里开了
//   512 字节,写头就溢出,测试进程直接崩（0xC0000409）。
//   用 union 叠一层:既能安全地按结构体写,又能按字节算长度。
//   ★ 改 kImageMaxCount 或 ImageEntry 布局时,这个数字必须跟着改,
//     否则会重现那个崩溃(而且是"测试自己崩",看起来像被测代码的错)。
const uint32_t kHdrSize = (uint32_t)sizeof(ImageBlobHeader);
const uint32_t kBufSize = 2560;          // 1548 头 + 充裕的像素区

union BlobBuf {
  ImageBlobHeader hdr;
  uint8_t bytes[kBufSize];
};

// 在 buf 里写一个 count 项的镜像头。数据区从 kHdrSize 开始。
// 每项固定 16 字节,data_bytes = count*16。
uint32_t makeBlob(BlobBuf& b, uint8_t count) {
  memset(b.bytes, 0, sizeof(b.bytes));
  ImageBlobHeader& h = b.hdr;
  h.magic = kImageBlobMagic;
  h.version = kImageBlobVersion;
  h.count = count;

  uint32_t off = 0;
  for (uint8_t i = 0; i < count; ++i) {
    ImageEntry& e = h.entries[i];
    // 每项做一张 4x2 的 RGB565 小图 = 4*2*2 = 16 字节
    e.w = 4;
    e.h = 2;
    e.cf = LV_COLOR_FORMAT_RGB565;
    e.stride_pad = 0;
    e.size = 16;
    e.offset = (uint16_t)off;
    e.role = (uint16_t)((i == 0) ? ImageRole::Background : ImageRole::BootFrame);
    e.order = i;
    // 名字:pic00 / pic01 ...
    e.name[0] = 'p'; e.name[1] = 'i'; e.name[2] = 'c';
    e.name[3] = (char)('0' + (i / 10));
    e.name[4] = (char)('0' + (i % 10));
    e.name[5] = '\0';
    off += e.size;
  }
  h.data_bytes = off;

  // 像素:每项用一个可区分的填充值
  for (uint8_t i = 0; i < count; ++i) {
    uint8_t* p = b.bytes + kHdrSize + i * 16;
    for (uint32_t k = 0; k < 16; ++k) p[k] = (uint8_t)(0xA0 + i);
  }
  return kHdrSize + off;
}

// 同上,但把第 0 项撑成 32 字节(4×2 + 每行 8 字节填充),
// 用来测行尾填充;其余项不存在(count 固定为 1)。
uint32_t makeBlobPadded(BlobBuf& b) {
  const uint32_t n = makeBlob(b, 1);
  ImageEntry& e = b.hdr.entries[0];
  e.stride_pad = 8;              // 每行 8+8 = 16 字节
  e.size = 32;                   // 16×2 行
  b.hdr.data_bytes = 32;
  for (uint32_t k = 0; k < 32; ++k) b.bytes[kHdrSize + k] = 0xB0;
  return kHdrSize + 32;
}

// 结构体大小是本文件与外部工具的契约之一,顺手钉住
//
// ★ 这些数字不是"随便记一下":它们就是 image.bin 的文件格式。
//   JS 打包器(image-blob-build.js)按同样的偏移写字节,两边靠
//   test-image-roundtrip.ps1 逐字节对账。这里失败 = 格式被改了,
//   而只改一边的话,设备端读到的是乱七八糟的宽高 → 越界指针 → 硬件异常。
static void assertLayoutSane() {
  TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)sizeof(lv_image_header_t));
  TEST_ASSERT_EQUAL_UINT32(44, (uint32_t)sizeof(ImageEntry));
  TEST_ASSERT_EQUAL_UINT32(12 + 32 * 44, (uint32_t)sizeof(ImageBlobHeader));
  TEST_ASSERT_EQUAL_UINT32(32, (uint32_t)kImageMaxCount);
  // offset 必须是 u32:一张 480×480 的 RGB565 背景就要 450KB,
  // u16 会让整个镜像装不下 64KB(老代码就是这个坑)
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)sizeof(((ImageEntry*)nullptr)->offset));
  TEST_ASSERT_TRUE((uint32_t)sizeof(ImageBlobHeader) < kBufSize);
}

// 逐字段检查 C 结构体的实际偏移 —— 和 JS 侧的字节断言是同一份契约的两半。
// ★ 必须用 offsetof/sizeof 让**编译器**报数,不能靠人"数格子":
//   这个文件里就数错过两次(一次漏了 u32 之间的洞,一次多算了尾部填充),
//   两次都是这条断言先发现。
static void test_entry_field_offsets(void) {
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)offsetof(ImageEntry, offset));
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)offsetof(ImageEntry, size));
  TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)offsetof(ImageEntry, w));
  TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)offsetof(ImageEntry, h));
  TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)offsetof(ImageEntry, cf));
  TEST_ASSERT_EQUAL_UINT32(13, (uint32_t)offsetof(ImageEntry, stride_pad));
  TEST_ASSERT_EQUAL_UINT32(14, (uint32_t)offsetof(ImageEntry, role));
  TEST_ASSERT_EQUAL_UINT32(16, (uint32_t)offsetof(ImageEntry, order));
  TEST_ASSERT_EQUAL_UINT32(18, (uint32_t)offsetof(ImageEntry, name));
  TEST_ASSERT_EQUAL_UINT32(44, (uint32_t)sizeof(ImageEntry));

  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)offsetof(ImageBlobHeader, magic));
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)offsetof(ImageBlobHeader, version));
  TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)offsetof(ImageBlobHeader, count));
  TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)offsetof(ImageBlobHeader, data_bytes));
  TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)offsetof(ImageBlobHeader, entries));
}

// 角色编号是界面下拉框、打包器和固件三方共用的契约。
// 改编号会让"右屏显示成左屏的表情",而且不报错 —— 所以钉死。
static void test_role_ids(void) {
  TEST_ASSERT_EQUAL_UINT16(1, (uint16_t)ImageRole::Background);
  TEST_ASSERT_EQUAL_UINT16(2, (uint16_t)ImageRole::BootFrame);
  TEST_ASSERT_EQUAL_UINT16(3, (uint16_t)ImageRole::FaceIdle);
  TEST_ASSERT_EQUAL_UINT16(4, (uint16_t)ImageRole::FaceRedline);
  TEST_ASSERT_EQUAL_UINT16(5, (uint16_t)ImageRole::FaceSurprise);
  // 右屏(转速表)一整套
  TEST_ASSERT_EQUAL_UINT16(6, (uint16_t)ImageRole::FaceIdleR);
  TEST_ASSERT_EQUAL_UINT16(7, (uint16_t)ImageRole::FaceRedlineR);
  TEST_ASSERT_EQUAL_UINT16(8, (uint16_t)ImageRole::FaceSurpriseR);
  // ★ 9/10 是**保留编号**(曾是左右屏开机图,已去掉:开机画面走程序化扫表动画)。
  //   这里故意断言它们"没有被复用" —— 一旦有人把新角色塞进 9/10,
  //   别人已经导出的 image.bin 会突然变成另一个角色,而且不报错。
  //   真要加角色请从 11 开始。
  TEST_ASSERT_EQUAL_UINT16(8, (uint16_t)ImageRole::FaceSurpriseR);

  // 左屏和右屏的角色必须互不相同(复制粘贴最容易犯的错)
  TEST_ASSERT_TRUE(ImageRole::FaceIdle != ImageRole::FaceIdleR);
  TEST_ASSERT_TRUE(ImageRole::FaceRedline != ImageRole::FaceRedlineR);
  TEST_ASSERT_TRUE(ImageRole::FaceSurprise != ImageRole::FaceSurpriseR);
}

}  // namespace

// 结构体大小是本文件与外部工具之间的契约:头不能撑破测试缓冲,
// lv_image_header_t 的宽度也必须跟 format.md 里写的一致。
static void test_layout_sane(void) {
  assertLayoutSane();
}

// 正常镜像:能解析,且每项解出的像素位置与内容都对
static void test_parse_ok_and_get(void) {
  BlobBuf b;
  const uint32_t n = makeBlob(b, 2);

  ImageBlobHeader h;
  TEST_ASSERT_TRUE(imageBlobParse(b.bytes, n, &h));
  TEST_ASSERT_EQUAL_UINT16(2, h.count);
  TEST_ASSERT_EQUAL_UINT32(32, h.data_bytes);

  ImageView v;
  TEST_ASSERT_TRUE(imageBlobGet(b.bytes, n, h, 0, &v));
  TEST_ASSERT_TRUE(v.valid());
  TEST_ASSERT_EQUAL_UINT16(4, v.w);
  TEST_ASSERT_EQUAL_UINT16(2, v.h);
  TEST_ASSERT_EQUAL_UINT8(LV_COLOR_FORMAT_RGB565, v.cf);
  TEST_ASSERT_EQUAL_UINT32(8, v.strideBytes());      // 4 px × 2 B
  TEST_ASSERT_EQUAL_UINT32(16, v.imageBytes());
  TEST_ASSERT_EQUAL_STRING("pic00", v.name);
  TEST_ASSERT_EQUAL_UINT8(0xA0, v.pixels[0]);        // 第 0 项的填充值
  TEST_ASSERT_EQUAL_UINT8(0xA0, v.pixels[15]);

  TEST_ASSERT_TRUE(imageBlobGet(b.bytes, n, h, 1, &v));
  TEST_ASSERT_EQUAL_STRING("pic01", v.name);
  TEST_ASSERT_EQUAL_UINT8(0xA1, v.pixels[0]);        // 第 1 项不同
  TEST_ASSERT_EQUAL_UINT8(0xA1, v.pixels[15]);
}

// ★ 关键:坏镜像必须被拒,不能返回越界指针
static void test_rejects_bad_blob(void) {
  BlobBuf b, save;
  ImageBlobHeader h;

  // 太短，装不下头
  TEST_ASSERT_FALSE(imageBlobParse(nullptr, 0, &h));
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, 8, &h));

  // 魔数不对
  uint32_t n = makeBlob(b, 1);
  memcpy(save.bytes, b.bytes, n);
  b.hdr.magic = 0xDEADBEEF;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // 版本不对（宁可拒收,也不用可能改过布局的数据）
  memcpy(b.bytes, save.bytes, n);
  b.hdr.version = (uint16_t)(kImageBlobVersion + 1);
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // count 超过上限
  memcpy(b.bytes, save.bytes, n);
  b.hdr.count = kImageMaxCount + 1;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // count 为 0
  memcpy(b.bytes, save.bytes, n);
  b.hdr.count = 0;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // data_bytes 超出实际长度（声明有数据其实没有）
  memcpy(b.bytes, save.bytes, n);
  b.hdr.data_bytes = 9999;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // ★ 单项 offset 越界:声明在数据区之外
  memcpy(b.bytes, save.bytes, n);
  b.hdr.entries[0].offset = 5000;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // ★ size 与 w/h/cf 不自洽:声称 16 字节,却按 480×480 描述
  memcpy(b.bytes, save.bytes, n);
  b.hdr.entries[0].w = 480;
  b.hdr.entries[0].h = 480;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // 未知颜色格式
  memcpy(b.bytes, save.bytes, n);
  b.hdr.entries[0].cf = 0xEE;   // 不是任何已知格式
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // w/h 为 0
  memcpy(b.bytes, save.bytes, n);
  b.hdr.entries[0].w = 0;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // 名字没终止符
  memcpy(b.bytes, save.bytes, n);
  memset(b.hdr.entries[0].name, 'x', kImageNameMax);
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));

  // 镜像被截断:头说有两项,实际只有一项的数据
  n = makeBlob(b, 2);
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n - 8, &h));
}

// 取图越界要返回 false,而不是给个野指针
static void test_get_out_of_range(void) {
  BlobBuf b;
  const uint32_t n = makeBlob(b, 2);
  ImageBlobHeader h;
  TEST_ASSERT_TRUE(imageBlobParse(b.bytes, n, &h));

  ImageView v;
  TEST_ASSERT_FALSE(imageBlobGet(b.bytes, n, h, 2, &v));      // count=2,索引 2 越界
  TEST_ASSERT_FALSE(imageBlobGet(b.bytes, n, h, 200, &v));
  TEST_ASSERT_FALSE(imageBlobGet(nullptr, n, h, 0, &v));
  TEST_ASSERT_FALSE(imageBlobGet(b.bytes, n, h, 0, nullptr));
}

// 按角色查找:同角色多帧时按 order 升序返回
static void test_find_role_sorted(void) {
  BlobBuf b;
  const uint32_t n = makeBlob(b, 4);
  ImageBlobHeader h;

  // 把 3 张 BootFrame 的 order 故意打乱,验证查找结果按 order 排序
  b.hdr.entries[1].order = 30;
  b.hdr.entries[2].order = 10;
  b.hdr.entries[3].order = 20;

  TEST_ASSERT_TRUE(imageBlobParse(b.bytes, n, &h));

  ImageView v[4];
  const uint8_t got = imageBlobFindRole(b.bytes, n, h, ImageRole::BootFrame, v, 4);
  TEST_ASSERT_EQUAL_UINT8(3, got);
  TEST_ASSERT_EQUAL_STRING("pic02", v[0].name);   // order 10
  TEST_ASSERT_EQUAL_STRING("pic03", v[1].name);   // order 20
  TEST_ASSERT_EQUAL_STRING("pic01", v[2].name);   // order 30

  // 只有一个时
  const uint8_t bg = imageBlobFindRole(b.bytes, n, h, ImageRole::Background, v, 4);
  TEST_ASSERT_EQUAL_UINT8(1, bg);
  TEST_ASSERT_EQUAL_STRING("pic00", v[0].name);

  // 没有的角色返回 0
  TEST_ASSERT_EQUAL_UINT8(0, imageBlobFindRole(b.bytes, n, h, ImageRole::FaceIdle, v, 4));

  // 输出容量小于找到的数量:只写这么多,且不越界
  const uint8_t partial = imageBlobFindRole(b.bytes, n, h, ImageRole::BootFrame, v, 2);
  TEST_ASSERT_EQUAL_UINT8(2, partial);
  TEST_ASSERT_EQUAL_STRING("pic02", v[0].name);
  TEST_ASSERT_EQUAL_STRING("pic03", v[1].name);
}

// 颜色格式 → 每像素字节数
static void test_cf_bytes_per_pixel(void) {
  TEST_ASSERT_EQUAL_UINT8(2, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_RGB565));
  TEST_ASSERT_EQUAL_UINT8(3, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_RGB888));
  TEST_ASSERT_EQUAL_UINT8(4, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_ARGB8888));
  TEST_ASSERT_EQUAL_UINT8(4, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_XRGB8888));
  TEST_ASSERT_EQUAL_UINT8(1, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_A8));
  TEST_ASSERT_EQUAL_UINT8(1, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_I8));
  TEST_ASSERT_EQUAL_UINT8(1, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_L8));
  TEST_ASSERT_EQUAL_UINT8(0, ImageView::cfBytesPerPixel(0xEE));   // 未知
}

// 行尾填充要计入每行字节数（LCD 对齐需要）
static void test_stride_padding(void) {
  BlobBuf b;
  const uint32_t n = makeBlobPadded(b);
  // 4×2 RGB565 + 每行 8 字节填充 → 每行 16 字节,共 32 字节
  ImageBlobHeader h;
  TEST_ASSERT_TRUE(imageBlobParse(b.bytes, n, &h));
  ImageView v;
  TEST_ASSERT_TRUE(imageBlobGet(b.bytes, n, h, 0, &v));
  TEST_ASSERT_EQUAL_UINT32(16, v.strideBytes());
  TEST_ASSERT_EQUAL_UINT32(32, v.imageBytes());
  TEST_ASSERT_EQUAL_UINT8(0xB0, v.pixels[0]);

  // 填充与 size 不自洽时也要拒收:每行 16、两行 32,而 size 谎称 16
  b.hdr.entries[0].size = 16;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, n, &h));
}

// 空分区(全 0xFF)与全 0 都必须被拒 —— 设备上"没刷过图片"就是这种情况
static void test_blank_partition_rejected(void) {
  static BlobBuf b;
  ImageBlobHeader h;

  memset(b.bytes, 0xFF, sizeof(b.bytes));
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, sizeof(b.bytes), &h));

  memset(b.bytes, 0x00, sizeof(b.bytes));
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, sizeof(b.bytes), &h));
}

// 头部布局是本文件与外部工具之间的**硬契约**:
// lv_image_header_t 是位域,小端下字节序为
//   [0]=magic [1]=cf [2..3]=flags [4..5]=w [6..7]=h
// 这里直接检查字节,防止将来有人"顺手重排结构体"而无声破坏格式。
static void test_header_byte_layout(void) {
  TEST_ASSERT_EQUAL_UINT32(12, (uint32_t)sizeof(lv_image_header_t));
  TEST_ASSERT_EQUAL_UINT8(0x19, LV_IMAGE_HEADER_MAGIC);
  TEST_ASSERT_EQUAL_UINT8(0x12, LV_COLOR_FORMAT_RGB565);

  lv_image_header_t hd;
  memset(&hd, 0, sizeof(hd));
  hd.magic = LV_IMAGE_HEADER_MAGIC;
  hd.cf = LV_COLOR_FORMAT_RGB565;
  hd.w = 0x1234;
  hd.h = 0x5678;

  const uint8_t* b = (const uint8_t*)&hd;
  TEST_ASSERT_EQUAL_HEX8(0x19, b[0]);   // magic
  TEST_ASSERT_EQUAL_HEX8(0x12, b[1]);   // cf
  TEST_ASSERT_EQUAL_HEX8(0x34, b[4]);   // w 低字节（小端）
  TEST_ASSERT_EQUAL_HEX8(0x12, b[5]);   // w 高字节
  TEST_ASSERT_EQUAL_HEX8(0x78, b[6]);   // h 低字节
  TEST_ASSERT_EQUAL_HEX8(0x56, b[7]);   // h 高字节
}

void register_image_blob_tests(void) {
  RUN_TEST(test_layout_sane);
  RUN_TEST(test_entry_field_offsets);
  RUN_TEST(test_role_ids);
  RUN_TEST(test_parse_ok_and_get);
  RUN_TEST(test_rejects_bad_blob);
  RUN_TEST(test_get_out_of_range);
  RUN_TEST(test_find_role_sorted);
  RUN_TEST(test_cf_bytes_per_pixel);
  RUN_TEST(test_stride_padding);
  RUN_TEST(test_blank_partition_rejected);
  RUN_TEST(test_header_byte_layout);
}
