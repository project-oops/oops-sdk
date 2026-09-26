/*
 * The audio port, private to the audio subsystem and its host tests. The
 * chunking in oops_audio_write() is the whole of what this subsystem computes,
 * so the tests build a port here with a recording sink in place of the
 * platform's output call and drive it directly. Nothing in this file is API.
 */
#ifndef OOPS_AUDIO_PORT_H
#define OOPS_AUDIO_PORT_H

#include "oops/audio.h"
#include <stddef.h>
#include <stdint.h>

#define OOPS_AUDIO_MAX_CHUNK 2048
#define OOPS_AUDIO_DEFAULT_FRAMES 512
#define OOPS_AUDIO_CHANNELS 2

/* The output call: on hardware the platform's, in tests a recorder. */
typedef int (*oops_audio_sink_fn)(int handle, const void *chunk);

struct oops_audio_port {
    int handle;
    int channels;
    int sample_rate;
    int chunk_frames; /* frames per sink call */
    int pending;      /* frames held in staging, not yet handed to the sink */
    oops_audio_sink_fn sink;
    /* Aligned staging buffer: one chunk, for partial tails */
    int16_t staging[OOPS_AUDIO_MAX_CHUNK * OOPS_AUDIO_CHANNELS]
        __attribute__((aligned(64)));
};

#endif /* OOPS_AUDIO_PORT_H */
