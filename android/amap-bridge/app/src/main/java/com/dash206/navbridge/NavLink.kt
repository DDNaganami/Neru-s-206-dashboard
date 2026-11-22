package com.dash206.navbridge

import android.annotation.SuppressLint
import android.app.*
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.*
import android.os.*
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.*
import kotlin.math.roundToInt

/**
 * 导航桥（阶段 A）：高德广播 -> 39 字节短报文 -> BLE 写给左屏 ESP32。
 *
 * 为什么这么写：
 *  · 高德字段又多又随版本变 ⇒ 解析全部 "有就用、没有就跳过"，绝不因为一个 extra 缺失就崩；
 *  · 板子只认 `lib/dashcore/nav_proto.h` 那一帧 ⇒ 这里组帧必须与固件**逐字节**一致
 *    （黄金字节见固件测试 test_nav_proto.cpp）；
 *  · 前台服务 + 3 s 重连 + 停发 3 s 推"无效帧"，保证屏上不会停在旧指令上。
 */
class NavLink private constructor(private val ctx: Context) {

    companion object {
        private const val TAG = "NavLink"

        /** 高德导航广播（手机版/车机版都有） */
        const val AMAP_ACTION = "AUTONAVI_STANDARD_BROADCAST_SEND"

        // ---- 与固件 nav_proto.h 对齐的常量（改一个就要两边一起改）----
        const val MAGIC = 0xA7
        const val VERSION = 1
        const val FLAG_NIGHT = 0x01
        const val FLAG_NIGHT_VALID = 0x02
        const val FLAG_ARRIVED = 0x04
        const val ROAD_MAX = 23

        // ---- BLE 契约（与板子固件一致）----
        val SVC: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val DEV_NAME = "206dash-nav"

        @Volatile private var inst: NavLink? = null
        fun get(c: Context): NavLink = inst ?: synchronized(this) {
            inst ?: NavLink(c.applicationContext).also { inst = it }
        }
    }

    // ---- 组帧（与 nav_proto.cpp 的 nav_pack 一一对应）----
    fun buildFrame(turn: Int, distM: Int, speed: Int, headingCdeg: Int, remainMin: Int,
                   speedLimit: Int, night: Boolean?, arrived: Boolean, road: String): ByteArray {
        val roadBytes = road.toByteArray(Charsets.UTF_8)
        val n = minOf(roadBytes.size, ROAD_MAX)
        val flags = (if (night == true) FLAG_NIGHT else 0) or
                (if (night != null) FLAG_NIGHT_VALID else 0) or
                (if (arrived) FLAG_ARRIVED else 0)
        val buf = ByteBuffer.allocate(14 + n + 2).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(MAGIC.toByte()); buf.put(VERSION.toByte()); buf.put((seq++ and 0xFF).toByte())
        buf.put(turn.toByte()); buf.put(flags.toByte())
        buf.putShort(distM.coerceIn(0, 0xFFFF).toShort())
        buf.put(speed.coerceIn(0, 255).toByte())
        buf.putShort(headingCdeg.coerceIn(0, 35999).toShort())
        buf.put(remainMin.coerceIn(0, 255).toByte())
        buf.put(speedLimit.coerceIn(0, 255).toByte())
        buf.put(n.toByte()); buf.put(0.toByte())
        buf.put(roadBytes, 0, n)
        val crc = crc16(buf.array(), 0, buf.position())
        buf.put(crc.toByte()); buf.put((crc shr 8).toByte())
        return buf.array()
    }

    /** 阶段 B2：发一张 JPEG（0xA9 分片，与固件 nav_img.h 逐字节一致） */
    fun sendImage(jpeg: ByteArray) {
        if (jpeg.isEmpty()) return
        val chunkMax = 160
        val total = (jpeg.size + chunkMax - 1) / chunkMax
        if (total == 0 || total > 200) return
        val fid = imageSeq++
        for (idx in 0 until total) {
            val off = idx * chunkMax
            val n = minOf(chunkMax, jpeg.size - off)
            val buf = ByteBuffer.allocate(7 + n + 2).order(ByteOrder.LITTLE_ENDIAN)
            buf.put(0xA9.toByte()); buf.put(1.toByte())
            buf.put((fid and 0xFF).toByte()); buf.put(idx.toByte()); buf.put(total.toByte())
            buf.putShort(n.toShort())
            buf.put(jpeg, off, n)
            val crc = crc16(buf.array(), 0, 7 + n)
            buf.put(crc.toByte()); buf.put((crc shr 8).toByte())
            push(buf.array())
        }
        if (fid % 5 == 1) Log.i(TAG, "image: ${jpeg.size}B in $total chunks (fid=$fid)")
    }

