#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
obd-ble-sim.py -- make THIS PC pretend to be the car's BLE OBD adapter.

Why it exists
-------------
The dashboard's BLE path (NimBLE *central* -> adapter's GATT server) could only be
tested in the car, because the real ELM327 adapter lives there. That made every
bring-up question ("is it RAM? is it the radio? is it the adapter?") cost a trip.
This script turns the desktop into a stand-in adapter: same service, same
characteristics, same ASCII question/answer protocol, so the board can connect
and read PIDs on the bench.

What it imitates (measured on the real adapter, see docs/BLE-OBD.md):
  advertised name        'OBDBLE' on the real one; here the PC's Bluetooth name
                         (the firmware filters on the SERVICE UUID, not the name)
  service                0000fff0-0000-1000-8000-00805f9b34fb
  notify  (adapter->car) 0000fff1  props = read + notify      <- answers go here
  write   (car->adapter) 0000fff2  props = write + write-w/o-response <- commands
  answers                ASCII, CR-terminated, end with '>' prompt, and they
                         arrive FRAGMENTED (--chunk, default 13 bytes)

Implementation notes (why Python and not PowerShell)
-----------------------------------------------------
  * Windows *can* act as a BLE peripheral: BluetoothAdapter.IsPeripheralRoleSupported
    is True on the Intel adapter here, and GattServiceProvider works.
  * Windows PowerShell 5.1 CANNOT: Register-ObjectEvent refuses WinRT events
    ("Windows PowerShell cannot subscribe to Windows RT events"), and a GATT
    server without a WriteRequested handler cannot receive commands. Add-Type
    cannot reference WinRT metadata either (there is no Windows.winmd to point at).
  * So the server runs on pywinrt (pip, cp314 wheels exist). Events arrive on a
    foreign thread; each is handed to the asyncio loop with call_soon_threadsafe.

Usage
-----
  py = C:\\Users\\<you>\\AppData\\Local\\Python\\pythoncore-3.14-64\\python.exe

  # constant values (bench idle)
  & $py tools\\bt-obd\\obd-ble-sim.py --mode idle --rpm 900 --coolant 88 --speed 0 --intake 40

  # needles sweeping, so the whole UI chain can be watched
  & $py tools\\bt-obd\\obd-ble-sim.py --mode sweep

  # replay a recorded in-car session (real ECU bytes)
  & $py tools\\bt-obd\\obd-ble-sim.py --mode replay --csv tools\\serial-capture\\drive-2026-09-22-obd.csv

  # prove what a PASSIVE scanner sees (the board scans passively)
  & $py tools\\bt-obd\\obd-ble-sim.py --selftest

  # live control while it runs: write key=value lines into the control file
  & $py tools\\bt-obd\\obd-ble-sim.py --control sim-control.txt
      (then:  Set-Content sim-control.txt 'rpm=3000'; Add-Content ... 'speed=60')

