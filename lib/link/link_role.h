#pragma once
#include <stdint.h>

#include "link_frame.h"

// ============================================================
// 双板链路协议 v1 —— **角色**（§5）
//
// 契约：**编译期 env 是唯一权威**，运行期自检**只报警、绝不改角色**。
//   ① 角色冲突：收到**对端 ROLE == 本机 ROLE** 的帧 ⇒ role_conflict=1 + 一行日志，
//      **该帧丢弃**（§5 定案 L12；★ 不升级为"只收不发" —— L1 已于 2026-09-23 关闭：
//      43/44 上不对打，两块同角色互发**不是电气风险**）；
//   ② 主板侧：`-DVAN_PHY_GPIO=1` 但 `van: edges=` 恒 0，同时又收到了 DATA 帧
//      ⇒ 打一行"本机像从板"；
//   ③ 从板侧：本地看到 VAN 边沿（说明这块板才该是接了收发器的主板）⇒ 打一行。
//   本文件把这三条判据都写成**纯函数**（§5 的"把所有'我是谁'的判断收敛到一个函数里"
//   —— 将来真要合并成一个 env 时，把宏改成变量即可，协议与帧格式都不用动）。
//   判决结果的**日志与计数**由调用方做（从板侧经 STATUS.flags 报给主板，§2/§3）。
//
// ★ 本轮**不动 `platformio.ini`**（§5：两个角色 env 的名字与写法待定、
//   要等最终 2.8" 板（微雪 2.8C）到货再落地；那节的"本轮不做"是明确的）。
//   所以这里只留**宏 + 判据**：谁都没 -D 的时候走下面的默认值。
// ============================================================

#ifndef LINK_ROLE
// ★ 默认值 = 0 = **从板（左）**。三条理由，按重要性排：
//   ① **0 就是零值** —— 静态初始化、清零的结构体、忘记 -D 的构建，全都落在
//      **被动侧**；被动侧不驱动链路，不会悄悄变成第二个发送方。
//   ② **漏配的后果最轻、且立刻可见**：两块都漏配 ⇒ 谁都不发，而从板的
//      TICK 超时三档（>100 ms / >500 ms / >3 s）马上会把"等不到主板"写进日志；
//      反过来默认主板，则可能出现两个发送方 —— §5 的 role_conflict 能发现它，
//      但那要等帧真的发出去才知道。
//   ③ 与 §2 的 ROLE 字段编号一致（1 = 主板/右，0 = 从板/左），不留第三个值。
#define LINK_ROLE 0
#endif

static_assert(LINK_ROLE == 0 || LINK_ROLE == 1,
              "LINK_ROLE 只能是 0(从板/左) 或 1(主板/右)—— §2 的 ROLE 字段就这两位取值，"
              "别的值一定是 -D 写错了(宁可在编译期炸掉，也不要运行期猜角色)");

namespace dashlink {

// 本机角色：编译期定死，运行期**没有任何**代码能改它（只报警、不改角色，§5）。
static const uint8_t kLocalRole = (uint8_t)LINK_ROLE;

inline const char* roleName(uint8_t role) {
  return role == kRoleMaster ? "master(A/right)" : "slave(B/left)";
}

// §5 ①：对端 ROLE 与本机 ROLE 相同 ⇒ 角色冲突：**丢帧 + 报警**。
// 只比较等值：ROLE 字段按 §2 只有 0/1 两种取值，别的值由整帧 CRC 兜着。
inline bool roleConflict(uint8_t frame_role, uint8_t local_role) {
  return frame_role == local_role;
}

// 对端角色（0 ↔ 1）。§2 的 ROLE 只有两种取值，所以"对端"就是取反。
inline uint8_t peerRoleOf(uint8_t role) {
  return (uint8_t)(role == kRoleMaster ? kRoleSlave : kRoleMaster);
}

// ============================================================
// ★★ **单板回环**的两个角色（`src/link_loopback.cpp` 用；这里是唯一出处，
//    宿主机用例读的是同一份 —— test_link_phy.cpp 的背靠背用例与角色口径用例）
//
// 一块板同时扮演两端时，上面那条 §5 ① 就是最容易踩的坑：
//   · **发端**写进帧里的 ROLE = **本机**角色（`kLoopbackTxRole`）—— 与生产固件
//     一字不差，线上字节不变；
//   · **收端**必须扮演**对端**（`kLoopbackRxRole = peerRoleOf(kLocalRole)`）——
//     回环里回来的字节就是本机 TX 自己的字节，而真实链路上收到这些字节的是
//     **对端那块板**，所以回环的收端要按对端的身份做 §5 ① 自检。
//
// ★ 两边都写本机角色会怎样（2026-09-23 上板实测，症状极具误导性）：回环里回来的
//   **每一帧**都满足 `frame_role == local_role` ⇒ 帧在 `decodeFrame()` 返回 **Ok
//   之后**被丢（`LinkRx::advance()` 的 role_conflict 分支），于是串口上看到的是
//   "收到 2841 字节 / 0 帧 / CRC 错 0 / bad_len 0 / 未知类型 0 / 噪声 1 字节" ——
//   **所有"帧坏了"的计数都不动**，只有 role_conflict 在涨（而它当时还没进汇总）。
//   ⇒ 别再把它当成"PHY 或者分帧坏了"去查。
//
//   ★ 为什么收端扮演对端**不是**"绕开 §5 ①"：回环里回来的字节在真实链路上是
//     对端那块板收到的，所以按对端身份自检才是这条链路的实际形态；反过来说，
//     §5 ① 的判据一行都没动（两块板刷了同一份固件时照样丢帧 + 报警）。
// ============================================================
static const uint8_t kLoopbackTxRole = kLocalRole;
static const uint8_t kLoopbackRxRole = peerRoleOf(kLocalRole);

// §5 ②：主板侧的自检 —— "本机像从板"。
// 判据（三个都要成立）：本机是主板角色、开着 VAN 物理层却一个边沿都没有、
// 而且已经收到了对端的 DATA 帧（说明这条链路上有人在发数据）。
inline bool masterSelfCheckLooksLikeSlave(uint8_t local_role, bool van_phy_enabled,
                                          uint32_t van_edges, bool got_data_frame) {
  return local_role == kRoleMaster && van_phy_enabled && van_edges == 0u && got_data_frame;
}

// §5 ③：从板侧的自检 —— "这块板才该是主板"。
// 判据：本机是从板角色，却在本地看到了 VAN 边沿（说明收发器接在这块板上）。
inline bool slaveSelfCheckLooksLikeMaster(uint8_t local_role, uint32_t van_edges) {
  return local_role == kRoleSlave && van_edges > 0u;
}

}  // namespace dashlink
