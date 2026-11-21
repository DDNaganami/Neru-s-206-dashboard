package com.dash206.navbridge

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.app.Activity

/** 极简壳：点一下就把前台服务拉起来（真正的活都在 NavLink.Svc 里）。 */
class MainActivity : Activity() {
    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS,
                Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT), 1)
        }
        startForegroundService(Intent(this, NavLink.Svc::class.java))
    }
}
