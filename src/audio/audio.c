#include "oops/audio.h"

/* Platform symbols from libSceAudioOut */
__attribute__((weak)) int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
__attribute__((weak)) int sceAudioOutClose(int handle);
__attribute__((weak)) int sceAudioOutOutput(int handle, const void *ptr);

struct oops_audio_port {
    int handle;
    int channels;
    int sample_rate;
};

static struct oops_audio_port s_default_port = { -1, 2, 48000 };

oops_audio_port_t *oops_audio_open(int sample_rate, int channels, int buffer_frames) {
    if (!sceAudioOutOpen) return NULL;
    int handle = sceAudioOutOpen(0xFF, 0, 0, (unsigned int)buffer_frames, (unsigned int)sample_rate, 0);
    if (handle < 0) return NULL;

    s_default_port.handle = handle;
    s_default_port.sample_rate = sample_rate;
    s_default_port.channels = channels;
    return &s_default_port;
}

int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, size_t frame_count) {
    (void)frame_count;
    if (!port || port->handle < 0 || !sceAudioOutOutput || !pcm_samples) return -1;
    return sceAudioOutOutput(port->handle, pcm_samples);
}

void oops_audio_close(oops_audio_port_t *port) {
    if (port && port->handle >= 0 && sceAudioOutClose) {
        sceAudioOutClose(port->handle);
        port->handle = -1;
    }
}
