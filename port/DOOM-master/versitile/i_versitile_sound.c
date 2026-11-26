#include "doomdef.h"
#include "i_sound.h"
#include "w_wad.h"

void I_InitSound(void)
{
    puts("Doom sound output is disabled on this build.");
}

void I_UpdateSound(void)
{
}

void I_SubmitSound(void)
{
}

void I_ShutdownSound(void)
{
}

void I_SetChannels(void)
{
}

int I_GetSfxLumpNum(sfxinfo_t *sound)
{
    char name[9];
    if (!sound || !sound->name)
        return -1;
    snprintf(name, sizeof(name), "ds%s", sound->name);
    int lump = W_CheckNumForName(name);
    return lump >= 0 ? lump : W_CheckNumForName("DSPISTOL");
}

int I_StartSound(int id, int volume, int separation, int pitch, int priority)
{
    (void)id;
    (void)volume;
    (void)separation;
    (void)pitch;
    (void)priority;
    return 0;
}

void I_StopSound(int handle)
{
    (void)handle;
}

int I_SoundIsPlaying(int handle)
{
    (void)handle;
    return 0;
}

void I_UpdateSoundParams(int handle, int volume, int separation, int pitch)
{
    (void)handle;
    (void)volume;
    (void)separation;
    (void)pitch;
}

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
}

void I_SetMusicVolume(int volume)
{
    (void)volume;
}

void I_PauseSong(int handle)
{
    (void)handle;
}

void I_ResumeSong(int handle)
{
    (void)handle;
}

int I_RegisterSong(void *data)
{
    (void)data;
    return 0;
}

void I_PlaySong(int handle, int looping)
{
    (void)handle;
    (void)looping;
}

void I_StopSong(int handle)
{
    (void)handle;
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
}
