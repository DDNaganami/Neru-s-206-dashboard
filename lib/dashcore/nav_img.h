#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ============================================================
// 地图图片分片（阶段 B2，魔数 0xA9）—— 手机端截一张 JPEG，板上重组后当背景贴图
//
// 为什么独立魔数：和 0xA7（快照）/0xA8（路线）共用同一条字节流，
// 分派只按魔数走 ⇒ 老的两种帧一个字节都不用改（同一套纪律）。
//
// 分片格式（小端）：
//   +0  magic 0xA9
//   +1  ver   1
//   +2  frame_id  （每张新图 +1）
//   +3  idx       （0..total-1）
//   +4  total     （<= kNavImgMaxChunks）
//   +5..6  len    （u16 LE，本片数据字节数，<= kNavImgChunkMax）
//   +7..   data[len]
//   ...  crc16     （覆盖整片，复用 nav_crc16）
//
// ★ 为什么叫"重组"而不是"拼包"：BLE 一个 notify 未必一片、一片也未必一个 notify，
//   所以片是**自描述**的（frame_id/idx/total 都在片头），丢了哪片、跨了帧都能看出来。
// ============================================================

static const uint8_t kNavImgMagic   = 0xA9u;
static const uint8_t kNavImgVersion = 1u;
static const size_t  kNavImgChunkMax = 256u;     // 单片的**数据**上限
static const uint8_t kNavImgMaxChunks = 200u;
static const size_t  kNavImgBufferMax = 32u * 1024u;   // 一张图（JPEG）上限

struct NavImgChunk {
  uint8_t        frame_id = 0;
  uint8_t        idx = 0;
  uint8_t        total = 0;
  uint16_t       len = 0;
  const uint8_t* data = nullptr;   // 指向输入缓冲内部（零拷贝）
};

// 打包一片；成功返回片长，失败 0
size_t nav_img_pack_chunk(const NavImgChunk& c, uint8_t* out, size_t cap);

// 解包一片（data 指向 in 内部，调用方保证 in 生命周期）；任何不合规返回 false
bool nav_img_unpack_chunk(const uint8_t* in, size_t len, NavImgChunk* out);

// ---- 重组器（调用方给缓冲：板上放 PSRAM，宿主机测试给数组）----
struct NavImgRx {
  uint8_t* buf = nullptr;
  size_t   cap = 0;
  uint8_t  frame_id = 0xFF;
  uint8_t  total = 0;
  uint8_t  got = 0;
  size_t   len = 0;
  bool     complete = false;
  uint8_t  seen[(kNavImgMaxChunks + 7u) / 8u] = {0};   // 每片是否已收（位图）
};

void nav_img_rx_init(NavImgRx& rx, uint8_t* buf, size_t cap);

// 喂一片；返回 true = **这一片让整张图收齐了**（此后 buf/len 有效）
// 规则：frame_id 变了 ⇒ 丢掉未完成的旧图重新开始；重复片忽略；越界一律丢弃
bool nav_img_rx_push(NavImgRx& rx, const NavImgChunk& c);
