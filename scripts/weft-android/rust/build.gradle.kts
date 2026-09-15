// weft-android/rust/build.gradle.kts
// The Rust crate. Builds via cargo-ndk into .so files for arm64-v8a and
// armeabi-v7a, then copies them to :weft's jniLibs directory.
//
// This module is a "build-only" module — it produces no .aar, just .so files.

plugins {
    id("com.android.library")  // Use AGP's JNI handling, even though no Kotlin.
}

android {
    namespace = "dev.weft.rust"
    compileSdk = 34

    defaultConfig {
        minSdk = 26
    }

    // No Kotlin/Java in this module — just a build script.
    sourceSets {
        getByName("main").manifest.srcFile("src/main/AndroidManifest.xml")
    }
}

// Cargo-ndk build task. Runs `cargo ndk build` for each ABI.
//
// cargo-ndk wraps `cargo build --target` and produces a .so for Android.
// Install: `cargo install cargo-ndk` (one-time).
val cargoTargets = mapOf(
    "arm64-v8a"     to "aarch64-linux-android",
    "armeabi-v7a"   to "armv7-linux-androideabi",
)

val buildRust by tasks.registering {
    group = "rust"
    description = "Build Rust .so files via cargo-ndk"

    inputs.files(fileTree("src").matching { include("**/*.rs") }))
    inputs.file("Cargo.toml")
    outputs.dir(layout.buildDirectory.dir("intermediates/rust"))

    doLast {
        val outDir = layout.buildDirectory.dir("intermediates/rust").get().asFile
        outDir.mkdirs()

        for ((abi, target) in cargoTargets) {
            // `cargo ndk --target $target build --release --features jni`
            // outputs to rust/target/<target>/release/libweft_core.so
            val cargoOut = file("target/$target/release/libweft_core.so")
            if (!cargoOut.exists()) {
                logger.lifecycle("Building Rust for $abi ($target)...")
                exec {
                    workingDir = projectDir
                    commandLine("cargo", "ndk",
                        "-t", target,
                        "--platform", if (abi == "arm64-v8a") "26" else "21",
                        "build", "--release", "--features", "jni")
                }
            }

            // Copy to :weft's jniLibs.
            val dest = rootProject.file("weft/src/main/jniLibs/$abi")
            dest.mkdirs()
            cargoOut.copyTo(File(dest, "libweft_core.so"), overwrite = true)
        }
    }
}

// Make :weft's preBuild depend on Rust build, so cargo runs automatically.
project(":weft").afterEvaluate {
    project(":weft").tasks.named("preBuild") {
        dependsOn(buildRust)
    }
}