    /** 阶段 B：发路线几何帧（0xA8），与固件 nav_route.h 逐字节一致 */
    fun sendRoute(pts: List<Pair<Int, Int>>) {
        if (pts.isEmpty()) return
        val n = minOf(pts.size, 24)
        val buf = ByteBuffer.allocate(4 + n * 4 + 2).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(0xA8.toByte()); buf.put(1.toByte())
        buf.put((routeSeq++ and 0xFF).toByte()); buf.put(n.toByte())
        for (i in 0 until n) {
            buf.putShort(pts[i].first.toShort())
            buf.putShort(pts[i].second.toShort())
        }
        val crc = crc16(buf.array(), 0, 4 + n * 4)
        buf.put(crc.toByte()); buf.put((crc shr 8).toByte())
        if (routeSeq % 5 == 1) {
            val dxs = (0 until n).map { pts[it].first }
            val dys = (0 until n).map { pts[it].second }
            Log.i(TAG, "route frame: n=$n seq=$routeSeq dx=${dxs.min()}..${dxs.max()} dy=${dys.min()}..${dys.max()}")
        }
        push(buf.array())
    }

    /** CRC-16/CCITT-FALSE（0xFFFF / 0x1021，不反射）—— 与固件同一条 */
    private fun crc16(p: ByteArray, off: Int, len: Int): Int {
        var crc = 0xFFFF
        for (i in off until off + len) {
            crc = crc xor ((p[i].toInt() and 0xFF) shl 8)
            repeat(8) { crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF else (crc shl 1) and 0xFFFF }
        }
        return crc
    }

    @Volatile private var seq = 0
    @Volatile private var routeSeq = 0
    @Volatile private var imageSeq = 0

    // ---------------- 高德解析 ----------------
    fun onAmapBroadcast(i: Intent) {
        val type = i.getIntExtra("KEY_TYPE", -1)
        // ★ 诊断：把收到的每类广播打一行，用来回答"这个版本的高德还发不发"（go/no-go 判据）
        Log.i(TAG, "amap KEY_TYPE=$type extras=${i.extras?.keySet()?.joinToString(",")}")
        when (type) {
            10001 -> {
                val icon = i.getIntExtra("NEW_ICON", i.getIntExtra("ICON", 0))
                val dist = i.getIntExtra("SEG_REMAIN_DIS_AUTO", 0)
                val speed = i.getIntExtra("CUR_SPEED", 0)
                // CAR_DIRECTION：高德给的是角度（度）；我们线上用 0.01° ⇒ 乘 100
                val heading = (i.getFloatExtra("CAR_DIRECTION", 0f) * 100f).roundToInt()
                val remainMin = i.getIntExtra("ROUTE_REMAIN_TIME_AUTO", 0) / 60
                val limit = i.getIntExtra("LIMITED_SPEED", 0)
                val road = i.getStringExtra("NEXT_ROAD_NAME")?.take(ROAD_MAX) ?: ""
                val arrived = icon == 15
                lastNavMs = SystemClock.elapsedRealtime()
                Log.i(TAG, "nav icon=$icon dist=$dist spd=$speed hdg=$heading road=$road")
                push(buildFrame(icon, dist, speed, heading, remainMin, limit, nightFlag,
                                arrived, road))
            }
            10019 -> {
                val st = i.getIntExtra("EXTRA_STATE", 0)
                nightFlag = when (st) { 37 -> false; 38 -> true; else -> null }
            }
        }
    }

    @Volatile private var nightFlag: Boolean? = null
    @Volatile private var lastNavMs = 0L

