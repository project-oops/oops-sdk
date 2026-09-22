/*
 * hud.c - the GPU 2D overlay declared in <oops/hud.h>.
 *
 * Fixed-function OpenGL 1.1, immediate mode, one texture. The design and the reason it is drawn on
 * the GPU rather than with `oops_draw_text` are in the header. The short version: a Mesa title
 * scans out a tiled buffer the CPU cannot write into, so text has to be drawn the way the scene is.
 *
 * The glyphs are the collection's one 8x8 font (`../draw/font8x8.h`), baked once into a texture
 * atlas of 16 columns by 6 rows of 8x8 cells - 96 cells for the 95 printable characters, the last
 * unused. Each texel is white with the glyph bit in its alpha, so `GL_MODULATE` against the current
 * colour tints the text and blends its edges against the scene.
 */

#include "oops/hud.h"
#include "oops/heap.h"
#include "oops/system.h"

#include "../draw/font8x8.h"

#include <GL/gl.h>

#include <stddef.h>
#include <stdint.h>

/* The atlas geometry. 16 x 6 cells of 8 x 8 -> 128 x 48 texels. */
#define HUD_ATLAS_COLS  16
#define HUD_ATLAS_ROWS  6
#define HUD_CELL        OOPS_FONT8X8_WIDTH            /* 8; width == height */
#define HUD_ATLAS_W     (HUD_ATLAS_COLS * HUD_CELL)   /* 128 */
#define HUD_ATLAS_H     (HUD_ATLAS_ROWS * HUD_CELL)   /* 48 */

struct oops_hud {
    GLuint tex;
    int    fb_w;
    int    fb_h;

    /* State saved across begin/end, so a title (a shader pipeline included) is left as it was. */
    GLint     saved_program;
    GLint     saved_tex_bind;
    GLboolean saved_depth;
    GLboolean saved_cull;
    GLboolean saved_blend;
    GLboolean saved_tex2d;
    GLfloat   saved_color[4];
};

static void hud_log(const char *msg)
{
    oops_klog("OOPS-HUD", msg);
}

/*
 * Fill one 8x8 cell of the atlas from a glyph. `gr` is the glyph row with 0 at the top and `gc`
 * the column with 0 at the left (bit 7 of the row byte); the cell is placed top-down and
 * left-to-right, so a texture coordinate that runs with screen y and x draws the glyph upright.
 */
static void bake_glyph(uint8_t *px, int cell_col, int cell_row, const uint8_t *glyph)
{
    for (int gr = 0; gr < HUD_CELL; gr++) {
        const uint8_t bits = glyph[gr];
        for (int gc = 0; gc < HUD_CELL; gc++) {
            const int on = (bits >> (7 - gc)) & 1;
            const int ax = cell_col * HUD_CELL + gc;
            const int ay = cell_row * HUD_CELL + gr;
            uint8_t *t = px + ((size_t)ay * HUD_ATLAS_W + (size_t)ax) * 4u;

            t[0] = 255u;
            t[1] = 255u;
            t[2] = 255u;
            t[3] = on ? 255u : 0u;
        }
    }
}

oops_hud_t *oops_hud_create(int fb_width, int fb_height)
{
    oops_hud_t *hud = (oops_hud_t *)oops_calloc(1, sizeof(*hud));
    if (hud == NULL) {
        return NULL;
    }
    hud->fb_w = fb_width;
    hud->fb_h = fb_height;

    uint8_t *atlas = (uint8_t *)oops_malloc((size_t)HUD_ATLAS_W * (size_t)HUD_ATLAS_H * 4u);
    if (atlas == NULL) {
        oops_free(hud);
        return NULL;
    }
    for (int i = 0; i < OOPS_FONT8X8_COUNT; i++) {
        bake_glyph(atlas, i % HUD_ATLAS_COLS, i / HUD_ATLAS_COLS, oops_font8x8[i]);
    }

    glGenTextures(1, &hud->tex);
    if (hud->tex == 0) {
        hud_log("no texture name for the font atlas; the overlay will not draw");
        oops_free(atlas);
        oops_free(hud);
        return NULL;
    }

    /* What the caller had bound, so it can be put back. Building the atlas has to bind
     * something, and this function used to end with `glBindTexture(GL_TEXTURE_2D, 0)` - leaving
     * the caller's active unit empty rather than as it was found. `oops_hud_begin`/`_end` below
     * already save and restore exactly this for the drawing pass, so the contract was stated
     * and then broken a hundred lines above it.
     *
     * It cost a hardware session to find, which is the reason for the length of this comment.
     * `mesa-cube` binds its texture once at setup, points its sampler at unit 0 and then creates
     * the overlay; from the first frame it sampled an empty unit, `texture()` returned zero, and
     * the cube drew as a solid black silhouette - with the geometry, the spin, the depth test,
     * the direct scanout and the 59.94 fps pacing all exactly right, and the overlay itself
     * drawing perfectly on top of it. It read as a driver or compiler fault rather than a state
     * leak, and was nearly filed as a clang 21 regression. (REQ-20260922T0040Z-c93d) */
    GLint prev_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    glBindTexture(GL_TEXTURE_2D, hud->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HUD_ATLAS_W, HUD_ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);

    oops_free(atlas);
    return hud;
}

