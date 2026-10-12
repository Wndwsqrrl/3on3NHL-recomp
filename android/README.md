# 3 on 3 NHL Arcade - Android

Android (arm64) port of the ReXGlue-recompiled game, wrapped in an SDL3 activity.
It boots and renders (title screen and menus) on real hardware with the Vulkan
renderer; input is the main feature still in progress (see below).

## How it fits together

- The game is compiled as a **shared library** `libmain.so` (CMake target
  `nhl3on3`, `OUTPUT_NAME main`). SDL3 is statically linked into it.
- `RexActivity` extends SDL3's `SDLActivity` and loads only `main`.
- `SDLActivity` calls `SDL_main`, which the SDK's `windowed_app_main_sdl.cpp`
  defines for Android - it runs the Android init hooks, points the data roots at
  the app's external storage, and starts the normal app loop.
- The renderer is **Vulkan** (`vkCreateAndroidSurfaceKHR` via the SDK's
  `AndroidNativeWindowSurface`); D3D12 is off.
- The GPU plugin (`librexgpu-xenos*.so`) is packaged in the APK and loaded by
  soname at runtime.

## Prerequisites

- Android Studio with the Android SDK, **NDK (r26+)**, and CMake.
- A JDK 17+ for Gradle (Android Studio's bundled JBR works; set `JAVA_HOME` to
  `<Android Studio>/jbr` for command-line builds).
- A host build of the ReXGlue SDK (see [../BUILDING.md](../BUILDING.md) step 1):
  the recompiler runs on the host, so the Android build reuses the host
  `rexglue.exe`. `build.gradle` finds it under `rexglue-sdk/out/...` and passes
  it as `REXGLUE_HOST_CODEGEN`.
- The `rexglue-sdk` checkout. `gradle.properties` -> `rexSdkDir` points at it
  (default `../../rexglue-sdk`, a sibling of this repo).
- The game files staged at `../game/extracted/` (same as the desktop build).

## Build

Open `3on3NHL-recomp/android/` in Android Studio, or from the command line:

```
./gradlew :app:assembleDebug        # or :app:assembleRelease (needs a signing config)
```

Gradle drives the NDK/CMake cross-build: it cross-compiles the ReXGlue SDK and
the recompiled game for `arm64-v8a` and packages the APK at
`app/build/outputs/apk/debug/app-debug.apk`. The native code is built
RelWithDebInfo (`-O2`) in both variants - a `-O0` build of the ~230 MB of
recompiled C++ is impractical.

A standalone native configure (no Gradle), handy for smoke-testing CMake:

```
cmake --preset android-arm64          # from 3on3NHL-recomp/, needs ANDROID_NDK_HOME
cmake --build --preset android-arm64
```

## Staging game data on the device

The APK ships **no game data** - you provide your own, exactly like the desktop
build. Push your extracted files so `default.xex` lands under the app's external
storage, then make them readable by the app:

```
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb push ../game/extracted "/sdcard/Android/data/com.wndwsqrrl.nhl3on3/files/game/"
adb shell chmod -R 777 "/sdcard/Android/data/com.wndwsqrrl.nhl3on3/files/game"
```

`adb push` creates the files owned by `shell`, so the `chmod` is needed for the
app to read them; redo it whenever you re-push. A first-run importer (hand the
app your XBLA package, it extracts it to its own storage) will replace this and
remove the `chmod` step.

## Remaining work

- **Input:** on-screen touch controls, and confirming external gamepad support.
- **In-app game-data import** (replaces the manual `adb push` + `chmod`).
- Force landscape orientation (currently letterboxed in portrait).
- App icon and launcher polish.
- Quiet the Adreno Vulkan validation warnings (harmless but noisy).
