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
// dashcore 的表情槽位表:本文件要把它与 ImageRole 逐条对账
// (见 test_face_role_ids_match_stages)。测试跨模块引用是故意的 ——
// 这两张表分居两层,"对不上"只有在这里才能被发现。
#include "face_stages.h"

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
    // 角色随便给一个"非表情"的固定值即可:这里测的是解析/查找,不是角色语义
    // (角色语义由 test_role_ids / test_face_role_ids_match_stages 单独钉)
    e.role = (uint16_t)((i == 0) ? ImageRole::Background : (ImageRole)9);
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
  // 左屏(转速表):怠速 / 红区 / 巡航 / 运动 / 高转
  TEST_ASSERT_EQUAL_UINT16(3, (uint16_t)ImageRole::FaceIdle);
  TEST_ASSERT_EQUAL_UINT16(4, (uint16_t)ImageRole::FaceRedline);
  TEST_ASSERT_EQUAL_UINT16(12, (uint16_t)ImageRole::FaceCruise);
  TEST_ASSERT_EQUAL_UINT16(13, (uint16_t)ImageRole::FaceSport);
  // 高转(左)与市区(右)是 2026-09-18 新增的第五档,编号从 21 起接
  TEST_ASSERT_EQUAL_UINT16(21, (uint16_t)ImageRole::FaceHigh);
  // 右屏(速度表):静止 / 超速 / 快速路 / 高速 / 市区
  // ★ 超速这张的角色号 8 沿用当年的"惊喜"(只改名不改号,已导出的 image.bin 不受影响)
  TEST_ASSERT_EQUAL_UINT16(6, (uint16_t)ImageRole::FaceIdleR);
  TEST_ASSERT_EQUAL_UINT16(8, (uint16_t)ImageRole::FaceOverspeedR);
  TEST_ASSERT_EQUAL_UINT16(17, (uint16_t)ImageRole::FaceCruiseR);
  TEST_ASSERT_EQUAL_UINT16(18, (uint16_t)ImageRole::FaceSportR);
  TEST_ASSERT_EQUAL_UINT16(22, (uint16_t)ImageRole::FaceCityR);

  // 左屏和右屏的角色必须互不相同(复制粘贴最容易犯的错)
  TEST_ASSERT_TRUE(ImageRole::FaceIdle != ImageRole::FaceIdleR);
  TEST_ASSERT_TRUE(ImageRole::FaceCruise != ImageRole::FaceCruiseR);
  TEST_ASSERT_TRUE(ImageRole::FaceSport != ImageRole::FaceSportR);
  // 新增的两张也各自独立:高转是左屏的、市区是右屏的,不能互相顶替
  TEST_ASSERT_TRUE(ImageRole::FaceHigh != ImageRole::FaceCityR);
  TEST_ASSERT_TRUE(ImageRole::FaceHigh != ImageRole::FaceRedline);
  TEST_ASSERT_TRUE(ImageRole::FaceCityR != ImageRole::FaceCruiseR);

  // ★ 保留编号一个都不能被复用。它们分别是:
  //   2=开机帧、5=左屏惊喜、7=右屏红区、9/10=开机图、11/16=眨眼图、
  //   14/15/19/20=冷车/过热图(水温已不参与表情)。
  //   复用会让别人已导出的 image.bin 里那几张图静默变成别的表情。
  const uint16_t kReserved[] = {2, 5, 7, 9, 10, 11, 14, 15, 16, 19, 20};
  for (uint16_t r : kReserved) {
    TEST_ASSERT_FALSE_MESSAGE(imageRoleIsFace((ImageRole)r),
                              "保留编号被当成了表情角色");
  }
  TEST_ASSERT_FALSE(imageRoleIsFace((ImageRole)0));    // 0 = 这屏用不到
  // 21/22 现在**已经分配**给高转(左)/市区(右)了,所以下一个没用到的号是 23 ——
  // 这条同时防止有人把 21/22 又当成"保留号"或者把 23 提前占掉。
  TEST_ASSERT_FALSE(imageRoleIsFace((ImageRole)23));   // 还没分配
  TEST_ASSERT_TRUE(imageRoleIsFace(ImageRole::FaceHigh));
  TEST_ASSERT_TRUE(imageRoleIsFace(ImageRole::FaceCityR));
  TEST_ASSERT_TRUE(imageRoleIsFace(ImageRole::FaceIdle));
  TEST_ASSERT_TRUE(imageRoleIsFace(ImageRole::FaceSportR));
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

  // 把 3 张"保留编号 9"的图 order 故意打乱,验证查找结果按 order 排序
  b.hdr.entries[1].order = 30;
  b.hdr.entries[2].order = 10;
  b.hdr.entries[3].order = 20;

  TEST_ASSERT_TRUE(imageBlobParse(b.bytes, n, &h));

  ImageView v[4];
  const uint8_t got = imageBlobFindRole(b.bytes, n, h, (ImageRole)9, v, 4);
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
  const uint8_t partial = imageBlobFindRole(b.bytes, n, h, (ImageRole)9, v, 2);
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

