#include "oops/display.h"
#include "agc/display.h"
#include "gnm/display.h"

struct oops_display {
    oops_display_backend_t backend;
    union {
        agc_display_t *agc;
        gnm_display_t *gnm;
    } u;
};

static struct oops_display s_unified_display;

oops_display_t *oops_display_open(oops_display_backend_t backend, unsigned int width, unsigned int height) {
    struct oops_display *disp = &s_unified_display;
    disp->backend = backend;

    if (backend == OOPS_DISPLAY_BACKEND_AUTO) {
        disp->backend = OOPS_DISPLAY_BACKEND_AGC;
        disp->u.agc = agc_display_open(width, height);
        if (disp->u.agc && agc_display_is_ready(disp->u.agc)) {
            return disp;
        }
        /* Fall back to GNM if AGC open was refused */
        disp->backend = OOPS_DISPLAY_BACKEND_GNM;
        disp->u.gnm = gnm_display_open(width, height);
        return disp;
    } else if (backend == OOPS_DISPLAY_BACKEND_AGC) {
        disp->backend = OOPS_DISPLAY_BACKEND_AGC;
        disp->u.agc = agc_display_open(width, height);
        return disp;
    } else {
        disp->backend = OOPS_DISPLAY_BACKEND_GNM;
        disp->u.gnm = gnm_display_open(width, height);
        return disp;
    }
}

int oops_display_is_ready(const oops_display_t *disp) {
    if (!disp) return 0;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_is_ready(disp->u.agc);
    } else {
        return gnm_display_is_ready(disp->u.gnm);
    }
}

uint32_t *oops_display_get_framebuffer(oops_display_t *disp) {
    if (!disp) return 0;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_get_framebuffer(disp->u.agc);
    } else {
        return gnm_display_get_framebuffer(disp->u.gnm);
    }
}

unsigned int oops_display_get_width(const oops_display_t *disp) {
    if (!disp) return 0;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_get_width(disp->u.agc);
    } else {
        return gnm_display_get_width(disp->u.gnm);
    }
}

unsigned int oops_display_get_height(const oops_display_t *disp) {
    if (!disp) return 0;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_get_height(disp->u.agc);
    } else {
        return gnm_display_get_height(disp->u.gnm);
    }
}

uint64_t oops_display_get_flip_count(const oops_display_t *disp) {
    if (!disp) return 0;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_get_flip_count(disp->u.agc);
    } else {
        return gnm_display_get_flip_count(disp->u.gnm);
    }
}

int oops_display_get_last_error(const oops_display_t *disp) {
    if (!disp) return -1;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_get_last_error(disp->u.agc);
    } else {
        return gnm_display_get_last_error(disp->u.gnm);
    }
}

const char *oops_display_get_backend_name(const oops_display_t *disp) {
    if (!disp) return "none";
    return (disp->backend == OOPS_DISPLAY_BACKEND_AGC) ? "AGC (Prospero)" : "GNM (Orbis)";
}

void oops_display_clear(oops_display_t *disp, uint32_t color) {
    if (!disp) return;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        agc_display_clear(disp->u.agc, color);
    } else {
        gnm_display_clear(disp->u.gnm, color);
    }
}

int oops_display_flip(oops_display_t *disp) {
    if (!disp) return -1;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        return agc_display_flip(disp->u.agc);
    } else {
        return gnm_display_flip(disp->u.gnm);
    }
}

void oops_display_close(oops_display_t *disp) {
    if (!disp) return;
    if (disp->backend == OOPS_DISPLAY_BACKEND_AGC) {
        agc_display_close(disp->u.agc);
    } else {
        gnm_display_close(disp->u.gnm);
    }
}
