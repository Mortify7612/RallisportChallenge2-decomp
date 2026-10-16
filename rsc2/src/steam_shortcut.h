#ifndef RSC2_STEAM_SHORTCUT_H
#define RSC2_STEAM_SHORTCUT_H

#include <stddef.h>

/* Adds the game to Steam as a non-Steam shortcut; `msg` gets a sentence for
 * the player either way. 1 when an entry was written. */
int rsc2_steam_add(char *msg, size_t n);

#endif
