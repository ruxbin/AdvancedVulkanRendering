plugins {
    id("com.android.application")
}

val generatedAssets = layout.buildDirectory.dir("generated/bistroAssets")
val runtimeBistro = layout.buildDirectory.file("generated/bistro/bistro.astc.runtime.bin")
val assetRoot = rootProject.projectDir.parentFile
val environmentHdr = file("$assetRoot/textures/san_giuseppe_bridge_2k.hdr")
val convertBistroAstc = tasks.register<Exec>("convertBistroAstc") {
    val converter = file("$assetRoot/Tools/convert_bistro_astc.py")
    val source = file("$assetRoot/bistro.astc.bin")
    val reference = file("$assetRoot/bistro.dxt.bin")
    inputs.files(converter, source, reference)
    outputs.file(runtimeBistro)
    commandLine(providers.gradleProperty("bistroPython").getOrElse("python"),
        converter.absolutePath, source.absolutePath,
        "--reference", reference.absolutePath,
        "--output", runtimeBistro.get().asFile.absolutePath)
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
            // Only this staged directory is packaged, so old raw archives copied
            // by previous builds cannot accidentally enter the APK.
            assets.setSrcDirs(listOf(generatedAssets.get().asFile))
        }
    }
}

// Keep hand-authored extra assets, but replace the legacy generated resources.
tasks.register<Sync>("copyAssets") {
    dependsOn(convertBistroAstc)
    doFirst {
        check(environmentHdr.isFile) { "Required IBL environment is missing: $environmentHdr" }
    }
    from("src/main/assets") {
        exclude("bistro*.bin", "debug1.bin", "scene.scene", "shaders/**", "textures/san_giuseppe_bridge_2k.hdr")
    }
    from(environmentHdr) {
        into("textures")
    }
    from("${rootProject.projectDir}/../shaders") {
        include("*.spv")
        into("shaders")
    }
    from("${rootProject.projectDir}/..") {
        include("scene.scene")
    }
    from(runtimeBistro)
    into(generatedAssets)
}

tasks.matching { it.name.startsWith("externalNativeBuild") || it.name.startsWith("merge") && it.name.contains("Assets") }.configureEach {
    dependsOn("copyAssets")
}