    // ---------------- BLE ----------------
    private val handler = android.os.Handler(android.os.Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    @Volatile private var connecting = false
    private val scanner: BluetoothLeScanner? by lazy {
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter?.bluetoothLeScanner
    }
    @Volatile private var rx: BluetoothGattCharacteristic? = null
    // ★ 2026-11-22 实测：这台小米 14 重启蓝牙后**扫描回调一条都不来**
    //   （ScanManager 显示 startScan 已注册、没有 onScanFailed），但按地址直连是可靠的。
    //   车上是"一块固定板子"，所以扫描只当快路径，5 s 内没扫到就**按 MAC 直连**。
    private val BOARD_ADDR = "90:70:69:EA:F6:B5"
    // ★★ 2026-11-22 实测：默认 MTU 23 ⇒ 单包只能写 20 字节，而我们的帧 15~39 字节
    //   （带路名就 25+）⇒ Android 会截断，板子侧 CRC 全错、帧数 0。必须先谈 MTU 再写。
    @Volatile private var mtu = 23

    @SuppressLint("MissingPermission")
    fun startBle() {
        Log.i(TAG, "startBle: scanning for $DEV_NAME")
        try {
            scanner?.startScan(null, ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCb)
        } catch (t: Throwable) {
            Log.e(TAG, "startScan 抛异常: ${t.message}")
        }
        // 直连兜底：5 s 内没扫到就按地址连（不依赖扫描）
        handler.postDelayed(object : Runnable {
            override fun run() {
                if (rx == null && gatt == null) {
                    Log.w(TAG, "connectDirect: 扫描没结果，按 MAC 直连 $BOARD_ADDR")
                    try {
                        val adapter = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
                        connectTo(adapter.getRemoteDevice(BOARD_ADDR))
                    } catch (t: Throwable) {
                        Log.e(TAG, "直连失败: ${t.message}")
                    }
                }
            }
        }, 5_000)

        // watchdog：10 s 还没连上就停/起一次扫描（配额被限时这能把它"抖"回来）
        handler.postDelayed(object : Runnable {
            override fun run() {
                if (rx == null && !connecting) {
                    Log.w(TAG, "watchdog: 10 s 没连上，重启扫描")
                    try { scanner?.stopScan(scanCb) } catch (_: Throwable) {}
                    startBle()
                }
            }
        }, 10_000)
    }

    @SuppressLint("MissingPermission")
    private fun connectTo(dev: BluetoothDevice) {
        if (connecting) return
        connecting = true
        try { scanner?.stopScan(scanCb) } catch (_: Throwable) {}
        Log.i(TAG, "connectGatt -> ${dev.address}")
        try {
            gatt?.close()                      // 旧连接必须关掉，否则新写入会一直 false
        } catch (_: Throwable) {}
        gatt = dev.connectGatt(ctx, false, gattCb)
    }

    private val scanCb = object : ScanCallback() {
        // ★ Android 对"App 扫描太频繁"有配额（30 s 内 5 次起），超了会**只回调 onScanFailed**
        //   而一条扫描结果都不给（2026-11-22 实测：scan 行数 = 0、蓝牙却是开的、权限也 granted）
        override fun onScanFailed(errorCode: Int) {
            Log.e(TAG, "onScanFailed: $errorCode（SCAN_FAILED_APPLICATION_REGISTRATION_FAILED=1, 过多=6）")
        }
        @SuppressLint("MissingPermission")
        override fun onScanResult(type: Int, r: ScanResult) {
            val uuids = r.scanRecord?.serviceUuids?.joinToString(",") { it.uuid.toString() }
            Log.i(TAG, "scan: ${r.device.address} name=${r.device.name} uuids=$uuids")
            // ★ 128 位服务 UUID(16B)+名字(11B)+flags(3B) 超过 31 字节广播上限 ⇒ NimBLE 会把
            //   名字挤出广播包（实测手机扫到板子时 name=null）⇒ **按服务 UUID 认板子**，名字只作日志。
            val byName = r.device.name == DEV_NAME
            val byUuid = uuids?.contains(SVC.toString(), ignoreCase = true) == true
            if (byName || byUuid) {
                scanner?.stopScan(this)
                connectTo(r.device)
            }
        }
    }

    private val gattCb = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "gatt state: status=$status newState=$newState")
            connecting = false
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestMtu(185); g.discoverServices()
            } else {
                rx = null; connecting = false
                try { g.close() } catch (_: Throwable) {}
                gatt = null
                Handler(Looper.getMainLooper()).postDelayed({ startBle() }, 3000)
            }
        }
        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            rx = g.getService(SVC)?.getCharacteristic(RX)
            Log.i(TAG, "服务发现完成，RX=${rx != null} mtu=$mtu")
        }

        override fun onCharacteristicWrite(g: BluetoothGatt?, c: BluetoothGattCharacteristic?, status: Int) {
            writing = false
            pumpWrites()
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = newMtu
            Log.i(TAG, "mtu changed: $newMtu status=$status")
        }
    }

    /** 高德停发 >3 s：推一帧"无效"（固件看到会降级成 NO NAV） */
    fun setInvalid() {
        push(buildFrame(0, 0, 0, 0, 0, 0, nightFlag, false, ""))
    }

    // ★ 2026-11-22 实测：一次连发 46 个图片分片，从第 2 片起 write 全返回 false
    //   —— BLE 的 GATT 一次只允许一个未完成的写。改成"写完一个再发下一个"：
    //   队列 + onCharacteristicWrite 回调推进。
    private val writeQueue = java.util.ArrayDeque<ByteArray>()
    @Volatile private var writing = false

    @SuppressLint("MissingPermission")
    private fun push(frame: ByteArray) {
        if (rx == null) return
        writeQueue.addLast(frame)
        pumpWrites()
    }

    @SuppressLint("MissingPermission")
    @Synchronized
    private fun pumpWrites() {
        if (writing) return
        val c = rx ?: return
        val frame = writeQueue.pollFirst() ?: return
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        c.value = frame
        writing = true
        val ok = gatt?.writeCharacteristic(c) ?: false
        if (!ok) {
            writing = false
            Log.w(TAG, "write failed (len=${frame.size} mtu=$mtu)")
        }
    }

    // ---------------- 前台服务 ----------------
    class Svc : Service() {
        private lateinit var link: NavLink
        private val recv = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) = link.onAmapBroadcast(i)
        }

        override fun onCreate() {
            super.onCreate()
            link = NavLink.get(this)
            createChannel()
            startForeground(7, Notification.Builder(this, "nav")
                .setContentTitle("206dash 导航桥").setContentText("监听高德导航…")
                .setSmallIcon(android.R.drawable.ic_menu_mylocation).build())

            // ★ Android 13+/HyperOS：动态注册才能收到高德广播
            // ★ Android 14（targetSdk 34）起：动态注册必须显式声明能不能接收外部广播。
            //   高德是**别的应用** ⇒ 必须 RECEIVER_EXPORTED，否则 SecurityException 直接崩服务
            //   （2026-11-22 在小米 14 上一次真机启动就复现了）。
            val filter = IntentFilter(NavLink.AMAP_ACTION)
            if (Build.VERSION.SDK_INT >= 33) {
                registerReceiver(recv, filter, Context.RECEIVER_EXPORTED)
            } else {
                @Suppress("UnspecifiedRegisterReceiverFlag")
                registerReceiver(recv, filter)
            }
            link.startBle()

            // ★★ 2026-11-22 实测删掉"每秒推一帧全 0 当无效"：
            //   它会把真数据盖成 0（板子串口看到 rx 帧数在涨、字段全 0）。
            //   现在无效判定交给**固件侧超时降级**（nav_state_tick，3 s 无新帧 ⇒ NO NAV），
            //   手机只发真实数据。
        }

        override fun onDestroy() {
            try { unregisterReceiver(recv) } catch (_: Exception) {}
            super.onDestroy()
        }

        private fun createChannel() {
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(NotificationChannel("nav", "导航桥",
                    NotificationManager.IMPORTANCE_LOW))
        }

        override fun onBind(i: Intent?): IBinder? = null
    }
}
