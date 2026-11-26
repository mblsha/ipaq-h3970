/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef H3970_DOOM_AUDIO_H
#define H3970_DOOM_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

enum { H3970_PCM_RATE = 16000 };

bool H3970_OpenAudio(void);
void H3970_LockAudio(void);
void H3970_UnlockAudio(void);
void H3970_PollMusic(void);
void H3970_RenderMusic(int16_t *output, unsigned int frames);

#endif
