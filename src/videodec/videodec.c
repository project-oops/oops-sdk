#include "oops/videodec.h"
#include "oops/sysmodule.h"
#include <stddef.h>

/*
 * Platform symbols from libSceVideodec2.
 *
 * All nine are confirmed to exist on hardware - obSCEne's 107-videodec census
 * resolved each one (ps4_mode, real addresses). The signatures below are
 * deliberately partial: the calls that only take scalars or opaque pointers are
 * declared with those, and the two that take layout-sensitive
 * config/input/output structures are declared with `void *` placeholders,
 * because this subsystem does not call them until a struct-layout probe
 * confirms the shapes. Declaring the true struct types now would invite
 * populating them from a guess.
 */
__attribute__((weak)) int sceVideodec2QueryComputeMemoryInfo(void *info);
__attribute__((weak)) int
sceVideodec2AllocateComputeQueue(const void *queue_info, void *queue_out);
__attribute__((weak)) int sceVideodec2ReleaseComputeQueue(void *queue);
__attribute__((weak)) int
sceVideodec2CreateDecoder(const void *config, void *queue, void *decoder_out);
__attribute__((weak)) int sceVideodec2DeleteDecoder(void *decoder);
__attribute__((weak)) int sceVideodec2Decode(void *decoder, const void *input,
                                             void *frame_out,
                                             void *picture_out);
__attribute__((weak)) int sceVideodec2Flush(void *decoder, void *frame_out,
                                            void *picture_out);
__attribute__((weak)) int sceVideodec2Reset(void *decoder);
__attribute__((weak)) int sceVideodec2GetPictureInfo(const void *picture_out,
                                                     void *p1, void *p2);

struct oops_videodec {
  int codec;
  uint32_t width;
  uint32_t height;
  /* The platform decoder handle and its compute queue live here once the create
   * path is unblocked. Kept opaque and unused until then. */
  void *decoder;
  void *queue;
};

static int s_last_error = OOPS_VIDEODEC_OK;

int oops_videodec_last_error(void) { return s_last_error; }

int oops_videodec_available(void) {
  /* Best-effort: the decode library is not resident by default in every
   * context. In ps4_mode it already is (that is where the census resolved it);
   * elsewhere a sysmodule load may bring it in. A failure here is not fatal -
   * the weak symbols are the real test. */
  (void)oops_sysmodule_load(OOPS_SYSMODULE_VIDEODEC);

  /* The capability is real iff the entry points that gate a decode resolved.
   * This is the measured question, and it is the part of this subsystem that
   * works today. */
  if (sceVideodec2CreateDecoder && sceVideodec2Decode &&
      sceVideodec2DeleteDecoder) {
    return 1;
  }
  return 0;
}

oops_videodec_t *oops_videodec_open(int codec, uint32_t width,
                                    uint32_t height) {
  if (codec != OOPS_VIDEODEC_H264 && codec != OOPS_VIDEODEC_HEVC &&
      codec != OOPS_VIDEODEC_VP9) {
    s_last_error = OOPS_VIDEODEC_EPARAM;
    return NULL;
  }
  if (width == 0 || height == 0) {
    s_last_error = OOPS_VIDEODEC_EPARAM;
    return NULL;
  }
  if (!oops_videodec_available()) {
    s_last_error = OOPS_VIDEODEC_EUNAVAIL;
    return NULL;
  }

  /* The library is here and the symbols resolve, but sceVideodec2CreateDecoder
   * takes a decoder-config structure whose layout OOPS has not confirmed.
   * Passing a guessed layout would corrupt the stack rather than fail, so this
   * refuses loudly instead. Completing it needs the obSCEne struct-layout probe
   * (see the header). */
  s_last_error = OOPS_VIDEODEC_ELAYOUT;
  return NULL;
}

int oops_videodec_decode(oops_videodec_t *dec, const void *au, size_t au_size,
                         oops_videodec_frame_t *out_frame) {
  if (!dec || !au || au_size == 0 || !out_frame) {
    s_last_error = OOPS_VIDEODEC_EPARAM;
    return OOPS_VIDEODEC_EPARAM;
  }
  /* Same boundary as open: the input/output struct layouts are unconfirmed. */
  s_last_error = OOPS_VIDEODEC_ELAYOUT;
  return OOPS_VIDEODEC_ELAYOUT;
}

void oops_videodec_close(oops_videodec_t *dec) {
  if (!dec) {
    return;
  }
  if (dec->decoder && sceVideodec2DeleteDecoder) {
    sceVideodec2DeleteDecoder(dec->decoder);
    dec->decoder = NULL;
  }
  if (dec->queue && sceVideodec2ReleaseComputeQueue) {
    sceVideodec2ReleaseComputeQueue(dec->queue);
    dec->queue = NULL;
  }
  /* No allocation was handed out while the create path is gated, so there is
   * nothing to free here yet; kept for when open() begins returning a real
   * handle. */
}
