package io.github.rsc2recomp;

import org.libsdl.app.SDLActivity;

/* SDL's activity, loading libSDL2.so and then libmain.so (the game), whose
 * SDL_main is in rsc2/src/main.c. */
public class RSC2Activity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
