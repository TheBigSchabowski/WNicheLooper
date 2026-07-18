import org.jetbrains.compose.desktop.application.dsl.TargetFormat
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.3.21"
    id("org.jetbrains.kotlin.plugin.compose") version "2.3.21"
    id("org.jetbrains.compose") version "1.11.1"
}

repositories {
    mavenCentral()
    google()
}

kotlin {
    jvmToolchain(21)
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_21)
    }
}

dependencies {
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
}

// ---- Native engine (C++ core + ASIO/WASAPI + VST3 hosting + JNI bridge) ----
// Windows-only: compiled with MSVC (cl.exe) found via vswhere/vcvars64. The
// resulting nichelooper.dll is embedded as a classpath resource.

// JNI headers: prefer the Gradle JVM, fall back to JAVA_HOME.
fun findJniHome(): File {
    val current = File(System.getProperty("java.home"))
    if (File(current, "include/jni.h").exists()) return current
    val env = System.getenv("JAVA_HOME")
    if (env != null && File(env, "include/jni.h").exists()) return File(env)
    error("No JDK with JNI headers found — set JAVA_HOME to a JDK (17-21).")
}

fun findVcvars64(): File {
    val programFilesX86 = System.getenv("ProgramFiles(x86)") ?: "C:/Program Files (x86)"
    val vswhere = File(programFilesX86, "Microsoft Visual Studio/Installer/vswhere.exe")
    check(vswhere.isFile) {
        "vswhere.exe not found — install the Visual Studio 2022 Build Tools " +
            "(workload \"Desktop development with C++\")."
    }
    val process = ProcessBuilder(
        vswhere.absolutePath, "-latest", "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath",
    ).redirectErrorStream(true).start()
    val installPath = process.inputStream.bufferedReader().readText().trim().lines().firstOrNull()
    check(process.waitFor() == 0 && !installPath.isNullOrBlank()) {
        "vswhere found no MSVC toolset — install the C++ build tools workload."
    }
    val vcvars = File(installPath, "VC/Auxiliary/Build/vcvars64.bat")
    check(vcvars.isFile) { "vcvars64.bat missing under $installPath" }
    return vcvars
}

val nativeResourceDir = layout.buildDirectory.dir("generated/nativeResources")

// Our sources plus the VST3 SDK hosting/base translation units the host needs.
val nativeSources = listOf(
    "native/LooperEngine.cpp",
    "native/RhythmSection.cpp",
    "native/miniaudio_impl.cpp",
    "native/AsioBackend.cpp",
    "native/WinAudioEngine.cpp",
    "native/VstPluginChain.cpp",
    "native/vst_iids.cpp",
    "native/MediaDecode.cpp",
    "native/jni_bridge.cpp",
)
val vst3SdkSources = listOf(
    "pluginterfaces/base/conststringtable.cpp",
    "pluginterfaces/base/coreiids.cpp",
    "pluginterfaces/base/funknown.cpp",
    "pluginterfaces/base/ustring.cpp",
    "base/source/baseiids.cpp",
    "base/source/fbuffer.cpp",
    "base/source/fdebug.cpp",
    "base/source/fdynlib.cpp",
    "base/source/fobject.cpp",
    "base/source/fstreamer.cpp",
    "base/source/fstring.cpp",
    "base/source/timer.cpp",
    "base/source/updatehandler.cpp",
    "base/thread/source/fcondition.cpp",
    "base/thread/source/flock.cpp",
    "public.sdk/source/common/commonstringconvert.cpp",
    "public.sdk/source/common/memorystream.cpp",
    "public.sdk/source/common/threadchecker_win32.cpp",
    "public.sdk/source/vst/vstinitiids.cpp",
    "public.sdk/source/vst/utility/stringconvert.cpp",
    "public.sdk/source/vst/hosting/connectionproxy.cpp",
    "public.sdk/source/vst/hosting/eventlist.cpp",
    "public.sdk/source/vst/hosting/hostclasses.cpp",
    "public.sdk/source/vst/hosting/module.cpp",
    "public.sdk/source/vst/hosting/module_win32.cpp",
    "public.sdk/source/vst/hosting/parameterchanges.cpp",
    "public.sdk/source/vst/hosting/pluginterfacesupport.cpp",
    "public.sdk/source/vst/hosting/plugprovider.cpp",
    "public.sdk/source/vst/hosting/processdata.cpp",
)