// ★ 带透明通道的格式(RGB565A8):它的**总字节数不等于 strideBytes()*h**。
//
// 这条踩过一次真坑:RGBA→RGB565A8 打包出来是 3 字节/像素
// (2 字节色 + 1 字节 alpha 平面),而校验当时用 strideBytes()*h 算,
// 得到 2 字节/像素 → 把完全合法的镜像**拒收**了
// (实测症状:打包器写 30000 字节,校验按 20000 算,报"尺寸不合法",
//  而 blob 本身没有任何问题)。
// 所以 packedBytes() 必须把追加的 A8 平面算进去,这里钉住它。
static void test_rgb565a8_size_includes_alpha_plane(void) {
  ImageView v;
  v.w = 100;
  v.h = 100;
  v.cf = LV_COLOR_FORMAT_RGB565A8;
  v.stride_pad = 0;

  // 颜色平面每行 200 字节(2B/px),A8 平面每行 100 字节
  TEST_ASSERT_EQUAL_UINT32(200, v.strideBytes());
  // 总大小 = 颜色平面 20000 + A8 平面 10000 = 30000
  TEST_ASSERT_EQUAL_UINT32(30000, v.packedBytes());
  // imageBytes() 与 packedBytes() 同义(旧调用点也拿到正确值)
  TEST_ASSERT_EQUAL_UINT32(30000, v.imageBytes());
  // 每像素字节数仍是 2(只管颜色平面)—— 别把它改成 3,
  // 否则 strideBytes() 会算成 300,行宽就错了
  TEST_ASSERT_EQUAL_UINT8(2, ImageView::cfBytesPerPixel(LV_COLOR_FORMAT_RGB565A8));

  // 对照:普通 RGB565 两个函数必须一致(没有追加平面)
  ImageView p;
  p.w = 100; p.h = 100; p.cf = LV_COLOR_FORMAT_RGB565; p.stride_pad = 0;
  TEST_ASSERT_EQUAL_UINT32(p.strideBytes() * p.h, p.packedBytes());
  TEST_ASSERT_EQUAL_UINT32(20000, p.packedBytes());
}

// 解析器必须**接受** RGB565A8(按含 alpha 平面的尺寸校验)
static void test_parse_accepts_rgb565a8(void) {
  BlobBuf b;
  const uint32_t n = makeBlob(b, 1);          // 4×2 RGB565,size=16
  // 改成 RGB565A8:每行 4*2=8 色字节 + 每行 4 字节 alpha → size 要重算
  ImageEntry& e = b.hdr.entries[0];
  e.cf = LV_COLOR_FORMAT_RGB565A8;
  const uint32_t color = 4 * 2 * 2;           // 8 字节/行 × 2 行
  const uint32_t alpha = 4 * 2;               // 4 字节/行 × 2 行
  e.size = color + alpha;
  b.hdr.data_bytes = e.size;

  ImageBlobHeader h;
  TEST_ASSERT_TRUE_MESSAGE(imageBlobParse(b.bytes, kHdrSize + e.size, &h),
                           "RGB565A8 的合法镜像被拒收了(alpha 平面没算进去?)");
  ImageView v;
  TEST_ASSERT_TRUE(imageBlobGet(b.bytes, kHdrSize + e.size, h, 0, &v));
  TEST_ASSERT_EQUAL_UINT8(LV_COLOR_FORMAT_RGB565A8, v.cf);
  TEST_ASSERT_EQUAL_UINT32(e.size, v.packedBytes());

  // 反向:size 少算 alpha 平面(只写颜色字节)必须被拒
  e.size = color;
  b.hdr.data_bytes = color;
  TEST_ASSERT_FALSE(imageBlobParse(b.bytes, kHdrSize + color, &h));
}

