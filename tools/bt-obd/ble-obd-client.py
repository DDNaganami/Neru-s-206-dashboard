#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble-obd-client.py —— 把 BLE OBD 诊断头当串口用(**读取这一侧唯一实测可用的实现**)

============================================================================
为什么要有它(2026-09-27 实测):
  同目录的 `ble-obd-client.ps1` 把**写入**打通了(见该文件头部),但**读取**那一侧
  在 PowerShell 里做不到 —— WinRT 的 ValueChanged 回调交给脚本的是**未投影的
  System.__ComObject**,既转不成 IBuffer、也读不出 Length;轮询 ReadValueAsync
  永远回 20 个 0 字节。用自己声明的 COM ABI(IBufferByteAccess)去读会让
  PowerShell 进程直接崩掉。根因是 Add-Type 的 csc 无法引用 WinRT 元数据
  (Windows.Storage.winmd),所以这事在纯 PowerShell 里不划算。

  bleak 把 BLE 写/通知都包好了,和这个头**开箱即通**。所以:
    · 写入路径的经验 → 看 ble-obd-client.ps1(给板上 NimBLE 固件抄)
    · 真正收数据     → 用本脚本

用法:
  & "C:\\Users\\<你>\\AppData\\Local\\Python\\pythoncore-3.14-64\\python.exe" tools\\bt-obd\\ble-obd-client.py
  ... ble-obd-client.py "ATZ,ATE0,ATSP0,0100"

★ 只发标准 ELM327 **读**指令。绝不发清故障码/写类指令(04、2F 等)。
  这台车天天开,不许乱动 ECU 状态。

实测记录(2026-09-27,标致 206,点火到 ON):
  ATZ   -> 'ELM327 v1.5\\r\\n>'
  ATE0  -> 'OK\\r\\n>'
  ATSP0 -> 'OK\\r\\n>'
  ATRV  -> '13.4V\\r\\n>'                       (电瓶电压,13.4V 说明在充电/发动机在转)
  ATDP  -> 'AUTO,ISO 14230-4 (KWP FAST)\\r\\n>'  ← ★ 206 是 KWP FAST,**不是 CAN**
           (注意:紧跟 ATSP0 之后立刻查可能只回 'AUTO' —— 协议还没探测出来,
            等 0100 跑过一次之后再查就准了。)
  0100  -> 'SEARCHING...\\r41 00 BE 3E B0 11 \\r41 00 98 18 00 00 \\r\\r>'
           ★ 正是预期的位图;两个 ECU 各答一条,后一条是 29 位格式。
             不强制 ATSP0 时也可能直接回 '41 00 BE 3E B0 11'。
  010C  -> '41 0C 0F 20 \\r\\r>'   (转速 (0x0F20)/4 = 968 rpm;另一帧 0x1E CD ≈ 1971)
  010D  -> '41 0D 00 \\r\\r>'      (车速 0)
  0105  -> '41 05 7F \\r\\r>'      (冷却液 0x7F-40 = 87°C)
  010F  -> '41 0F 6C \\r\\r>'      (进气温度 0x6C-40 = 68°C)

★ 两个必须知道的特性:
  1. `0100` 第一次很可能**只回 "SEARCHING..." 不跟数据** —— 那是诊断头在自动
     搜协议(KWP FAST 上尤其慢)。**必须等**,给足 10~12 秒,它会补上
     '41 00 BE 3E B0 11'。别当成失败。
  2. 回答是**分片**来的(ATZ 实测被切成 1 字节 + 13 字节两片),
     要用 '>' 提示符判断结束,不能假设一帧一答。
"""

import asyncio
import sys
import time

# Windows PowerShell 5.1 的控制台默认是 GBK,中文输出会变乱码;强制成 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("缺少 bleak。装法(可能要代理):")
    print('  $env:HTTPS_PROXY="http://127.0.0.1:7890"; python -m pip install bleak')
    sys.exit(2)

ADDR = "AABBCC122233"
NAME = "OBDBLE"
RX = "0000fff1-0000-1000-8000-00805f9b34fb"   # 0x12 = 读 + 通知 ← 回答从这来
TX = "0000fff2-0000-1000-8000-00805f9b34fb"   # 0x0C = 写 + 无响应写 ← 指令往这写

DEFAULT_CMDS = ["ATZ", "ATE0", "ATL0", "ATH0", "ATSP0", "0100"]


def log(*a):
    print(*a, flush=True)


async def find_target(retries=6, scan_s=6.0):
    """这个诊断头空闲会自己关机/停止广播,所以扫描要重试。"""
    for i in range(retries):
        devs = await BleakScanner.discover(timeout=scan_s)
        t = next((d for d in devs
                  if (d.address or "").upper().replace(":", "") == ADDR), None)
        if t is not None:
            log(f"找到 {t.address}  {t.name}")
            return t
        log(f"  第 {i+1} 次扫描没看到 {NAME},重试...")
        await asyncio.sleep(2.0)
    return None


async def main():
    cmds = DEFAULT_CMDS
    if len(sys.argv) > 1:
        raw = " ".join(sys.argv[1:])
        cmds = [c for c in raw.replace(",", " ").replace(";", " ").split() if c]

    target = await find_target()
    if target is None:
        log(f"★ 找不到 {NAME} / {ADDR}。诊断头插好了吗?钥匙拧到 ON 了吗?")
        return 4

    acc = []

    def on_notify(_handle, data: bytearray):
        acc.append(bytes(data))

    async with BleakClient(target) as c:
        log(f"已连接 = {c.is_connected}")
        await c.start_notify(RX, on_notify)
        log("已在 FFF1 上订阅通知(CCCD=Notify)")

        async def send(cmd, wait):
            acc.clear()
            payload = (cmd + "\r").encode("ascii")   # ★ ELM327 必须回车收尾
            # 这个特征带 write-without-response;两种模式这个头都接受
            await c.write_gatt_char(TX, payload, response=False)
            t0 = time.time()
            while time.time() - t0 < wait:
                await asyncio.sleep(0.1)
                if b">" in b"".join(acc):
                    break
            out = b"".join(acc).decode("ascii", "replace")
            log(f"  {cmd:<7} -> {out!r}")
            return out

        log("=== 握手 (AT 指令,不需要 ECU) ===")
        for cmd in cmds:
            # ★ ATZ 要长一点;0100 这类走 ECU 的要给足 12 秒等 SEARCHING 补数据
            wait = 4.0 if cmd.upper() == "ATZ" else (12.0 if cmd[:2] in ("01", "02", "03") else 2.5)
            await send(cmd, wait)
            await asyncio.sleep(0.3)

        try:
            await c.stop_notify(RX)
        except Exception:
            pass

    log("")
    log("完成。★ 板上 NimBLE 要照抄的:服务 FFF0 / 通知 FFF1 / 写 FFF2;")
    log("  先 CCCD=Notify 订阅 FFF1,再往 FFF2 写 ASCII+CR;")
    log("  按 '>' 收尾(回答分片);'0100' 前几秒只回 SEARCHING... 是正常的。")
    return 0


sys.exit(asyncio.run(main()))
