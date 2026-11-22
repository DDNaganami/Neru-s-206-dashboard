package com.dash206.navbridge

import android.content.Context
import android.util.Log
import com.amap.api.navi.AMapNavi
import com.amap.api.navi.model.AMapNaviLocation
import com.amap.api.navi.model.NaviLatLng
import kotlin.math.cos
import kotlin.math.sin

/**
 * 阶段 B 的核心：**自己跑高德导航**，把路线几何发给板子。
 *
 * 为什么不用广播：高德广播只给"转向图标 + 距离"，**没有路线坐标**（研判 §11）。
 * 要靠 SDK：`AMapNavi` 算出路线（`getNaviPath().getCoordList()`），
 * 再用 `onLocationChange` 的位置/车头角把整条路线换算成"相对车的米数"，
 * 裁到前方 300 m / 最多 24 点，走 0xA8 路线帧发给板子。
 *
 * 坐标系换算（局部米）：
 *   东 = (lon - lon0) × 111320 × cos(lat0)
 *   北 = (lat - lat0) × 111320
 *   车头 bearing（0=正北，顺时针）
 *   前 = 东×sin(b) + 北×cos(b)     右 = 东×cos(b) - 北×sin(b)
 *   线上约定：dx = 右（米），dy = **-前**（屏坐标向上为负）
 *
 * ★ 只发前方这段：板子没有地图数据，发整条路线既没必要也装不下（一帧 24 点）。
 */
object AmapRouteBridge {
    private const val TAG = "AmapRoute"
    private const val MAX_POINTS = 24
    private const val AHEAD_M = 300.0

    @Volatile private var navi: AMapNavi? = null
    private var started = false

    @Volatile private var destLat = 0.0
    @Volatile private var destLon = 0.0
    @Volatile private var hasDest = false

    /** 目的地（之后可以改成从 Amap 广播的 endPOIName 解析，或 App 里选点） */
    fun setDestination(lat: Double, lon: Double) {
        destLat = lat
        destLon = lon
        hasDest = true
    }

    fun start(ctx: Context) {
        if (started) return
        started = true
        try {
            val n = AMapNavi.getInstance(ctx.applicationContext)
            navi = n
            n.addAMapNaviListener(object : NaviAdapter() {
                override fun onLocationChange(loc: AMapNaviLocation) {
                    val loc2 = loc;
                    val path = navi?.naviPath ?: return       // getNaviPath()
                    val coords = path.coordList ?: return      // getCoordList()
                    val p = loc2.coord ?: return
                    val pts = project(
                        p.latitude, p.longitude, loc2.bearing,
                        coords.map { it.latitude to it.longitude }
                    )
                    if (pts.size >= 2) {
                        NavLink.get(ctx).sendRoute(pts)
                    }
                }
            })
            Log.i(TAG, "AMapNavi 已初始化")
        } catch (t: Throwable) {
            Log.e(TAG, "AMapNavi 初始化失败: ${t.message}")
        }
    }

    /** 起一次导航（目的地由 setDestination 给） */
    fun startNavigation() {
        val n = navi ?: return
        if (!hasDest) {
            Log.w(TAG, "没有目的地，跳过")
            return
        }
        try {
            val end = listOf(NaviLatLng(destLat, destLon))
            val ok = n.calculateDriveRoute(null, end, null, 1 /* 避免拥堵 */)
            Log.i(TAG, "calculateDriveRoute=$ok")
        } catch (t: Throwable) {
            Log.e(TAG, "起导航失败: ${t.message}")
        }
    }

    /**
     * 经纬度路线 → 车体局部米（纯函数，好单测）。
     * 返回按前进顺序排列、只保留前方 AHEAD_M 米内、最多 MAX_POINTS 个点。
     */
    fun project(lat0: Double, lon0: Double, bearingDeg: Float,
                route: List<Pair<Double, Double>>): List<Pair<Int, Int>> {
        val br = Math.toRadians(bearingDeg.toDouble())
        val cosB = cos(br)
        val sinB = sin(br)
        val mPerDegLat = 111320.0
        val mPerDegLon = 111320.0 * cos(Math.toRadians(lat0))
        val out = ArrayList<Pair<Int, Int>>(MAX_POINTS)
        var lastForward = -1.0
        for ((lat, lon) in route) {
            val east = (lon - lon0) * mPerDegLon
            val north = (lat - lat0) * mPerDegLat
            val fwd = east * sinB + north * cosB
            val right = east * cosB - north * sinB
            if (fwd < -2.0) continue                 // 车后面的点不要
            if (fwd > AHEAD_M) break
            // 隔点抽稀：保证 24 点能覆盖 300 m（~12 m 一个点）
            if (fwd - lastForward < 12.0 && out.isNotEmpty()) continue
            lastForward = fwd
            // dy 用负：屏坐标 y 向下，前方 = 负
            out.add(Pair(right.toInt(), (-fwd).toInt()))
            if (out.size >= MAX_POINTS) break
        }
        return out
    }
}
