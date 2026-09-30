3 on 3 NHL Arcade - PC port
===========================

This is a native PC port of 3 on 3 NHL Arcade (Xbox 360, XBLA). It contains no game
code or data: you need your own copy of the game's Xbox Live Arcade package.

Setup
-----
1. Unzip this folder anywhere.
2. Put your XBLA package next to it. That is the file inside
   58410975\000D0000\ (its name is a long string of letters and digits).
   You can copy the whole "58410975" folder in, or pick the file with Browse.
3. Run "NHL 3on3 Launcher.exe" and click "Set up game". It checks the package is
   3 on 3 NHL Arcade and unpacks it into a "game" folder.
4. Click Play. Afterwards you can also start nhl3on3.exe directly.

Controls
--------
Xbox and PlayStation controllers work. If a PlayStation controller is not detected,
close Steam (Steam Input can take over the controller) and start the game again.

Achievements
------------
Press F7 in-game to see them. Unlocks are saved in Documents
hl3on3chievements.
The two online achievements (Let's Play, Team Player) can't be earned yet.

Settings
--------
nhl3on3.toml holds the settings the game needs; command-line flags override it.

Known issues
------------
- Online play (Xbox Live menu) is not available.
- Higher internal resolution (--draw_resolution_scale_x/y) breaks text and some textures.

Licenses are in the licenses folder.
