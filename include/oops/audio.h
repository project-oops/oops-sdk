#ifndef OOPS_AUDIO_H
#define OOPS_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_audio_port oops_audio_port_t;

oops_audio_port_t *oops_audio_open(int sample_rate, int channels, int buffer_frames);
int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, size_t frame_count);
void oops_audio_close(oops_audio_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AUDIO_H */