void oops_hud_destroy(oops_hud_t *hud)
{
    if (hud == NULL) {
        return;
    }
    if (hud->tex != 0) {
        glDeleteTextures(1, &hud->tex);
    }
    oops_free(hud);
}

void oops_hud_begin(oops_hud_t *hud)
{
    if (hud == NULL) {
        return;
    }

    /* Save what this pass changes. The program is saved and cleared so fixed-function draws take
     * effect over a title running its own shaders, then restored in end. */
    hud->saved_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &hud->saved_program);
    glUseProgram(0);

    hud->saved_depth = glIsEnabled(GL_DEPTH_TEST);
    hud->saved_cull = glIsEnabled(GL_CULL_FACE);
    hud->saved_blend = glIsEnabled(GL_BLEND);
    hud->saved_tex2d = glIsEnabled(GL_TEXTURE_2D);
    hud->saved_tex_bind = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &hud->saved_tex_bind);
    glGetFloatv(GL_CURRENT_COLOR, hud->saved_color);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Pixel coordinates, origin at the top-left - the same convention oops_draw_text uses. */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)hud->fb_w, (GLdouble)hud->fb_h, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glBindTexture(GL_TEXTURE_2D, hud->tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
}

static void set_color(uint32_t c)
{
    glColor4f((GLfloat)((c >> 16) & 0xffu) / 255.0f,
              (GLfloat)((c >> 8) & 0xffu) / 255.0f,
              (GLfloat)(c & 0xffu) / 255.0f,
              (GLfloat)((c >> 24) & 0xffu) / 255.0f);
}

void oops_hud_rect(oops_hud_t *hud, int x, int y, int w, int h, uint32_t color)
{
    if (hud == NULL) {
        return;
    }
    const GLfloat x0 = (GLfloat)x;
    const GLfloat y0 = (GLfloat)y;
    const GLfloat x1 = (GLfloat)(x + w);
    const GLfloat y1 = (GLfloat)(y + h);

    glDisable(GL_TEXTURE_2D);
    set_color(color);
    glBegin(GL_TRIANGLES);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

void oops_hud_text(oops_hud_t *hud, int x, int y, int scale, uint32_t color, const char *str)
{
    if (hud == NULL || str == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const GLfloat cell = (GLfloat)(HUD_CELL * scale);
    const GLfloat du = (GLfloat)HUD_CELL / (GLfloat)HUD_ATLAS_W;
    const GLfloat dv = (GLfloat)HUD_CELL / (GLfloat)HUD_ATLAS_H;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, hud->tex);
    set_color(color);

    glBegin(GL_TRIANGLES);
    GLfloat pen_x = (GLfloat)x;
    const GLfloat pen_y = (GLfloat)y;
    for (const char *p = str; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;

        if (ch >= OOPS_FONT8X8_FIRST && ch <= OOPS_FONT8X8_LAST) {
            const int idx = (int)ch - OOPS_FONT8X8_FIRST;
            const int col = idx % HUD_ATLAS_COLS;
            const int row = idx / HUD_ATLAS_COLS;
            const GLfloat u0 = (GLfloat)(col * HUD_CELL) / (GLfloat)HUD_ATLAS_W;
            const GLfloat v0 = (GLfloat)(row * HUD_CELL) / (GLfloat)HUD_ATLAS_H;
            const GLfloat u1 = u0 + du;
            const GLfloat v1 = v0 + dv;
            const GLfloat qx0 = pen_x;
            const GLfloat qy0 = pen_y;
            const GLfloat qx1 = pen_x + cell;
            const GLfloat qy1 = pen_y + cell;

            /* Screen top-left samples the cell's top-left (v0), so the glyph draws upright. */
            glTexCoord2f(u0, v0); glVertex2f(qx0, qy0);
            glTexCoord2f(u1, v0); glVertex2f(qx1, qy0);
            glTexCoord2f(u1, v1); glVertex2f(qx1, qy1);
            glTexCoord2f(u0, v0); glVertex2f(qx0, qy0);
            glTexCoord2f(u1, v1); glVertex2f(qx1, qy1);
            glTexCoord2f(u0, v1); glVertex2f(qx0, qy1);
        }
        pen_x += cell; /* a blank (space, or an out-of-range char) still advances the pen */
    }
    glEnd();
}

int oops_hud_text_width(int scale, const char *str)
{
    if (str == NULL) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    int n = 0;
    for (const char *p = str; *p != '\0'; p++) {
        n++;
    }
    return n * HUD_CELL * scale;
}

void oops_hud_end(oops_hud_t *hud)
{
    if (hud == NULL) {
        return;
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    /* Restore exactly what begin saved, to the enable/disable each capability was in. */
    glBindTexture(GL_TEXTURE_2D, (GLuint)hud->saved_tex_bind);
    if (hud->saved_tex2d) { glEnable(GL_TEXTURE_2D); } else { glDisable(GL_TEXTURE_2D); }
    if (hud->saved_blend) { glEnable(GL_BLEND); } else { glDisable(GL_BLEND); }
    if (hud->saved_cull) { glEnable(GL_CULL_FACE); } else { glDisable(GL_CULL_FACE); }
    if (hud->saved_depth) { glEnable(GL_DEPTH_TEST); } else { glDisable(GL_DEPTH_TEST); }
    glColor4f(hud->saved_color[0], hud->saved_color[1], hud->saved_color[2], hud->saved_color[3]);
    glUseProgram((GLuint)hud->saved_program);
}
