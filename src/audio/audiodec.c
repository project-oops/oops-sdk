/*
 * Compressed audio decode over libSceAudiodec and libSceAjm. Availability is real;
 * open and decode refuse with OOPS_AUDIODEC_ELAYOUT because the control structure
 * layouts are unconfirmed.
 */
#include "oops/audiodec.h"
#include "oops/sysmodule.h"
#include "oops/system.h"
#include <stddef.h>

/*
 * Platform symbols from libSceAudiodec and libSceAjm, as the obSCEne probe
 * 108-audiodec resolved them on hardware: the base libSceAudiodec
 * create/decode/delete/clear entry points (the Initialize/Terminate/`...Ex` spellings
 * do not resolve), and libSceAjm entry points beneath them. The layout-sensitive
 * control and batch structures are `void *` placeholders until a struct-layout probe
 * confirms their shapes.
 */
__attribute__((weak)) int sceAudiodecCreateDecoder(const void *ctrl, int codec_type);
__attribute__((weak)) int sceAudiodecDeleteDecoder(int handle);
__attribute__((weak)) int sceAudiodecDecode(int handle, void *au_info, void *pcm_item);
__attribute__((weak)) int sceAudiodecClearContext(int handle);

__attribute__((weak)) int sceAjmInitialize(int64_t reserved, void *context_out);
__attribute__((weak)) int sceAjmModuleRegister(void *context, int codec,
                                               int64_t reserved);
__attribute__((weak)) int sceAjmInstanceCreate(void *context, int codec, uint64_t flags,
                                               void *instance_out);

struct oops_audiodec {
    int codec;
    int handle; /* platform decoder handle; unset while the create path is gated */
};

static int s_last_error = OOPS_AUDIODEC_OK;

int oops_audiodec_last_error(void) {
    return s_last_error;
}

int oops_audiodec_available(void) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_AUDIO_DEC);

    /* Available when the base decode entry points resolved. */
    if (sceAudiodecCreateDecoder && sceAudiodecDecode && sceAudiodecDeleteDecoder) {
        oops_log_trace("AUDIODEC", "available: entry points resolved");
        return 1;
    }
    oops_log_trace("AUDIODEC",
                   "available: missing entry points (create=%p decode=%p del=%p)",
                   (void *)sceAudiodecCreateDecoder, (void *)sceAudiodecDecode,
                   (void *)sceAudiodecDeleteDecoder);
    return 0;
}

int oops_audiodec_offload_available(void) {
    /* The engine beneath the codec, reported separately because the codec entry
     * points can resolve without it. */
    if (sceAjmInitialize && sceAjmInstanceCreate) {
        oops_log_trace("AUDIODEC", "offload available: AJM entry points resolved");
        return 1;
    }
    oops_log_trace("AUDIODEC", "offload available: missing AJM entry points");
    return 0;
}

oops_audiodec_t *oops_audiodec_open(int codec) {
    oops_log_debug("AUDIODEC", "open codec=%d", codec);
    if (codec != OOPS_AUDIODEC_AAC && codec != OOPS_AUDIODEC_MP3) {
        oops_log_warn("AUDIODEC", "open: unsupported codec=%d", codec);
        s_last_error = OOPS_AUDIODEC_EPARAM;
        return NULL;
    }
    if (!oops_audiodec_available()) {
        oops_log_warn("AUDIODEC", "open: libSceAudiodec not available");
        s_last_error = OOPS_AUDIODEC_EUNAVAIL;
        return NULL;
    }

    /* sceAudiodecCreateDecoder takes a control structure whose layout is
     * unconfirmed, and a guessed layout corrupts the stack rather than failing, so
     * this refuses until a struct-layout probe confirms it (see the header). */
    oops_log_warn("AUDIODEC", "open: struct layout unconfirmed (ELAYOUT)");
    s_last_error = OOPS_AUDIODEC_ELAYOUT;
    return NULL;
}

int oops_audiodec_decode(oops_audiodec_t *dec, const void *au, size_t au_size,
                         int16_t *pcm_out, size_t pcm_capacity) {
    if (!dec || !au || au_size == 0 || !pcm_out || pcm_capacity == 0) {
        oops_log_warn("AUDIODEC",
                      "decode: invalid arguments dec=%p au=%p sz=%zu pcm=%p cap=%zu",
                      (void *)dec, au, au_size, (void *)pcm_out, pcm_capacity);
        s_last_error = OOPS_AUDIODEC_EPARAM;
        return OOPS_AUDIODEC_EPARAM;
    }
    oops_log_trace("AUDIODEC", "decode: dec=%p sz=%zu -> ELAYOUT", (void *)dec,
                   au_size);
    s_last_error = OOPS_AUDIODEC_ELAYOUT;
    return OOPS_AUDIODEC_ELAYOUT;
}

void oops_audiodec_close(oops_audiodec_t *dec) {
    if (!dec) {
        return;
    }
    oops_log_debug("AUDIODEC", "close dec=%p handle=%d", (void *)dec, dec->handle);
    if (dec->handle > 0 && sceAudiodecDeleteDecoder) {
        sceAudiodecDeleteDecoder(dec->handle);
        dec->handle = -1;
    }
}
