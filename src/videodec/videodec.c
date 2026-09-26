/*
 * Video decode over libSceVideodec2. Availability is real; open and decode refuse with
 * OOPS_VIDEODEC_ELAYOUT because the decoder's config, input and output structure
 * layouts are unconfirmed.
 */
#include "oops/videodec.h"
#include "oops/sysmodule.h"
#include "oops/system.h"
#include <stddef.h>

/*
 * Platform symbols from libSceVideodec2. All nine resolve on hardware (the obSCEne
 * probe 107-videodec, ps4_mode). The two that take layout-sensitive config, input and
 * output structures are declared with `void *` placeholders, since declaring the true
 * struct types would invite populating them from a guess.
 */
__attribute__((weak)) int sceVideodec2QueryComputeMemoryInfo(void *info);
__attribute__((weak)) int sceVideodec2AllocateComputeQueue(const void *queue_info,
                                                           void *queue_out);
__attribute__((weak)) int sceVideodec2ReleaseComputeQueue(void *queue);
__attribute__((weak)) int sceVideodec2CreateDecoder(const void *config, void *queue,
                                                    void *decoder_out);
__attribute__((weak)) int sceVideodec2DeleteDecoder(void *decoder);
__attribute__((weak)) int sceVideodec2Decode(void *decoder, const void *input,
                                             void *frame_out, void *picture_out);
__attribute__((weak)) int sceVideodec2Flush(void *decoder, void *frame_out,
                                            void *picture_out);
__attribute__((weak)) int sceVideodec2Reset(void *decoder);
__attribute__((weak)) int sceVideodec2GetPictureInfo(const void *picture_out, void *p1,
                                                     void *p2);

struct oops_videodec {
    int codec;
    uint32_t width;
    uint32_t height;
    /* The platform decoder handle and its compute queue, opaque; the create path
     * does not fill them while the layouts are unconfirmed. */
    void *decoder;
    void *queue;
};

static int s_last_error = OOPS_VIDEODEC_OK;

int oops_videodec_last_error(void) {
    return s_last_error;
}

int oops_videodec_available(void) {
    /* The decode library is resident in ps4_mode; elsewhere a sysmodule load may
     * bring it in. A failure here is not fatal, since the weak symbols are the test. */
    (void)oops_sysmodule_load(OOPS_SYSMODULE_VIDEODEC);

    /* Available when the entry points that gate a decode resolved. */
    if (sceVideodec2CreateDecoder && sceVideodec2Decode && sceVideodec2DeleteDecoder) {
        oops_log_trace("VIDEODEC", "available: entry points resolved");
        return 1;
    }
    oops_log_trace("VIDEODEC",
                   "available: missing entry points (create=%p decode=%p del=%p)",
                   (void *)sceVideodec2CreateDecoder, (void *)sceVideodec2Decode,
                   (void *)sceVideodec2DeleteDecoder);
    return 0;
}

oops_videodec_t *oops_videodec_open(int codec, uint32_t width, uint32_t height) {
    oops_log_debug("VIDEODEC", "open codec=%d size=%ux%u", codec, width, height);
    if (codec != OOPS_VIDEODEC_H264 && codec != OOPS_VIDEODEC_HEVC &&
        codec != OOPS_VIDEODEC_VP9) {
        oops_log_warn("VIDEODEC", "open: unsupported codec=%d", codec);
        s_last_error = OOPS_VIDEODEC_EPARAM;
        return NULL;
    }
    if (width == 0 || height == 0) {
        oops_log_warn("VIDEODEC", "open: invalid dimensions %ux%u", width, height);
        s_last_error = OOPS_VIDEODEC_EPARAM;
        return NULL;
    }
    if (!oops_videodec_available()) {
        oops_log_warn("VIDEODEC", "open: libSceVideodec2 not available");
        s_last_error = OOPS_VIDEODEC_EUNAVAIL;
        return NULL;
    }

    /* sceVideodec2CreateDecoder takes a decoder-config structure whose layout is
     * unconfirmed, and a guessed layout would corrupt the stack rather than fail, so
     * this refuses until a struct-layout probe confirms it (see the header). */
    oops_log_warn("VIDEODEC", "open: struct layout unconfirmed (ELAYOUT)");
    s_last_error = OOPS_VIDEODEC_ELAYOUT;
    return NULL;
}

int oops_videodec_decode(oops_videodec_t *dec, const void *au, size_t au_size,
                         oops_videodec_frame_t *out_frame) {
    if (!dec || !au || au_size == 0 || !out_frame) {
        oops_log_warn("VIDEODEC",
                      "decode: invalid arguments dec=%p au=%p sz=%zu frame=%p",
                      (void *)dec, au, au_size, (void *)out_frame);
        s_last_error = OOPS_VIDEODEC_EPARAM;
        return OOPS_VIDEODEC_EPARAM;
    }
    /* Same boundary as open: the input/output struct layouts are unconfirmed. */
    oops_log_trace("VIDEODEC", "decode: dec=%p sz=%zu -> ELAYOUT", (void *)dec,
                   au_size);
    s_last_error = OOPS_VIDEODEC_ELAYOUT;
    return OOPS_VIDEODEC_ELAYOUT;
}

void oops_videodec_close(oops_videodec_t *dec) {
    if (!dec) {
        return;
    }
    oops_log_debug("VIDEODEC", "close dec=%p decoder=%p queue=%p", (void *)dec,
                   dec->decoder, dec->queue);
    if (dec->decoder && sceVideodec2DeleteDecoder) {
        sceVideodec2DeleteDecoder(dec->decoder);
        dec->decoder = NULL;
    }
    if (dec->queue && sceVideodec2ReleaseComputeQueue) {
        sceVideodec2ReleaseComputeQueue(dec->queue);
        dec->queue = NULL;
    }
    /* open() hands out no allocation while the create path is gated, so there is
     * nothing to free. */
}