val buildNative = tasks.register("buildNative") {
    inputs.files(fileTree("native") { include("*.cpp", "*.h") })
    outputs.file(nativeResourceDir.map { it.file("native/nichelooper.dll") })

    doLast {
        check(System.getProperty("os.name").startsWith("Windows")) {
            "buildNative needs Windows (MSVC). On other platforms only the Kotlin " +
                "code can be compiled."
        }
        val vst3Sdk = file("third_party/vst3sdk")
        check(File(vst3Sdk, "pluginterfaces/base/funknown.h").isFile) {
            "VST3 SDK missing — run: git submodule update --init --recursive " +
                "(or: git submodule update --init third_party/vst3sdk && " +
                "cd third_party/vst3sdk && git submodule update --init base " +
                "pluginterfaces public.sdk)"
        }
        check(file("third_party/asiosdk/common/iasiodrv.h").isFile) {
            "ASIO SDK missing — download https://www.steinberg.net/asiosdk and " +
                "extract it to third_party/asiosdk"
        }

        val jniHome = findJniHome()
        val vcvars = findVcvars64()
        val outputFile = nativeResourceDir.get().file("native/nichelooper.dll").asFile
        outputFile.parentFile.mkdirs()
        val objDir = layout.buildDirectory.dir("native-obj").get().asFile
        objDir.mkdirs()

        // Everything on ONE line: cl response files have no reliable
        // flag/argument split across lines. Paths with spaces are quoted;
        // /I<path> is passed as a single token. Object files land in the
        // process working directory (objDir), so no /Fo path is needed.
        fun q(path: String) = if (path.contains(' ')) "\"$path\"" else path
        val args = mutableListOf(
            "/nologo", "/std:c++17", "/O2", "/EHsc", "/MD", "/W3", "/utf-8", "/LD",
            "/DNDEBUG", "/DRELEASE=1", "/DUNICODE", "/D_UNICODE", "/DNOMINMAX",
            "/D_CRT_SECURE_NO_WARNINGS",
            "/I${q(file("native").absolutePath)}",
            "/I${q(vst3Sdk.absolutePath)}",
            "/I${q(file("third_party/asiosdk/common").absolutePath)}",
            "/I${q(File(jniHome, "include").absolutePath)}",
            "/I${q(File(jniHome, "include/win32").absolutePath)}",
            "/Fe${q(outputFile.absolutePath)}",
        )
        args += nativeSources.map { q(file(it).absolutePath) }
        args += vst3SdkSources.map { q(File(vst3Sdk, it).absolutePath) }
        args += listOf(
            "/link", "user32.lib", "gdi32.lib", "ole32.lib", "oleaut32.lib",
            "advapi32.lib", "shell32.lib", "uuid.lib", "winmm.lib",
            "mfplat.lib", "mfreadwrite.lib", "mfuuid.lib",
        )

        val rspFile = File(objDir, "cl_args.rsp")
        rspFile.writeText(args.joinToString(" "))
        val batFile = File(objDir, "build_native.bat")
        batFile.writeText(
            "@echo off\r\n" +
                "call \"${vcvars.absolutePath}\" >NUL 2>&1\r\n" +
                "cl @\"${rspFile.absolutePath}\"\r\n",
        )

        val process = ProcessBuilder("cmd", "/c", batFile.absolutePath)
            .directory(objDir)
            .redirectErrorStream(true)
            .start()
        val output = process.inputStream.bufferedReader().readText()
        val exit = process.waitFor()
        if (exit != 0 || !outputFile.isFile) {
            logger.error(output)
            error("Native build failed (exit $exit) — see compiler output above.")
        }
        logger.lifecycle("Built ${outputFile.name} (${outputFile.length() / 1024} KiB)")
    }
}

sourceSets["main"].resources.srcDir(nativeResourceDir)
tasks.named("processResources") { dependsOn(buildNative) }

compose.desktop {
    application {
        mainClass = "nichelooper.MainKt"

        nativeDistributions {
            // MSI needs the WiX toolset (https://wixtoolset.org) on PATH.
            targetFormats(TargetFormat.Msi)
            packageName = "NicheLooper"
            packageVersion = "1.0.0"
            windows {
                menuGroup = "NicheLooper"
                upgradeUuid = "6f9b1c3a-8f27-4e0a-9c3d-2b7a4e5d6c81"
            }
        }
    }
}