Fault injection (to exercise the dash's fallback paths):
  --drop-after 20     stop advertising after 20 s   (adapter "unplugged")
  --kill-after 45     stop notifying after 45 s     (adapter alive but mute)
  --fail-pids 0105    answer 'NO DATA' for a PID
  --search-first      first 0100 answers only 'SEARCHING...' (KWP FAST warm-up)
  --latency-ms 300    slow answers

Exit: 0 normal, 2 environment/argument problem, 3 GATT setup failed.
"""

import argparse
import asyncio
import csv
import math
import os
import re
import sys
import time
import uuid

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
WRITE_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"

# Supported-PID bitmap reported for 0100. This is the REAL byte pattern read from
# the car (docs/BLE-OBD.md): PIDs 01,04,05,06,07,0B,0C,0D,0E,0F,11,13,14,15...
# 0x0C rpm / 0x0D speed / 0x05 coolant / 0x0F intake are all in it.
DEFAULT_BITMAP = "BE 3E B0 11"

PID_BYTES = {0x05: 1, 0x0C: 2, 0x0D: 1, 0x0F: 1}


def log(msg, quiet=False):
    if quiet:
        return
    print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


# ---------------------------------------------------------------------------
# WinRT imports (kept in one place so a missing package gives a clear message)
# ---------------------------------------------------------------------------
def winrt_imports():
    try:
        from winrt import runtime
        from winrt.windows.devices.bluetooth import BluetoothAdapter
        from winrt.windows.devices.bluetooth.genericattributeprofile import (
            GattServiceProvider,
            GattLocalCharacteristicParameters,
            GattCharacteristicProperties,
            GattServiceProviderAdvertisingParameters,
        )
        from winrt.windows.storage.streams import DataReader, DataWriter
    except ImportError as e:
        print("missing pywinrt package: %s" % e)
        print("install with:")
        print("  python -m pip install winrt-runtime winrt-Windows.Devices.Bluetooth "
              "winrt-Windows.Devices.Bluetooth.GenericAttributeProfile "
              "winrt-Windows.Storage.Streams winrt-Windows.Foundation "
              "winrt-Windows.Foundation.Collections")
        return None
    return dict(runtime=runtime, BluetoothAdapter=BluetoothAdapter,
                GattServiceProvider=GattServiceProvider,
                GattLocalCharacteristicParameters=GattLocalCharacteristicParameters,
                GattCharacteristicProperties=GattCharacteristicProperties,
                GattServiceProviderAdvertisingParameters=GattServiceProviderAdvertisingParameters,
                DataReader=DataReader, DataWriter=DataWriter)


def buffer_to_bytes(buf, DataReader):
    """IBuffer -> bytes (pywinrt has no implicit conversion)."""
    n = int(buf.length)
    if n == 0:
        return b""
    reader = DataReader.from_buffer(buf)
    arr = bytearray(n)
    reader.read_bytes(arr)
    return bytes(arr)


def bytes_to_buffer(data, DataWriter):
    w = DataWriter()
    w.write_bytes(data)
    return w.detach_buffer()


# ---------------------------------------------------------------------------
# ELM327 emulation
# ---------------------------------------------------------------------------
class Elm327:
    """The ASCII question/answer layer only; values come from a profile."""

    def __init__(self, args):
        self.args = args
        self.bitmap = DEFAULT_BITMAP
        self.fail = set(x.strip().upper() for x in args.fail_pids.split(",") if x.strip())
        self.search_pending = bool(args.search_first)
        self.cmds = 0
        self.unknown = 0
        self.rpm = float(args.rpm)
        self.coolant = float(args.coolant)
        self.speed = float(args.speed)
        self.intake = float(args.intake)
        self.replay = Replay(args.csv) if args.mode == "replay" else None
        self.t0 = time.time()

    def update(self, t):
        """Advance the synthetic values (mode sweep) / pick replay row."""
        if self.args.mode == "sweep":
            # 0..1..0 over 20 s for rpm and speed, in antiphase for a lively screen
            ph = (math.sin(t / 20.0 * 2 * math.pi) + 1.0) / 2.0
            self.rpm = 800.0 + 5200.0 * ph
            self.speed = 130.0 * (1.0 - ph)
        if self.replay is not None:
            self.replay.seek(t)

    def answer(self, cmd):
        """cmd: one line, already stripped of CR/LF and upper-cased."""
        c = cmd.strip().upper()
        if not c:
            return ""
        self.cmds += 1

        if c.startswith("AT"):
            return self._at(c)
        if re.fullmatch(r"01[0-9A-F]{2}", c):
            return self._pid(int(c[2:4], 16))
        if re.fullmatch(r"0[0-9]", c) or c in ("03", "07", "0A"):
            return "NO DATA\r"
        self.unknown += 1
        return "?\r"

    def _at(self, c):
        if c in ("ATZ", "ATWS"):
            return "ELM327 v1.5\r\n"
        if c == "ATI":
            return "ELM327 v1.5\r"
        if c == "ATRV":
            return "12.6V\r"
        if c == "ATDP":
            return "AUTO, ISO 14230-4 (KWP FAST)\r"
        if c == "ATDPN":
            return "A5\r"
        if c.startswith("ATSP"):
            return "OK\r"
        return "OK\r"

    def _pid(self, pid):
        if ("01%02X" % pid) in self.fail:
            return "NO DATA\r"
        if pid == 0x00:
            head = ""
            if self.search_pending:
                head = "SEARCHING...\r"
                self.search_pending = False
            return head + "41 00 " + self.bitmap + " \r"
        if pid not in PID_BYTES:
            return "NO DATA\r"
        if self.replay is not None:
            raw = self.replay.raw_for(pid)
            if raw is not None:
                return raw
        if pid == 0x0C:
            v = int(max(0.0, min(16383.75, self.rpm)) * 4.0)
            return "41 0C %02X %02X \r" % ((v >> 8) & 0xFF, v & 0xFF)
        if pid == 0x05:
            return "41 05 %02X \r" % ((int(self.coolant) + 40) & 0xFF)
        if pid == 0x0F:
            return "41 0F %02X \r" % ((int(self.intake) + 40) & 0xFF)
        if pid == 0x0D:
            return "41 0D %02X \r" % (int(self.speed) & 0xFF)
        return "NO DATA\r"


class Replay:
    """Serve the raw ECU answers recorded during a real drive."""

    def __init__(self, path):
        self.rows = {}
        self.duration = 0.0
        with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
            for row in csv.DictReader(fh):
                try:
                    t = float(row.get("t_s") or 0.0)
                    pid = (row.get("pid") or "").strip().upper()
                    ok = (row.get("ok") or "1").strip()
                    raw = row.get("raw") or ""
                except Exception:
                    continue
                if ok not in ("1", "True", "true"):
                    continue
                m = re.search(r"41\s+([0-9A-Fa-f]{2}(?:\s+[0-9A-Fa-f]{2})*)", raw)
                if not m:
                    continue
                p = int(pid[2:4], 16) if re.fullmatch(r"01[0-9A-F]{2}", pid) else None
                if p is None:
                    continue
                self.rows.setdefault(p, []).append((t, "41 " + m.group(1).upper() + " \r"))
                self.duration = max(self.duration, t)
        self.t = 0.0
        self.idx = {}

    def seek(self, t):
        self.t = t

    def raw_for(self, pid):
        rows = self.rows.get(pid)
        if not rows:
            return None
        i = self.idx.get(pid, 0)
        while i + 1 < len(rows) and rows[i + 1][0] <= self.t:
            i += 1
        self.idx[pid] = i
        return rows[i][1]


# ---------------------------------------------------------------------------
# GATT server
# ---------------------------------------------------------------------------
class Sim:
    def __init__(self, args, api):
        self.args = args
        self.api = api
        self.loop = None
        self.pending = None          # asyncio.Queue of GattWriteRequestedEventArgs
        self.line_buf = bytearray()  # command fragments are glued until CR (UART-like)
        self.notify_char = None
        self.provider = None
        self.elm = Elm327(args)
        self.subscribers = 0
        self.notifies = 0
        self.writes = 0
        self.errors = 0
        self.t_start = time.time()
        self.muted = False
        self.advertising = False

    # -- events arrive on foreign threads -> hop onto the loop --------------
    def _on_write(self, sender, args):
        try:
            self.loop.call_soon_threadsafe(self.pending.put_nowait, args)
        except Exception:
            pass

    def _on_subs(self, sender, args):
        try:
            cl = list(sender.subscribed_clients)
            self.loop.call_soon_threadsafe(self._set_subs, len(cl))
        except Exception:
            pass

    def _set_subs(self, n):
        self.subscribers = n
        log("client %s  (subscribed clients now %d)" % ("subscribed" if n else "unsubscribed", n), self.args.quiet)

    # -- setup -------------------------------------------------------------
    async def setup(self):
        g = self.api
        self.loop = asyncio.get_running_loop()
        self.pending = asyncio.Queue()

        res = await g["GattServiceProvider"].create_async(uuid.UUID(SERVICE_UUID))
        self.provider = res.service_provider
        if self.provider is None:
            print("GattServiceProvider.create_async failed: status=%s" % res.status)
            return False
        service = self.provider.service

        p = g["GattLocalCharacteristicParameters"]()
        p.characteristic_properties = (g["GattCharacteristicProperties"].READ |
                                       g["GattCharacteristicProperties"].NOTIFY)
        r1 = await service.create_characteristic_async(uuid.UUID(NOTIFY_UUID), p)
        self.notify_char = r1.characteristic
        if self.notify_char is None:
            print("create fff1 failed: status=%s" % r1.status)
            return False
        self.notify_char.add_subscribed_clients_changed(self._on_subs)
        log("fff1 ready (read+notify)", self.args.quiet)

        p2 = g["GattLocalCharacteristicParameters"]()
        p2.characteristic_properties = (g["GattCharacteristicProperties"].WRITE |
                                        g["GattCharacteristicProperties"].WRITE_WITHOUT_RESPONSE)
        r2 = await service.create_characteristic_async(uuid.UUID(WRITE_UUID), p2)
        wchar = r2.characteristic
        if wchar is None:
            print("create fff2 failed: status=%s" % r2.status)
            return False
        wchar.add_write_requested(self._on_write)
        log("fff2 ready (write + write-without-response)", self.args.quiet)

        adv = g["GattServiceProviderAdvertisingParameters"]()
        adv.is_discoverable = True
        adv.is_connectable = True
        # pywinrt splits the two WinRT overloads by name: StartAdvertising() ->
        # start_advertising(), StartAdvertising(params) -> start_advertising_with_parameters().
        self.provider.start_advertising_with_parameters(adv)
        self.advertising = True
        # The very first status can be ABORTED for a moment before it settles on
        # STARTED; read it again after the stack has caught up.
        await asyncio.sleep(1.0)
        st = self.provider.advertisement_status
        log("advertising service %s  status=%s" % (SERVICE_UUID, st), self.args.quiet)
        if int(st) not in (2, 4):
            log("!! advertising did NOT start (status=%s). "
                "Status 0/1 = not advertising, 3 = aborted." % st, self.args.quiet)
        return True

    # -- notifications -----------------------------------------------------
    async def notify(self, text):
        if self.muted or self.notify_char is None:
            return
        data = text.encode("ascii", "replace")
        chunks = [data] if self.args.chunk <= 0 else [data[i:i + self.args.chunk]
                                                      for i in range(0, len(data), self.args.chunk)]
        for ch in chunks:
            try:
                await self.notify_char.notify_value_async(bytes_to_buffer(ch, self.api["DataWriter"]))
                self.notifies += 1
                log("  > %r" % ch.decode("ascii", "replace"))
            except Exception as e:
                self.errors += 1
                log("  ! notify failed: %s" % e)
                return
            if self.args.chunk > 0 and len(chunks) > 1:
                await asyncio.sleep(0.005)   # real adapter does not send one blob

    # -- the write consumer -------------------------------------------------
    async def consume_writes(self):
        while True:
            args = await self.pending.get()
            try:
                req = await args.get_request_async()
                data = buffer_to_bytes(req.value, self.api["DataReader"])
            except Exception as e:
                self.errors += 1
                log("! read write request failed: %s" % e)
                continue
            text = data.decode("ascii", "replace")
            self.writes += 1
            log("< %r" % text)
            try:
                req.respond()
            except Exception:
                pass

            # A real adapter has a UART behind it: fragments of one command
            # ("01" + "0C" + CR) are glued back together before the ELM327 sees
            # them. The board DOES send such fragments as separate ATT writes.
            self.line_buf.extend(data)
            while True:
                idx_cr = self.line_buf.find(b"\r")
                idx_lf = self.line_buf.find(b"\n")
                idx = min([i for i in (idx_cr, idx_lf) if i >= 0], default=-1)
                if idx < 0:
                    break
                line = bytes(self.line_buf[:idx])
                del self.line_buf[:idx + 1]
                cmd = line.decode("ascii", "replace").strip()
                if not cmd:
                    continue
                ans = self.elm.answer(cmd)
                if not ans:
                    continue
                if self.args.latency_ms:
                    await asyncio.sleep(self.args.latency_ms / 1000.0)
                await self.notify(ans + "\r>")

    # -- periodic work ------------------------------------------------------
    async def ticker(self):
        t0 = time.time()
        last = 0.0
        while True:
            await asyncio.sleep(0.2)
            el = time.time() - t0
            self.elm.update(el)
            self.apply_control()
            if self.args.drop_after and el >= self.args.drop_after and self.advertising:
                self.provider.stop_advertising()
                self.advertising = False
                log("*** --drop-after reached: advertising STOPPED (adapter unplugged) ***")
            if self.args.kill_after and el >= self.args.kill_after and not self.muted:
                self.muted = True
                log("*** --kill-after reached: answering NOTHING (adapter mute) ***")
            if el - last >= 5.0:
                last = el
                log("t=%5.1fs subs=%d cmd=%d notify=%d write=%d err=%d | rpm=%.0f cool=%.0f speed=%.0f intake=%.0f"
                    % (el, self.subscribers, self.elm.cmds, self.notifies, self.writes, self.errors,
                       self.elm.rpm, self.elm.coolant, self.elm.speed, self.elm.intake), self.args.quiet)

    def apply_control(self):
        """Live tweaks without restarting: key=value lines in --control file."""
        if not self.args.control:
            return
        try:
            with open(self.args.control, "r", encoding="ascii", errors="replace") as fh:
                txt = fh.read()
        except OSError:
            return
        for line in txt.splitlines():
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip().lower()
            v = v.strip()
            try:
                if k == "rpm":
                    self.elm.rpm = float(v)
                elif k in ("coolant", "cool"):
                    self.elm.coolant = float(v)
                elif k == "speed":
                    self.elm.speed = float(v)
                elif k == "intake":
                    self.elm.intake = float(v)
                elif k == "mute":
                    self.muted = v not in ("0", "false", "no")
                elif k == "bitmap":
                    self.elm.bitmap = v.upper()
            except ValueError:
                pass


# ---------------------------------------------------------------------------
# --selftest: prove a PASSIVE scanner sees our service UUID in ADV_IND
# ---------------------------------------------------------------------------
async def selftest(args, api):
    """The board scans passively; if the UUID only lands in the SCAN_RSP it never
    sees us. This prints exactly what goes out in the advertisement itself."""
    from winrt.windows.devices.bluetooth.advertisement import (
        BluetoothLEAdvertisementWatcher, BluetoothLEScanningMode)

    sim = Sim(args, api)
    if not await sim.setup():
        return 3

    seen = {}
    done = asyncio.Event()

    def on_adv(sender, a):
        try:
            adv = a.advertisement
            uuids = [str(u).lower() for u in adv.service_uuids]
            name = adv.local_name or ""
            key = (name, tuple(uuids))
            if key in seen:
                return
            seen[key] = True
            print("--- advertisement seen by a PASSIVE scanner (rssi=%d addr=%012X type=%s) ---"
                  % (a.raw_signal_strength_in_dbm, a.bluetooth_address, a.advertisement_type))
            print("  local name    : %r" % name)
            print("  service uuids : %s" % (uuids if uuids else "(none)"))
            for s in adv.data_sections:
                print("  ad section    : type=0x%02X len=%d" % (s.data_type, s.data.length))
            hit = SERVICE_UUID in uuids
            print("  VERDICT       : %s" % ("FFF0 is in the advertisement -> the board's PASSIVE scan will see it"
                                            if hit else
                                            "FFF0 NOT in the advertisement -> board would never find us"))
            if hit:
                done.set()
        except Exception as e:
            print("watcher handler error: %s %s" % (type(e).__name__, e))

    w = BluetoothLEAdvertisementWatcher()
    w.scanning_mode = BluetoothLEScanningMode.PASSIVE
    w.add_received(on_adv)
    w.start()
    try:
        await asyncio.wait_for(done.wait(), timeout=args.seconds or 15)
    except asyncio.TimeoutError:
        print("no matching advertisement observed in %ds" % (args.seconds or 15))
    w.stop()
    sim.provider.stop_advertising()
    return 0


# ---------------------------------------------------------------------------
async def main_async(args, api):
    if args.selftest:
        return await selftest(args, api)

    sim = Sim(args, api)
    if not await sim.setup():
        return 3

    if args.mode == "replay":
        if sim.elm.replay is None:
            print("--mode replay needs --csv <file>")
            return 2
        print("replay loaded: %d pids, %.1f s of recorded drive" %
              (len(sim.elm.replay.rows), sim.elm.replay.duration))

    tasks = [asyncio.create_task(sim.consume_writes()),
             asyncio.create_task(sim.ticker())]
    try:
        if args.seconds:
            await asyncio.sleep(args.seconds)
        else:
            print("running. Ctrl+C to stop.")
            while True:
                await asyncio.sleep(1.0)
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        for t in tasks:
            t.cancel()
        try:
            sim.provider.stop_advertising()
        except Exception:
            pass
        log("stopped: cmd=%d notify=%d write=%d err=%d" %
            (sim.elm.cmds, sim.notifies, sim.writes, sim.errors))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Pretend this PC is the car's BLE OBD adapter")
    ap.add_argument("--mode", choices=["idle", "sweep", "replay"], default="idle")
    ap.add_argument("--csv", default="")
    ap.add_argument("--rpm", type=float, default=900.0)
    ap.add_argument("--coolant", type=float, default=88.0)
    ap.add_argument("--speed", type=float, default=0.0)
    ap.add_argument("--intake", type=float, default=40.0)
    ap.add_argument("--chunk", type=int, default=13, help="notify fragment size, 0 = one blob")
    ap.add_argument("--latency-ms", type=int, default=20)
    ap.add_argument("--drop-after", type=float, default=0.0)
    ap.add_argument("--kill-after", type=float, default=0.0)
    ap.add_argument("--fail-pids", default="")
    ap.add_argument("--search-first", action="store_true")
    ap.add_argument("--control", default="")
    ap.add_argument("--seconds", type=float, default=0.0)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.csv and not os.path.exists(args.csv):
        print("csv not found: %s" % args.csv)
        return 2

    api = winrt_imports()
    if api is None:
        return 2
    try:
        # pywinrt 3.x takes the apartment type as a required argument.
        api["runtime"].init_apartment(api["runtime"].ApartmentType.MULTI_THREADED)
    except Exception as e:
        print("init_apartment skipped: %s" % e)

    try:
        return asyncio.run(main_async(args, api))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
