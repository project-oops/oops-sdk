#include "oops/audiodec.h"
#include "oops/sysmodule.h"
#include <stddef.h>

/*
 * Platform symbols from libSceAudiodec and libSceAjm.
 *
 * The set below is exactly what obSCEne's 108-audiodec census resolved on hardware: the base
 * libSceAudiodec create/decode/delete/clear entry points (the Initialize/Terminate/`...Ex`
 * spellings did not resolve, so they are not declared), and the seven libSceAjm batch entry
 * points beneath them. Signatures are partial on purpose - the layout-sensitive control and
 * batch structures are `void *` placeholders because this subsystem does not call the
 * decode/batch paths until a struct-layout probe confirms their shapes.
 */
__attribute__((weak)) int sceAudiodecCreateDecoder(const void *ctrl, int codec_type);
__attribute__((weak)) int sceAudiodecDeleteDecoder(int handle);
__attribute__((weak)) int sceAudiodecDecode(int handle, void *au_info, void *pcm_item);
__attribute__((weak)) int sceAudiodecClearContext(int handle);

__attribute__((weak)) int sceAjmInitialize(int64_t reserved, void *context_out);
__attribute__((weak)) int sceAjmModuleRegister(void *context, int codec, int64_t reserved);
__attribute__((weak)) int sceAjmInstanceCreate(void *context, int codec, uint64_t flags, void *instance_out);

struct oops_audiodec {
    int codec;
    int handle; /* platform decoder handle once the create path is unblocked */
};

static int s_last_error = OOPS_AUDIODEC_OK;

int oops_audiodec_last_error(void) {
    return s_last_error;
}

int oops_audiodec_available(void) {
    (void)oops_sysmodule_load(OOPS_SYSMODULE_AUDIO_DEC);

    /* Real, measured capability: the base decode entry points resolved. */
    if (sceAudiodecCreateDecoder && sceAudiodecDecode && sceAudiodecDeleteDecoder) {
        return 1;
    }
    return 0;
}

int oops_audiodec_offload_available(void) {
    /* The engine beneath the codec. Present separately because a front door without the
     * engine is a distinct finding a caller may want to know. */
    if (sceAjmInitialize && sceAjmInstanceCreate) {
        return 1;
    }
    return 0;
}

oops_audiodec_t *oops_audiodec_open(int codec) {
    if (codec != OOPS_AUDIODEC_AAC && codec != OOPS_AUDIODEC_MP3) {
        s_last_error = OOPS_AUDIODEC_EPARAM;
        return NULL;
    }
    if (!oops_audiodec_available()) {
        s_last_error = OOPS_AUDIODEC_EUNAVAIL;
        return NULL;
    }

    /* sceAudiodecCreateDecoder takes a control structure whose layout OOPS has not confirmed.
     * A guessed layout corrupts the stack rather than failing, so this refuses loudly.
     * Completing it needs the obSCEne struct-layout probe (see the header). */
    s_last_error = OOPS_AUDIODEC_ELAYOUT;
    return NULL;
}

int oops_audiodec_decode(oops_audiodec_t *dec, const void *au, size_t au_size,
                         int16_t *pcm_out, size_t pcm_capacity) {
    if (!dec || !au || au_size == 0 || !pcm_out || pcm_capacity == 0) {
        s_last_error = OOPS_AUDIODEC_EPARAM;
        return OOPS_AUDIODEC_EPARAM;
    }
    s_last_error = OOPS_AUDIODEC_ELAYOUT;
    return OOPS_AUDIODEC_ELAYOUT;
}

void oops_audiodec_close(oops_audiodec_t *dec) {
    if (!dec) {
        return;
    }
    if (dec->handle > 0 && sceAudiodecDeleteDecoder) {
        sceAudiodecDeleteDecoder(dec->handle);
        dec->handle = -1;
    }
}
