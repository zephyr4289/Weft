// weft-android/settings.gradle.kts
// Root settings for the Weft Android library project.
//
// Module layout:
//   :rust       — Rust crate compiled to .so via cargo-ndk (cdylib)
//   :weft       — Kotlin/Compose library (the public API)

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    @Suppress("UnstableApiUsage")
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "weft-android"

include(":weft")
include(":rust")
