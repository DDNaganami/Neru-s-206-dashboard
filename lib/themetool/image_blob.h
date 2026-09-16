#pragma once
#include <stdint.h>
// 用 LVGL 的颜色格式常量计算每行字节数 —— 不硬编码数字,
// 否则 LVGL 升版改了枚举值就会静默算错。
#include <lvgl.h>

// ============================================================
// 图片资源（放 flash 分区）
//
// 与主题同一思路:换图只重刷一个小分区,固件不用重新编译。
//
// ---- 为什么要研究 LVGL 的二进制格式 ----
// LVGL 认两种"图片源":
//   LV_IMAGE_SRC_VARIABLE  —— 编译进固件的 lv_image_dsc_t
//   LV_IMAGE_SRC_FILE      —— 走文件系统(需要注册 lv_fs 驱动)
// 我们要的是第三种用法:**图片放在 flash 分区里、mmap 进来直接用**,
// 既不进固件、也不需要文件系统。做法是:
//   1. 读分区里的图片头(原始 lv_image_header_t 字节)
//   2. 用头里的 w/h/cf 填一个 lv_image_dsc_t,data 指向 mmap 到的像素
//   3. 把这个 dsc 交给 lv_image_set_src()
// 关键细节(从 LVGL 9.5 源码确认,别凭记忆改):
//   · lv_image_header_t 是位域,小端下字节序为
//       [0]=magic(必须是 0x19) [1]=cf [2..3]=flags [4..5]=w [6..7]=h
//       [8..9]=stride [10..11]=reserved_2
//   · magic 不对 LVGL 只当"旧格式"处理(把 cf 当成 magic),不会崩,
//     但会画出乱码 —— 所以 magic 必须写对
//   · LV_IMAGE_SRC_VARIABLE 路径**不会**把像素复制到 RAM(FILE 路径才会),
//     所以 mmap 指过去是安全的,不额外吃 DRAM
//
// ---- 分区里的布局 ----
//   镜像头 ImageBlobHeader
//   然后依次是 index 项的像素数据(紧凑排列,无对齐要求)
//   每张图的数据按 cf 的字节宽度紧排,行间可能有 stride 填充(本项目不用)
//
// ---- 未验证项(必须承认) ----
// LVGL 的 bin 解码器在 LV_IMAGE_SRC_VARIABLE 下对我们的 dsc 到底怎么处理,
// 以及真屏驱动取像素的路径,都**没有硬件可验**。所以:
//   · 上层(dash_ui)先不接图片,见本文件末尾说明
//   · 本文件与测试只保证"文件能被正确解析、像素能被正确解出"
// ============================================================

// 镜像头魔数:"206D" + 版本,便于人工识别一个分区里放的是什么
static const uint32_t kImageBlobMagic   = 0x44363032u;  // '2','0','6','D' 小端读作 0x44363032
static const uint16_t kImageBlobVersion = 1;

// ============================================================
// 一次踩过的坑,记在这里免得再犯:offset 曾经是 uint16_t
// ------------------------------------------------------------
// 后果:整个镜像的像素数据被卡在 64KB 以内 —— 而一张 480×480 的
// RGB565 背景就要 450KB,一个 1MB 的 image 分区等于白给。
// 更麻烦的是它**不会报错**:打包器只是拒绝,或者更糟 —— 偏移溢出后
// 设备端拿到错误的像素位置,画面是花的,但日志一切正常。
// 所以改成 uint32_t(项大小 44 字节不变,因为 size 那 4 字节腾出来给
// offset 之后仍然紧排),顺便把图片数量上限从 16 提到 32
// (双屏背景 + 2×3 表情 + 若干开机帧,16 个太紧)。
// 改这个结构就是改文件格式,**JS 打包器必须同步改**
// (tools/theme-editor/image-blob-build.js),否则两边对不上 ——
// 靠 test-image-roundtrip.ps1 兜底,它会逐字节对账。
// ============================================================

