/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Mix Doom sound effects and OPL2 music into the H3970 playback device. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

#include "deh_main.h"
#include "doom_audio.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

enum {
    RATE = H3970_PCM_RATE,
    PERIOD = 256,
    BUFFER = 4 * PERIOD,
    CHANNELS = 32,
    FRACTION_BITS = 16,
    FADE_FRAMES = RATE / 500,
    FADE_LEVELS = 256,
    EFFECT_GAIN_DIVISOR = 1 << (FRACTION_BITS + 1),
    MUSIC_GAIN = 3,
    GAIN_SHIFT = 8,
    UNITY_GAIN = 1 << GAIN_SHIFT,
    MIX_RAMP_SHIFT = 8,
};

_Static_assert(FADE_FRAMES <= PERIOD, "An effect tail must fit in one mix period");
_Static_assert((1 << MIX_RAMP_SHIFT) == PERIOD, "The volume ramp spans one mix period");

struct voice {
    const byte *samples;
    unsigned int length;
    uint64_t position;
    uint64_t frames_left;
    uint32_t step;
    unsigned int attack;
    int left;
    int right;
    int last_left;
    int last_right;
};

struct effect_tail {
    unsigned int frames_left;
    int left;
    int right;
};

static struct voice voices[CHANNELS];
static struct effect_tail tails[CHANNELS];
static int pcm = -1;
static int mixer = -1;
static int16_t period[PERIOD][2];
static unsigned int cursor = PERIOD;
static boolean use_prefix;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_started;
static bool stopping_worker;
static bool trace_audio;
static unsigned int pcm_frames, next_trace, music_frames, effect_frames, xruns;
static unsigned int music_peak, effect_peak, output_peak;
static int music_gain = UNITY_GAIN;
static int effect_gain = UNITY_GAIN;

/* The guest has a dedicated H3970 card. Use the stock diagnostic's route
 * and volume, and keep the previous control values for shutdown. */
static struct {
    const char *name;
    int count;
    long setting;
    struct snd_ctl_elem_value saved;
    boolean changed;
} controls[] = {
    { "Headphones Switch", 1, 0 },
    { "Speaker Switch", 1, 1 },
    { "Master Playback Volume", 2, 228 },
    { "Master Playback Switch", 1, 1 },
    { "PCM Playback Volume", 1, 228 },
    { "PCM Playback Switch", 1, 1 },
    { "ADC Playback Switch", 1, 0 },
};

void H3970_LockAudio(void)
{
    pthread_mutex_lock(&audio_lock);
}

void H3970_UnlockAudio(void)
{
    pthread_mutex_unlock(&audio_lock);
}

static void shutdown_audio(void)
{
    if (worker_started) {
        H3970_LockAudio();
        stopping_worker = true;
        H3970_UnlockAudio();
        pthread_join(worker, NULL);
        worker_started = false;
    }
    if (pcm >= 0) {
        ioctl(pcm, SNDRV_PCM_IOCTL_DROP, 0);
        ioctl(pcm, SNDRV_PCM_IOCTL_HW_FREE, 0);
    }
    for (unsigned int i = sizeof(controls) / sizeof(controls[0]); i-- > 0;) {
        if (controls[i].changed) {
            ioctl(mixer, SNDRV_CTL_IOCTL_ELEM_WRITE, &controls[i].saved);
            controls[i].changed = false;
        }
    }
    if (pcm >= 0) {
        close(pcm);
        pcm = -1;
    }
    if (mixer >= 0) {
        close(mixer);
        mixer = -1;
    }
    memset(voices, 0, sizeof(voices));
    memset(tails, 0, sizeof(tails));
    cursor = PERIOD;
}

static int setup_mixer(void)
{
    for (unsigned int i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        struct snd_ctl_elem_value *saved = &controls[i].saved;
        struct snd_ctl_elem_value next;

        saved->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
        snprintf((char *)saved->id.name, sizeof(saved->id.name), "%s", controls[i].name);
        if (ioctl(mixer, SNDRV_CTL_IOCTL_ELEM_LOCK, &saved->id) < 0 ||
            ioctl(mixer, SNDRV_CTL_IOCTL_ELEM_READ, saved) < 0) {
            return -1;
        }
        next = *saved;
        for (int ch = 0; ch < controls[i].count; ch++) {
            next.value.integer.value[ch] = controls[i].setting;
        }
        if (ioctl(mixer, SNDRV_CTL_IOCTL_ELEM_WRITE, &next) < 0) {
            return -1;
        }
        controls[i].changed = true;
    }
    return 0;
}

