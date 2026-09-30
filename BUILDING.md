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

Files in this repository:

| File | Purpose |
|---|---|
| `nhl3on3_manifest.toml` | ReXGlue project manifest: which XEX to recompile |
| `nhl3on3_functions.toml` | Functions the recompiler's analysis misses (C++ thunks, leaf getters reached only through vtables or callbacks) |
| `nhl3on3.toml` | Runtime settings, copied next to the executable |
| `src/` | The app class and kernel exports the runtime lacks |

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