// 图片类型 —— 决定上层拿它做什么
// 左右屏各自一整套表情:车速表和转速表要显示不同的表情差分,
// 所以角色必须按"屏 × 状态"分开命名。
//
// ★ 编号 9/10 曾经是"左右屏开机图",已经**去掉**了。原因:
//   开机画面用程序化动画就够(boot_anim.cpp 的淡入 + 扫表 + 睁眼),
//   boot_stagger_ms 本来就让两屏错峰启动,观感上已经是两个不同的开机过程;
//   而两张开机图要多占 144KB,还要多一条"什么时候显示什么图"的时序逻辑。
//   9/10 作为**保留编号**留着不回收 —— 以后真要加开机图或别的角色,
//   直接接着用,不需要动已有图片的编号(动编号 = 所有人的图片都要重导)。
enum class ImageRole : uint16_t {
  // ---- 左屏(车速表) ----
  Background   = 1,   // 表盘背景图（衬在圆弧下面,两屏共用一张）
  BootFrame    = 2,   // 开机动画的一帧（多帧按 order 排序播放）
  FaceIdle     = 3,   // 表情：常态
  FaceRedline  = 4,   // 表情：红区
  FaceSurprise = 5,   // 表情：惊喜

  // ---- 右屏(转速表,下方带水温表) ----
  FaceIdleR     = 6,  // 表情：常态
  FaceRedlineR  = 7,  // 表情：红区
  FaceSurpriseR = 8,  // 表情：惊喜

  // 9 / 10 保留(曾是开机图,见上面的说明)
};

static const uint8_t kImageNameMax = 24;
static const uint8_t kImageMaxCount = 32;

// 索引项（紧凑,便于从 flash 直接读）
// ★ 布局就是文件格式:偏移写在这里,JS 打包器与测试必须一致。
//   这份布局**没有任何对齐空洞**(每个字段都紧跟前一个),因为它是刻意排的:
//     [0..3]   offset      u32
//     [4..7]   size        u32    ← 两个 u32 挨着放,中间不会有洞
//     [8..9]   w           u16
//     [10..11] h           u16
//     [12]     cf          u8
//     [13]     stride_pad  u8
//     [14..15] role        u16
//     [16..17] order       u16
//     [18..41] name[24]
//   合计 44 字节。最宽的成员是 u32(对齐 4),而 44 正好是 4 的倍数,
//   所以尾部也不需要填充 —— sizeof 就是各字段之和。
//
//   ★ 这里踩过两次坑,都是"想当然地算对齐":
//     1) 把 offset 和 size 分开放 → 中间多出 4 字节洞,结构体 44 ≠ JS 的 48;
//        把两个 u32 挨着放,洞就没了。
//     2) 以为尾部要补 2 字节,硬加了一个 uint16 reserved —— 结果
//        **2 个 u16 只占 [16..17],不是 [16..19]**,name 从 18 开始正好
//        把那个 reserved 覆盖掉,结构体还是 44,白加一场。
//   教训:别用"数格子"的方法推结构体布局,写一条 offsetof/sizeof 断言让
//   编译器告诉你(image_blob 的 test_entry_field_offsets 就是干这个的)。
struct ImageEntry {
  uint32_t offset;     // 相对镜像头结尾的字节偏移（u32:见上面 offset 的坑）
  uint32_t size;       // 字节数
  uint16_t w;
  uint16_t h;
  uint8_t  cf;         // LVGL 颜色格式(LV_COLOR_FORMAT_*)
  uint8_t  stride_pad; // 每行末尾的填充字节数(0 = 紧排)
  uint16_t role;       // ImageRole
  uint16_t order;      // 同 role 内多帧的播放顺序
  char     name[kImageNameMax];
};

struct ImageBlobHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t data_bytes;    // 所有像素数据总字节数
  ImageEntry entries[kImageMaxCount];
};

// 一张图的视图(指向 mmap 或内存中的像素,不拥有数据)
struct ImageView {
  const uint8_t* pixels = nullptr;
  uint16_t w = 0;
  uint16_t h = 0;
  uint8_t  cf = 0;          // LV_COLOR_FORMAT_*
  uint8_t  stride_pad = 0;
  ImageRole role = ImageRole::Background;
  uint16_t order = 0;
  const char* name = nullptr;

  bool valid() const { return pixels != nullptr && w > 0 && h > 0; }

  // 每像素字节数(cf → 字节)。未知格式返回 0。
  //
  // ★ RGB565A8 返回 **2** —— 这是"颜色平面每像素占几字节"。
  //   它另有**一张独立的 A8 平面追加在整个颜色数据之后**
  //   (LVGL 布局:size = stride*h + (stride/2)*h,已在 lv_draw_buf.c 确认)。
  //   所以判断"这张图总共占多少字节"不能用这个函数乘,见 packedBytes()。
  static uint8_t cfBytesPerPixel(uint8_t cf);

