// weft-android/weft/build.gradle.kts
// The Weft Kotlin library module. Exposes Steward, Weft, Heddle, TriadNative.
//
// The Rust .so is built via cargo-ndk and placed in jniLibs/ via the
// `:rust` project's assemble task. This module declares a dependency on it.

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.weft"
    compileSdk = 34

    defaultConfig {
        minSdk = 26        // Android 8.0 — required for DirectByteBuffer stability
        consumerProguardFiles("consumer-rules.pro")

        // ABI filters: production only. x86 is excluded — emulators can fall
        // back to ARM translation.
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        // The Rust .so is built by cargo-ndk in the :rust module and copied
        // here as a dependency. We declare the source set so AGP picks it up.
        sourceSets {
            getByName("main") {
                // The :rust module copies .so files here at assemble time.
                jniLibs.srcDirs("src/main/jniLibs")
            }
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        freeCompilerArgs = listOf(
            "-Xjvm-default=all",            // For Java interop (Steward.finalize, etc.)
            "-opt-in=kotlin.RequiresOptIn",
        )
    }

    buildFeatures {
        compose = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.10"  // matches Kotlin 1.9.22
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false  // consumer-rules.pro handles R8 for the consumer
        }
    }

    // Publish to Maven Central — see publishing{} block.
    publishing {
        singleVariant("release") {
            withJavadocJar()
            withSourcesJar()
        }
    }
}

dependencies {
    // Compose — the Heddle draws via Modifier.drawWithContent.
    val composeBom = platform("androidx.compose:compose-bom:2024.02.00")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")

    // For Steward.finalize logging.
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.7.0")

    // JVM unit tests — use Compose UI test for Steward/Heddle lifecycle tests.
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.8.0")
    testImplementation("org.mockito:mockito-core:5.10.0")
    testImplementation("org.mockito.kotlin:mockito-kotlin:5.3.0")

    // Instrumented tests (androidTest) — for the demo app, not the library itself.
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
}
