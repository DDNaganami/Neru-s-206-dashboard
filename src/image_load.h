#pragma once
#include <stdint.h>
#include "image_blob.h"
// 需要 lv_image_dsc_t / lv_color_format_t,所以这个头文件依赖 LVGL。
// 想让 image_load 完全脱离 LVGL 也可以(只暴露上面的裸指针接口),
// 但那样调用方还得自己填 dsc —— 那正是最容易填错的一步,所以放这里。
#include <lvgl.h>

// ============================================================
// 图片资源的"取文件"部分 —— 与 theme_load 同一套思路:
//   设备端从 flash 的 image 分区做只读映射
//   宿主机从环境变量 IMAGE_BLOB 指定的文件读
//
// 和主题一样,**降级路径是刻意的**:没有分区 / 没刷过 / 镜像坏了,
// 都只打印一行日志并继续跑(背景退回纯色)。图片缺失绝不能让固件起不来。
//
// ★ 为什么现在就接上(而不是等真屏到货):
//   不接的话,"刷了图片却没有反应"在设备上完全静默 —— 到时候没法判断
//   是分区表错了、还是 mmap 失败、还是格式不对。接上之后,上电串口就会
//   明确打印 `image: 已加载 N 张图 (M 字节数据)` 或失败原因。
//   这一步只依赖分区查找 + mmap + 解析,不涉及任何渲染,风险极低。
//
//   至于"把图画到屏幕上":真屏仍然要等,但**渲染前的准备工作在宿主机
//   就能验完** —— pcpreview 会把真实 LVGL 的渲染结果落成 BMP 帧,
//   所以 dsc 拼得对不对、图层顺序对不对,现在就能逐像素核对。
//   只剩"屏本身"要等(分辨率/色深/时序)。
// ============================================================

// 加载图片镜像。成功返回 true。
// 失败时内部的镜像指针保持为 null,image_blob() 返回 nullptr。
bool image_load();

// 已加载的镜像起始地址(设备上指向 mmap 区域,宿主机指向静态缓冲)。
// 没加载成功时返回 nullptr。配套的长度用 image_blob_len()。
const uint8_t* image_blob();

// 已加载镜像的字节数(未加载时为 0)。
uint32_t image_blob_len();

// 已解析出的镜像索引(未加载时 count 为 0)。
// 上层接渲染时用它按角色找图,不用自己再解析一遍。
const ImageBlobHeader* image_blob_header();

// ------------------------------------------------------------
// 按角色取一张图,并组装成 LVGL 能直接用的 lv_image_dsc_t。
//
// 为什么要这一层:分区里的像素是"裸"的 RGB565/RGB565A8,而 LVGL 要的是
// 带 header 的描述符。两者之间只差一个结构体,但填错了 LVGL 会读越界 ——
// 所以集中在这里填一次,而不是让每个调用点各填一遍。
//
// ★ 零拷贝:返回的 dsc 里 data 直接指向 mmap 的像素(LV_IMAGE_SRC_VARIABLE
//   路径下 LVGL 不会复制像素)。所以返回的指针在整个运行期都有效,
//   不需要(也不能)由调用方释放。
//
// role: 要取的角色(ImageRole)。同一角色多张时取 order 最小的那张。
// out:  调用方提供的 dsc 存储(必须由调用方持有,LVGL 会一直引用它)。
// 返回 true 表示 out 已填好;false 表示没有这个角色(调用方走降级路径)。
//
// 典型用法:
//     static lv_image_dsc_t dsc;              // 必须活到 LVGL 用完
//     if (image_dsc_for_role(ImageRole::Background, &dsc)) {
//       lv_image_set_src(obj, &dsc);
//     }
// ------------------------------------------------------------
bool image_dsc_for_role(ImageRole role, lv_image_dsc_t* out);

// 这个角色的图是否带透明通道(RGB565A8)。给需要区别对待的调用方用。
bool image_role_has_alpha(ImageRole role);
