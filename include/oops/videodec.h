#ifndef OOPS_VIDEODEC_H
#define OOPS_VIDEODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware video decode (libSceVideodec2).
 *
 * The platform decodes H.264 / HEVC / VP9 into caller-owned, GPU-visible memory
 * that the display path can sample with no CPU copy.
 *
 * The nine libSceVideodec2 entry points resolve on hardware (the obSCEne probe
 * `107-videodec`, ps4_mode context), so `oops_videodec_available` answers a
 * measured question. The layouts of the structures the decode calls take are
 * unconfirmed, and a wrong field offset corrupts the stack rather than failing, so
 * `oops_videodec_open` and `oops_videodec_decode` return `OOPS_VIDEODEC_ELAYOUT`
 * until a struct-layout probe confirms them. The interface below is settled.
 */

/* Codec selection. OOPS's own values; the mapping to the platform's codec
 * constant belongs to the layout-gated decode path. */
enum {
    OOPS_VIDEODEC_H264 = 1,
    OOPS_VIDEODEC_HEVC = 2,
    OOPS_VIDEODEC_VP9 = 3,
};

/* Return / error codes. */
enum {
    OOPS_VIDEODEC_OK = 0,
    OOPS_VIDEODEC_EUNAVAIL =
        -1, /* the library or its entry points did not resolve here */
    OOPS_VIDEODEC_ELAYOUT =
        -2, /* the call is real but its struct layout is unconfirmed */
    OOPS_VIDEODEC_EPARAM =
        -3, /* a caller argument was rejected before any platform call */
};

typedef struct oops_videodec oops_videodec_t;

/*
 * A decoded frame, described in OOPS's own terms: where the luma and chroma
 * planes landed in caller-visible memory and how they are laid out. This is the
 * SDK's contract with its caller, not the platform's output struct; the buffer
 * is GPU-visible and can be handed to the display path directly. Populated by a
 * successful `oops_videodec_decode`.
 */
typedef struct oops_videodec_frame {
    void *luma;   /* Y plane */
    void *chroma; /* interleaved UV plane (NV12-style) */
    uint32_t width;
    uint32_t height;
    uint32_t pitch; /* row stride in bytes */
} oops_videodec_frame_t;

/*
 * Whether hardware video decode is reachable in this process: the library loads
 * (a best-effort sysmodule load is attempted) and the create/decode entry
 * points resolve. Returns 1 if usable, 0 if not. Safe to call before anything
 * else; an app checks it before offering decode.
 */
int oops_videodec_available(void);

/*
 * Open a decoder for `codec` at `width` x `height`. Returns a handle, or NULL
 * on failure with the reason available from `oops_videodec_last_error`.
 *
 * Gated on the struct-layout confirmation described above: returns NULL with the
 * last error `OOPS_VIDEODEC_ELAYOUT` when the library is present, rather than pass
 * a guessed struct to the platform.
 */
oops_videodec_t *oops_videodec_open(int codec, uint32_t width, uint32_t height);

/*
 * Decode one access unit (`au`, `au_size` bytes) and, on success, fill
 * `out_frame` with the decoded picture in caller-visible memory. Returns
 * OOPS_VIDEODEC_OK or a negative code.
 *
 * Gated on the struct-layout confirmation above: returns OOPS_VIDEODEC_ELAYOUT.
 */
int oops_videodec_decode(oops_videodec_t *dec, const void *au, size_t au_size,
                         oops_videodec_frame_t *out_frame);

/* Release a decoder opened with oops_videodec_open. Safe on NULL. */
void oops_videodec_close(oops_videodec_t *dec);

/* The last error this subsystem recorded, process-wide. */
int oops_videodec_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_VIDEODEC_H */
