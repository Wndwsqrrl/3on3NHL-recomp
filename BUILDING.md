# Building

Windows 10/11 x64. The build needs your own copy of the game: the recompiler reads its
`default.xex`, which is why there is no CI.

## Requirements

- Visual Studio 2022 or newer with the **Desktop development with C++** workload and the
  individual components **C++ Clang Compiler for Windows** (Clang 20+) and
  **MSBuild support for LLVM (clang-cl) toolset**. CMake 3.25+ and Ninja come with Visual Studio.
- Git.
- .NET 8 SDK (for the launcher only).
- Your 3 on 3 NHL Arcade XBLA package (title ID `58410975`).

Run all build commands from an **x64 Native Tools / Developer PowerShell** for Visual Studio,
with Visual Studio's Clang on `PATH`, e.g.
`$env:PATH = "$env:VCINSTALLDIR\Tools\Llvm\x64\bin;$env:PATH"`.

## 1. Build the ReXGlue SDK

```
git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git
cd rexglue-sdk
git apply <this-repo>/patches/rexglue/0001-input-system-lock.patch
git apply <this-repo>/patches/rexglue/0002-codegen-vpk-unsigned-alias.patch
git apply <this-repo>/patches/rexglue/0003-android-support.patch
cmake --preset win-amd64
cmake --build out/build/win-amd64 --target install
```

The install step registers the SDK with CMake, so this project finds it automatically.
See [patches/rexglue](patches/rexglue/README.md) for what the patches fix.

**Symlinks:** some SDK submodules contain symlinks (e.g. `thirdparty/libmspack`). If Git has
`core.symlinks=false` (the Windows default without Developer Mode), they check out as one-line
text files and the build fails with errors on line 1 of files such as `lzxd.c`. Enable Developer
Mode and clone with `git -c core.symlinks=true clone ...`, or replace each such file with a copy
of the file it points to.

## 2. Add the game files

Unpack the XBLA package so that `default.xex` ends up at `game/extracted/default.xex` in this
repository (for example with Velocity or Horizon). The `game/` folder is ignored by Git.

## 3. Build the game

```
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release
```

The first build runs the recompiler (about a minute), which writes roughly 230 MB of C++ to
`generated/default/`, then compiles it, which takes a while. The result is
`out/build/win-amd64-release/nhl3on3.exe` with its DLLs and `nhl3on3.toml`. It finds
`game/extracted` on its own; pass `--game_data_root=<folder>` to use another copy.

Other presets: `win-amd64-debug` (slow, for tracking down crashes) and
`win-amd64-relwithdebinfo` (optimized, with debug info). Release is built at `-O2` on purpose:
at `-O3` the in-game scoreboard does not render.

What's in this repository:

| Path | Purpose |
|---|---|
| `nhl3on3_manifest.toml` | ReXGlue project manifest: which XEX to recompile |
| `nhl3on3_functions.toml` | Functions the recompiler's analysis misses (C++ thunks, leaf getters reached only through vtables or callbacks) |
| `nhl3on3.toml` | Runtime settings, copied next to the executable |
| `src/` | The app class and kernel exports the runtime lacks or only stubs |
| `launcher/` | The launcher (C#, .NET 8) |
| `android/` | The Android (arm64) app: a Gradle/SDL3 project that builds the game into an APK |
| `patches/rexglue/` | Fixes the port needs in ReXGlue |
| `dist/` | Player README and license texts shipped with releases |
| `docs/images/` | Screenshots for the README |

The recompiler's generated C++ (`generated/default/`) is built locally and never committed.

## 4. Build the launcher

```
dotnet publish launcher -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o out/launcher
```

## 5. Assemble a release

One folder containing:

- from `out/build/win-amd64-release/`: `nhl3on3.exe`, `rexruntime.dll`, `rexgpu-xenos.dll`, `nhl3on3.toml`
- from `out/launcher/`: `NHL 3on3 Launcher.exe`
- `dist/README.txt`
- `dist/licenses/` as `licenses/`, plus this repository's `LICENSE` as `licenses/nhl3on3.txt`

Never include anything from `game/` or `generated/`.

`rexruntime.dll` contains FFmpeg and libmspack under the LGPL 2.1; see
[THIRD_PARTY.md](THIRD_PARTY.md) for how to rebuild it with modified versions.

## Android (arm64)

The game also builds for Android (arm64-v8a): a shared library (`libmain.so`) loaded by an
SDL3 activity, with the renderer on Vulkan. The project is in [`android/`](android/); its
[README](android/README.md) has more detail.

**Prerequisites (in addition to the above):**

- Android Studio, with the Android SDK, **NDK (r26+)** and CMake (SDK Manager -> SDK Tools).
- A JDK 17+ for Gradle; Android Studio's bundled JBR works (point `JAVA_HOME` at
  `<Android Studio>/jbr` for command-line builds).
- The ReXGlue SDK with the patches applied (step 1). The Android build cross-compiles the SDK
  from source and reuses the **host** recompiler for codegen, so build the host SDK first - the
  app's `build.gradle` finds `rexglue-sdk/out/.../rexglue.exe` automatically and passes it as
  `REXGLUE_HOST_CODEGEN` (codegen can't run a cross-compiled tool on the build host).
- The game files staged at `game/extracted/` (step 2), same as the desktop build.

**Build the APK:**

Point `android/gradle.properties` -> `rexSdkDir` at your `rexglue-sdk` checkout (default
`../../rexglue-sdk`, i.e. a sibling of this repo), then from `android/`:

```
./gradlew :app:assembleDebug        # or :app:assembleRelease (needs a signing config)
```

Gradle drives the NDK/CMake cross-build and packages `app/build/outputs/apk/debug/app-debug.apk`
(~75 MB: `libmain.so`, `librexruntime*.so`, the `librexgpu-xenos*.so` Vulkan plugin, and
`libc++_shared.so`). The native code is built RelWithDebInfo (`-O2`) in both variants - a `-O0`
build of the ~230 MB of recompiled C++ is impractical. Opening `android/` in Android Studio also
works.

**Install and provide the game data on-device:**

```
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The APK ships no game content; each player supplies their own package, just like the desktop
build. Until the in-app importer lands, stage the extracted files under the app's external
storage:

```
adb push game/extracted "/sdcard/Android/data/com.wndwsqrrl.nhl3on3/files/game/"
adb shell chmod -R 777 "/sdcard/Android/data/com.wndwsqrrl.nhl3on3/files/game"
```

(The `chmod` is needed because `adb push` creates the files owned by `shell`; redo it whenever
you re-push. A first-run importer that lets the app extract an XBLA package it is handed will
replace this.)
