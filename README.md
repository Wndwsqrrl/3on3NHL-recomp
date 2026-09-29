# nhl3on3-recomp

A native PC port of **NHL 3 on 3 Arcade** (Xbox 360, XBLA, 2009), built by statically
recompiling the original PowerPC executable to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). No emulator at runtime.

Inspired by [OpenJam](https://github.com/GTTeancum/OpenJam).

## Status

Early setup. Nothing boots yet.

## You need your own copy

This repository contains **no game code or data**. You must supply your own legally
obtained XBLA package. Put it in `game/`, which git ignores.

## What this repository contains

Only the tooling, configuration and patches needed to recompile and run the game.
The recompiler's generated C++ is built locally and never committed.

## License

MIT for the code in this repository. See [LICENSE](LICENSE). NHL 3 on 3 Arcade and all of
its assets are property of Electronic Arts; this project is not affiliated with or endorsed
by EA or the NHL.
