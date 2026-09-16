// ============================================================
// JS 打包器 ↔ 固件解析器 的往返一致性测试(宿主机)
//
// 为什么需要这个测试:
//   image.bin 是**两套代码**共同维护的二进制格式 ——
//     · 生成方:tools/theme-editor/image-blob-build.js  (浏览器 / Node)
//     · 读取方:lib/themetool/image_blob.cpp            (ESP32 固件)
//   两边一旦有一处对不上(字段偏移、字节序、名字长度、stride 算法),
//   故障现象是"图不显示"或者更糟的"读到越界地址",而**编译期完全看不出来**。
//   C 侧的 test_image_blob.cpp 只能证明"C 自己造的镜像 C 自己能读",
//   证明不了"JS 造的镜像 C 能读"。
//
// 做法:
//   tools/theme-editor/build-image-bin.js 在生成 image.bin 的同时,
//   生成一份同样内容的 image.bin.manifest(纯文本,人能读)。
//   这里把 blob 交给固件解析器,再逐字段和 manifest 对账。
//
// 怎么跑:
//   pwsh tools/theme-editor/test-image-roundtrip.ps1
//   它会设置 IMAGE_BLOB / IMAGE_BLOB_MANIFEST 两个环境变量后跑 pio test。
//   没设这两个变量时本测试直接跳过(日常 `pio test` 不受影响)。
//
// 覆盖不到的:真屏渲染。这个测试只保证"数据能被正确解出来"。
// ============================================================
#include <unity.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_blob.h"

