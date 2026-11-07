#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>

#include "config.h"
#include "doomtype.h"
#include "doomfeatures.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "m_misc.h"
#include "deh_str.h"

extern int snd_samplerate;

#define NUM_CHANNELS 8
#define OUT_RATE 11025
#define OUT_MAX_SAMPLES 1024
extern void OPL_Local_Render(int16_t *buffer, unsigned int nsamples);

#define MAX_BACKLOG (OUT_RATE) /* one second of catch-up at most */

int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

typedef struct
{
    const unsigned char *samples;
    int length;
} alsa_sfx_t;

typedef struct
{
    const unsigned char *samples;
    int length;
    int pos;
    int vol;
    boolean active;
} alsa_channel_t;

static alsa_channel_t channels[NUM_CHANNELS];
static FILE *aplay;
static boolean sound_initialized = false;
static boolean use_sfx_prefix = true;
static long long samples_written;
static struct timespec t_start;
static int16_t mixbuf[OUT_MAX_SAMPLES];

static double ElapsedSeconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t_start.tv_sec)
         + (now.tv_nsec - t_start.tv_nsec) / 1e9;
}

static int SfxLumpNumber(sfxinfo_t *sfxinfo)
{
    char namebuf[9];
    sfxinfo_t *sfx = sfxinfo;

    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    if (use_sfx_prefix)
    {
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(namebuf, DEH_String(sfx->name), sizeof(namebuf));
    }

    return W_GetNumForName(namebuf);
}

static boolean CacheSfx(sfxinfo_t *sfxinfo)
{
    int lumpnum;
    unsigned int lumplen;
    unsigned char *data;
    unsigned int length;

    if (sfxinfo->driver_data != NULL)
    {
        return true;
    }

    lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0)
    {
        lumpnum = SfxLumpNumber(sfxinfo);
        sfxinfo->lumpnum = lumpnum;
    }

    if (lumpnum < 0)
    {
        return false;
    }

    data = W_CacheLumpNum(lumpnum, PU_STATIC);
    lumplen = W_LumpLength(lumpnum);

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
    {
        return false;
    }

    length = (unsigned int) data[4]
           | ((unsigned int) data[5] << 8)
           | ((unsigned int) data[6] << 16)
           | ((unsigned int) data[7] << 24);

    if (length > lumplen - 8 || length <= 48)
    {
        return false;
    }

    /* The DMX library trims 16 bytes off each end of the sample data. */
    if (length <= 32)
    {
        return false;
    }

    {
        alsa_sfx_t *sfx = malloc(sizeof(*sfx));

        if (sfx == NULL)
        {
            return false;
        }

        sfx->samples = data + 16;
        sfx->length = (int) (length - 32);
        sfxinfo->driver_data = sfx;
    }

    return true;
}

static boolean I_ALSA_InitSound(boolean sfx_prefix)
{
    char cmd[200];

    use_sfx_prefix = sfx_prefix;

    /* The music synth is configured from this. */
    snd_samplerate = OUT_RATE;

    memset(channels, 0, sizeof(channels));

    /* aplay may die if the device is busy. */
    signal(SIGPIPE, SIG_IGN);

    M_snprintf(cmd, sizeof(cmd),
        "aplay -q -t raw -f S16_LE -r %d -c 1 "
        "--period-size=512 --buffer-size=2048 2>/dev/null",
        OUT_RATE);

    aplay = popen(cmd, "w");
    if (aplay == NULL)
    {
        return false;
    }

    setvbuf(aplay, NULL, _IONBF, 0);

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    samples_written = 0;
    sound_initialized = true;

    return true;
}

static void I_ALSA_ShutdownSound(void)
{
    if (!sound_initialized)
    {
        return;
    }

    sound_initialized = false;
    if (aplay != NULL)
    {
        pclose(aplay);
        aplay = NULL;
    }
}

static int I_ALSA_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    return SfxLumpNumber(sfxinfo);
}

static void I_ALSA_UpdateSoundParams(int channel, int vol, int sep)
{
    (void) sep; /* the panel is mono */

    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS)
    {
        return;
    }

    channels[channel].vol = vol;
}

static int I_ALSA_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    int i;

    if (!sound_initialized)
    {
        return -1;
    }

    if (channel < 0)
    {
        for (i = 0; i < NUM_CHANNELS; ++i)
        {
            if (!channels[i].active)
            {
                channel = i;
                break;
            }
        }
        if (channel < 0)
        {
            return -1;
        }
    }
    else if (channel >= NUM_CHANNELS)
    {
        return -1;
    }

    if (!CacheSfx(sfxinfo))
    {
        return -1;
    }

    {
        alsa_sfx_t *cached = (alsa_sfx_t *) sfxinfo->driver_data;

        channels[channel].samples = cached->samples;
        channels[channel].length = cached->length;
    }
    channels[channel].pos = 0;
    channels[channel].vol = vol;
    channels[channel].active = true;

    I_ALSA_UpdateSoundParams(channel, vol, sep);

    return channel;
}

static void I_ALSA_StopSound(int channel)
{
    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS)
    {
        return;
    }

    channels[channel].active = false;
}

static boolean I_ALSA_SoundIsPlaying(int channel)
{
    if (!sound_initialized || channel < 0 || channel >= NUM_CHANNELS)
    {
        return false;
    }

    return channels[channel].active
        && channels[channel].pos < channels[channel].length;
}

static void MixSamples(int count)
{
    int i, ch;

    memset(mixbuf, 0, (size_t) count * sizeof(int16_t));

    for (ch = 0; ch < NUM_CHANNELS; ++ch)
    {
        alsa_channel_t *c = &channels[ch];

        if (!c->active)
        {
            continue;
        }

        for (i = 0; i < count && c->pos < c->length; ++i, ++c->pos)
        {
            int sample, mixed;

            sample = ((int) c->samples[c->pos] - 128) * c->vol * 2;
            mixed = (int) mixbuf[i] + sample;

            if (mixed > 32767)
            {
                mixed = 32767;
            }
            else if (mixed < -32768)
            {
                mixed = -32768;
            }

            mixbuf[i] = (int16_t) mixed;
        }
    }
}

static void I_ALSA_UpdateSound(void)
{
    long long due, need;

    if (!sound_initialized || aplay == NULL)
    {
        return;
    }

    due = (long long) (ElapsedSeconds() * OUT_RATE);
    need = due - samples_written;

    if (need > MAX_BACKLOG)
    {
        /* We stalled. */
        samples_written = due - OUT_MAX_SAMPLES;
        need = OUT_MAX_SAMPLES;
    }

    while (need > 0)
    {
        int count = need > OUT_MAX_SAMPLES ? OUT_MAX_SAMPLES : (int) need;

        MixSamples(count);
        OPL_Local_Render(mixbuf, (unsigned int) count);

        if (fwrite(mixbuf, sizeof(int16_t), (size_t) count, aplay)
            != (size_t) count)
        {
            sound_initialized = false;
            return;
        }

        samples_written += count;
        need -= count;
    }
}

static snddevice_t alsa_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    alsa_devices,
    arrlen(alsa_devices),
    I_ALSA_InitSound,
    I_ALSA_ShutdownSound,
    I_ALSA_GetSfxLumpNum,
    I_ALSA_UpdateSound,
    I_ALSA_UpdateSoundParams,
    I_ALSA_StartSound,
    I_ALSA_StopSound,
    I_ALSA_SoundIsPlaying,
    NULL,
};
