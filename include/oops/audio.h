#ifndef OOPS_AUDIO_H
#define OOPS_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PCM output. One port, 16-bit signed interleaved stereo: a frame is one left sample then one
 * right sample. The hardware consumes fixed chunks of `buffer_frames` frames (rounded to a
 * multiple of 256 within 256..2048). A write hands over whole chunks, blocking on each until
 * the hardware has room, and holds a partial tail until the next write completes it or
 * oops_audio_flush() pads it with silence. A stream written in any sizes therefore plays
 * without a gap; a one-shot sound needs a flush to hear its end.
 *
 * Only stereo is honoured. The write path has one shape, so a channel count it would then
 * ignore is refused rather than stored. See audio.c on the platform's format selector.
 */

enum {
    OOPS_AUDIO_OK       = 0,
    OOPS_AUDIO_EUNAVAIL = -1001, /* the platform's audio output did not resolve here */
    OOPS_AUDIO_EPARAM   = -1002, /* a caller argument was rejected before any platform call */
    OOPS_AUDIO_EBUSY    = -1003, /* the port is already open; close it before reopening */
};

typedef struct oops_audio_port oops_audio_port_t;

/* Open the output port. Returns NULL with the reason in oops_audio_get_last_error(). */
oops_audio_port_t *oops_audio_open(int sample_rate, int channels, int buffer_frames);
int oops_audio_get_last_error(void);

/* Frames per hardware chunk after rounding; writing multiples of it never leaves a tail. */
int oops_audio_get_chunk_frames(const oops_audio_port_t *port);

/* Queue `frame_count` stereo frames. 0 on success, -1 on a bad argument, or the platform's
 * negative code from the output call that failed. */
int oops_audio_write(oops_audio_port_t *port, const int16_t *pcm_samples, size_t frame_count);

/* Emit a held partial chunk, padded with silence. 0 if nothing was held or it went out. */
int oops_audio_flush(oops_audio_port_t *port);

int oops_audio_set_volume(oops_audio_port_t *port, float left, float right);

/* Flushes, then releases the platform handle. Safe on NULL. */
void oops_audio_close(oops_audio_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AUDIO_H */
