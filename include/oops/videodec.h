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
 * The platform decodes H.264 / HEVC / VP9 into caller-owned, GPU-visible
 * memory, and that output buffer can be sampled directly by the display path
 * with no CPU copy - the missing front half of a stream client or media player.
 * The existing `display` subsystem presents linear SDR RGB and knows nothing
 * about decode; this is the piece that feeds it.
 *
 * # State of this subsystem, honestly
 *
 * The nine libSceVideodec2 entry points this binds are **confirmed to exist on
 * hardware** - obSCEne's `107-videodec` census resolved every one of them (in
 * the ps4_mode context, with real addresses). So capability detection here is
 * real: `oops_videodec_available` answers a measured question.
 *
 * What is **not** yet confirmed is the layout of the structures the decode
 * calls take. A probe that only resolves a symbol proves the symbol is there;
 * it does not prove the shape of the struct you pass it, and a wrong field
 * offset does not fail cleanly - it corrupts the stack and crashes somewhere
 * unrelated. OOPS does not ship a guessed layout (the same rule obSCEne holds).
 * So `oops_videodec_open` and `oops_videodec_decode` are present as the settled
 * interface but return `OOPS_VIDEODEC_ELAYOUT` until an obSCEne struct-layout
 * probe confirms the config/input/output layouts. When it does, only the
 * internal struct definitions and the two call bodies need filling - the
 * interface below does not change.
 */

/* Codec selection. OOPS's own values; the mapping to the platform's codec
 * constant lives in the (layout-gated) decode path, not here. */
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
 * SDK's contract with its caller, not the platform's output struct - the point
 * of the subsystem is that this buffer is GPU-visible and can be handed to the
 * display path directly. Populated by a successful `oops_videodec_decode`.
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
 * else, and the honest gate an app should check before offering decode.
 */
int oops_videodec_available(void);

/*
 * Open a decoder for `codec` at `width` x `height`. Returns a handle, or NULL
 * on failure with the reason available from `oops_videodec_last_error`.
 *
 * NOTE: gated on the struct-layout confirmation described above - currently
 * returns NULL and sets the last error to `OOPS_VIDEODEC_ELAYOUT` when the
 * library is present but the decoder config layout is unconfirmed, rather than
 * pass a guessed struct to the platform.
 */
oops_videodec_t *oops_videodec_open(int codec, uint32_t width, uint32_t height);

/*
 * Decode one access unit (`au`, `au_size` bytes) and, on success, fill
 * `out_frame` with the decoded picture in caller-visible memory. Returns
 * OOPS_VIDEODEC_OK or a negative code.
 *
 * NOTE: gated on the struct-layout confirmation above - currently returns
 * OOPS_VIDEODEC_ELAYOUT.
 */
int oops_videodec_decode(oops_videodec_t *dec, const void *au, size_t au_size,
                         oops_videodec_frame_t *out_frame);

/* Release a decoder opened with oops_videodec_open. Safe on NULL. */
void oops_videodec_close(oops_videodec_t *dec);

/* The last error recorded by this subsystem on the calling thread's most recent
 * call. */
int oops_videodec_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_VIDEODEC_H */
