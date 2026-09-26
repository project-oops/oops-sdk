#include "oops/audio.h"
#include "src/audio/audio_port.h"
#include "tests/test_common.h"

/* Unit tests for `oops/audio.h`: argument checks, host unavailability, and how writes
 * are cut into the platform's fixed-size chunks. */

/* A recording sink in place of the platform's output call: each call keeps one
 * chunk. */
#define SINK_MAX_CALLS 8
#define SENTINEL 0x7777
static int16_t s_sink_log[SINK_MAX_CALLS][OOPS_AUDIO_MAX_CHUNK * OOPS_AUDIO_CHANNELS];
static int s_sink_calls;
static int s_sink_chunk; /* frames per call, set by the test */
static int s_sink_handle_seen;
static int s_sink_fail_at; /* call index that returns an error, or -1 */

static int recording_sink(int handle, const void *chunk) {
    int call = s_sink_calls++;
    if (call == s_sink_fail_at)
        return -77;
    if (call < SINK_MAX_CALLS) {
        const int16_t *p = (const int16_t *)chunk;
        for (int i = 0; i < s_sink_chunk * OOPS_AUDIO_CHANNELS; i++)
            s_sink_log[call][i] = p[i];
    }
    s_sink_handle_seen = handle;
    return 0;
}

static struct oops_audio_port s_port;

static void port_reset(int chunk_frames) {
    for (size_t i = 0; i < sizeof(s_port); i++)
        ((unsigned char *)&s_port)[i] = 0;
    s_port.handle = 7;
    s_port.channels = OOPS_AUDIO_CHANNELS;
    s_port.sample_rate = 48000;
    s_port.chunk_frames = chunk_frames;
    s_port.sink = recording_sink;
    s_sink_calls = 0;
    s_sink_chunk = chunk_frames;
    s_sink_handle_seen = -1;
    s_sink_fail_at = -1;
    for (int c = 0; c < SINK_MAX_CALLS; c++) {
        for (int i = 0; i < OOPS_AUDIO_MAX_CHUNK * OOPS_AUDIO_CHANNELS; i++)
            s_sink_log[c][i] = SENTINEL;
    }
}

/* Interleaved frames with a recognisable ramp: frame f carries (first + f,
 * -(first + f)). */
static void fill_ramp(int16_t *buf, int frames, int first) {
    for (int f = 0; f < frames; f++) {
        buf[2 * f] = (int16_t)(first + f);
        buf[2 * f + 1] = (int16_t)(-(first + f));
    }
}

/* Left sample of frame f in sink call c. */
#define SINK_L(c, f) (s_sink_log[(c)][2 * (f)])

/* NULL ports, NULL samples and zero frames are refused without reaching the sink. */
static void test_audio_null_and_bounds(void) {
    int16_t samples[512 * 2];
    ASSERT_EQ(oops_audio_write(NULL, samples, 512), -1);
    ASSERT_EQ(oops_audio_flush(NULL), -1);
    ASSERT_EQ(oops_audio_set_volume(NULL, 1.0f, 1.0f), -1);
    ASSERT_EQ(oops_audio_get_chunk_frames(NULL), 0);
    oops_audio_close(NULL);

    port_reset(256);
    ASSERT_EQ(oops_audio_write(&s_port, NULL, 512), -1);
    ASSERT_EQ(oops_audio_write(&s_port, samples, 0), -1);
    ASSERT_EQ(s_sink_calls, 0);
}

/* Argument checks come before availability, so a host with no platform still
 * reports a bad call as bad; a good call is unavailable, never a fabricated
 * port. */
static void test_audio_open_contract_on_host(void) {
    ASSERT_TRUE(oops_audio_open(48000, 1, 512) == NULL);
    ASSERT_EQ(oops_audio_get_last_error(), OOPS_AUDIO_EPARAM);
    ASSERT_TRUE(oops_audio_open(48000, 6, 512) == NULL);
    ASSERT_EQ(oops_audio_get_last_error(), OOPS_AUDIO_EPARAM);
    ASSERT_TRUE(oops_audio_open(48000, 2, 512) == NULL);
    ASSERT_EQ(oops_audio_get_last_error(), OOPS_AUDIO_EUNAVAIL);
    ASSERT_TRUE(oops_audio_open(0, 0, 0) == NULL); /* defaults are accepted */
    ASSERT_EQ(oops_audio_get_last_error(), OOPS_AUDIO_EUNAVAIL);
}

