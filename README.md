# nhl3on3-recomp

A native PC port of **NHL 3 on 3 Arcade** (Xbox 360, XBLA, 2009), built by statically
recompiling the original PowerPC executable to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). No emulator at runtime.

## Status

Playable: intro movies, menus and full matches work with graphics, audio and controller input.

Known issues:
- Online play (the Xbox Live menu) is not available.
- Higher internal resolution (`--draw_resolution_scale_x/y`) breaks text and some textures.

## Playing it

You need your own copy of the game's Xbox Live Arcade package (title ID `58410975`).
This repository and its releases contain **no game code or data**.

1. Download the latest release and unzip it.
2. Put your XBLA package next to it (the file inside `58410975\000D0000\`; copying the whole
   `58410975` folder in works too).
3. Run `NHL 3on3 Launcher.exe`, click **Set up game**, then **Play**.

Xbox and PlayStation controllers work. If a PlayStation controller is not detected, close Steam,
whose Steam Input can take the controller over.

Achievements work: press **F7** in-game for the list. Unlocks are saved to
`Documents\nhl3on3\achievements\`. The two online achievements can't be earned yet.
`Documents

Settings live in `nhl3on3.toml` next to the game; command-line flags override them.

## Building

See [BUILDING.md](BUILDING.md). The build needs the ReXGlue SDK with the fixes in
[patches/rexglue](patches/rexglue/README.md), and your own copy of the game.

## What this repository contains

| Path | Contents |
|---|---|
| `nhl3on3_manifest.toml`, `nhl3on3_functions.toml` | Recompiler configuration |
| `nhl3on3.toml` | Runtime settings |
| `src/` | The port's own code |
| `launcher/` | The launcher (C#) that sets up the game from your package |
| `patches/rexglue/` | Fixes the port needs in ReXGlue |
| `dist/` | Player README and license texts shipped with releases |

The recompiler's generated C++ is built locally and never committed.

## Credits

Built on [ReXGlue](https://github.com/rexglue/rexglue-sdk), which includes code derived from
the [Xenia](https://xenia.jp) Xbox 360 emulator. Third-party software is listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

## License

MIT for the code in this repository. See [LICENSE](LICENSE). NHL 3 on 3 Arcade and all of
its assets are property of Electronic Arts; this project is not affiliated with or endorsed
by EA or the NHL.
