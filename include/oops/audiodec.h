#ifndef OOPS_AUDIODEC_H
#define OOPS_AUDIODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware audio decode (libSceAudiodec, offloaded through libSceAjm).
 *
 * Turns a compressed elementary stream (AAC, MP3) into the PCM that
 * `oops_audio_write` plays, using the platform's fixed-function decode engine.
 *
 * The obSCEne probe `108-audiodec` (ps4_mode) shapes this header:
 *   - libSceAudiodec resolves `sceAudiodecCreateDecoder`,
 *     `sceAudiodecDeleteDecoder`, `sceAudiodecDecode` and
 *     `sceAudiodecClearContext`, but not `sceAudiodecInitialize`,
 *     `sceAudiodecTerminate` or the `...Ex` variants, so the API is the base form
 *     with no global initialize.
 *   - libSceAjm resolves all seven batch entry points beneath it, so the decode is
 *     hardware-offloaded.
 *   - The Opus decoders are absent in that context: there is no native Opus route.
 *
 * The layouts of the control, param, AU and PCM structures the decode call takes
 * are unconfirmed, so `oops_audiodec_open`/`oops_audiodec_decode` return
 * `OOPS_AUDIODEC_ELAYOUT` until a struct-layout probe confirms them. The interface
 * below is settled.
 */

/* Codec selection. OOPS's own values; the mapping to the platform codec
 * constant belongs to the layout-gated decode path. */
enum {
    OOPS_AUDIODEC_AAC = 1,
    OOPS_AUDIODEC_MP3 = 2,
};

enum {
    OOPS_AUDIODEC_OK = 0,
    OOPS_AUDIODEC_EUNAVAIL =
        -1, /* the library or its entry points did not resolve here */
    OOPS_AUDIODEC_ELAYOUT =
        -2, /* the call is real but its struct layout is unconfirmed */
    OOPS_AUDIODEC_EPARAM =
        -3, /* a caller argument was rejected before any platform call */
};

typedef struct oops_audiodec oops_audiodec_t;

/*
 * Whether hardware audio decode is reachable in this process: libSceAudiodec
 * loads (a best-effort sysmodule load is attempted) and the create/decode entry
 * points resolve. Returns 1 if usable, 0 if not.
 */
int oops_audiodec_available(void);

/*
 * Whether the AJM offload engine beneath the codec resolved - i.e. whether a
 * successful decode would be hardware-offloaded rather than falling to a
 * software path. Returns 1/0. Separate from availability because a decode
 * library can be present without its engine.
 */
int oops_audiodec_offload_available(void);

/*
 * Open a decoder for `codec`. Returns a handle or NULL (reason via
 * oops_audiodec_last_error).
 *
 * Gated on the struct-layout confirmation above: returns NULL with the last error
 * OOPS_AUDIODEC_ELAYOUT when the library is present.
 */
oops_audiodec_t *oops_audiodec_open(int codec);

/*
 * Decode one access unit (`au`, `au_size`) into interleaved 16-bit PCM in
 * `pcm_out` (room for `pcm_capacity` int16 samples). On success returns the
 * number of samples written; on failure a negative code. The PCM is in the
 * shape `oops_audio_write` consumes.
 *
 * Gated on the struct-layout confirmation above: returns OOPS_AUDIODEC_ELAYOUT.
 */
int oops_audiodec_decode(oops_audiodec_t *dec, const void *au, size_t au_size,
                         int16_t *pcm_out, size_t pcm_capacity);

/* Release a decoder opened with oops_audiodec_open. Safe on NULL. */
void oops_audiodec_close(oops_audiodec_t *dec);

/* The last error this subsystem recorded, process-wide. */
int oops_audiodec_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AUDIODEC_H */
