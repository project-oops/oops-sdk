/*
 * oops/hud.h - a 2D overlay drawn by the GPU, over whatever a title is rendering.
 *
 * # Why this exists
 *
 * A title that wants a frame counter, a build stamp or a line of telemetry over its 3D scene has
 * two ways to put text on the screen, and on this platform only one of them works everywhere.
 *
 * `oops/draw.h` writes pixels into a CPU-addressable surface. That is right for `oops-gl`, whose
 * scanout buffer is linear and reachable from the CPU. It is *wrong* for a Mesa title: since the
 * present path scans out Mesa's own tiled buffer directly (oops-mesa D012), there is no linear
 * surface to write into, and drawing text with `oops_draw_text` would either land in a detiled
 * scratch copy the CPU has to pay for every frame, or scatter a lattice across a tiled buffer the
 * display reads as raw pixels.
 *
 * So the overlay is drawn the way the scene is: on the GPU. This library issues fixed-function
 * OpenGL 1.1 - an orthographic projection, a font baked into a texture atlas once, and one
 * textured quad per glyph - which runs unchanged on `oops-gl` (native fixed-function) and on Mesa
 * (whose default context is a compatibility profile, so fixed-function and the title's own shaders
 * both work). It uses immediate mode rather than vertex arrays deliberately: immediate mode does
 * not touch the bound vertex-array object or client-array state, so it composes over a title that
 * drives the modern pipeline without disturbing it.
 *
 * # What it does not do
 *
 * It does not open the display, own a context, or present - the title does all three, and calls
 * this between its last draw and its present. It draws only ASCII 0x20..0x7E, in the collection's
 * one 8x8 font (`src/draw/font8x8.h`, the same glyphs `oops_draw_text` uses), at integer scales.
 * Colour is a 32-bit ARGB word - the layout of `oops_color_t` and the `OOPS_COLOR_*` constants
 * in `<oops/draw.h>`, so those pass straight in - and this header stays free of that dependency.
 *
 * # How a title uses it
 *
 *     oops_hud_t *hud = oops_hud_create(fb_w, fb_h);   // once, after the context is current
 *     ... per frame, after the scene draw and before present:
 *     oops_hud_begin(hud);
 *     oops_hud_rect(hud, 40, 30, 360, 60, 0xC0101820u);           // a panel behind the text
 *     oops_hud_text(hud, 52, 44, 2, OOPS_COLOR_WHITE, "MESA-CUBE");
 *     oops_hud_end(hud);
 *     ... then oops_gl_present() / glSwapBuffers().
 *
 * `begin` saves the GL state it changes (including the bound program, so a shader title is left as
 * it was) and `end` restores it. Everything between them is in pixel coordinates with the origin
 * at the top-left, the same convention `oops_draw_text` uses.
 */
#ifndef OOPS_HUD_H
#define OOPS_HUD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque. Holds the font atlas texture and the framebuffer size. A title never looks inside. */
typedef struct oops_hud oops_hud_t;

/*
 * Create an overlay for a framebuffer `fb_width` x `fb_height` pixels, and upload the font atlas.
 * Must be called with a GL context already current, because it allocates a texture. Returns NULL
 * if the texture could not be created (the only failure) - a title that gets NULL should carry on
 * without a HUD rather than refuse to run.
 *
 * The size is what pixel coordinates are measured against; pass the drawable's extent
 * (`oops_gl_extent`, or `glGetIntegerv(GL_VIEWPORT)`).
 */
oops_hud_t *oops_hud_create(int fb_width, int fb_height);

/* Free the atlas texture and the handle. Safe on NULL. Needs the context current. */
void oops_hud_destroy(oops_hud_t *hud);

/*
 * Begin an overlay pass: save the GL state this library changes, set an orthographic projection
 * matching the framebuffer's pixels (top-left origin), enable alpha blending, and bind the atlas.
 * Every `oops_hud_text`/`oops_hud_rect` call must sit between a begin and an end.
 */
void oops_hud_begin(oops_hud_t *hud);

/*
 * Draw `str` with its top-left corner at (`x`, `y`), each glyph cell `8 * scale` pixels, tinted
 * `color` (ARGB; the alpha is honoured). Characters outside 0x20..0x7E are drawn as blanks. New
 * lines are not interpreted - a caller wanting several lines calls this once per line.
 */
void oops_hud_text(oops_hud_t *hud, int x, int y, int scale, uint32_t color, const char *str);

/* The pixel width `str` would occupy at `scale`, for right-aligning or sizing a panel. Needs no
 * HUD and draws nothing, so it is safe to call outside a begin/end pair. */
int oops_hud_text_width(int scale, const char *str);

/* A filled rectangle at (`x`, `y`), `w` x `h` pixels, `color` (ARGB, alpha honoured). Its use is
 * a dimmed panel behind text; draw it before the text so the text lands on top. */
void oops_hud_rect(oops_hud_t *hud, int x, int y, int w, int h, uint32_t color);

/* End the overlay pass and restore the GL state `begin` saved. */
void oops_hud_end(oops_hud_t *hud);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_HUD_H */
