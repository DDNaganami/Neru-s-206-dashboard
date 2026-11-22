package com.dash206.navbridge

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import java.io.ByteArrayOutputStream

/** 极简壳：拉起前台服务（导航桥 + BLE），启动导航，并周期发测试图。 */
class MainActivity : Activity() {
    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(
                arrayOf(
                    Manifest.permission.POST_NOTIFICATIONS,
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.ACCESS_FINE_LOCATION
                ), 1
            )
        }
        startForegroundService(Intent(this, NavLink.Svc::class.java))

        // 阶段 B：自己跑高德导航（拿路线几何）。目的地先用一个固定点做联调
        // （天安门，39.908722 / 116.397499）；以后改成 App 里选点或从广播的 endPOIName 解析。
        AmapRouteBridge.start(this)
        AmapRouteBridge.setDestination(39.908722, 116.397499)
        AmapRouteBridge.startNavigation()

        // 阶段 B2 链路测试：每 2 秒发一张 240×240 测试 JPEG（网格 + 文字）。
        // 先证明"传输 -> 重组 -> 解码 -> 上屏"，再换成高德地图截图。
        val jpeg = buildTestJpeg()
        val h = Handler(Looper.getMainLooper())
        var sent = 0
        val task = object : Runnable {
            override fun run() {
                if (sent++ < 12) {
                    NavLink.get(this@MainActivity).sendImage(jpeg)
                    h.postDelayed(this, 10000)
                }
            }
        }
        h.postDelayed(task, 2000)
    }

    private fun buildTestJpeg(): ByteArray {
        val bmp = Bitmap.createBitmap(240, 240, Bitmap.Config.ARGB_8888)
        val cv = Canvas(bmp)
        val p = Paint()
        p.style = Paint.Style.FILL
        p.color = Color.rgb(18, 22, 26)
        cv.drawRect(0f, 0f, 240f, 240f, p)

        p.color = Color.rgb(53, 198, 244)
        p.strokeWidth = 3f
        for (i in 0..6) {
            cv.drawLine(0f, i * 40f, 240f, i * 40f, p)
            cv.drawLine(i * 40f, 0f, i * 40f, 240f, p)
        }
        p.color = Color.WHITE
        p.textSize = 34f
        cv.drawText("206dash", 58f, 128f, p)
        p.textSize = 18f
        p.color = Color.rgb(120, 220, 150)
        cv.drawText("MAP TEST", 76f, 160f, p)

        val bos = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.JPEG, 40, bos)
        return bos.toByteArray()
    }
}
