#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doomdef.h"
#include "d_main.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_misc.h"
#include "d_net.h"

static int game_start_time;

ticcmd_t emptycmd;
int mb_used = 6;

ticcmd_t *I_BaseTiccmd(void)
{
    return &emptycmd;
}

int I_GetHeapSize(void)
{
    return mb_used * 1024 * 1024;
}

byte *I_ZoneBase(int *size)
{
    *size = I_GetHeapSize();
    byte *memory = (byte *)malloc((size_t)*size);
    if (!memory)
        I_Error("could not allocate the game zone");
    return memory;
}

int I_GetTime(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        I_Error("monotonic clock is unavailable");
    return (int)(((uint64_t)now.tv_sec * TICRATE) +
        ((uint64_t)now.tv_nsec * TICRATE) / 1000000000ULL) -
        game_start_time;
}

void I_Init(void)
{
    game_start_time = I_GetTime();
    I_InitSound();
}

void I_Quit(void)
{
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

void I_WaitVBL(int count)
{
    int target = I_GetTime() + count;
    while (I_GetTime() < target)
        yield();
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte *I_AllocLow(int length)
{
    byte *memory = (byte *)calloc(1, (size_t)length);
    if (!memory)
        I_Error("could not allocate low memory");
    return memory;
}

void I_Tactile(int on, int off, int total)
{
    (void)on;
    (void)off;
    (void)total;
}

void I_Error(char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("doom: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);

    D_QuitNetGame();
    I_ShutdownGraphics();
    exit(1);
}

void I_InitNetwork(void)
{
    doomcom = (doomcom_t *)calloc(1, sizeof(*doomcom));
    if (!doomcom)
        I_Error("could not allocate network state");
    doomcom->id = DOOMCOM_ID;
    doomcom->ticdup = 1;
    doomcom->numnodes = 1;
    doomcom->numplayers = 1;
    doomcom->consoleplayer = 0;
    netgame = false;
}

void I_NetCmd(void)
{
    if (doomcom)
        doomcom->remotenode = -1;
}