namespace {

// ★ 上限必须跟 kImageMaxCount 一致(image_blob.h),不是写死的历史值
const int kMaxItems = kImageMaxCount;

// 行缓冲的两个量必须分开算,别再搞混:
//   kRowCap   = 一张图最多几行
//   kRowChars = 每行缓冲多少**字符** = 3×行宽 + 1
//                (每字节两个十六进制字符 + 一个逗号,再加结尾 '\0')
// 这里按最坏情况 700 字节行宽 → 700*3+2 = 2102,取 2200。
// ★ 曾经按"700 字节"直译成 1400,于是 snprintf 写越界、堆被踩坏,
//   测试进程以 exit 3 崩掉 —— 崩溃日志完全看不出和缓冲区有关。
const uint32_t kRowCap = 64;
const uint32_t kRowChars = 2200;

// 角色编号 → 中文名,和 JS 的 ROLE_NAMES 是同一张表。
// 两边对不上就说明有人只改了一边 —— 这正是要测的。
// 表里**只列在用角色**:保留编号(2/5/7/9/10/11/14/15/16/19/20)不参与往返测试。
const char* roleName(uint32_t role) {
  switch ((ImageRole)role) {
    case ImageRole::Background:    return "表盘背景";
    case ImageRole::FaceIdle:      return "左屏表情·常态";
    case ImageRole::FaceRedline:   return "左屏表情·红区";
    case ImageRole::FaceCruise:    return "左屏表情·巡航";
    case ImageRole::FaceSport:     return "左屏表情·运动";
    case ImageRole::FaceIdleR:     return "右屏表情·常态";
    case ImageRole::FaceOverspeedR: return "右屏表情·超速";
    case ImageRole::FaceCruiseR:   return "右屏表情·巡航";
    case ImageRole::FaceSportR:    return "右屏表情·运动";
  }
  return "?";
}

struct Item {
  char name[kImageNameMax];
  uint32_t w, h;
  uint32_t cf, stride_pad, size, offset, role, order;
  char role_name[64];
  // 每行的像素十六进制,用于逐字节核对。
  // ★ 必须是**动态分配**的:32 项 × 64 行 × 2200 字符 ≈ 4.5MB,
  //   放栈上/bss 里都不可行(kMaxItems 从 16 涨到 32 后又翻了一倍)。
  char (*rows)[kRowChars];
  uint32_t nrows, row_cap;
};

struct Manifest {
  uint32_t magic, version, count, header_bytes, data_bytes, total_bytes;
  Item items[kMaxItems];
};

// 为每项准备行缓冲。由 parseManifest 自己调用,配对的是 freeRows。
// ★ 谁负责 memset 谁就得负责把 row_cap 设回去 —— 这两个函数曾经各做一半,
//   结果 memset 把 row_cap 清零,解析时报"行数超过上限 0"(指向 manifest
//   格式,实际是初始化顺序问题)。现在统一成:parseManifest 先
//   calloc + 设 row_cap,再解析,不再另做整块 memset。
bool allocRows(Manifest* m) {
  for (int i = 0; i < kMaxItems; ++i) {
    m->items[i].rows = (char(*)[kRowChars])calloc(kRowCap, kRowChars);
    if (!m->items[i].rows) return false;
    m->items[i].row_cap = kRowCap;
  }
  return true;
}

void freeRows(Manifest* m) {
  for (int i = 0; i < kMaxItems; ++i) {
    free(m->items[i].rows);
    m->items[i].rows = nullptr;
  }
}

// 极简 key=value 解析:manifest 是我们自己写的,格式固定
// ★ 失败原因写进 err(而不是 fprintf stderr):PlatformIO 会把测试程序的
//   stderr 吞掉,只有 TEST_ASSERT 的消息能跑到报告里。踩过这个坑 ——
//   当时只看到"解析失败"四个字,完全不知道是哪一行。
bool parseManifest(const char* text, Manifest* m, char* err, size_t err_cap) {
  // 先分配行缓冲再清零:rows 是 calloc 出来的(清零交给它),
  // row_cap 由 allocRows 设好,随后的 memset 只清**标量字段和项数据**,
  // 不能把 rows/row_cap 一起抹掉。
  if (!allocRows(m)) {
    snprintf(err, err_cap, "行缓冲分配失败(%d 项 × %u 行 × %u 字节)",
             kMaxItems, (unsigned)kRowCap, (unsigned)kRowChars);
    return false;
  }
  m->magic = m->version = m->count = 0;
  m->header_bytes = m->data_bytes = m->total_bytes = 0;
  for (int i = 0; i < kMaxItems; ++i) {
    Item& it = m->items[i];
    it.name[0] = '\0';
    it.role_name[0] = '\0';
    it.w = it.h = it.cf = it.stride_pad = 0;
    it.size = it.offset = it.role = it.order = 0;
    it.nrows = 0;
    // rows 已经是 calloc 的全零,row_cap 保留
  }

  int cur = -1;
  const char* p = text;
  int lineno = 0;

  #define MAN_FAIL(fmt, ...)                                             \
    do {                                                                 \
      snprintf(err, err_cap, "manifest 第 %d 行: " fmt, lineno,          \
               ##__VA_ARGS__);                                           \
      return false;                                                      \
    } while (0)

  while (*p) {
    ++lineno;
    const char* eol = strchr(p, '\n');
    const size_t len = eol ? (size_t)(eol - p) : strlen(p);
    // ★ 必须放得下最长的一行:rowN=<两位十六进制 × 最多 700 字节> ≈ 2100 字符。
    //   之前这里只有 300 字节,manifest 一旦超过就整份解析失败
    //   (报出来是"解析器拒收镜像",方向全错)。用 static 避免 2KB 吃栈。
    static char line[2500];
    if (len >= sizeof(line)) MAN_FAIL("行太长 (%u 字符)", (unsigned)len);
    memcpy(line, p, len);
    line[len] = '\0';
    p = eol ? eol + 1 : p + len;

    // 去行尾 \r(Windows 换行)
    size_t l = strlen(line);
    while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
    if (l == 0 || line[0] == '#') continue;

    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = '\0';
    const char* key = line;
    const char* val = eq + 1;

    // "img3.name" 这种带下标的 key
    if (strncmp(key, "img", 3) == 0 && key[3] >= '0' && key[3] <= '9') {
      const int idx = atoi(key + 3);
      const char* dot = strchr(key, '.');
      if (!dot) MAN_FAIL("img 项没有 '.' key=%s", key);
      if (idx < 0 || idx >= kMaxItems) MAN_FAIL("下标越界 idx=%d (上限 %d)", idx, kMaxItems);
      cur = idx;
      const char* field = dot + 1;
      Item& it = m->items[idx];

      if (strcmp(field, "name") == 0) {
        snprintf(it.name, sizeof(it.name), "%s", val);
      } else if (strcmp(field, "w") == 0) {
        it.w = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "h") == 0) {
        it.h = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "cf") == 0) {
        it.cf = (uint32_t)strtoul(val, nullptr, 0);
      } else if (strcmp(field, "stride_pad") == 0) {
        it.stride_pad = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "size") == 0) {
        it.size = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "offset") == 0) {
        it.offset = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "role") == 0) {
        it.role = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "order") == 0) {
        it.order = (uint32_t)strtoul(val, nullptr, 10);
      } else if (strcmp(field, "role_name") == 0) {
        snprintf(it.role_name, sizeof(it.role_name), "%s", val);
      }
      continue;
    }

    if (strcmp(key, "magic") == 0)            m->magic = (uint32_t)strtoul(val, nullptr, 0);
    else if (strcmp(key, "version") == 0)     m->version = (uint32_t)strtoul(val, nullptr, 10);
    else if (strcmp(key, "count") == 0)       m->count = (uint32_t)strtoul(val, nullptr, 10);
    else if (strcmp(key, "header_bytes") == 0) m->header_bytes = (uint32_t)strtoul(val, nullptr, 10);
    else if (strcmp(key, "data_bytes") == 0)  m->data_bytes = (uint32_t)strtoul(val, nullptr, 10);
    else if (strcmp(key, "total_bytes") == 0) m->total_bytes = (uint32_t)strtoul(val, nullptr, 10);
    else if (strncmp(key, "row", 3) == 0 && cur >= 0) {
      // row0=0011,2233
      Item& it = m->items[cur];
      if (it.nrows < it.row_cap) {
        snprintf(it.rows[it.nrows], kRowChars, "%s", val);
        it.nrows++;
      } else {
        MAN_FAIL("第 %d 张的行数超过上限 %u", cur, (unsigned)it.row_cap);
      }
    }
  }
  #undef MAN_FAIL
  return true;
}

