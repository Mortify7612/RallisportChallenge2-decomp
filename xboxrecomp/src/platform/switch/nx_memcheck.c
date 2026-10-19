/*
 * nx_memcheck.c - stop with a readable message when the process is too small.
 *
 * hbmenu opened from the Album runs as a library applet, and so does
 * everything it starts: about 380 MB in all, of which the .nro itself takes
 * a sixth. The guest's memory and the renderer do not fit, and the first
 * mapping that fails leaves a device window missing -- the game then hangs
 * on a black screen. Started instead through a game (hold R while launching
 * it), hbmenu runs as that application and gets about 3.2 GB.
 *
 * Apart from nx_host.c because <switch.h>'s console, pad and applet headers
 * clash with win32_compat.h.
 */
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NX_MEM_NEEDED (1024ull * 1024 * 1024)   /* comfortably above applet mode */

/* 1 when there is room (or RSC2_NX_LOW_MEMORY=1 says to try anyway), else
 * shows why there is not, waits for a button and returns 0. */
int nx_memory_check(void)
{
    u64 total = 0;
    const char *force = getenv("RSC2_NX_LOW_MEMORY");
    PadState pad;

    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(stderr, "[SWITCH] %llu MB of memory (%s), %s\n", (unsigned long long)(total >> 20),
            appletGetAppletType() == AppletType_Application ? "application" : "applet mode",
            appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    if (total >= NX_MEM_NEEDED || (force && strcmp(force, "1") == 0))
        return 1;

    fprintf(stderr, "[SWITCH] not enough memory: start hbmenu by holding R while"
            " launching a game (RSC2_NX_LOW_MEMORY=1 tries anyway)\n");
    consoleInit(NULL);
    printf("\n  RalliSport Challenge 2\n\n"
           "  Only %llu MB of memory is available: hbmenu is running in applet\n"
           "  mode (opened from the Album). The game needs more than that.\n\n"
           "  Start hbmenu through a game instead: hold R while launching any\n"
           "  game from the HOME menu, then start rsc2 (or press Y there for\n"
           "  nxlink).\n\n"
           "  Press + to exit.\n", (unsigned long long)(total >> 20));
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
