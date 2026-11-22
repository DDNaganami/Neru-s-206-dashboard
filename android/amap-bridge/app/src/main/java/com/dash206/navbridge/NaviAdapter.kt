package com.dash206.navbridge

import com.amap.api.navi.model.*

// 高德 Navi 回调接口的 36 个抽象方法（javap 自动生成，2026-11-22）。
open class NaviAdapter : com.amap.api.navi.AMapNaviListener {
    override fun onInitNaviFailure() {}
    override fun onInitNaviSuccess() {}
    override fun onStartNavi(a0: Int) {}
    override fun onTrafficStatusUpdate() {}
    override fun onLocationChange(a0: AMapNaviLocation) {}
    override fun onGetNavigationText(a0: Int, a1: String) {}
    override fun onGetNavigationText(a0: String) {}
    override fun onEndEmulatorNavi() {}
    override fun onArriveDestination() {}
    override fun onCalculateRouteFailure(a0: Int) {}
    override fun onReCalculateRouteForYaw() {}
    override fun onReCalculateRouteForTrafficJam() {}
    override fun onArrivedWayPoint(a0: Int) {}
    override fun onGpsOpenStatus(a0: Boolean) {}
    override fun onNaviInfoUpdate(a0: NaviInfo) {}
    override fun updateCameraInfo(a0: Array<AMapNaviCameraInfo>) {}
    override fun updateIntervalCameraInfo(a0: AMapNaviCameraInfo, a1: AMapNaviCameraInfo, a2: Int) {}
    override fun onServiceAreaUpdate(a0: Array<AMapServiceAreaInfo>) {}
    override fun showCross(a0: AMapNaviCross) {}
    override fun hideCross() {}
    override fun showModeCross(a0: AMapModelCross) {}
    override fun hideModeCross() {}
    override fun showLaneInfo(a0: Array<AMapLaneInfo>, a1: ByteArray, a2: ByteArray) {}
    override fun showLaneInfo(a0: AMapLaneInfo) {}
    override fun hideLaneInfo() {}
    override fun onCalculateRouteSuccess(a0: IntArray) {}
    override fun notifyParallelRoad(a0: Int) {}
    override fun OnUpdateTrafficFacility(a0: Array<AMapNaviTrafficFacilityInfo>) {}
    override fun OnUpdateTrafficFacility(a0: AMapNaviTrafficFacilityInfo) {}
    override fun updateAimlessModeStatistics(a0: AimLessModeStat) {}
    override fun updateAimlessModeCongestionInfo(a0: AimLessModeCongestionInfo) {}
    override fun onPlayRing(a0: Int) {}
    override fun onCalculateRouteSuccess(a0: AMapCalcRouteResult) {}
    override fun onCalculateRouteFailure(a0: AMapCalcRouteResult) {}
    override fun onNaviRouteNotify(a0: AMapNaviRouteNotifyData) {}
    override fun onGpsSignalWeak(a0: Boolean) {}
}