static void pcm_mask(struct snd_pcm_hw_params *hw, unsigned int key, unsigned int bit)
{
    struct snd_mask *mask = &hw->masks[key - SNDRV_PCM_HW_PARAM_FIRST_MASK];

    memset(mask, 0, sizeof(*mask));
    mask->bits[bit / 32] = 1u << (bit % 32);
}

static void pcm_interval(struct snd_pcm_hw_params *hw, unsigned int key, unsigned int value)
{
    hw->intervals[key - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL] =
        (struct snd_interval){ .min = value, .max = value, .integer = 1 };
}

static int setup_pcm(void)
{
    struct snd_pcm_hw_params hw = {0};
    struct snd_pcm_sw_params sw = {
        .period_step = 1,
        .avail_min = PERIOD,
        .xfer_align = 1,
        .start_threshold = 2 * PERIOD,
        .stop_threshold = BUFFER,
        .boundary = BUFFER,
    };

    memset(hw.masks, 0xff, sizeof(hw.masks));
    for (unsigned int i = 0; i < sizeof(hw.intervals) / sizeof(hw.intervals[0]); ++i) {
        hw.intervals[i].max = UINT_MAX;
    }
    hw.rmask = UINT_MAX;
    pcm_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    pcm_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    pcm_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    pcm_interval(&hw, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    pcm_interval(&hw, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    pcm_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    pcm_interval(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
    pcm_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
    pcm_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER);
    while (sw.boundary <= (LONG_MAX - BUFFER) / 2) {
        sw.boundary *= 2;
    }
    if (ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0 ||
        ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        return -1;
    }
    return ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE, 0);
}

bool H3970_OpenAudio(void)
{
    struct snd_ctl_card_info card = {0};

    if (pcm >= 0) {
        return true;
    }
    mixer = open("/dev/snd/controlC0", O_RDWR | O_CLOEXEC);
    if (mixer < 0 || ioctl(mixer, SNDRV_CTL_IOCTL_CARD_INFO, &card) < 0) {
        goto error;
    }
    if (strcmp((const char *)card.name, "H3970") != 0) {
        errno = ENODEV;
        goto error;
    }
    pcm = open("/dev/snd/pcmC0D0p", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (pcm < 0 || setup_pcm() < 0 || setup_mixer() < 0) {
        goto error;
    }

    const char *trace = getenv("H3970_AUDIO_TRACE");
    trace_audio = trace && strcmp(trace, "1") == 0;
    pcm_frames = music_frames = effect_frames = xruns = 0;
    music_peak = effect_peak = output_peak = 0;
    next_trace = 2 * RATE;
    atexit(shutdown_audio);
    fprintf(stderr, "H3970 DOOM: ALSA PCM at %d Hz\n", RATE);
    return true;

error:
    perror("H3970 DOOM: ALSA");
    shutdown_audio();
    return false;
}

static boolean init_audio(boolean prefix)
{
    use_prefix = prefix;
    boolean enabled = H3970_OpenAudio();
    if (enabled) {
        fprintf(stderr, "H3970 DOOM: sound effects enabled\n");
    }
    return enabled;
}

static int lookup_sfx(sfxinfo_t *effect)
{
    char name[9];

    if (effect->link) {
        effect = effect->link;
    }
    snprintf(name, sizeof(name), "%s%s", use_prefix ? "DS" : "", DEH_String(effect->name));
    return W_CheckNumForName(name);
}

static void update_voice(int handle, int volume, int separation)
{
    if (handle >= 0 && handle < CHANNELS) {
        H3970_LockAudio();
        voices[handle].left = volume * (254 - separation);
        voices[handle].right = volume * separation;
        H3970_UnlockAudio();
    }
}

static void stop_voice(int handle)
{
    if (handle >= 0 && handle < CHANNELS) {
        H3970_LockAudio();
        struct voice *voice = &voices[handle];
        if (voice->samples) {
            tails[handle] = (struct effect_tail){
                .frames_left = FADE_FRAMES,
                .left = voice->last_left,
                .right = voice->last_right,
            };
            voice->samples = NULL;
        }
        H3970_UnlockAudio();
    }
}

static boolean voice_playing(int handle)
{
    if (pcm < 0 || handle < 0 || handle >= CHANNELS) {
        return false;
    }
    H3970_LockAudio();
    boolean playing = voices[handle].samples != NULL;
    H3970_UnlockAudio();
    return playing;
}

static int start_voice(sfxinfo_t *effect, int handle, int volume, int separation)
{
    const byte *data;
    unsigned int length, rate;

    if (pcm < 0 || handle < 0 || handle >= CHANNELS || effect->lumpnum < 0) {
        return -1;
    }
    stop_voice(handle);
    if (W_LumpLength(effect->lumpnum) < 8) {
        return -1;
    }

    /* Keep sample data resident while any voice can reference it. */
    data = W_CacheLumpNum(effect->lumpnum, PU_STATIC);
    rate = data[2] | data[3] << 8;
    length = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
             (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
    if (data[0] != 3 || data[1] != 0 || !rate ||
        length <= 48 || length > W_LumpLength(effect->lumpnum) - 8) {
        return -1;
    }

    /* DMX sound lumps include 16 padding samples at each end. */
    unsigned int sample_count = length - 32;
    uint32_t step = ((uint32_t)rate << FRACTION_BITS) / RATE;
    H3970_LockAudio();
    voices[handle] = (struct voice){
        .samples = data + 8 + 16,
        .length = sample_count,
        .frames_left = (((uint64_t)sample_count << FRACTION_BITS) + step - 1) / step,
        .step = step,
        .left = volume * (254 - separation),
        .right = volume * separation,
    };
    H3970_UnlockAudio();
    return handle;
}

static int16_t pcm_sample(int mixed)
{
    const int knee = 1 << 14;
    int level = abs(mixed);
    if (level > knee) {
        /* Smoothly compress loud combinations before they reach full scale. */
        level = 2 * knee - (knee * knee) / level;
    }
    return mixed < 0 ? -level : level;
}

static void record_peak(unsigned int *peak, int sample)
{
    unsigned int level = abs(sample);
    if (level > *peak) {
        *peak = level;
    }
}

static void mix_period(void)
{
    static int last_music_gain = UNITY_GAIN;
    static int last_effect_gain = UNITY_GAIN;
    int mixed[PERIOD][2] = {0};
    int16_t music[PERIOD];

    H3970_RenderMusic(music, PERIOD);
    for (unsigned int ch = 0; ch < CHANNELS; ++ch) {
        struct voice *voice = &voices[ch];
        struct effect_tail *tail = &tails[ch];

        for (unsigned int i = 0; tail->frames_left; ++i, --tail->frames_left) {
            mixed[i][0] += tail->left * (int)tail->frames_left / FADE_FRAMES;
            mixed[i][1] += tail->right * (int)tail->frames_left / FADE_FRAMES;
        }

        if (!voice->samples) {
            continue;
        }
        for (unsigned int i = 0; i < PERIOD && voice->frames_left; ++i) {
            unsigned int index = voice->position >> FRACTION_BITS;
            int fraction = (voice->position >> (FRACTION_BITS - 8)) & 255;
            int a = voice->samples[index];
            int b = index + 1 < voice->length ? voice->samples[index + 1] : 128;
            int sample = (a - 128) * 256 + (b - a) * fraction;
            unsigned int envelope = voice->attack;
            if (voice->frames_left < envelope) {
                envelope = voice->frames_left;
            }
            sample = sample * (int)(envelope * FADE_LEVELS / FADE_FRAMES) / FADE_LEVELS;
            voice->last_left = sample * voice->left / EFFECT_GAIN_DIVISOR;
            voice->last_right = sample * voice->right / EFFECT_GAIN_DIVISOR;
            mixed[i][0] += voice->last_left;
            mixed[i][1] += voice->last_right;
            voice->position += voice->step;
            --voice->frames_left;
            voice->attack += voice->attack < FADE_FRAMES;
        }
        if (!voice->frames_left) {
            voice->samples = NULL;
        }
    }
    const int music_delta = music_gain - last_music_gain;
    const int effect_delta = effect_gain - last_effect_gain;
    for (int i = 0; i < PERIOD; ++i) {
        int song_gain = last_music_gain + ((music_delta * i) >> MIX_RAMP_SHIFT);
        int sfx_gain = last_effect_gain + ((effect_delta * i) >> MIX_RAMP_SHIFT);
        int left = (mixed[i][0] * sfx_gain) >> GAIN_SHIFT;
        int right = (mixed[i][1] * sfx_gain) >> GAIN_SHIFT;
        int song = (music[i] * MUSIC_GAIN * song_gain) >> GAIN_SHIFT;
        period[i][0] = pcm_sample(left + song);
        period[i][1] = pcm_sample(right + song);
        if (trace_audio) {
            music_frames += song != 0;
            effect_frames += left != 0 || right != 0;
            record_peak(&music_peak, song);
            record_peak(&effect_peak, left);
            record_peak(&effect_peak, right);
            record_peak(&output_peak, period[i][0]);
            record_peak(&output_peak, period[i][1]);
        }
    }
    last_music_gain = music_gain;
    last_effect_gain = effect_gain;
}

static void read_mix_levels(void)
{
    unsigned int music = 100, effects = 100;
    FILE *file = fopen("/tmp/doom-mix", "r");
    if (file) {
        if (fscanf(file, "%u %u", &music, &effects) != 2 || music > 100 || effects > 100) {
            music = effects = 100;
        }
        fclose(file);
    }
    int next_music = (music * UNITY_GAIN + 50) / 100;
    int next_effect = (effects * UNITY_GAIN + 50) / 100;
    if (trace_audio && (next_music != music_gain || next_effect != effect_gain)) {
        fprintf(stderr, "H3970 DOOM: music volume %u, sound effects volume %u\n", music, effects);
    }
    music_gain = next_music;
    effect_gain = next_effect;
}

static void trace_pcm(unsigned int frames)
{
    pcm_frames += frames;
    if (pcm_frames >= next_trace) {
        struct snd_pcm_status status = {0};
        if (ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status) == 0) {
            fprintf(stderr,
                    "H3970 AUDIO: written=%u played=%lu state=%d music=%u sfx=%u xruns=%u\n",
                    pcm_frames, (unsigned long)status.hw_ptr, status.state,
                    music_frames, effect_frames, xruns);
            fprintf(stderr, "H3970 AUDIO LEVELS: music_peak=%u sfx_peak=%u output_peak=%u\n",
                    music_peak, effect_peak, output_peak);
        }
        next_trace += 2 * RATE;
    }
}

static bool pump_audio(void)
{
    for (unsigned int i = 0; pcm >= 0 && i < BUFFER / PERIOD; ++i) {
        struct snd_xferi transfer;

        if (cursor == PERIOD) {
            mix_period();
            cursor = 0;
        }
        transfer = (struct snd_xferi){ .buf = period + cursor, .frames = PERIOD - cursor };
        if (ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer) == 0) {
            cursor += transfer.result;
            if (trace_audio) {
                trace_pcm(transfer.result);
            }
        } else if (errno == EAGAIN || errno == EINTR) {
            return true;
        } else if (errno == EPIPE && ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE, 0) == 0) {
            ++xruns;
        } else {
            perror("H3970 DOOM: PCM write");
            return false;
        }
    }
    return true;
}

