#pragma once
#include <stdint.h>
#include "van_source.h"

// VAN 离线回放:从串口贴十六进制帧喂给 VanSource::onPacket()。
// 实车接收发器之前,先把抓到的帧(逻辑分析仪/别人分享)粘过来联调,不必先焊板。
//
// 行格式:VAN <iden 3 位十六进制> <数据字节 十六进制…>,大小写不限。
// iden 写满 3 位(824 写 824,0x7C 写 07C);字节 2 位一组,空格可有可无。例:
//   VAN 824 18 F8 27 10 00 00 00
//   VAN82418F82710000000
//
// 解析成功返回 true 并填好 pkt(含 rx_ms = now_ms);失败返回 false。
bool parseVanReplayLine(const char* line, VanPacket* pkt, uint32_t now_ms);
