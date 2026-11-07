//
// Minimal in-process OPL driver for doomgeneric.
//
// Chocolate Doom's OPL driver runs a separate thread fed by an SDL audio
// device. Here the game's own mixer pulls samples, so time only advances
// when audio is generated and a callback queue replaces the timer thread.

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "opl.h"
#include "opl_internal.h"
#include "opl3.h"
#include "opl_queue.h"

static opl3_chip chip;
static opl_callback_queue_t *callback_queue;
static int paused;
static unsigned int register_num;
static int opl3mode;

typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;
} local_timer_t;

static local_timer_t timer1 = { 12500, 0, 0, 0 };
static local_timer_t timer2 = { 3125, 0, 0, 0 };

static uint64_t NowUs(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * OPL_SECOND + (uint64_t) ts.tv_nsec / 1000;
}

static void TimerUpdate(local_timer_t *timer)
{
    if (timer->enabled)
    {
        unsigned int tics = 0x100 - timer->value;

        timer->expire_time = NowUs()
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static int Local_Init(unsigned int port_base)
{
    (void) port_base;

    callback_queue = OPL_Queue_Create();
    if (callback_queue == NULL)
    {
        return 0;
    }

    OPL3_Reset(&chip, opl_sample_rate);

    paused = 0;
    register_num = 0;
    opl3mode = 0;

    return 1;
}

static void Local_Shutdown(void)
{
    if (callback_queue != NULL)
    {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
}

static unsigned int Local_PortRead(opl_port_t port)
{
    unsigned int result = 0;
    uint64_t now = NowUs();

    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0xff;
    }

    if (timer1.enabled && now > timer1.expire_time)
    {
        result |= 0x80 | 0x40;
    }
    if (timer2.enabled && now > timer2.expire_time)
    {
        result |= 0x80 | 0x20;
    }

    return result;
}

static void Local_WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            TimerUpdate(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            TimerUpdate(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    TimerUpdate(&timer1);
                }
                if ((value & 0x20) == 0)
                {
                    timer2.enabled = (value & 0x02) != 0;
                    TimerUpdate(&timer2);
                }
            }
            break;

        case OPL_REG_NEW:
            opl3mode = value & 0x01;
            /* fallthrough */
        default:
            OPL3_WriteRegBuffered(&chip, (Bit16u) reg_num, (Bit8u) value);
            break;
    }
}

static void Local_PortWrite(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_REGISTER_PORT_OPL3)
    {
        register_num = value | 0x100;
    }
    else if (port == OPL_DATA_PORT)
    {
        Local_WriteRegister(register_num, value);
    }
}

static void Local_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    OPL_Queue_Push(callback_queue, callback, data, NowUs() + us);
}

static void Local_ClearCallbacks(void)
{
    OPL_Queue_Clear(callback_queue);
}

static void Local_Lock(void) { }
static void Local_Unlock(void) { }
static void Local_SetPaused(int value) { paused = value; }

static void Local_AdjustCallbacks(float factor)
{
    OPL_Queue_AdjustCallbacks(callback_queue, NowUs(), factor);
}

opl_driver_t opl_local_driver =
{
    "local",
    Local_Init,
    Local_Shutdown,
    Local_PortRead,
    Local_PortWrite,
    Local_SetCallback,
    Local_ClearCallbacks,
    Local_Lock,
    Local_Unlock,
    Local_SetPaused,
    Local_AdjustCallbacks,
};

void OPL_Local_Render(int16_t *buffer, unsigned int nsamples)
{
    static int16_t oplbuf[1024 * 2];
    unsigned int done = 0;

    if (paused)
    {
        return;
    }

    while (done < nsamples)
    {
        unsigned int chunk = nsamples - done;

        if (chunk > 256)
        {
            chunk = 256;
        }

        /* Run every timer callback that is due. */
        while (callback_queue != NULL && !OPL_Queue_IsEmpty(callback_queue)
            && OPL_Queue_Peek(callback_queue) <= NowUs())
        {
            opl_callback_t callback;
            void *data;

            if (!OPL_Queue_Pop(callback_queue, &callback, &data))
            {
                break;
            }
            if (callback != NULL)
            {
                callback(data);
            }
        }

        OPL3_GenerateStream(&chip, oplbuf, chunk);

        for (unsigned int i = 0; i < chunk; ++i)
        {
            int mixed = buffer[done + i]
                      + ((int) oplbuf[i * 2] + (int) oplbuf[i * 2 + 1]);

            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            buffer[done + i] = (int16_t) mixed;
        }

        done += chunk;
    }
}