/* Service the PCM device even when the engine is busy loading a level. */
static void *playback(void *unused)
{
    (void)unused;
    struct pollfd wait = { .fd = pcm, .events = POLLOUT };
    uint32_t next_mix_check = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint32_t ms = (uint32_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (!next_mix_check || (int32_t)(ms - next_mix_check) >= 0) {
            read_mix_levels();
            next_mix_check = ms + 200;
        }
        H3970_LockAudio();
        bool active = !stopping_worker && pump_audio();
        H3970_UnlockAudio();
        if (!active) {
            break;
        }
        if (poll(&wait, 1, 30) < 0 && errno != EINTR) {
            perror("H3970 DOOM: PCM poll");
            break;
        }
    }
    return NULL;
}

void H3970_PollMusic(void)
{
    if (pcm < 0 || worker_started) {
        return;
    }
    stopping_worker = false;
    int error = pthread_create(&worker, NULL, playback, NULL);
    if (error) {
        fprintf(stderr, "H3970 DOOM: PCM worker: %s\n", strerror(error));
        shutdown_audio();
        return;
    }
    worker_started = true;
}

static snddevice_t devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    .sound_devices = devices,
    .num_sound_devices = sizeof(devices) / sizeof(devices[0]),
    .Init = init_audio,
    .Shutdown = shutdown_audio,
    .GetSfxLumpNum = lookup_sfx,
    .Update = H3970_PollMusic,
    .UpdateSoundParams = update_voice,
    .StartSound = start_voice,
    .StopSound = stop_voice,
    .SoundIsPlaying = voice_playing,
};
