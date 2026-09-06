#ifndef OOPS_AUDIODEC_H
#define OOPS_AUDIODEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware audio decode (libSceAudiodec, offloaded through libSceAjm).
 *
 * The existing `audio` subsystem is PCM *output* - it plays samples. This is the missing
 * front half: turning a compressed elementary stream (AAC, MP3) into the PCM that
 * `oops_audio_write` already plays, using the platform's fixed-function decode engine rather
 * than a software codec.
 *
 * # State of this subsystem, honestly
 *
 * obSCEne's `108-audiodec` census confirmed the shape of this stack on hardware (ps4_mode),
 * and the result was precise enough to shape this header:
 *   - libSceAudiodec resolved `sceAudiodecCreateDecoder`, `sceAudiodecDeleteDecoder`,
 *     `sceAudiodecDecode`, `sceAudiodecClearContext`. It did **not** resolve
 *     `sceAudiodecInitialize`, `sceAudiodecTerminate`, or the `...Ex` variants - so the real
 *     API is the base form, with no explicit global initialize. This subsystem is built to
 *     the surface that exists, not the one a header might assume.
 *   - libSceAjm resolved all seven batch entry points beneath it - the decode is genuinely
 *     hardware-offloaded.
 *   - The Opus decoders were absent in that context: there is no native Opus route there,
 *     which is a finding an app should respect rather than route around.
 *
 * As with video decode, what is confirmed is that the symbols exist; the layout of the
 * control/param/au/pcm structures the decode call takes is not. OOPS does not pass a guessed
 * layout. So `oops_audiodec_open`/`oops_audiodec_decode` return `OOPS_AUDIODEC_ELAYOUT` until
 * an obSCEne struct-layout probe confirms the shapes; the interface below is settled.
 */

/* Codec selection. OOPS's own values; the mapping to the platform codec constant lives in
 * the (layout-gated) decode path. */
enum {
    OOPS_AUDIODEC_AAC = 1,
    OOPS_AUDIODEC_MP3 = 2,
};

enum {
    OOPS_AUDIODEC_OK       = 0,
    OOPS_AUDIODEC_EUNAVAIL = -1, /* the library or its entry points did not resolve here */
    OOPS_AUDIODEC_ELAYOUT  = -2, /* the call is real but its struct layout is unconfirmed */
    OOPS_AUDIODEC_EPARAM   = -3, /* a caller argument was rejected before any platform call */
};

typedef struct oops_audiodec oops_audiodec_t;

/*
 * Whether hardware audio decode is reachable in this process: libSceAudiodec loads (a
 * best-effort sysmodule load is attempted) and the create/decode entry points resolve.
 * Returns 1 if usable, 0 if not.
 */
int oops_audiodec_available(void);

/*
 * Whether the AJM offload engine beneath the codec resolved - i.e. whether a successful
 * decode would be hardware-offloaded rather than falling to a software path. Returns 1/0.
 * Separate from availability because a decode library present without its engine is a real,
 * distinct state.
 */
int oops_audiodec_offload_available(void);

/*
 * Open a decoder for `codec`. Returns a handle or NULL (reason via oops_audiodec_last_error).
 *
 * NOTE: gated on the struct-layout confirmation above - currently returns NULL with the last
 * error set to OOPS_AUDIODEC_ELAYOUT when the library is present but the layout is unconfirmed.
 */
oops_audiodec_t *oops_audiodec_open(int codec);

/*
 * Decode one access unit (`au`, `au_size`) into interleaved 16-bit PCM in `pcm_out` (room for
 * `pcm_capacity` int16 samples). On success returns the number of samples written; on failure
 * a negative code. The PCM is in the shape `oops_audio_write` consumes.
 *
 * NOTE: gated on the struct-layout confirmation above - currently returns OOPS_AUDIODEC_ELAYOUT.
 */
int oops_audiodec_decode(oops_audiodec_t *dec, const void *au, size_t au_size,
                         int16_t *pcm_out, size_t pcm_capacity);

/* Release a decoder opened with oops_audiodec_open. Safe on NULL. */
void oops_audiodec_close(oops_audiodec_t *dec);

/* The last error recorded by this subsystem on the calling thread's most recent call. */
int oops_audiodec_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AUDIODEC_H */
