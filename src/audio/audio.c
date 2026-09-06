#include "oops/audio.h"
#include <stddef.h>

/* Platform symbols from libSceAudioOut and libSceUserService */
__attribute__((weak)) int sceAudioOutInit(void);
__attribute__((weak)) int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
__attribute__((weak)) int sceAudioOutClose(int handle);
__attribute__((weak)) int sceAudioOutOutput(int handle, const void *ptr);
__attribute__((weak)) int sceAudioOutSetVolume(int handle, int flag, int *volumes);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

#define OOPS_AUDIO_MAX_CHUNK 2048
#define OOPS_AUDIO_DEFAULT_FRAMES 512

struct oops_audio_port {
    int handle;
    int channels;
    int sample_rate;
    int chunk_frames;
    /* Aligned staging buffer for hardware output */
    int16_t staging[OOPS_AUDIO_MAX_CHUNK * 2] __attribute__((aligned(64)));
};

static struct oops_audio_port s_default_audio = { -1, 2, 48000, OOPS_AUDIO_DEFAULT_FRAMES, {0} };
static int s_audio_initialized = 0;

static int s_last_audio_error = 0;

int oops_audio_get_last_error(void) {
    return s_last_audio_error;
}

oops_audio_port_t *oops_audio_open(int sample_rate, int channels, int buffer_frames) {
    if (!sceAudioOutOpen) {
        s_last_audio_error = -1001;
        return NULL;
    }

    if (!s_audio_initialized) {
        if (sceAudioOutInit) {
            int init_rc = sceAudioOutInit();
            (void)init_rc;
        }
        s_audio_initialized = 1;
    }

    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
            sceUserServiceInitialize(NULL);
            (void)sceUserServiceGetInitialUser(&user);
        }
    }

    /* Ensure buffer frames is a multiple of 256 within [256, 2048] */
    int frames = buffer_frames;
    if (frames <= 0) frames = OOPS_AUDIO_DEFAULT_FRAMES;
    frames = (frames + 255) & ~255;
    if (frames < 256) frames = 256;
    if (frames > OOPS_AUDIO_MAX_CHUNK) frames = OOPS_AUDIO_MAX_CHUNK;

    int rate = (sample_rate <= 0) ? 48000 : sample_rate;
    int ch = (channels <= 0) ? 2 : channels;

    int handle = -1;
    /* Try with resolved user first */
    if (user >= 0) {
        handle = sceAudioOutOpen(user, 0, 0, (unsigned int)frames, (unsigned int)rate, 0);
    }
    /* Fallback with system user 0xFF if failed */
    if (handle < 0) {
        handle = sceAudioOutOpen(0xFF, 0, 0, (unsigned int)frames, (unsigned int)rate, 0);
    }
    if (handle < 0) {
        s_last_audio_error = handle;
        return NULL;
    }

    s_default_audio.handle = handle;
    s_default_audio.channels = ch;
    s_default_audio.sample_rate = rate;
    s_default_audio.chunk_frames = frames;

    return &s_default_audio;
}

int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, size_t frame_count) {
    if (!port || port->handle < 0 || !sceAudioOutOutput || !pcm_samples || frame_count == 0) {
        return -1;
    }

    size_t frames_left = frame_count;
    const int16_t *src = pcm_samples;
    int chunk = port->chunk_frames;

    while (frames_left > 0) {
        size_t to_write = (frames_left > (size_t)chunk) ? (size_t)chunk : frames_left;
        if (to_write == (size_t)chunk) {
            int rc = sceAudioOutOutput(port->handle, src);
            if (rc < 0) return rc;
        } else {
            /* Zero-pad partial chunk */
            for (size_t i = 0; i < (size_t)chunk * 2; i++) {
                port->staging[i] = 0;
            }
            for (size_t i = 0; i < to_write * 2; i++) {
                port->staging[i] = src[i];
            }
            int rc = sceAudioOutOutput(port->handle, port->staging);
            if (rc < 0) return rc;
        }
        frames_left -= to_write;
        src += to_write * 2;
    }

    return 0;
}

int oops_audio_set_volume(oops_audio_port_t *port, float left, float right) {
    if (!port || port->handle < 0 || !sceAudioOutSetVolume) return -1;

    if (left < 0.0f) left = 0.0f;
    if (left > 1.0f) left = 1.0f;
    if (right < 0.0f) right = 0.0f;
    if (right > 1.0f) right = 1.0f;

    /* Hardware scale: 0 to 32768 */
    int volumes[2];
    volumes[0] = (int)(left * 32768.0f);
    volumes[1] = (int)(right * 32768.0f);

    return sceAudioOutSetVolume(port->handle, 3, volumes);
}

void oops_audio_close(oops_audio_port_t *port) {
    if (port && port->handle >= 0 && sceAudioOutClose) {
        sceAudioOutClose(port->handle);
        port->handle = -1;
    }
}
