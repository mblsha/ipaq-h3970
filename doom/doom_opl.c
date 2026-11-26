/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Feed the Chocolate Doom sequencer from the same sample clock as the mixer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doom_audio.h"
#include "fmopl.h"
#include "opl.h"
#include "opl_queue.h"

static FM_OPL *chip;
static opl_callback_queue_t *callbacks;
static uint64_t music_frames;
static bool paused;

opl_init_result_t OPL_Init(unsigned int port_base)
{
    (void)port_base;
    chip = OPLCreate(3579545, H3970_PCM_RATE);
    if (!chip) {
        return OPL_INIT_NONE;
    }
    callbacks = OPL_Queue_Create();
    if (!H3970_OpenAudio()) {
        OPL_Shutdown();
        return OPL_INIT_NONE;
    }
    atexit(OPL_Shutdown);
    fprintf(stderr, "H3970 DOOM: WAD music via OPL2 at %d Hz\n", H3970_PCM_RATE);
    return OPL_INIT_OPL2;
}

void OPL_Shutdown(void)
{
    OPL_Lock();
    if (chip) {
        OPL_Queue_Destroy(callbacks);
        OPLDestroy(chip);
        callbacks = NULL;
        chip = NULL;
        music_frames = 0;
        paused = false;
    }
    OPL_Unlock();
}

void OPL_SetSampleRate(unsigned int rate)
{
    /* Both streams must use the PCM device's fixed rate. */
    (void)rate;
}

void OPL_WriteRegister(int reg, int value)
{
    OPLWrite(chip, 0, reg);
    OPLWrite(chip, 1, value);
}

void OPL_InitRegisters(int opl3)
{
    (void)opl3;
    /* OPL2 initialization from Chocolate Doom's OPL_InitRegisters. */
    for (int reg = 1; reg < OPL_REGS_LEVEL; ++reg) {
        OPL_WriteRegister(reg, 0);
    }
    for (int reg = OPL_REGS_LEVEL; reg <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++reg) {
        OPL_WriteRegister(reg, 0x3f);
    }
    for (int reg = OPL_REGS_ATTACK; reg <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++reg) {
        OPL_WriteRegister(reg, 0);
    }
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x80);
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);
    OPL_WriteRegister(OPL_REG_FM_MODE, 0x40);
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    const uint64_t frames = (us * H3970_PCM_RATE + OPL_SECOND - 1) / OPL_SECOND;
    OPL_Queue_Push(callbacks, callback, data, music_frames + frames);
}

void OPL_AdjustCallbacks(float factor)
{
    OPL_Queue_AdjustCallbacks(callbacks, music_frames, factor);
}

void OPL_ClearCallbacks(void)
{
    OPL_Queue_Clear(callbacks);
}

void OPL_Lock(void) { H3970_LockAudio(); }
void OPL_Unlock(void) { H3970_UnlockAudio(); }

void OPL_SetPaused(int pause)
{
    paused = pause != 0;
}

void H3970_RenderMusic(int16_t *output, unsigned int frames)
{
    if (!chip) {
        memset(output, 0, frames * sizeof(*output));
        return;
    }
    if (paused) {
        YM3812UpdateOne(chip, output, frames);
        return;
    }
    while (frames) {
        uint64_t next = OPL_Queue_IsEmpty(callbacks) ? UINT64_MAX : OPL_Queue_Peek(callbacks);
        if (next <= music_frames) {
            opl_callback_t callback;
            void *data;
            if (OPL_Queue_Pop(callbacks, &callback, &data)) {
                callback(data);
            }
            continue;
        }
        unsigned int block = next - music_frames < frames ? next - music_frames : frames;
        YM3812UpdateOne(chip, output, block);
        output += block;
        frames -= block;
        music_frames += block;
    }
}
