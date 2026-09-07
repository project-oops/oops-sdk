#include "oops/audio.h"
#include "audio_port.h"
#include <stddef.h>

/* Platform symbols from libSceAudioOut and libSceUserService */
__attribute__((weak)) int sceAudioOutInit(void);
__attribute__((weak)) int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
__attribute__((weak)) int sceAudioOutClose(int handle);
__attribute__((weak)) int sceAudioOutOutput(int handle, const void *ptr);
__attribute__((weak)) int sceAudioOutSetVolume(int handle, int flag, int *volumes);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

/*
 * The sample-format selector handed to the platform's open call. The write path produces
 * 16-bit signed interleaved stereo and nothing else, so this value must select exactly that
 * on hardware. It is the value this wrapper has always passed and it is not yet confirmed
 * from a capture: if the first sound out of a payload is at the wrong speed or missing a
 * channel, this is the first thing to check.
 */
#define OOPS_AUDIO_FORMAT_PARAM 0u

static struct oops_audio_port s_default_audio = {
    -1, OOPS_AUDIO_CHANNELS, 48000, OOPS_AUDIO_DEFAULT_FRAMES, 0, NULL, {0}
};
static int s_audio_initialized = 0;
static int s_last_audio_error = OOPS_AUDIO_OK;

int oops_audio_get_last_error(void) {
    return s_last_audio_error;
}

int oops_audio_get_chunk_frames(const oops_audio_port_t *port) {
    return port ? port->chunk_frames : 0;
}

oops_audio_port_t *oops_audio_open(int sample_rate, int channels, int buffer_frames) {
    /* Arguments first, so a host without the platform still reports a bad call as bad. */
    int ch = (channels <= 0) ? OOPS_AUDIO_CHANNELS : channels;
    if (ch != OOPS_AUDIO_CHANNELS) {
        s_last_audio_error = OOPS_AUDIO_EPARAM;
        return NULL;
    }
    if (s_default_audio.handle >= 0) {
        s_last_audio_error = OOPS_AUDIO_EBUSY;
        return NULL;
    }
    /* A port that opens but can never output is not a port. */
    if (!sceAudioOutOpen || !sceAudioOutOutput) {
        s_last_audio_error = OOPS_AUDIO_EUNAVAIL;
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

    int handle = -1;
    /* Try with resolved user first */
    if (user >= 0) {
        handle = sceAudioOutOpen(user, 0, 0, (unsigned int)frames, (unsigned int)rate, OOPS_AUDIO_FORMAT_PARAM);
    }
    /* Fallback with system user 0xFF if failed */
    if (handle < 0) {
        handle = sceAudioOutOpen(0xFF, 0, 0, (unsigned int)frames, (unsigned int)rate, OOPS_AUDIO_FORMAT_PARAM);
    }
    if (handle < 0) {
        s_last_audio_error = handle;
        return NULL;
    }

    s_default_audio.handle = handle;
    s_default_audio.channels = ch;
    s_default_audio.sample_rate = rate;
    s_default_audio.chunk_frames = frames;
    s_default_audio.pending = 0;
    s_default_audio.sink = sceAudioOutOutput;
    s_last_audio_error = OOPS_AUDIO_OK;

    return &s_default_audio;
}

/* One chunk to the sink; the platform's negative code passes through, anything else is 0. */
static int oops_audio_emit(struct oops_audio_port *port, const int16_t *chunk) {
    int rc = port->sink(port->handle, chunk);
    return (rc < 0) ? rc : 0;
}

int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, size_t frame_count) {
    if (!port || port->handle < 0 || !port->sink || !pcm_samples || frame_count == 0) {
        return -1;
    }
    if (port->chunk_frames <= 0 || port->chunk_frames > OOPS_AUDIO_MAX_CHUNK) {
        return -1;
    }

    const size_t chunk = (size_t)port->chunk_frames;
    const int16_t *src = pcm_samples;
    size_t left = frame_count;

    while (left > 0) {
        if (port->pending == 0 && left >= chunk) {
            /* A whole chunk straight from the caller: no copy. */
            int rc = oops_audio_emit(port, src);
            if (rc < 0) return rc;
            src += chunk * OOPS_AUDIO_CHANNELS;
            left -= chunk;
            continue;
        }

        /* Fill the held tail; hand it over when it becomes a whole chunk. */
        size_t room = chunk - (size_t)port->pending;
        size_t n = (left < room) ? left : room;
        int16_t *dst = port->staging + (size_t)port->pending * OOPS_AUDIO_CHANNELS;
        for (size_t i = 0; i < n * OOPS_AUDIO_CHANNELS; i++) {
            dst[i] = src[i];
        }
        port->pending += (int)n;
        src += n * OOPS_AUDIO_CHANNELS;
        left -= n;

        if ((size_t)port->pending == chunk) {
            int rc = oops_audio_emit(port, port->staging);
            port->pending = 0;
            if (rc < 0) return rc;
        }
    }

    return 0;
}

int oops_audio_flush(oops_audio_port_t *port) {
    if (!port || port->handle < 0 || !port->sink) return -1;
    if (port->pending == 0) return 0;

    size_t chunk_samples = (size_t)port->chunk_frames * OOPS_AUDIO_CHANNELS;
    for (size_t i = (size_t)port->pending * OOPS_AUDIO_CHANNELS; i < chunk_samples; i++) {
        port->staging[i] = 0;
    }
    int rc = oops_audio_emit(port, port->staging);
    port->pending = 0;
    return rc;
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
    if (!port) return;
    if (port->handle >= 0) {
        (void)oops_audio_flush(port);
        if (sceAudioOutClose) {
            sceAudioOutClose(port->handle);
        }
        port->handle = -1;
    }
    port->pending = 0;
    port->sink = NULL;
}
