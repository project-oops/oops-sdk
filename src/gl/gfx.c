/*
 * gfx.c - the oops-gl backend of <oops/gfx.h>.
 *
 * oops-gl's own setup is a display the title opens and a context built on it
 * (`glContextCreate`). This composes the two into the one call `oops/gfx.h` promises,
 * so a title written against that header runs on oops-gl without knowing it, and the
 * same source runs on Mesa (whose backend lives in oops-mesa) by a build switch.
 *
 * It is a faithful wrapper, not a rewrite: it opens the display exactly as a title did
 * by hand, so behaviour is unchanged. The reason the display open moved inside `create`
 * - naming scanout buffers at open for a copy-free present (D011, D012) - is an
 * optimisation the oops-gl swap path can take later behind this same call; nothing here
 * forecloses it.
 */

#include "oops/gfx.h"
#include "oops/display.h"
#include "oops/system.h"

#include "GL/gl.h"

#include <stddef.h>

struct oops_gfx {
    oops_display_t *disp;
    void *ctx;
    uint32_t width;
    uint32_t height;
    bool in_use;
};

/*
 * One instance, not a heap allocation. oops-gl has exactly one display and one context
 * - the current context is global state (`glContextMakeCurrent`) - so a second live gfx
 * could not mean anything here. A file-static handle is the honest shape of that, and
 * it keeps this backend from pulling in the heap: `oops/gfx.h` is compiled into every
 * oops-gl app's host self-test through `OOPS_GL_SRCS`, and those tests do not link an
 * allocator.
 */
static struct oops_gfx s_gfx;

static void gfx_log(const char *msg) {
    oops_klog("OOPS-GFX", msg);
}

oops_gfx_t *oops_gfx_create(const oops_gfx_desc_t *desc) {
    const uint32_t want_w =
        (desc != NULL && desc->width != 0u) ? desc->width : OOPS_DISPLAY_DEFAULT_WIDTH;
    const uint32_t want_h = (desc != NULL && desc->height != 0u)
                                ? desc->height
                                : OOPS_DISPLAY_DEFAULT_HEIGHT;

    if (s_gfx.in_use) {
        gfx_log(
            "a renderer is already up; this platform has one display and one context");
        return NULL;
    }

    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, want_w, want_h);
    if (disp == NULL) {
        gfx_log("the display did not open");
        return NULL;
    }

    void *ctx = glContextCreate(disp);
    if (ctx == NULL) {
        gfx_log("the GL context would not be created");
        oops_display_close(disp);
        return NULL;
    }
    glContextMakeCurrent(ctx);

    s_gfx.disp = disp;
    s_gfx.ctx = ctx;
    s_gfx.width = oops_display_get_width(disp);
    s_gfx.height = oops_display_get_height(disp);
    s_gfx.in_use = true;
    return &s_gfx;
}

bool oops_gfx_present(oops_gfx_t *gfx) {
    if (gfx == NULL) {
        return false;
    }
    /* oops-gl presents the current context, which `create` made current. The flip is on
     * vsync and `glSwapBuffers` reports no status, so there is nothing to fail on from
     * here. */
    glSwapBuffers();
    return true;
}

void oops_gfx_extent(const oops_gfx_t *gfx, uint32_t *width, uint32_t *height) {
    if (gfx == NULL) {
        return;
    }
    if (width != NULL) {
        *width = gfx->width;
    }
    if (height != NULL) {
        *height = gfx->height;
    }
}

oops_display_t *oops_gfx_display(oops_gfx_t *gfx) {
    return (gfx != NULL) ? gfx->disp : NULL;
}

const char *oops_gfx_backend_name(void) {
    return "oops-gl";
}

void oops_gfx_destroy(oops_gfx_t *gfx) {
    if (gfx == NULL) {
        return;
    }
    if (gfx->ctx != NULL) {
        glContextDestroy(gfx->ctx);
    }
    if (gfx->disp != NULL) {
        oops_display_close(gfx->disp);
    }
    gfx->disp = NULL;
    gfx->ctx = NULL;
    gfx->in_use = false;
}
