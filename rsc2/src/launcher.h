/* Pre-game launcher and saved settings (launcher.c). */
#ifndef RSC2_LAUNCHER_H
#define RSC2_LAUNCHER_H

/* Loads the saved settings, shows the launcher window when wanted, and
 * turns the settings into the RECOMP_* environment the runtime reads. An
 * environment variable already set by hand wins over the saved value.
 * Returns 1 to start the game, 0 when the player quit from the launcher. */
int rsc2_launcher(int argc, char **argv);

#endif
