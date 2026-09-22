/*
 * oops/gfx.h - one way to bring up a renderer, whichever renderer a title is built against.
 *
 * # What this is
 *
 * A title that wants a GL context has, today, two different shapes of setup depending on which
 * renderer it links: oops-gl hands out a context for a display the title opened
 * (`glContextCreate`), while oops-mesa opens the whole stack in one call (`oops_gl_create`). This
 * is the single entry point that hides that difference: the same source builds against either,
 * and `OOPS_RENDERER` (a build switch in oops-apps) decides which backend answers.
 *
 * # Why create opens the display
 *
 * On this platform the display's scanout buffers are named to the compositor exactly once, at
 * open, and can never be changed afterwards (oops-sdk D011). So a renderer that wants its own
 * target scanned out without a per-frame copy has to hand that target over *at open* - which means
 * it has to own the display, not be handed one the title already opened. That is why
 * `oops_gfx_create` opens the display itself rather than taking one, and it is the whole reason
 * this API exists rather than a title calling `oops_display_open` and a context call by hand
 * (oops-sdk D012).
 *
 * The oops-gl backend does not yet take that shortcut - it opens the display and builds a context
 * the same way a title did by hand - but the shape of the call is now the one that lets it, so the
 * optimisation is a change behind this API rather than a change to every title.
 *
 * # What a title does with it
 *
 *     oops_gfx_t *gfx = oops_gfx_create(&(oops_gfx_desc_t){ .width = 0, .height = 0,
 *                                                           .depth = true, .vsync = true });
 *     if (!gfx) { ... }
 *     ... per frame: draw, then
 *     oops_gfx_present(gfx);
 *     ... at the end:
 *     oops_gfx_destroy(gfx);
 *
 * The context is made current on the calling thread by `create`, and there is exactly one - so
 * there is no `make_current`, because a call that can only ever succeed is not worth having
 * (oops-sdk D001). `glSwapBuffers()` still works for ported GL source; it presents the same frame
 * `oops_gfx_present` does.
 */
#ifndef OOPS_GFX_H
#define OOPS_GFX_H

#include "oops/display.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque. Owns the display and the context, and whatever a backend needs to tie them together. */
typedef struct oops_gfx oops_gfx_t;

typedef struct oops_gfx_desc {
    uint32_t width;   /* 0 means the display's own width */
    uint32_t height;  /* 0 means the display's own height */
    bool     depth;   /* a depth buffer is wanted (oops-gl always has one; a hint elsewhere) */
    bool     vsync;   /* present paces itself to scanout (oops-gl always does; a hint elsewhere) */
} oops_gfx_desc_t;

/*
 * Bring the renderer up at the requested size and make its context current on this thread. Opens
 * the display, so a title must not have opened one itself. `desc` may be NULL for all defaults.
 * Returns NULL and logs the step it stopped on; a NULL is a failure to start, never a silent fall
 * back to something slower.
 */
oops_gfx_t *oops_gfx_create(const oops_gfx_desc_t *desc);

/*
 * Present what has been drawn. Returns false only when a backend can tell the frame did not reach
 * the screen - the oops-gl backend flips on vsync and cannot report a failed flip, so it returns
 * true; the Mesa backend returns false if the flip was refused.
 */
bool oops_gfx_present(oops_gfx_t *gfx);

/* The drawable's extent - what was opened, which may differ from a size the title asked for. */
void oops_gfx_extent(const oops_gfx_t *gfx, uint32_t *width, uint32_t *height);

/* The display this opened, for the callers that still need it directly - the input pump, a CPU
 * overlay surface. It belongs to the gfx and is closed by `oops_gfx_destroy`; do not close it. */
oops_display_t *oops_gfx_display(oops_gfx_t *gfx);

/* Which backend answered, for a title that wants to log it. Never NULL. */
const char *oops_gfx_backend_name(void);

/* Tear down the context and the display, in that order. Safe on NULL. */
void oops_gfx_destroy(oops_gfx_t *gfx);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_GFX_H */