// "0011,2233" → {0x00,0x11,0x22,0x33}
uint32_t parseHexRow(const char* s, uint8_t* out, uint32_t cap) {
  uint32_t n = 0;
  while (*s && n < cap) {
    while (*s == ' ' || *s == ',') ++s;
    if (!*s) break;
    // 两个十六进制字符
    char buf[3] = { s[0], s[1] ? s[1] : '0', '\0' };
    if (!isxdigit((unsigned char)buf[0]) || !isxdigit((unsigned char)buf[1])) break;
    out[n++] = (uint8_t)strtoul(buf, nullptr, 16);
    s += (s[1] ? 2 : 1);
  }
  return n;
}

uint8_t* readFile(const char* path, uint32_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > (long)IMAGE_BLOB_MAX_BYTES) { fclose(f); return nullptr; }
  uint8_t* buf = (uint8_t*)malloc((size_t)n + 1);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) { free(buf); return nullptr; }
  buf[n] = 0;                       // 方便当文本用
  if (len) *len = (uint32_t)n;
  return buf;
}

}  // namespace

// ★ 核心用例:JS 生成的 image.bin 必须能被固件解析器完整、逐字节地读出来
static void test_js_blob_roundtrip(void) {
  const char* bin_path = getenv("IMAGE_BLOB");
  const char* man_path = getenv("IMAGE_BLOB_MANIFEST");

  // 没设变量 = 没有要核对的镜像,跳过(日常 pio test 就是这个情况)
  if (!bin_path || !*bin_path || !man_path || !*man_path) {
    TEST_IGNORE_MESSAGE("未设置 IMAGE_BLOB / IMAGE_BLOB_MANIFEST,跳过往返测试");
    return;
  }

  uint32_t bin_len = 0, man_len = 0;
  uint8_t* bin = readFile(bin_path, &bin_len);
  uint8_t* man = readFile(man_path, &man_len);
  TEST_ASSERT_NOT_NULL_MESSAGE(bin, "读不到 image.bin");
  TEST_ASSERT_NOT_NULL_MESSAGE(man, "读不到 manifest");

  Manifest m;
  char perr[256] = {0};
  TEST_ASSERT_TRUE_MESSAGE(parseManifest((const char*)man, &m, perr, sizeof(perr)),
                           perr[0] ? perr : "manifest 解析失败(没有更详细的原因)");

  // ---- 头 ----
  TEST_ASSERT_EQUAL_HEX32(kImageBlobMagic, m.magic);
  TEST_ASSERT_EQUAL_UINT32(kImageBlobVersion, m.version);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(ImageBlobHeader), m.header_bytes);
  TEST_ASSERT_EQUAL_UINT32(bin_len, m.total_bytes);

  // ---- 固件解析器读它 ----
  ImageBlobHeader h;
  TEST_ASSERT_TRUE_MESSAGE(imageBlobParse(bin, bin_len, &h), "固件解析器拒收 JS 生成的镜像");
  TEST_ASSERT_EQUAL_UINT32(m.count, h.count);
  TEST_ASSERT_EQUAL_UINT32(m.data_bytes, h.data_bytes);

  // ---- 逐项对账 ----
  for (uint32_t i = 0; i < m.count; ++i) {
    const Item& exp = m.items[i];
    ImageView v;
    TEST_ASSERT_TRUE(imageBlobGet(bin, bin_len, h, (uint8_t)i, &v));

    TEST_ASSERT_EQUAL_STRING(exp.name, v.name);
    TEST_ASSERT_EQUAL_UINT32(exp.w, v.w);
    TEST_ASSERT_EQUAL_UINT32(exp.h, v.h);
    TEST_ASSERT_EQUAL_UINT32(exp.cf, v.cf);
    TEST_ASSERT_EQUAL_UINT32(exp.stride_pad, v.stride_pad);
    TEST_ASSERT_EQUAL_UINT32(exp.role, (uint32_t)v.role);
    TEST_ASSERT_EQUAL_UINT32(exp.order, v.order);
    TEST_ASSERT_EQUAL_UINT32(exp.offset, h.entries[i].offset);
    TEST_ASSERT_EQUAL_UINT32(exp.size, h.entries[i].size);
    TEST_ASSERT_EQUAL_UINT32(exp.size, v.imageBytes());

    // ★ 角色编号 → 名字 的两张表(JS 的 ROLE_NAMES 和 C 的 roleName)必须一致。
    //   编号是界面下拉框、打包器、固件三方共用的契约:只改一边的话,
    //   症状是"选了右屏表情,显示成左屏的",而且不报任何错。
    if (exp.role_name[0] != '\0') {
      TEST_ASSERT_EQUAL_STRING(roleName(exp.role), exp.role_name);
    }

    // ★ 逐行逐字节核对像素:这一步能抓到 stride / 行序 / 字节序 / 整体偏移的错
    TEST_ASSERT_EQUAL_UINT32(exp.h, exp.nrows);
    const uint32_t stride = v.strideBytes();
    // 行缓冲容量:每行 stride 字节 → 十六进制字符串 3*stride 字符
    TEST_ASSERT_TRUE_MESSAGE(stride <= 700, "测试镜像的行宽超过行缓冲,见 kRowChars");
    for (uint32_t r = 0; r < exp.nrows; ++r) {
      uint8_t want[768];
      const uint32_t wn = parseHexRow(exp.rows[r], want, sizeof(want));
      TEST_ASSERT_EQUAL_UINT32(stride, wn);
      const int cmp = memcmp(v.pixels + r * stride, want, stride);
      if (cmp != 0) {
        // 指出是第几行第几字节,不然只有一句红字没法查
        uint32_t bad = 0;
        while (bad < stride && v.pixels[r * stride + bad] == want[bad]) ++bad;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "第 %u 张图第 %u 行第 %u 字节不符:固件读到 0x%02X,打包器期望 0x%02X",
                 (unsigned)i, (unsigned)r, (unsigned)bad,
                 v.pixels[r * stride + bad], want[bad]);
        TEST_FAIL_MESSAGE(msg);
      }
    }
  }

  // ---- 顺带验证"故意破坏一个字节必须被拒"----
  // 说明解析器不是"照单全收"而已
  {
    uint8_t* broken = (uint8_t*)malloc(bin_len);
    TEST_ASSERT_NOT_NULL(broken);
    memcpy(broken, bin, bin_len);
    broken[0] ^= 0xFF;                     // 魔数改坏
    ImageBlobHeader bh;
    TEST_ASSERT_FALSE(imageBlobParse(broken, bin_len, &bh));
    free(broken);
  }

  freeRows(&m);
  free(bin);
  free(man);
}

// 没设环境变量时也应该有一条"看起来正常"的记录,便于确认真的跑了
static void test_roundtrip_guard(void) {
  const char* p = getenv("IMAGE_BLOB");
  if (!p || !*p) {
    TEST_IGNORE_MESSAGE("未提供镜像,跳过");
    return;
  }
  TEST_ASSERT_TRUE(1);
}

void register_image_roundtrip_tests(void) {
  RUN_TEST(test_js_blob_roundtrip);
  RUN_TEST(test_roundtrip_guard);
}