/* A write of whole chunks goes straight to the sink, in order, with nothing held. */
static void test_audio_write_whole_chunks_direct(void) {
    int16_t pcm[512 * 2];
    port_reset(256);
    fill_ramp(pcm, 512, 0);

    ASSERT_EQ(oops_audio_write(&s_port, pcm, 512), 0);
    ASSERT_EQ(s_sink_calls, 2);
    ASSERT_EQ(s_sink_handle_seen, 7);
    ASSERT_EQ(s_port.pending, 0);
    ASSERT_EQ(SINK_L(0, 0), 0);
    ASSERT_EQ(SINK_L(0, 255), 255);
    ASSERT_EQ(s_sink_log[0][2 * 255 + 1], -255);
    ASSERT_EQ(SINK_L(1, 0), 256);
    ASSERT_EQ(SINK_L(1, 255), 511);
}

/* A partial chunk is held and joined to the next write; flush pads it with silence. */
static void test_audio_write_holds_partial_tail(void) {
    int16_t pcm[300 * 2];
    port_reset(256);

    fill_ramp(pcm, 100, 0);
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 100), 0);
    ASSERT_EQ(s_sink_calls, 0); /* nothing whole to hand over yet */
    ASSERT_EQ(s_port.pending, 100);

    fill_ramp(pcm, 200, 100);
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 200), 0);
    ASSERT_EQ(s_sink_calls, 1);
    ASSERT_EQ(s_port.pending, 44);
    ASSERT_EQ(SINK_L(0, 99), 99);   /* first write ... */
    ASSERT_EQ(SINK_L(0, 100), 100); /* ... joined to the second without a gap */
    ASSERT_EQ(SINK_L(0, 255), 255);

    ASSERT_EQ(oops_audio_flush(&s_port), 0);
    ASSERT_EQ(s_sink_calls, 2);
    ASSERT_EQ(s_port.pending, 0);
    ASSERT_EQ(SINK_L(1, 0), 256);
    ASSERT_EQ(SINK_L(1, 43), 299); /* last real frame */
    ASSERT_EQ(SINK_L(1, 44), 0);   /* silence from here */
    ASSERT_EQ(SINK_L(1, 255), 0);

    ASSERT_EQ(oops_audio_flush(&s_port), 0);
    ASSERT_EQ(s_sink_calls, 2); /* nothing held: no call */
}

/* Sizes that are not chunk multiples, written back to back, join seamlessly: padding
 * each write with silence would be a click per write. */
static void test_audio_write_mixed_sizes_no_gap(void) {
    int16_t pcm[300 * 2];
    port_reset(256);

    fill_ramp(pcm, 300, 0);
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 300), 0);
    ASSERT_EQ(s_sink_calls, 1); /* 256 direct, 44 held */
    fill_ramp(pcm, 300, 300);
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 300), 0);
    ASSERT_EQ(s_sink_calls, 2); /* 44 held + 212 new, 88 held */
    ASSERT_EQ(s_port.pending, 88);
    ASSERT_EQ(SINK_L(0, 255), 255);
    ASSERT_EQ(SINK_L(1, 0), 256);
    ASSERT_EQ(SINK_L(1, 43), 299);
    ASSERT_EQ(SINK_L(1, 44), 300);
    ASSERT_EQ(SINK_L(1, 255), 511);

    ASSERT_EQ(oops_audio_flush(&s_port), 0);
    ASSERT_EQ(s_sink_calls, 3);
    ASSERT_EQ(SINK_L(2, 0), 512);
    ASSERT_EQ(SINK_L(2, 87), 599);
    ASSERT_EQ(SINK_L(2, 88), 0);
}

/* A sink error stops the write and is returned to the caller. */
static void test_audio_write_reports_sink_error(void) {
    int16_t pcm[512 * 2];
    port_reset(256);
    fill_ramp(pcm, 512, 0);
    s_sink_fail_at = 1;
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 512), -77);
    ASSERT_EQ(s_sink_calls, 2); /* first chunk went, second failed, stop there */
}

/* Close flushes a held tail, and a closed port refuses writes. */
static void test_audio_close_flushes_tail(void) {
    int16_t pcm[10 * 2];
    port_reset(256);
    fill_ramp(pcm, 10, 0);
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 10), 0);
    ASSERT_EQ(s_sink_calls, 0);
    oops_audio_close(&s_port);
    ASSERT_EQ(s_sink_calls, 1);
    ASSERT_EQ(SINK_L(0, 9), 9);
    ASSERT_EQ(SINK_L(0, 10), 0);
    ASSERT_EQ(s_port.handle, -1);
    ASSERT_EQ(oops_audio_write(&s_port, pcm, 10), -1); /* closed */
}

void run_unit_tests_audio(void) {
    TEST_SUITE_BEGIN("Audio PCM Subsystem");
    RUN_TEST(test_audio_null_and_bounds);
    RUN_TEST(test_audio_open_contract_on_host);
    RUN_TEST(test_audio_write_whole_chunks_direct);
    RUN_TEST(test_audio_write_holds_partial_tail);
    RUN_TEST(test_audio_write_mixed_sizes_no_gap);
    RUN_TEST(test_audio_write_reports_sink_error);
    RUN_TEST(test_audio_close_flushes_tail);
}