// ★ 屏 ↔ 表情角色的对应:装反了会"右屏显示左屏的脸",而且**不会报错**。
// 法系车(标致 206 实车):左屏 = 转速表,右屏 = 速度表。
// 这里只钉住"背景不属于任何一屏"和"两屏的常态不是同一个编号",
// 更完整的对照见 test_face_role_ids_match_stages(它读 face_stages.h)。
static void test_face_role_side_mapping(void) {
  TEST_ASSERT_EQUAL_UINT16(3, (uint16_t)ImageRole::FaceIdle);    // 左 = 转速表
  TEST_ASSERT_EQUAL_UINT16(6, (uint16_t)ImageRole::FaceIdleR);   // 右 = 速度表
  TEST_ASSERT_TRUE(ImageRole::FaceIdle != ImageRole::FaceIdleR);
  TEST_ASSERT_EQUAL_UINT16(1, (uint16_t)ImageRole::Background);  // 背景两屏共用
}

// face_stages.h 的 kFaceRoleId 与 image_blob.h 的 ImageRole **逐条比对**。
//
// 这两份表说的是同一件事(dash_ui 用第一份去取第二份的图):错一位就会
// "右屏显示左屏的脸"或"巡航显示成红区",而且完全不会报错、也不崩。
// 所以这里不靠注释,靠断言。
static void test_face_role_ids_match_stages(void) {
  // 槽位顺序 = Face 枚举顺序:Idle Cruise Sport Redline Overspeed
  struct { int slot; ImageRole L, R; } kExpect[5] = {
    {0, ImageRole::FaceIdle,     ImageRole::FaceIdleR},       // Idle
    {1, ImageRole::FaceCruise,   ImageRole::FaceCruiseR},     // Cruise
    {2, ImageRole::FaceSport,    ImageRole::FaceSportR},      // Sport
    {3, ImageRole::FaceRedline,  (ImageRole)0},               // Redline:只有左屏有
    {4, (ImageRole)0,            ImageRole::FaceOverspeedR},  // Overspeed:只有右屏有
  };
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Count, kFaceSlotCount);
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_INT(i, kExpect[i].slot);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)kExpect[i].L, kFaceRoleId[0][i]);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)kExpect[i].R, kFaceRoleId[1][i]);
    // 有编号的必须是真表情;0 表示这屏用不到
    if (kFaceRoleId[0][i] != 0) TEST_ASSERT_TRUE(imageRoleIsFace((ImageRole)kFaceRoleId[0][i]));
    if (kFaceRoleId[1][i] != 0) TEST_ASSERT_TRUE(imageRoleIsFace((ImageRole)kFaceRoleId[1][i]));
    // 同一屏里不能有两个状态共用一个角色编号(否则切状态时画面不变)
    for (int j = 0; j < i; ++j) {
      if (kFaceRoleId[0][i] != 0 && kFaceRoleId[0][j] != 0) {
        TEST_ASSERT_TRUE(kFaceRoleId[0][i] != kFaceRoleId[0][j]);
      }
      if (kFaceRoleId[1][i] != 0 && kFaceRoleId[1][j] != 0) {
        TEST_ASSERT_TRUE(kFaceRoleId[1][i] != kFaceRoleId[1][j]);
      }
    }
  }
}

void register_image_blob_tests(void) {
  RUN_TEST(test_layout_sane);
  RUN_TEST(test_entry_field_offsets);
  RUN_TEST(test_role_ids);
  RUN_TEST(test_face_role_side_mapping);
  RUN_TEST(test_face_role_ids_match_stages);
  RUN_TEST(test_rgb565a8_size_includes_alpha_plane);
  RUN_TEST(test_parse_accepts_rgb565a8);
  RUN_TEST(test_parse_ok_and_get);
  RUN_TEST(test_rejects_bad_blob);
  RUN_TEST(test_get_out_of_range);
  RUN_TEST(test_find_role_sorted);
  RUN_TEST(test_cf_bytes_per_pixel);
  RUN_TEST(test_stride_padding);
  RUN_TEST(test_blank_partition_rejected);
  RUN_TEST(test_header_byte_layout);
}
