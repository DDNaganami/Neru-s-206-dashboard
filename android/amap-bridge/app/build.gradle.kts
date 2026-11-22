import java.util.Properties

// 高德 Key 从 local.properties 读（不进 git）
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val amapKey: String = localProps.getProperty("amap.key") ?: ""

plugins {
    id("com.android.application") version "8.5.2"
    id("org.jetbrains.kotlin.android") version "1.9.24"
}

android {
    namespace = "com.dash206.navbridge"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.dash206.navbridge"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        buildConfigField("String", "AMAP_KEY", "\"$amapKey\"")
        manifestPlaceholders["amapKey"] = amapKey
    }
    buildFeatures { buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    // 高德导航 SDK（含地图 + 导航 + 路线搜索）
    implementation("com.amap.api:navi-3dmap:latest.integration")
}
