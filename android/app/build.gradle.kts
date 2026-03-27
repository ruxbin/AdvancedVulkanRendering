plugins {
    id("com.android.application")
}

android {
    namespace = "com.vulkan.advancedrendering"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.vulkan.advancedrendering"
        minSdk = 28
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/jni/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    androidResources {
        noCompress += listOf("bin", "spv", "scene")
    }

    sourceSets {
        getByName("main") {
            assets.srcDirs("src/main/assets")
        }
    }
}

// Copy assets from project root into src/main/assets before build
tasks.register<Copy>("copyAssets") {
    from("${rootProject.projectDir}/../shaders") {
        include("*.spv")
        into("shaders")
    }
    from("${rootProject.projectDir}/..") {
        include("debug1.bin")
        include("scene.scene")
    }
    into("${projectDir}/src/main/assets")
}

tasks.matching { it.name.startsWith("externalNativeBuild") || it.name.startsWith("merge") && it.name.contains("Assets") }.configureEach {
    dependsOn("copyAssets")
}
