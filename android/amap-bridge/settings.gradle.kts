pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        // 高德 SDK 的构件在阿里云镜像里（jcenter 的下游）
        maven { url = uri("https://maven.aliyun.com/repository/public") }
    }
}
rootProject.name = "amap-bridge"
include(":app")
