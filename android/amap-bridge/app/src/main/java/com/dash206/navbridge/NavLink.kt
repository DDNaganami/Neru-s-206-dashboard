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

    // ---------------- 高德解析 ----------------
    fun onAmapBroadcast(i: Intent) {
        val type = i.getIntExtra("KEY_TYPE", -1)
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
    private var gatt: BluetoothGatt? = null
    private val scanner: BluetoothLeScanner? by lazy {
        (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter?.bluetoothLeScanner
    }
    @Volatile private var rx: BluetoothGattCharacteristic? = null

    @SuppressLint("MissingPermission")
    fun startBle() {
        scanner?.startScan(null, ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCb)
    }

    private val scanCb = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(type: Int, r: ScanResult) {
            if (r.device.name == DEV_NAME) {
                scanner?.stopScan(this)
                gatt = r.device.connectGatt(ctx, false, gattCb)
            }
        }
    }

    private val gattCb = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.requestMtu(64); g.discoverServices()
            } else {
                g.close(); gatt = null; rx = null
                Handler(Looper.getMainLooper()).postDelayed({ startBle() }, 3000)
            }
        }
        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            rx = g.getService(SVC)?.getCharacteristic(RX)
            Log.i(TAG, "服务发现完成，RX=${rx != null}")
        }
    }

    /** 高德停发 >3 s：推一帧"无效"（固件看到会降级成 NO NAV） */
    fun setInvalid() {
        push(buildFrame(0, 0, 0, 0, 0, 0, nightFlag, false, ""))
    }

    @SuppressLint("MissingPermission")
    private fun push(frame: ByteArray) {
        val c = rx ?: return
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        c.value = frame
        gatt?.writeCharacteristic(c)
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
            registerReceiver(recv, IntentFilter(NavLink.AMAP_ACTION))
            link.startBle()

            // 高德停发时补一帧"无效"（3 s 后），避免屏上停在旧指令
            Handler(Looper.getMainLooper()).postDelayed(invalidator, 3000)
        }

        private val invalidator = object : Runnable {
            override fun run() {
                if (SystemClock.elapsedRealtime() - link.lastNavMs > 3000) {
                    // 无效帧：road_len = 0、flags = 0（固件按超时降级为 NO NAV）
                    link.setInvalid()
                }
                Handler(Looper.getMainLooper()).postDelayed(this, 1000)
            }
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