  // 每行字节数(每像素字节数 × w + 行尾填充)
  uint32_t strideBytes() const {
    return (uint32_t)cfBytesPerPixel(cf) * w + stride_pad;
  }

  // 整张图的实际字节数,**含 RGB565A8 追加的 A8 平面**。
  //
  // ★ 这个函数是必需的,别用 strideBytes()*h 代替:对 RGB565A8 那样算会
  //   少算 alpha 平面(3 字节/像素 vs 2),于是镜像校验会**拒收**一张完全
  //   合法的图 —— 实测踩过:打包器写 30000 字节,校验按 20000 算,直接报
  //   "尺寸不合法",而 blob 本身没错。
  uint32_t packedBytes() const {
    const uint32_t color = strideBytes() * h;
    if (cf == LV_COLOR_FORMAT_RGB565A8) {
      return color + (strideBytes() / 2) * h;   // A8 平面行宽是颜色行宽的一半
    }
    return color;
  }

  // 兼容旧调用点(旧语义 = strideBytes*h,不含 alpha 平面)。
  // 新代码请用 packedBytes()。
  uint32_t imageBytes() const { return packedBytes(); }
};

// ---------------- 解析（纯逻辑,宿主机可测） ----------------
// 把一段内存(镜像)解析成索引。成功返回 true。
// 会做完整校验:魔数、版本、count 上限、每项 offset/size 是否落在
// data_bytes 范围内。**坏镜像必须被拒,不能让上层拿到越界指针。**
bool imageBlobParse(const uint8_t* blob, uint32_t len, ImageBlobHeader* out);

// 从已解析的镜像里取第 i 张图。越界或数据不完整返回 false。
bool imageBlobGet(const uint8_t* blob, uint32_t len,
                  const ImageBlobHeader& hdr, uint8_t i, ImageView* out);

// 按角色查找。同角色多帧时返回 order 最小的那张;index 用于遍历(见下)。
// 返回找到的数量(0 表示没有)。
uint8_t imageBlobFindRole(const uint8_t* blob, uint32_t len,
                          const ImageBlobHeader& hdr, ImageRole role,
                          ImageView* out, uint8_t out_cap);

// ---------------- 加载（平台相关） ----------------
// 设备:从 flash 的 image 分区读头并 mmap 像素。
// 宿主机:从环境变量 IMAGE_BLOB 指定的文件读(便于预览与单测)。
// 返回镜像内存起始指针(设备上指向 mmap、宿主机指向已读入的缓冲),
// 失败返回 nullptr。blob_len 输出镜像长度。
const uint8_t* imageBlobLoad(uint32_t* blob_len);

// 设备/宿主机的镜像分区位置(供文档与工具引用)
// ★ 必须与 partitions.csv 里 image 分区的 Size 一致:宿主机用它挡掉过大的
//   镜像文件(否则"预览能跑、刷进设备就被截断"这种错很难查)。
//   改分区大小 → 这里和 tools/theme-editor/image-blob-build.js 的
//   PARTITION_BYTES 都要改(JS 那边也有一份,用来给用户算占用)。
#define IMAGE_PARTITION_LABEL "image"
#define IMAGE_PARTITION_BYTES (1024u * 1024u)   // partitions.csv: 0x100000
#define IMAGE_BLOB_MAX_BYTES  IMAGE_PARTITION_BYTES

// ============================================================
// 为什么上层(dash_ui)暂时还没接图片
// ------------------------------------------------------------
// 把图片画到 LVGL 上要动 dash_ui.cpp 的图层结构:
//   · 背景图要衬在圆弧下面(现在背景只有一个纯色 lv_obj)
//   · 表情要从"形状组合"换成 lv_image
// 而这两件事的**唯一验收方式是看真屏**——桩驱动丢弃画面,pcpreview 也
// 只是把同一套 LVGL 调用渲成 BMP。在屏到货前接上,只能证明"编译过了",
// 证明不了"画对了"。
// 所以本次先交付:格式 + 解析 + 加载 + 单测(全部可在宿主机验证),
// 以及编辑器的图片转换与预览。等屏到货、真驱动能出图,再接 dash_ui。
// ============================================================
