plugins { id("com.android.application"); kotlin("android") }
android {
    namespace = "com.dash206.navbridge"
    compileSdk = 34
    defaultConfig { applicationId = "com.dash206.navbridge"; minSdk = 26; targetSdk = 34 }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}
