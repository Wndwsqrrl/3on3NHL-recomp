package com.wndwsqrrl.nhl3on3;

import org.libsdl.app.SDLActivity;

/**
 * Entry activity for the 3 on 3 NHL Arcade Android port.
 *
 * ReXGlue links SDL3 statically into libmain.so, so we load only that single
 * library. SDLActivity's default getLibraries() would also try to load a
 * separate libSDL3.so, which does not exist in this build.
 *
 * SDLActivity then resolves the main shared object from the last entry here
 * (libmain.so) and calls its SDL_main (our Android entry in the SDK's
 * windowed_app_main_sdl.cpp).
 */
public class RexActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
