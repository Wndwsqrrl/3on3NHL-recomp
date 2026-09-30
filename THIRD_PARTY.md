# Third-party software

The release contains code from these projects. Their license texts are in
[`dist/licenses/`](dist/licenses) and ship in the release's `licenses` folder.

| Project | License | Where it is in the release |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0, including code derived from [Xenia](https://xenia.jp) | BSD 3-Clause | `rexruntime.dll`, `rexgpu-xenos.dll`, `nhl3on3.exe` |
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil), commit `0604b464c7` | LGPL 2.1 or later | `rexruntime.dll` |
| [libmspack](https://www.cabextract.org.uk/libmspack/), commit `3059077` | LGPL 2.1 | `rexruntime.dll` |
| [SDL 3](https://www.libsdl.org) | zlib | `rexruntime.dll` |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | `rexruntime.dll`, `nhl3on3.exe` |
| [spdlog](https://github.com/gabime/spdlog) | MIT | all binaries |
| [fmt](https://github.com/fmtlib/fmt) | MIT | all binaries (through spdlog) |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT | `rexruntime.dll` |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD 2-Clause | `nhl3on3.exe` |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT | `rexruntime.dll` (settings file loader) |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT | `nhl3on3.exe` |
| [UTF8-CPP](https://github.com/nemtrif/utfcpp) | Boost Software License 1.0 | header-only, used by the ReXGlue runtime |

The launcher is a .NET 8 application; the self-contained build includes the .NET runtime (MIT).

## LGPL components

FFmpeg is built without GPL, version-3 or non-free components (`CONFIG_GPL 0`,
`CONFIG_VERSION3 0`, `CONFIG_NONFREE 0`). FFmpeg and libmspack are linked statically into
`rexruntime.dll`. To use a modified version of either, rebuild `rexruntime.dll` from the
ReXGlue source (FFmpeg and libmspack are its `thirdparty/FFmpeg` and `thirdparty/libmspack`
submodules at the commits above) following [BUILDING.md](BUILDING.md), and replace the DLL in
the release folder. The complete source for this port and its build instructions are in this
repository.
