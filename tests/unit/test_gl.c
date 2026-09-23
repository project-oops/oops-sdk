/*
 * Unit tests for oops-gl core state machine, matrix stacks, and rasterization
 */

#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/glut.h"

/*
 * **Every extension spelling is the same value as the core one it aliases.**
 *
 * These were written as `#define GL_DEPTH_COMPONENT16_ARB GL_DEPTH_COMPONENT16`, which cannot be
 * wrong, and on 2026-09-20 that turned out to be unbuildable: a hosted title includes this
 * header and Mesa's `GL/glext.h`, which defines the same names as literals, and a macro
 * redefined with a *different* token sequence is a diagnostic under `-Werror` even when the
 * value is identical. They are literals now - and a literal can be mistyped, which the old form
 * could not. That is the whole reason for what follows.
 *
 * Compile-time, because a wrong one should stop the build rather than wait for a test to run
 * the one call that uses it. A port passes `GL_TEXTURE_WRAP_R_EXT` to `glTexParameteri` and
 * gets `GL_INVALID_ENUM` if the digit is wrong, which is a long way from the typo.
 */
_Static_assert(GL_DEPTH_COMPONENT16_ARB == GL_DEPTH_COMPONENT16, "alias value");
_Static_assert(GL_DEPTH_COMPONENT24_ARB == GL_DEPTH_COMPONENT24, "alias value");
_Static_assert(GL_DEPTH_COMPONENT32_ARB == GL_DEPTH_COMPONENT32, "alias value");
_Static_assert(GL_TEXTURE_DEPTH_SIZE_ARB == GL_TEXTURE_DEPTH_SIZE, "alias value");
_Static_assert(GL_DEPTH_TEXTURE_MODE_ARB == GL_DEPTH_TEXTURE_MODE, "alias value");
_Static_assert(GL_TEXTURE_COMPARE_MODE_ARB == GL_TEXTURE_COMPARE_MODE, "alias value");
_Static_assert(GL_TEXTURE_COMPARE_FUNC_ARB == GL_TEXTURE_COMPARE_FUNC, "alias value");
_Static_assert(GL_COMPARE_R_TO_TEXTURE_ARB == GL_COMPARE_R_TO_TEXTURE, "alias value");
_Static_assert(GL_SAMPLES_PASSED_ARB == GL_SAMPLES_PASSED, "alias value");
_Static_assert(GL_QUERY_COUNTER_BITS_ARB == GL_QUERY_COUNTER_BITS, "alias value");
_Static_assert(GL_CURRENT_QUERY_ARB == GL_CURRENT_QUERY, "alias value");
_Static_assert(GL_QUERY_RESULT_ARB == GL_QUERY_RESULT, "alias value");
_Static_assert(GL_QUERY_RESULT_AVAILABLE_ARB == GL_QUERY_RESULT_AVAILABLE, "alias value");
_Static_assert(GL_PACK_SKIP_IMAGES_EXT == GL_PACK_SKIP_IMAGES, "alias value");
_Static_assert(GL_PACK_IMAGE_HEIGHT_EXT == GL_PACK_IMAGE_HEIGHT, "alias value");
_Static_assert(GL_UNPACK_SKIP_IMAGES_EXT == GL_UNPACK_SKIP_IMAGES, "alias value");
_Static_assert(GL_UNPACK_IMAGE_HEIGHT_EXT == GL_UNPACK_IMAGE_HEIGHT, "alias value");
_Static_assert(GL_TEXTURE_3D_EXT == GL_TEXTURE_3D, "alias value");
_Static_assert(GL_PROXY_TEXTURE_3D_EXT == GL_PROXY_TEXTURE_3D, "alias value");
_Static_assert(GL_TEXTURE_DEPTH_EXT == GL_TEXTURE_DEPTH, "alias value");
_Static_assert(GL_TEXTURE_WRAP_R_EXT == GL_TEXTURE_WRAP_R, "alias value");
_Static_assert(GL_MAX_3D_TEXTURE_SIZE_EXT == GL_MAX_3D_TEXTURE_SIZE, "alias value");
_Static_assert(GL_TEXTURE_BINDING_3D_EXT == GL_TEXTURE_BINDING_3D, "alias value");
_Static_assert(GL_ACTIVE_TEXTURE_ARB == GL_ACTIVE_TEXTURE, "alias value");
_Static_assert(GL_CLIENT_ACTIVE_TEXTURE_ARB == GL_CLIENT_ACTIVE_TEXTURE, "alias value");
_Static_assert(GL_MAX_TEXTURE_UNITS_ARB == GL_MAX_TEXTURE_UNITS, "alias value");
/* The texture-unit selectors are a run of thirty-two; the two ends and a middle one pin it. */
_Static_assert(GL_TEXTURE0_ARB == GL_TEXTURE0, "alias value");
_Static_assert(GL_TEXTURE7_ARB == GL_TEXTURE7, "alias value");
_Static_assert(GL_TEXTURE31_ARB == GL_TEXTURE31, "alias value");
_Static_assert(GL_TEXTURE31 - GL_TEXTURE0 == 31, "the selectors are consecutive");
#include "oops/display.h"
/* The texture tests read a texture's own storage rather than a sampled result: a sampled check
 * would also pass if an update landed in the wrong place and the sampler happened to fetch the
 * right colour, so the bytes are what say which texels moved. `test_pm4.c` reaches into
 * `agc_internal.h` for the same reason. */
#include "src/gl/gl_internal.h"
#include "src/gl/gl_procs.h"
#include "src/gl/glsl_internal.h"
#include "tests/test_common.h"
#include <math.h>

static void test_gl_context_lifecycle(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
    ASSERT_TRUE(disp != NULL);

    void *ctx = glContextCreate(disp);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(glGetCurrentContext() == ctx);

    glViewport(10, 20, 300, 200);
    GLint vp[4] = {0};
    glGetIntegerv(GL_VIEWPORT, vp);
    ASSERT_EQ(vp[0], 10);
    ASSERT_EQ(vp[1], 20);
    ASSERT_EQ(vp[2], 300);
    ASSERT_EQ(vp[3], 200);

    glScissor(5, 15, 100, 80);
    GLint sc[4] = {0};
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    ASSERT_EQ(sc[0], 5);
    ASSERT_EQ(sc[1], 15);
    ASSERT_EQ(sc[2], 100);
    ASSERT_EQ(sc[3], 80);

    const GLubyte *vendor = glGetString(GL_VENDOR);
    ASSERT_TRUE(vendor != NULL && strlen((const char *)vendor) > 0);

    glContextDestroy(ctx);
    ASSERT_TRUE(glGetCurrentContext() == NULL);
    oops_display_close(disp);
}

static void test_gl_state_enables(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
    void *ctx = glContextCreate(disp);

    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_FALSE);

    ASSERT_EQ(glIsEnabled(GL_CULL_FACE), GL_FALSE);
    glEnable(GL_CULL_FACE);
    ASSERT_EQ(glIsEnabled(GL_CULL_FACE), GL_TRUE);

    ASSERT_EQ(glIsEnabled(GL_BLEND), GL_FALSE);
    glEnable(GL_BLEND);
    ASSERT_EQ(glIsEnabled(GL_BLEND), GL_TRUE);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_matrix_transforms(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
    void *ctx = glContextCreate(disp);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    for (int i = 0; i < 16; i++) {
        if (i % 5 == 0) {
            ASSERT_TRUE(fabsf(m[i] - 1.0f) < 1e-5f);
        } else {
            ASSERT_TRUE(fabsf(m[i]) < 1e-5f);
        }
    }

    /* Translation */
    glTranslatef(2.0f, 3.0f, -4.0f);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    ASSERT_TRUE(fabsf(m[12] - 2.0f) < 1e-5f);
    ASSERT_TRUE(fabsf(m[13] - 3.0f) < 1e-5f);
    ASSERT_TRUE(fabsf(m[14] - (-4.0f)) < 1e-5f);

    /* Push, modify, Pop */
    glPushMatrix();
    glTranslatef(10.0f, 0.0f, 0.0f);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    ASSERT_TRUE(fabsf(m[12] - 12.0f) < 1e-5f);

    glPopMatrix();
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    ASSERT_TRUE(fabsf(m[12] - 2.0f) < 1e-5f);

    /* Projection matrix & gluPerspective */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 1.0, 0.1, 100.0);
    GLfloat p[16];
    glGetFloatv(GL_PROJECTION_MATRIX, p);
    ASSERT_TRUE(fabsf(p[11] - (-1.0f)) < 1e-5f);
    ASSERT_TRUE(p[0] > 0.0f);
    ASSERT_TRUE(p[5] > 0.0f);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_clear_and_draw(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    uint32_t *fb = oops_display_get_framebuffer(disp);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ASSERT_EQ(fb[0], 0xff000000u);
    ASSERT_EQ(fb[320 * 120 + 160], 0xff000000u);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Draw immediate mode red triangle */
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.0f,  0.5f, 0.0f);
    glEnd();

    uint32_t center_pixel = fb[240 / 2 * 320 + 320 / 2];
    uint32_t cr = (center_pixel >> 16) & 0xff;
    uint32_t cg = (center_pixel >> 8) & 0xff;
    uint32_t cb = center_pixel & 0xff;
    ASSERT_TRUE(cr > 200 && cg < 50 && cb < 50);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_vertex_arrays_cube(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    uint32_t *fb = oops_display_get_framebuffer(disp);

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, (double)320 / 240.0, 0.1, 10.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -3.0f);

    static const GLfloat quad_verts[] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f
    };
    static const GLfloat quad_colors[] = {
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, quad_verts);
    glColorPointer(3, GL_FLOAT, 0, quad_colors);

    glDrawArrays(GL_QUADS, 0, 4);

    uint32_t center_pixel = fb[240 / 2 * 320 + 320 / 2];
    uint32_t cg = (center_pixel >> 8) & 0xff;
    ASSERT_TRUE(cg > 200);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_texture_lifecycle(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    ASSERT_TRUE(tex_id > 0);
    ASSERT_TRUE(glIsTexture(tex_id));

    glBindTexture(GL_TEXTURE_2D, tex_id);
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    ASSERT_EQ(bound, (GLint)tex_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    static const uint8_t checker_pixels[4 * 4] = {
        255, 255, 255, 255,   0, 0, 255, 255,
        0, 0, 255, 255,       255, 255, 255, 255
    };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker_pixels);

    glBindTexture(GL_TEXTURE_2D, 0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    ASSERT_EQ(bound, 0);

    glDeleteTextures(1, &tex_id);
    ASSERT_EQ(glIsTexture(tex_id), GL_FALSE);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_texture_rendering(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    uint32_t *fb = oops_display_get_framebuffer(disp);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);

    /* 2x2 solid blue texture: RGBA = (0, 0, 255, 255) */
    static const uint8_t blue_pixels[2 * 2 * 4] = {
        0, 0, 255, 255,
        0, 0, 255, 255,
        0, 0, 255, 255,
        0, 0, 255, 255
    };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue_pixels);
    /* A filter that reads no mipmaps: with the default GL_NEAREST_MIPMAP_LINEAR and one level,
     * the texture is incomplete and draws untextured - as on any GL. This test sampled an
     * incomplete texture until completeness was implemented on 2026-09-19. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Draw white quad modulated by blue texture -> result should be blue! */
    glBegin(GL_QUADS);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 0.5f, -0.5f, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 0.5f,  0.5f, 0.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.5f,  0.5f, 0.0f);
    glEnd();

    uint32_t center_pixel = fb[120 * 320 + 160];
    uint32_t cr = (center_pixel >> 16) & 0xff;
    uint32_t cg = (center_pixel >> 8) & 0xff;
    uint32_t cb = center_pixel & 0xff;

    /* Center pixel should be predominantly blue */
    ASSERT_TRUE(cb > 200);
    ASSERT_TRUE(cr < 50);
    ASSERT_TRUE(cg < 50);

    glDeleteTextures(1, &tex_id);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_lighting_state(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
    void *ctx = glContextCreate(disp);
    ASSERT_TRUE(ctx != NULL);

    /* Capabilities */
    ASSERT_EQ(glIsEnabled(GL_LIGHTING), GL_FALSE);
    ASSERT_EQ(glIsEnabled(GL_LIGHT0), GL_FALSE);
    ASSERT_EQ(glIsEnabled(GL_NORMALIZE), GL_FALSE);
    ASSERT_EQ(glIsEnabled(GL_COLOR_MATERIAL), GL_FALSE);

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);

    ASSERT_EQ(glIsEnabled(GL_LIGHTING), GL_TRUE);
    ASSERT_EQ(glIsEnabled(GL_LIGHT0), GL_TRUE);
    ASSERT_EQ(glIsEnabled(GL_NORMALIZE), GL_TRUE);
    ASSERT_EQ(glIsEnabled(GL_COLOR_MATERIAL), GL_TRUE);

    glDisable(GL_LIGHT0);
    ASSERT_EQ(glIsEnabled(GL_LIGHT0), GL_FALSE);
    glEnable(GL_LIGHT0);

    /* Light properties */
    float l_diff[4] = {0.8f, 0.6f, 0.4f, 1.0f};
    glLightfv(GL_LIGHT0, GL_DIFFUSE, l_diff);

    float l_pos[4] = {1.0f, 2.0f, 3.0f, 0.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, l_pos);

    /* Material properties */
    float m_spec[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, m_spec);
    glMaterialf(GL_FRONT, GL_SHININESS, 64.0f);

    /* Light model */
    float lm_amb[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_lighting_rendering(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    ASSERT_TRUE(ctx != NULL);
    uint32_t *fb = oops_display_get_framebuffer(disp);
    ASSERT_TRUE(fb != NULL);

    glViewport(0, 0, 320, 240);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);

    /* Setup directional light from +Z */
    float l_pos[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    float l_diff[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float l_zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, l_pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, l_diff);
    glLightfv(GL_LIGHT0, GL_AMBIENT, l_zero);
    glLightfv(GL_LIGHT0, GL_SPECULAR, l_zero);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, l_zero);

    /* Setup pure red material */
    float m_diff[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    glMaterialfv(GL_FRONT, GL_DIFFUSE, m_diff);
    glMaterialfv(GL_FRONT, GL_AMBIENT, l_zero);
    glMaterialfv(GL_FRONT, GL_SPECULAR, l_zero);
    glMaterialfv(GL_FRONT, GL_EMISSION, l_zero);

    /* Draw quad facing +Z: normal (0, 0, 1). N dot L = 1.0 */
    glBegin(GL_QUADS);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f,  0.5f, 0.0f);
    glVertex3f(-0.5f,  0.5f, 0.0f);
    glEnd();

    uint32_t px_lit = fb[120 * 320 + 160];
    uint32_t r_lit = (px_lit >> 16) & 0xff;
    uint32_t g_lit = (px_lit >> 8) & 0xff;
    uint32_t b_lit = px_lit & 0xff;

    /* Should be bright red */
    ASSERT_TRUE(r_lit > 240);
    ASSERT_EQ(g_lit, 0);
    ASSERT_EQ(b_lit, 0);

    /* Clear and redraw with normal facing perpendicular (1, 0, 0) -> N dot L = 0.0 */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glNormal3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f, -0.5f, 0.0f);
    glVertex3f( 0.5f,  0.5f, 0.0f);
    glVertex3f(-0.5f,  0.5f, 0.0f);
    glEnd();

    uint32_t px_unlit = fb[120 * 320 + 160];
    uint32_t r_unlit = (px_unlit >> 16) & 0xff;

    /* Should receive zero light -> completely black */
    ASSERT_EQ(r_unlit, 0);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_blending_modes(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    ASSERT_TRUE(disp != NULL);
    void *ctx = glContextCreate(disp);
    ASSERT_TRUE(ctx != NULL);

    /* 1. Verify blend function API and queries */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLint sfactor = 0, dfactor = 0;
    glGetIntegerv(GL_BLEND_SRC, &sfactor);
    glGetIntegerv(GL_BLEND_DST, &dfactor);
    ASSERT_EQ((GLenum)sfactor, GL_SRC_ALPHA);
    ASSERT_EQ((GLenum)dfactor, GL_ONE_MINUS_SRC_ALPHA);

    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_SRC_ALPHA);
    GLint s_rgb = 0, d_rgb = 0, s_a = 0, d_a = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &s_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &d_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &d_a);
    ASSERT_EQ((GLenum)s_rgb, GL_ONE);
    ASSERT_EQ((GLenum)d_rgb, GL_ONE);
    ASSERT_EQ((GLenum)s_a, GL_ZERO);
    ASSERT_EQ((GLenum)d_a, GL_SRC_ALPHA);

    glBlendEquation(GL_FUNC_ADD);
    GLint eq = 0;
    glGetIntegerv(GL_BLEND_EQUATION, &eq);
    ASSERT_EQ((GLenum)eq, GL_FUNC_ADD);

    /* 2. Test Alpha Blending rasterization */
    glViewport(0, 0, 320, 240);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Clear to black */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Draw solid red background */
    glDisable(GL_BLEND);
    glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f,  0.8f, 0.0f);
    glVertex3f(-0.8f,  0.8f, 0.0f);
    glEnd();

    /* Overdraw with 50% transparent green quad */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 1.0f, 0.0f, 0.5f);
    glBegin(GL_QUADS);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f,  0.8f, 0.0f);
    glVertex3f(-0.8f,  0.8f, 0.0f);
    glEnd();

    uint32_t *fb = (uint32_t *)oops_display_get_framebuffer(disp);
    uint32_t px = fb[120 * 320 + 160];
    uint32_t r = (px >> 16) & 0xff;
    uint32_t g = (px >> 8) & 0xff;
    uint32_t b = px & 0xff;

    /* Expected: red is blended (approx 127), green is blended (approx 127) */
    ASSERT_TRUE(r >= 120 && r <= 135);
    ASSERT_TRUE(g >= 120 && g <= 135);
    ASSERT_EQ(b, 0);

    /* 3. Test Additive Blending (GL_ONE, GL_ONE) */
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    glColor4f(0.5f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f,  0.8f, 0.0f);
    glVertex3f(-0.8f,  0.8f, 0.0f);
    glEnd();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor4f(0.5f, 0.5f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f, -0.8f, 0.0f);
    glVertex3f( 0.8f,  0.8f, 0.0f);
    glVertex3f(-0.8f,  0.8f, 0.0f);
    glEnd();

    px = fb[120 * 320 + 160];
    r = (px >> 16) & 0xff;
    g = (px >> 8) & 0xff;
    ASSERT_TRUE(r >= 250); /* 0.5 + 0.5 = 1.0 (clamped to 255) */
    ASSERT_TRUE(g >= 120 && g <= 135); /* 0.0 + 0.5 = 0.5 */

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_texture_env_modes(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);

    /* 1. Verify texture environment API */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    GLint env_mode = 0;
    glGetIntegerv(GL_TEXTURE_ENV_MODE, &env_mode);
    ASSERT_EQ((GLenum)env_mode, GL_REPLACE);

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glGetIntegerv(GL_TEXTURE_ENV_MODE, &env_mode);
    ASSERT_EQ((GLenum)env_mode, GL_ADD);

    /* Create solid white 2x2 texture */
    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    uint32_t white_tex[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, white_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); /* complete with one level */
    glEnable(GL_TEXTURE_2D);

    glViewport(0, 0, 320, 240);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Render with GL_REPLACE: vertex color blue (0,0,1) should be replaced by texture white (1,1,1) */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glColor4f(0.0f, 0.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 0.5f, -0.5f, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 0.5f,  0.5f, 0.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.5f,  0.5f, 0.0f);
    glEnd();

    uint32_t *fb = (uint32_t *)oops_display_get_framebuffer(disp);
    uint32_t px = fb[120 * 320 + 160];
    uint32_t r = (px >> 16) & 0xff;
    uint32_t g = (px >> 8) & 0xff;
    uint32_t b = px & 0xff;
    ASSERT_TRUE(r > 240);
    ASSERT_TRUE(g > 240);
    ASSERT_TRUE(b > 240);

    /* Render with GL_MODULATE: blue modulated by white is blue */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glClear(GL_COLOR_BUFFER_BIT);

    glColor4f(0.0f, 0.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 0.5f, -0.5f, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 0.5f,  0.5f, 0.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-0.5f,  0.5f, 0.0f);
    glEnd();

    px = fb[120 * 320 + 160];
    r = (px >> 16) & 0xff;
    g = (px >> 8) & 0xff;
    b = px & 0xff;
    ASSERT_EQ(r, 0);
    ASSERT_EQ(g, 0);
    ASSERT_TRUE(b > 240);

    glDeleteTextures(1, &tex_id);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_matrix_stack_limits(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Clear any error */
    while (glGetError() != GL_NO_ERROR);

    /* Push 15 times (depth from 0 to 15) */
    for (int i = 0; i < 15; i++) {
        glPushMatrix();
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* 16th push should exceed depth (OOPS_GL_MODELVIEW_STACK_CAPACITY = 16) */
    glPushMatrix();
    ASSERT_EQ(glGetError(), GL_STACK_OVERFLOW);

    /* Pop back 15 times */
    for (int i = 0; i < 15; i++) {
        glPopMatrix();
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* Extra pop on depth 0 should underflow */
    glPopMatrix();
    ASSERT_EQ(glGetError(), GL_STACK_UNDERFLOW);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* A mode this subset does not draw is refused, not silently skipped.
 *
 * The bug this pins was invisible by construction: GL_LINES reached a `default: break;`, so
 * the call drew nothing, set no error, and glGetError() kept answering GL_NO_ERROR. A caller
 * could not tell that apart from a successful draw - and in an instrument, an empty record
 * still looks like a record.
 *
 * Both halves are asserted, because either alone would pass while the other was broken: the
 * error is raised, AND the framebuffer is untouched. The triangle afterwards is the positive
 * control - without it this test would also pass against a build that refused everything. */
static void test_gl_unsupported_primitive_mode_is_refused(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    uint32_t *fb = oops_display_get_framebuffer(disp);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    (void)glGetError();

    /* **Lines draw now**, as a screen-width quad of two triangles. They were refused here until
     * 2026-09-17 because the geometry engine stalls on a two-vertex primitive - which is a fact
     * about the native primitive, not about whether a line can be drawn. */
    glBegin(GL_LINES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-0.9f, 0.0f, 0.0f);
    glVertex3f( 0.9f, 0.0f, 0.0f);
    glEnd();
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* **One row thick.** The line lies exactly between two rows of pixel centres, on its quad's
     * edges; both rows were drawn until the rasteriser's tie rule (2026-09-19), a width-1 line
     * two pixels thick. Now exactly one of them is. */
    {
        const GLboolean above = (GLboolean)(fb[(240 / 2 - 1) * 320 + 320 / 2] != 0xff000000u);
        const GLboolean below = (GLboolean)(fb[240 / 2 * 320 + 320 / 2] != 0xff000000u);
        ASSERT_TRUE(above != below);
    }

    /* An enum that is not a primitive mode at all is still refused. */
    glBegin((GLenum)0xdeadbeefu);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* **All three switches must agree about which modes draw** - they used to disagree, and a
     * mode accepted by glBegin but unhandled by the array assembly draws nothing and says
     * nothing, which is worse than a refusal. */
    glClear(GL_COLOR_BUFFER_BIT);
    static const GLfloat verts[9] = {
        -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.0f, 0.5f, 0.0f
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glColor3f(0.0f, 1.0f, 0.0f);
    glDrawArrays(GL_LINE_STRIP, 0, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    {
        int lit = 0;
        for (int i = 0; i < 320 * 240; i++) if (fb[i] != 0xff000000u) lit++;
        ASSERT_TRUE(lit > 0);
    }

    glClear(GL_COLOR_BUFFER_BIT);
    static const GLubyte idx[3] = {0, 1, 2};
    glDrawElements(GL_POLYGON, 3, GL_UNSIGNED_BYTE, idx);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    {
        int lit = 0;
        for (int i = 0; i < 320 * 240; i++) if (fb[i] != 0xff000000u) lit++;
        ASSERT_TRUE(lit > 0);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    /* Positive control: a mode this subset does draw still draws, and raises nothing. The array
     * stays enabled for it - disabling it here makes this draw nothing and the control passes
     * for the wrong reason. */
    glColor3f(1.0f, 0.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    uint32_t centre = fb[240 / 2 * 320 + 320 / 2];
    ASSERT_TRUE(((centre >> 16) & 0xff) > 200);

    glDisableClientState(GL_VERTEX_ARRAY);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* A negative count is an error; a zero count is a legal draw of nothing.
 *
 * These shared one silent `return` on `count <= 0`, which made a caller's broken arithmetic
 * indistinguishable from an empty batch. */
static void test_gl_draw_count_separates_empty_from_invalid(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    static const GLfloat verts[9] = {
        -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.0f, 0.5f, 0.0f
    };
    static const GLushort idx[3] = {0, 1, 2};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    (void)glGetError();

    glDrawArrays(GL_TRIANGLES, 0, -1);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    glDrawElements(GL_TRIANGLES, -3, GL_UNSIGNED_SHORT, idx);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glDrawArrays(GL_TRIANGLES, 0, 0);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDrawElements(GL_TRIANGLES, 0, GL_UNSIGNED_SHORT, idx);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    glDisableClientState(GL_VERTEX_ARRAY);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* An index type the reader cannot read is refused before it reads.
 *
 * Not a cosmetic check. The reader selects 16-bit and 8-bit explicitly and treats everything
 * else as 32-bit, so an unchecked type read four bytes per index out of an array the caller
 * sized for one - past the end of it. The three legal types are asserted alongside, so a
 * build that refused all four would not pass this. */
static void test_gl_draw_elements_refuses_an_unreadable_index_type(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    static const GLfloat verts[9] = {
        -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.0f, 0.5f, 0.0f
    };
    static const GLubyte  b_idx[3] = {0, 1, 2};
    static const GLushort s_idx[3] = {0, 1, 2};
    static const GLuint   i_idx[3] = {0, 1, 2};
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    (void)glGetError();

    glDrawElements(GL_TRIANGLES, 3, GL_FLOAT, b_idx);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, b_idx);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, s_idx);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, i_idx);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    glDisableClientState(GL_VERTEX_ARRAY);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* What the strings say, and that GL_VERSION is parseable at all.
 *
 * GL_VERSION had read "OpenGL 1.3 oops-gl 2.0": the wrong version, and with a word in front
 * of the number, so the conventional atof() on it returned 0.0. The specification requires
 * the string to begin with the version, and this asserts that shape rather than the exact
 * text, so the suffix stays editable. */
static void test_gl_strings_are_honest_and_parseable(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    (void)glGetError();

    const GLubyte *version = glGetString(GL_VERSION);
    ASSERT_TRUE(version != NULL);
    ASSERT_TRUE(version[0] >= '0' && version[0] <= '9');
    ASSERT_TRUE(strncmp((const char *)version, "1.1", 3) == 0);

    /* **The version is the caller's to state** (2026-09-20). A port written against a later 1.x
     * checks the badge before calling something this library does have, so it can say what it
     * targets - and nothing else changes: the suffix still says subset, and the same calls are
     * implemented either way. */
    GLuint maj = 0u, min = 9u;
    glContextGetVersion(&maj, &min);
    ASSERT_EQ(maj, 1u);
    ASSERT_EQ(min, 1u);
    ASSERT_EQ(glContextSetVersion(1, 4), GL_TRUE);
    version = glGetString(GL_VERSION);
    ASSERT_TRUE(strncmp((const char *)version, "1.4", 3) == 0);
    ASSERT_TRUE(strstr((const char *)version, "subset") != NULL);
    glContextGetVersion(&maj, &min);
    ASSERT_EQ(min, 4u);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* **2.0 is claimable since 2026-09-21**, because the programmable pipeline runs - and its
     * badge says `programmable` rather than `fixed-function`, which is the wrong word for the
     * one thing 2.0 adds. It still says `subset`. */
    ASSERT_EQ(glContextSetVersion(2, 0), GL_TRUE);
    version = glGetString(GL_VERSION);
    ASSERT_TRUE(strncmp((const char *)version, "2.0", 3) == 0);
    ASSERT_TRUE(strstr((const char *)version, "programmable") != NULL);
    ASSERT_TRUE(strstr((const char *)version, "subset") != NULL);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* **2.1 is the GLSL 1.20 one** and is claimable because the front end takes that dialect. */
    ASSERT_EQ(glContextSetVersion(2, 1), GL_TRUE);
    version = glGetString(GL_VERSION);
    ASSERT_TRUE(strncmp((const char *)version, "2.1", 3) == 0);

    /* Not above that, and a refusal leaves the version alone rather than half-setting it. */
    ASSERT_EQ(glContextSetVersion(1, 4), GL_TRUE);
    ASSERT_EQ(glContextSetVersion(2, 2), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    ASSERT_EQ(glContextSetVersion(3, 0), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    ASSERT_EQ(glContextSetVersion(1, 6), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    version = glGetString(GL_VERSION);
    ASSERT_TRUE(strncmp((const char *)version, "1.4", 3) == 0);

    ASSERT_EQ(glContextSetVersion(1, 1), GL_TRUE);
    version = glGetString(GL_VERSION);
    ASSERT_TRUE(strncmp((const char *)version, "1.1", 3) == 0);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The brand that used to be in GL_RENDERER is not in any of them (conventions 2). */
    const GLubyte *renderer = glGetString(GL_RENDERER);
    ASSERT_TRUE(renderer != NULL);
    ASSERT_TRUE(strstr((const char *)renderer, "PlayStation") == NULL);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* **The extension list names what this library implements under the name a program looks
     * for** (2026-09-19; empty before that).
     *
     * An extension string is a promise about that extension's own entry points, which is why it
     * was empty while only the core spellings existed. The entry points are here now
     * (test_gl_extension_entry_points_are_the_core_ones), so the extensions are listed - and
     * those that need no entry point, only state, beside them.
     *
     * Asserted by the rule and by both ends: every name begins with `GL_` and appears once;
     * extensions whose entry points exist are named; extensions this library does not keep on
     * every path are not. This context is the software rasteriser, which applies both texture
     * units, so GL_ARB_multitexture is on its list. */
    const GLubyte *ext = glGetString(GL_EXTENSIONS);
    ASSERT_TRUE(ext != NULL);
    ASSERT_TRUE(ext[0] != '\0');
    const char *ext_s = (const char *)ext;
    ASSERT_TRUE(strncmp(ext_s, "GL_", 3) == 0);
    ASSERT_TRUE(strstr(ext_s, "  ") == NULL);
    ASSERT_TRUE(ext_s[strlen(ext_s) - 1] != ' ');
    for (const char *p = strchr(ext_s, ' '); p; p = strchr(p + 1, ' ')) {
        ASSERT_TRUE(strncmp(p + 1, "GL_", 3) == 0);
    }
    static const char *const listed[] = {
        "GL_ARB_multitexture", "GL_ARB_vertex_buffer_object", "GL_ARB_window_pos",
        "GL_ARB_transpose_matrix", "GL_ARB_point_parameters", "GL_ARB_texture_env_combine",
        "GL_ARB_texture_env_dot3", "GL_ARB_texture_mirrored_repeat", "GL_EXT_secondary_color",
        "GL_EXT_fog_coord", "GL_EXT_draw_range_elements", "GL_EXT_multi_draw_arrays",
        "GL_EXT_blend_color", "GL_EXT_blend_minmax", "GL_EXT_blend_subtract", "GL_EXT_bgra",
        "GL_EXT_stencil_wrap", "GL_EXT_separate_specular_color", "GL_EXT_texture_lod_bias",
        /* Since 2026-09-20: it adds no entry point, only targets, enums and the two cube-map
         * generation modes, and both paths keep all of them. */
        "GL_ARB_texture_cube_map",
        /* Since 2026-09-20, when the console gained the volume sample. Its two entry points
         * arrived with it - the check below calls them. */
        "GL_EXT_texture3D",
        /* Since 2026-09-20, when the console gained the comparison sample. Neither adds an entry
         * point: a depth texture is glTexImage2D with a GL_DEPTH_COMPONENT internal format and
         * the comparison is glTexParameteri, both of which every path has. */
        "GL_ARB_depth_texture", "GL_ARB_shadow",
        /* Since 2026-09-20, when the GPU started counting. Its eight entry points arrived with
         * it - the check below calls them. */
        "GL_ARB_occlusion_query",
    };
    for (size_t i = 0; i < sizeof(listed) / sizeof(listed[0]); i++) {
        const char *first = strstr(ext_s, listed[i]);
        ASSERT_TRUE(first != NULL);
        ASSERT_TRUE(strstr(first + 1, listed[i]) == NULL); /* once */
    }
    /* Not listed: no entry points of its own here. `glVertexPointer` and its kin are core, not
     * GL_EXT_vertex_array's `glVertexPointerEXT`. */
    static const char *const absent[] = {
        "GL_EXT_vertex_array",
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        ASSERT_TRUE(strstr(ext_s, absent[i]) == NULL);
    }
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* An unrecognised name is NULL plus an error, not an empty string, which would read as a
     * real answer of "nothing" rather than as a refusal. */
    ASSERT_TRUE(glGetString((GLenum)0x1234u) == NULL);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* **Every extension in the list has its entry points, and they are the core functions**
 * (2026-09-19). A port finds an extension in glGetString's list and calls that extension's own
 * names; each one here is checked to do what the core spelling does. They exist at all only
 * because the linker resolved them, which is half the point - the other half is that they are
 * not stubs. */
static void test_gl_extension_entry_points_are_the_core_ones(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();
  GLint iv[4];
  GLfloat fv[16];

  /* GL_ARB_vertex_buffer_object: a buffer generated, filled, read back and mapped through the
   * ARB names, and seen by the core ones. */
  GLuint buf = 0;
  glGenBuffersARB(1, &buf);
  ASSERT_TRUE(buf != 0u);
  ASSERT_EQ(glIsBufferARB(buf), GL_TRUE);
  glBindBufferARB(GL_ARRAY_BUFFER, buf);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, iv);
  ASSERT_EQ(iv[0], (GLint)buf);
  static const GLfloat data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  glBufferDataARB(GL_ARRAY_BUFFER, (GLsizeiptrARB)sizeof(data), data, GL_STATIC_DRAW);
  glGetBufferParameterivARB(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, iv);
  ASSERT_EQ(iv[0], (GLint)sizeof(data));
  GLfloat back[4] = {0};
  glGetBufferSubDataARB(GL_ARRAY_BUFFER, 0, (GLsizeiptrARB)sizeof(back), back);
  ASSERT_TRUE(back[2] == 3.0f);
  const GLfloat two = 9.0f;
  glBufferSubDataARB(GL_ARRAY_BUFFER, (GLintptrARB)sizeof(GLfloat), (GLsizeiptrARB)sizeof(two), &two);
  void *mapped = glMapBufferARB(GL_ARRAY_BUFFER, GL_READ_ONLY);
  ASSERT_TRUE(mapped != NULL && ((const GLfloat *)mapped)[1] == 9.0f);
  void *ptr = NULL;
  glGetBufferPointervARB(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &ptr);
  ASSERT_TRUE(ptr == mapped);
  ASSERT_EQ(glUnmapBufferARB(GL_ARRAY_BUFFER), GL_TRUE);
  glBindBufferARB(GL_ARRAY_BUFFER, 0);
  glDeleteBuffersARB(1, &buf);
  ASSERT_EQ(glIsBufferARB(buf), GL_FALSE);

  /* GL_EXT_secondary_color and GL_EXT_fog_coord: the current values the core queries report. */
  glSecondaryColor3fEXT(0.25f, 0.5f, 0.75f);
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.25f && fv[1] == 0.5f && fv[2] == 0.75f);
  static const GLubyte sec_ub[3] = {255, 0, 255};
  glSecondaryColor3ubvEXT(sec_ub);
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == 0.0f);
  glFogCoordfEXT(2.5f);
  glGetFloatv(GL_CURRENT_FOG_COORD, fv);
  ASSERT_TRUE(fv[0] == 2.5f);

  /* GL_ARB_window_pos, GL_ARB_transpose_matrix, GL_ARB_point_parameters, GL_EXT_blend_*. */
  glWindowPos2iARB(7, 9);
  glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
  ASSERT_TRUE(fv[0] == 7.0f && fv[1] == 9.0f); /* the position as given, z from the depth range */
  static const GLfloat rows[16] = {1, 2, 3, 4, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  glMatrixMode(GL_MODELVIEW);
  glLoadTransposeMatrixfARB(rows);
  glGetFloatv(GL_MODELVIEW_MATRIX, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == 0.0f && fv[4] == 2.0f && fv[12] == 4.0f);
  glLoadIdentity();
  glPointParameterfARB(GL_POINT_SIZE_MIN, 2.0f);
  glGetFloatv(GL_POINT_SIZE_MIN, fv);
  ASSERT_TRUE(fv[0] == 2.0f);
  glPointParameterfEXT(GL_POINT_SIZE_MIN, 1.0f);
  glBlendColorEXT(0.5f, 0.25f, 0.125f, 1.0f);
  glGetFloatv(GL_BLEND_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.5f && fv[2] == 0.125f);
  glBlendEquationEXT(GL_FUNC_SUBTRACT);
  glGetIntegerv(GL_BLEND_EQUATION, iv);
  ASSERT_EQ(iv[0], (GLint)GL_FUNC_SUBTRACT);
  glBlendEquationEXT(GL_FUNC_ADD);

  /* GL_ARB_occlusion_query: a query generated, run and read through the ARB names and seen by
   * the core ones - `glGetQueryiv(GL_CURRENT_QUERY)` and `glIsQuery` reporting what an ARB begin
   * did is what says these share the state rather than each keeping their own.
   *
   * `glIsQuery` is false for a generated-but-never-begun name, which is GL's rule and Mesa's
   * (`main/queryobj.c`): the name is reserved, the object exists only once begun. */
  {
    GLuint aq = 0;
    glGenQueriesARB(1, &aq);
    ASSERT_TRUE(aq != 0u);
    ASSERT_EQ(glIsQueryARB(aq), GL_FALSE);
    glGetQueryivARB(GL_SAMPLES_PASSED_ARB, GL_QUERY_COUNTER_BITS_ARB, iv);
    ASSERT_EQ(iv[0], 32);
    glBeginQueryARB(GL_SAMPLES_PASSED_ARB, aq);
    /* The core calls see what the ARB one did: the name is current, and the object now exists. */
    glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, iv);
    ASSERT_EQ(iv[0], (GLint)aq);
    ASSERT_EQ(glIsQuery(aq), GL_TRUE);
    glEndQueryARB(GL_SAMPLES_PASSED_ARB);
    glGetQueryObjectivARB(aq, GL_QUERY_RESULT_AVAILABLE_ARB, iv);
    ASSERT_EQ(iv[0], (GLint)GL_TRUE);
    GLuint samples = 0xffffffffu;
    glGetQueryObjectuivARB(aq, GL_QUERY_RESULT_ARB, &samples);
    ASSERT_EQ(samples, 0u); /* nothing was drawn between the two */
    glDeleteQueriesARB(1, &aq);
    ASSERT_EQ(glIsQueryARB(aq), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
  }

  /* GL_EXT_texture3D: a volume uploaded and sub-uploaded through the EXT names, and the core
   * queries reporting what they made - the enums under their EXT spellings too, which are what
   * a program of that era writes. */
  {
    GLuint vol = 0;
    static const GLubyte voxels[2 * 2 * 2 * 4] = {0};
    static const GLubyte one[4] = {9, 9, 9, 255};
    glGenTextures(1, &vol);
    glBindTexture(GL_TEXTURE_3D_EXT, vol);
    glTexImage3DEXT(GL_TEXTURE_3D_EXT, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, voxels);
    glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH_EXT, iv);
    ASSERT_EQ(iv[0], 2);
    glTexSubImage3DEXT(GL_TEXTURE_3D_EXT, 0, 1, 1, 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetIntegerv(GL_TEXTURE_BINDING_3D_EXT, iv);
    ASSERT_EQ(iv[0], (GLint)vol);
    glBindTexture(GL_TEXTURE_3D_EXT, 0);
    glDeleteTextures(1, &vol);
  }

  /* GL_EXT_draw_range_elements and GL_EXT_multi_draw_arrays draw what the core spellings draw:
   * a red pixel each, through the arrays. */
  uint32_t *fb = oops_display_get_framebuffer(disp);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  static const GLfloat quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
  static const GLubyte idx[6] = {0, 1, 2, 0, 2, 3};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, quad);
  glColor3f(1.0f, 0.0f, 0.0f);
  glDrawRangeElementsEXT(GL_TRIANGLES, 0, 3, 6, GL_UNSIGNED_BYTE, idx);
  ASSERT_EQ(fb[32 * 64 + 32] & 0x00ffffffu, 0x00ff0000u);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(0.0f, 1.0f, 0.0f);
  static const GLint first[1] = {0};
  static const GLsizei counts[1] = {4};
  glMultiDrawArraysEXT(GL_QUADS, first, counts, 1);
  ASSERT_EQ(fb[32 * 64 + 32] & 0x00ffffffu, 0x0000ff00u);
  glDisableClientState(GL_VERTEX_ARRAY);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Capabilities, client arrays and matrix modes this subset does not have are refused.
 *
 * All three used to be dropped on a `default: break;`. glEnable is the worst of them, because
 * it is the first thing a GL program does and a dropped one renders wrong with nothing to say
 * why. glMatrixMode is the subtlest: an unrecognised mode left the previous mode in place, so
 * every matrix call afterwards edited a stack the caller was not thinking about.
 *
 * Each refusal is paired with a supported value asserted to still work, so a build that
 * refused everything would fail this too. */
/* **A captured frame replays to the same pixels.**
 *
 * This is the whole claim the capture rests on, so it is asserted against pixels rather than
 * against a call count: a stream that replays *nearly* right is worse than one that fails,
 * because its whole purpose is to be the reference a hardware run is compared to.
 *
 * Three renders of the same drawing. The first is the truth. The second runs with capture on,
 * which must not change what is drawn - a capture that perturbed the frame would be measuring
 * itself. The third is the replay of that capture into a cleared buffer, and has to match the
 * first exactly, not approximately.
 *
 * The drawing carries the things that are easy to get wrong in a serialiser: a float that has
 * to survive bit-exact, an enum, and a texture upload, whose pixels are a blob whose length
 * the command did not previously record - which is why `gl_list_rec_owned` grew a `bytes`
 * parameter and every one of its nine call sites was visited. */
static void test_gl_capture_replays_to_identical_pixels(void) {
    enum { W = 32, H = 32 };
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
    void *gc = glContextCreate(disp);
    ASSERT_TRUE(gc != NULL);
    glViewport(0, 0, W, H);

    static const GLubyte texels[4][4] = {
        {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255},
    };
    GLuint tex = 0;
    glGenTextures(1, &tex);

    /* One drawing, three times. `glGenTextures` is not compiled by GL and so is not captured;
       the texture name is made once, outside, exactly as a real frame would find it already
       made. */
    #define DRAW_THE_FRAME()                                                                   \
        do {                                                                                   \
            glClearColor(0.125f, 0.25f, 0.5f, 1.0f);                                           \
            glClear(GL_COLOR_BUFFER_BIT);                                                      \
            glBindTexture(GL_TEXTURE_2D, tex);                                                 \
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);                 \
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);                 \
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);                                             \
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,        \
                         texels);                                                              \
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);                                             \
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);                        \
            glEnable(GL_TEXTURE_2D);                                                           \
            glBegin(GL_QUADS);                                                                 \
            glTexCoord2f(0.0f, 0.0f); glVertex2f(-0.75f, -0.75f);                              \
            glTexCoord2f(1.0f, 0.0f); glVertex2f( 0.75f, -0.75f);                              \
            glTexCoord2f(1.0f, 1.0f); glVertex2f( 0.75f,  0.75f);                              \
            glTexCoord2f(0.0f, 1.0f); glVertex2f(-0.75f,  0.75f);                              \
            glEnd();                                                                           \
            glDisable(GL_TEXTURE_2D);                                                          \
        } while (0)

    static GLubyte truth[W * H * 4];
    static GLubyte during[W * H * 4];
    static GLubyte after[W * H * 4];

    DRAW_THE_FRAME();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, truth);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* With capture on: the frame must be unchanged, and the stream must be non-empty. */
    oops_gl_capture_begin();
    DRAW_THE_FRAME();
    oops_gl_capture_end();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, during);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_EQ(memcmp(truth, during, sizeof(truth)), 0);

    size_t bytes = 0;
    unsigned calls = 0;
    const void *stream = oops_gl_capture_data(&bytes, &calls);
    ASSERT_TRUE(stream != NULL);
    ASSERT_TRUE(calls > 0u);
    ASSERT_TRUE(bytes > 12u); /* more than the header alone */

    /* Wipe the buffer to something neither the clear colour nor any texel, so a replay that
       drew nothing at all would fail rather than coincidentally match. */
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, 0);

    const unsigned ran = oops_gl_capture_replay(stream, bytes);
    ASSERT_EQ(ran, calls);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, after);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_EQ(memcmp(truth, after, sizeof(truth)), 0);

    /* A stream that is not one of ours is refused rather than executed as noise. */
    static const char junk[16] = {'N', 'O', 'T', 'C', 'A', 'P', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(oops_gl_capture_replay(junk, sizeof(junk)), 0u);
    ASSERT_EQ(oops_gl_capture_replay(stream, 4u), 0u); /* shorter than a header */

    #undef DRAW_THE_FRAME
    glDeleteTextures(1, &tex);
    glContextDestroy(gc);
    oops_display_close(disp);
}

static void test_gl_unsupported_state_enums_are_refused(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    (void)glGetError();

    /* Real GL capabilities this subset does not implement. Written as their specification values
     * rather than added to the header, because a #define there would read as a claim to support
     * them.
     *
     * **GL_STENCIL_TEST used to be the first example here and is implemented now**, as
     * GL_ALPHA_TEST and GL_FOG were before it, and GL_LINE_SMOOTH after it. That is this test
     * doing its job again and again: a refusal that stops being a refusal shows up as a failure
     * rather than as silence. GL 1.4's GL_COLOR_SUM stood here until 2026-09-19, and was the
     * last GL 1.x core enable to go; **GL 2.0's GL_POINT_SPRITE stood here until 2026-09-22**,
     * and went because Neverball asks for it on every frame that draws particles - 102 refusals
     * in one short run, with the particle system falling back to flat untextured squares and
     * nothing in the title ever calling glGetError to find out. It is asserted below instead.
     *
     * GL_VERTEX_PROGRAM_POINT_SIZE takes its place: GL 2.0's "let the vertex shader write
     * gl_PointSize", which needs a vertex shader stage that writes it. */
    glEnable((GLenum)0x8642u); /* GL_VERTEX_PROGRAM_POINT_SIZE */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    /* GL_CONVOLUTION_1D: the imaging subset, which is optional and not advertised, so refusing
     * it is conformant for good. (GL_DITHER stood here until 2026-09-19; it is on by default in
     * every GL context and is accepted as state now.) */
    glDisable((GLenum)0x8010u);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_EQ(glIsEnabled(GL_DITHER), GL_TRUE);
    glDisable(GL_DITHER);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_EQ(glIsEnabled(GL_DITHER), GL_FALSE);
    glEnable(GL_DITHER);
    /* And the ones that left this list are accepted, each with its own state. */
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDisable(GL_ALPHA_TEST);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 1, 0xff);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDisable(GL_STENCIL_TEST);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glEnable(GL_FOG);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDisable(GL_FOG);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* The point sprite, with the per-unit GL_COORD_REPLACE that is the whole point of it and the
     * third glTexEnv target it arrives on. Neverball issues exactly this trio per frame. */
    glEnable(GL_POINT_SPRITE);
    ASSERT_EQ(glIsEnabled(GL_POINT_SPRITE), GL_TRUE);
    glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint replace = 0;
    glGetTexEnviv(GL_POINT_SPRITE, GL_COORD_REPLACE, &replace);
    ASSERT_EQ(replace, 1);
    /* GL_UPPER_LEFT by default, and the other corner settable; anything else is an enum error. */
    GLint origin = 0;
    glGetIntegerv(GL_POINT_SPRITE_COORD_ORIGIN, &origin);
    ASSERT_EQ(origin, (GLint)GL_UPPER_LEFT);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, (GLint)GL_LOWER_LEFT);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, (GLint)GL_NICEST);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, (GLint)GL_UPPER_LEFT);
    glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_FALSE);
    glDisable(GL_POINT_SPRITE);
    ASSERT_EQ(glIsEnabled(GL_POINT_SPRITE), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The ones it does have, including a light, still work and raise nothing. */
    glEnable(GL_DEPTH_TEST);
    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_TRUE);
    glEnable(GL_LIGHT0);
    glDisable(GL_DEPTH_TEST);
    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Client arrays. GL_INDEX_ARRAY was the refused example until 2026-09-19; it is colour-index
     * state, which an RGBA context keeps, so it is accepted now. An enum that is no array at all
     * is what is refused. */
    glEnableClientState(GL_INDEX_ARRAY);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDisableClientState(GL_INDEX_ARRAY);
    glEnableClientState((GLenum)GL_TEXTURE_2D);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* A refused matrix mode must not silently leave the previous one selected: set a known
     * mode, refuse another, then prove the known one is still the active one by pushing and
     * popping it without a stack error. */
    glMatrixMode(GL_PROJECTION);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glMatrixMode((GLenum)0x1700u + 99u);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glPushMatrix();
    glPopMatrix();
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glMatrixMode(GL_MODELVIEW);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* **The honesty badge, on a build with no GPU in it.**
 *
 * `glIsHardwareAccelerated()` is the one function here whose whole job is to not overclaim,
 * and it has had no test at all. The badge has lied once already - it used to report the code
 * path it took rather than a measurement - and the fix is only as good as something watching
 * it. A host build has no hardware, so the answer must be GL_FALSE, and the status struct must
 * agree with it rather than telling a different story.
 *
 * This test would also fail on target if the badge ever went back to reporting intent: there
 * is no path by which a host build can confirm a frame, so `frames_confirmed` is the thing
 * that cannot be faked. */
static void test_gl_hardware_badge_does_not_overclaim(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);

    ASSERT_EQ(glIsHardwareAccelerated(), GL_FALSE);

    gl_hw_status_t st;
    memset(&st, 0xcd, sizeof(st));
    glGetHardwareStatus(&st);
    /* The two must not disagree: a struct saying "verified" beside a badge saying false would
     * be the same overclaim wearing the other hat. */
    ASSERT_EQ(st.verified, glIsHardwareAccelerated());
    ASSERT_EQ(st.frames_confirmed, 0u);
    ASSERT_NE(st.fence, 0xbeefcafeu);

    /* The readback is NULL before any submission, rather than a stale or zeroed buffer that a
     * caller would hash and record as a frame. The sampled form answers the same way, including
     * for a stride of 0 - which it reads as 1 rather than dividing by it. */
    ASSERT_TRUE(glGetFrameReadback() == NULL);
    ASSERT_TRUE(glGetFrameReadbackSampled(1u) == NULL);
    ASSERT_TRUE(glGetFrameReadbackSampled(4u) == NULL);
    ASSERT_TRUE(glGetFrameReadbackSampled(0u) == NULL);

    /* The shader canaries report nothing ran, and the Ex form agrees with the short one. */
    GLuint vs = 0xffffffffu, ps = 0xffffffffu, vs0 = 1u, ps0 = 1u;
    glGetCanary(&vs, &ps);
    GLuint vs2 = 0u, ps2 = 0u;
    glGetCanaryEx(&vs2, &ps2, &vs0, &ps0);
    ASSERT_EQ(vs, vs2);
    ASSERT_EQ(ps, ps2);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* The matrix builders, against their definitions rather than against themselves.
 *
 * glFrustum, glLoadMatrixf, glMultMatrixf and glScalef were reachable and untested. Each is
 * arithmetic that is silently wrong when it is wrong - geometry still appears, in the wrong
 * place - so the values are checked against what the specification says they should be. */
/*
 * GLU's image and coordinate functions - what a port calls that GL itself does not provide.
 *
 * The mipmap check is the one with teeth: a chain built by sampling the *original* at each level
 * looks right at level 1 and wrong further down, where a level must be the average of everything
 * above it. A 4x4 of known values makes that visible - the 1x1 level is the mean of all sixteen
 * texels only if each level was built from the one above.
 */
static void test_gl_glu_scales_projects_and_builds_mipmaps(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);

    /* **gluScaleImage**, halving: each output pixel is the average of its 2x2 footprint. */
    static const GLubyte in4[4 * 4 * 4] = {
        0,0,0,255,      40,0,0,255,     80,0,0,255,     120,0,0,255,
        4,0,0,255,      44,0,0,255,     84,0,0,255,     124,0,0,255,
        8,0,0,255,      48,0,0,255,     88,0,0,255,     128,0,0,255,
        12,0,0,255,     52,0,0,255,     92,0,0,255,     132,0,0,255,
    };
    GLubyte out2[2 * 2 * 4];
    ASSERT_EQ(gluScaleImage(GL_RGBA, 4, 4, GL_UNSIGNED_BYTE, in4, 2, 2, GL_UNSIGNED_BYTE, out2), 0);
    /* Top-left output covers (0,0), (40,0), (4,0), (44,0): mean 22. */
    ASSERT_TRUE(out2[0] >= 21 && out2[0] <= 23);
    ASSERT_TRUE(out2[4] >= 101 && out2[4] <= 103); /* 80, 120, 84, 124 -> 102 */
    ASSERT_EQ(out2[3], 255);

    /* Magnification replicates rather than interpolating, as GLU's box filter does. */
    GLubyte out8[8 * 1 * 4];
    ASSERT_EQ(gluScaleImage(GL_RGBA, 4, 1, GL_UNSIGNED_BYTE, in4, 8, 1, GL_UNSIGNED_BYTE, out8), 0);
    ASSERT_EQ(out8[0], 0);
    ASSERT_EQ(out8[4], 0);
    ASSERT_EQ(out8[8], 40);

    /* Its errors are GLU's, and nothing reaches glGetError. */
    (void)glGetError();
    ASSERT_EQ(gluScaleImage(GL_RGBA, 0, 4, GL_UNSIGNED_BYTE, in4, 2, 2, GL_UNSIGNED_BYTE, out2),
              GLU_INVALID_VALUE);
    ASSERT_EQ(gluScaleImage(GL_RGBA, 4, 4, GL_BITMAP, in4, 2, 2, GL_UNSIGNED_BYTE, out2),
              GLU_INVALID_ENUM);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* **gluBuild2DMipmaps**: every level down to 1x1, each the average of the one above. */
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    ASSERT_EQ(gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, in4), 0);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    ASSERT_EQ(w, 4);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_HEIGHT, &h);
    ASSERT_EQ(w, 2);
    ASSERT_EQ(h, 2);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &w);
    ASSERT_EQ(w, 1);
    GLubyte back[4];
    glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, back);
    /* The mean of all sixteen reds is 66. Sampling the original at 1x1 would give one corner. */
    ASSERT_TRUE(back[0] >= 65 && back[0] <= 67);

    /* The caller's unpack state is put back, not left as the builder needed it - and level zero
     * is read *through* that state, so the source has to be the image that state describes. A row
     * length of 7 with an alignment of 8 puts the rows 32 bytes apart, which is more than the
     * 16 bytes a row of `in4` occupies: the 4x4 window goes in a staging buffer of that stride,
     * with the columns past it filled with a red the image does not contain. Passing `in4` here
     * would have the builder read 112 bytes out of 64. */
    GLubyte wide[4 * 32];
    memset(wide, 0xFF, sizeof(wide));
    for (int wy = 0; wy < 4; wy++) memcpy(wide + (size_t)wy * 32, in4 + (size_t)wy * 16, 16);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
    ASSERT_EQ(gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, wide), 0);
    GLint v = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &v);
    ASSERT_EQ(v, 8);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &v);
    ASSERT_EQ(v, 7);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    /* The same sixteen reds as before, so the same 1x1 level: the stride was honoured and the
     * padding columns never averaged in. Reading them would give 158, not 66. */
    glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, back);
    ASSERT_TRUE(back[0] >= 65 && back[0] <= 67);

    /* **gluProject and gluUnProject** are each other's inverse, through a projection that is not
     * the identity - an orthographic one would hide a mistake in the w divide. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, 4.0 / 3.0, 1.0, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -10.0f);
    GLdouble mv[16], pr[16];
    GLint vp[4] = {0, 0, 320, 240};
    glGetDoublev(GL_MODELVIEW_MATRIX, mv);
    glGetDoublev(GL_PROJECTION_MATRIX, pr);
    GLdouble wx = 0.0, wy = 0.0, wz = 0.0;
    ASSERT_EQ(gluProject(1.0, 2.0, -3.0, mv, pr, vp, &wx, &wy, &wz), GL_TRUE);
    /* In front of the eye and inside the viewport, right of centre and above it. */
    ASSERT_TRUE(wz > 0.0 && wz < 1.0);
    ASSERT_TRUE(wx > 160.0 && wx < 320.0);
    ASSERT_TRUE(wy > 120.0 && wy < 240.0);
    GLdouble ox = 0.0, oy = 0.0, oz = 0.0;
    ASSERT_EQ(gluUnProject(wx, wy, wz, mv, pr, vp, &ox, &oy, &oz), GL_TRUE);
    ASSERT_TRUE(ox > 0.999 && ox < 1.001);
    ASSERT_TRUE(oy > 1.999 && oy < 2.001);
    ASSERT_TRUE(oz > -3.001 && oz < -2.999);

    /* A point on the eye plane has no window position, and says so rather than dividing by 0.
     * The modelview translates by -10, so that plane is object z = +10, not -10. */
    ASSERT_EQ(gluProject(0.0, 0.0, 10.0, mv, pr, vp, &wx, &wy, &wz), GL_FALSE);

    ASSERT_TRUE(gluGetString(GLU_VERSION) != (const GLubyte *)0);
    ASSERT_TRUE(gluGetString(GL_RGBA) == (const GLubyte *)0);

    glDeleteTextures(1, &tex);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/*
 * The GLUT shim's main loop, which is the part a port depends on and the part that cannot be
 * checked by reading it: that a reshape arrives before the first display, that display runs only
 * when one has been posted and idle runs otherwise, that a timer fires, and that the loop can be
 * left - a console program whose only exit is a window close has none otherwise.
 */
static int s_glut_display, s_glut_idle, s_glut_reshape, s_glut_timer;
static int s_glut_reshape_w, s_glut_reshape_h, s_glut_reshape_first;

static void glut_test_display(void) {
    if (!s_glut_display && !s_glut_reshape) s_glut_reshape_first = 0; /* display came first */
    s_glut_display++;
    if (s_glut_display >= 2) glutLeaveMainLoop();
}
static void glut_test_idle(void) {
    s_glut_idle++;
    /* The second pass has nothing posted, so this runs; post once from here to prove the loop
     * goes back to the display callback, and leave from there. */
    if (s_glut_idle == 1) glutPostRedisplay();
    if (s_glut_idle > 8) glutLeaveMainLoop(); /* a guard, not the expected way out */
}
static void glut_test_reshape(int w, int h) {
    s_glut_reshape++;
    s_glut_reshape_w = w;
    s_glut_reshape_h = h;
}
static void glut_test_timer(int value) { s_glut_timer += value; }

static void test_gl_glut_main_loop_dispatches_and_can_be_left(void) {
    s_glut_display = s_glut_idle = s_glut_reshape = s_glut_timer = 0;
    s_glut_reshape_first = 1;
    glutInit((int *)0, (char **)0);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(320, 240);
    glutInitWindowPosition(17, 23); /* accepted and ignored: one window */
    ASSERT_EQ(glutCreateWindow("test"), 1);
    ASSERT_EQ(glutGet(GLUT_WINDOW_WIDTH), 320);
    ASSERT_EQ(glutGet(GLUT_WINDOW_HEIGHT), 240);
    ASSERT_EQ(glutGet(GLUT_WINDOW_RGBA), 1);
    ASSERT_EQ(glutGet(GLUT_DISPLAY_MODE_POSSIBLE), 1);

    glutDisplayFunc(glut_test_display);
    glutIdleFunc(glut_test_idle);
    glutReshapeFunc(glut_test_reshape);
    glutTimerFunc(0u, glut_test_timer, 7);
    glutMainLoop();

    /* It returned, which is the point of glutLeaveMainLoop. */
    ASSERT_TRUE(s_glut_reshape_first);
    ASSERT_EQ(s_glut_reshape, 1);
    ASSERT_EQ(s_glut_reshape_w, 320);
    ASSERT_EQ(s_glut_reshape_h, 240);
    /* Twice: once for the redisplay the loop posts itself at the start, once for the one the
     * idle callback posted. Idle ran in between, which is the alternation GLUT promises. */
    ASSERT_EQ(s_glut_display, 2);
    ASSERT_TRUE(s_glut_idle >= 1);
    ASSERT_EQ(s_glut_timer, 7); /* due immediately, fired once, not repeatedly */

    /* Colour index is the one mode this cannot give, and says so rather than drawing nothing. */
    glutInitDisplayMode(GLUT_INDEX);
    ASSERT_EQ(glutGet(GLUT_DISPLAY_MODE_POSSIBLE), 0);
    glutDestroyWindow(1);
}

/*
 * The quadrics, checked through feedback - which reports the vertices a primitive was assembled
 * from, so the geometry can be read back without a framebuffer.
 *
 * Two things are pinned here because a port cannot see either of them go wrong in its own source:
 * the **texture convention** (s = 0 at +y, 0.25 at +x - a sphere whose s runs the other way looks
 * plausible until the label on it reads backwards) and the **winding** (GLU_OUTSIDE must come out
 * counter-clockwise seen from outside, or the whole surface vanishes under the default cull).
 */
static void test_gl_glu_quadrics_wind_and_texture_as_the_spec_says(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
    void *ctx = glContextCreate(disp);
    GLUquadric *q = gluNewQuadric();
    ASSERT_TRUE(q != (GLUquadric *)0);

    /* Feedback reports **window** coordinates, so the transform is set up to be inverted exactly:
     * with this projection and viewport, object (x, y, z) arrives as (x + 1, y + 1, (1 - z) / 2),
     * and GLU_OBJ_* below takes it back. A round trip through a positive x and y scale and a
     * doubly negated z keeps the handedness, so the winding test below still means what it says. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, 2, 2);
    /* GL_4D_COLOR_TEXTURE reports x, y, z, w, four colours and four texture coordinates: twelve
     * floats a vertex, with s at 8 and t at 9. */
    #define GLU_FB_V 12
    #define GLU_OBJ_X(p) ((p)[0] - 1.0f)
    #define GLU_OBJ_Y(p) ((p)[1] - 1.0f)
    #define GLU_OBJ_Z(p) (1.0f - 2.0f * (p)[2])

    /* A sphere of radius 0.5 - inside the view volume, so nothing is clipped away. Its vertices
     * sit on that sphere. With four slices the ring vertices land on the axes, where the
     * specification names s. */
    gluQuadricTexture(q, GL_TRUE);
    static GLfloat fb[8192];
    glFeedbackBuffer(8192, GL_4D_COLOR_TEXTURE, fb);
    glRenderMode(GL_FEEDBACK);
    gluSphere(q, 0.5, 4, 2);
    GLint n = glRenderMode(GL_RENDER);
    ASSERT_TRUE(n > 0);

    /* GL_4D_COLOR_TEXTURE: a token, then per vertex x, y, z, w, four colours, four texture
     * coordinates - 13 floats a vertex. Walk the buffer and check every vertex. */
    int checked = 0, at_plus_y = 0, at_plus_x = 0;
    for (GLint i = 0; i < n;) {
        const GLfloat tok = fb[i];
        int verts = 0;
        if (tok == GL_POLYGON_TOKEN) {
            verts = (int)fb[i + 1];
            i += 2;
        } else if (tok == GL_LINE_TOKEN || tok == GL_LINE_RESET_TOKEN) {
            verts = 2;
            i += 1;
        } else if (tok == GL_POINT_TOKEN) {
            verts = 1;
            i += 1;
        } else {
            break;
        }
        for (int v = 0; v < verts && i + GLU_FB_V <= n; v++, i += GLU_FB_V) {
            const GLfloat x = GLU_OBJ_X(&fb[i]), y = GLU_OBJ_Y(&fb[i]), z = GLU_OBJ_Z(&fb[i]);
            const GLfloat s = fb[i + 8], t = fb[i + 9];
            const GLfloat len = (GLfloat)((double)x * x + (double)y * y + (double)z * z);
            ASSERT_TRUE(len > 0.24f && len < 0.26f); /* on the sphere of radius 0.5 */
            ASSERT_TRUE(s >= -0.001f && s <= 1.001f);
            ASSERT_TRUE(t >= -0.001f && t <= 1.001f);
            /* s = 0 at +y and 0.25 at +x, on the equator (z = 0 with two stacks). */
            if (z > -0.01f && z < 0.01f) {
                if (y > 0.49f && s < 0.01f) at_plus_y = 1;
                if (x > 0.49f && s > 0.24f && s < 0.26f) at_plus_x = 1;
            }
            checked++;
        }
    }
    ASSERT_TRUE(checked > 8);
    ASSERT_TRUE(at_plus_y);
    ASSERT_TRUE(at_plus_x);

    /* **Winding.** A GLU_OUTSIDE sphere's triangles face outwards: the cross product of a
     * polygon's first two edges points the same way as the polygon's own position. Under the
     * default GL_CCW front face that is what keeps it on screen when culling is on. */
    glRenderMode(GL_FEEDBACK);
    gluSphere(q, 0.5, 8, 4);
    n = glRenderMode(GL_RENDER);
    int polys = 0, outward = 0;
    for (GLint i = 0; i < n;) {
        if (fb[i] != GL_POLYGON_TOKEN) { i++; continue; }
        const int verts = (int)fb[i + 1];
        const GLfloat *v = &fb[i + 2];
        if (verts >= 3 && i + 2 + verts * GLU_FB_V <= n) {
            const GLfloat p0[3] = {GLU_OBJ_X(v), GLU_OBJ_Y(v), GLU_OBJ_Z(v)};
            const GLfloat *v1 = v + GLU_FB_V, *v2 = v + 2 * GLU_FB_V;
            const GLfloat p1[3] = {GLU_OBJ_X(v1), GLU_OBJ_Y(v1), GLU_OBJ_Z(v1)};
            const GLfloat p2[3] = {GLU_OBJ_X(v2), GLU_OBJ_Y(v2), GLU_OBJ_Z(v2)};
            const GLfloat ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
            const GLfloat bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
            const GLfloat cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
            /* The face's own position stands in for its outward direction, the sphere being at
             * the origin. */
            if (cx * p0[0] + cy * p0[1] + cz * p0[2] > 0.0f) outward++;
            polys++;
        }
        i += 2 + verts * GLU_FB_V;
    }
    ASSERT_TRUE(polys > 8);
    ASSERT_EQ(outward, polys);

    /* GLU_INSIDE turns them all round - the same test, the other sign. */
    gluQuadricOrientation(q, GLU_INSIDE);
    glRenderMode(GL_FEEDBACK);
    gluSphere(q, 0.5, 8, 4);
    n = glRenderMode(GL_RENDER);
    int inward = 0;
    polys = 0;
    for (GLint i = 0; i < n;) {
        if (fb[i] != GL_POLYGON_TOKEN) { i++; continue; }
        const int verts = (int)fb[i + 1];
        const GLfloat *v = &fb[i + 2];
        if (verts >= 3 && i + 2 + verts * GLU_FB_V <= n) {
            const GLfloat p0[3] = {GLU_OBJ_X(v), GLU_OBJ_Y(v), GLU_OBJ_Z(v)};
            const GLfloat *v1 = v + GLU_FB_V, *v2 = v + 2 * GLU_FB_V;
            const GLfloat p1[3] = {GLU_OBJ_X(v1), GLU_OBJ_Y(v1), GLU_OBJ_Z(v1)};
            const GLfloat p2[3] = {GLU_OBJ_X(v2), GLU_OBJ_Y(v2), GLU_OBJ_Z(v2)};
            const GLfloat ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
            const GLfloat bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
            const GLfloat cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
            if (cx * p0[0] + cy * p0[1] + cz * p0[2] < 0.0f) inward++;
            polys++;
        }
        i += 2 + verts * GLU_FB_V;
    }
    ASSERT_TRUE(polys > 8);
    ASSERT_EQ(inward, polys);
    gluQuadricOrientation(q, GLU_OUTSIDE);

    /* A cone's normals lean with its slope: a cylinder of base 1 and top 0 over height 1 has
     * nz = 1/sqrt(2), not 0. Read the normal back through lighting-free feedback by checking the
     * vertices instead: the side runs from radius 1 at z = 0 to a point at z = 1. */
    glRenderMode(GL_FEEDBACK);
    gluCylinder(q, 0.5, 0.0, 0.5, 6, 1);
    n = glRenderMode(GL_RENDER);
    int saw_base = 0, saw_apex = 0;
    for (GLint i = 0; i < n;) {
        if (fb[i] != GL_POLYGON_TOKEN) { i++; continue; }
        const int verts = (int)fb[i + 1];
        for (int v = 0; v < verts && i + 2 + (v + 1) * GLU_FB_V <= n; v++) {
            const GLfloat *p = &fb[i + 2 + v * GLU_FB_V];
            const GLfloat x = GLU_OBJ_X(p), y = GLU_OBJ_Y(p), z = GLU_OBJ_Z(p);
            const GLfloat r2 = x * x + y * y;
            if (z > -0.01f && z < 0.01f && r2 > 0.24f && r2 < 0.26f) saw_base = 1;
            if (z > 0.49f && z < 0.51f && r2 < 0.01f) saw_apex = 1;
        }
        i += 2 + verts * GLU_FB_V;
    }
    ASSERT_TRUE(saw_base && saw_apex);

    /* A disk's texture coordinates: (1, 0.5) at (outer, 0, 0), (0.5, 1) at (0, outer, 0). */
    glRenderMode(GL_FEEDBACK);
    gluDisk(q, 0.0, 0.5, 4, 1);
    n = glRenderMode(GL_RENDER);
    int at_x = 0, at_y = 0;
    for (GLint i = 0; i < n;) {
        if (fb[i] != GL_POLYGON_TOKEN) { i++; continue; }
        const int verts = (int)fb[i + 1];
        for (int v = 0; v < verts && i + 2 + (v + 1) * GLU_FB_V <= n; v++) {
            const GLfloat *p = &fb[i + 2 + v * GLU_FB_V];
            const GLfloat x = GLU_OBJ_X(p), y = GLU_OBJ_Y(p);
            if (x > 0.49f && y > -0.01f && y < 0.01f) {
                if (p[8] > 0.99f && p[9] > 0.49f && p[9] < 0.51f) at_x = 1;
            }
            if (y > 0.49f && x > -0.01f && x < 0.01f) {
                if (p[9] > 0.99f && p[8] > 0.49f && p[8] < 0.51f) at_y = 1;
            }
        }
        i += 2 + verts * GLU_FB_V;
    }
    ASSERT_TRUE(at_x && at_y);

    /* Bad arguments reach the error callback, and nothing is recorded in glGetError. */
    (void)glGetError();
    gluSphere(q, -1.0, 4, 2);
    gluDisk(q, 3.0, 1.0, 4, 1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    gluDeleteQuadric(q);
    #undef GLU_FB_V
    #undef GLU_OBJ_X
    #undef GLU_OBJ_Y
    #undef GLU_OBJ_Z
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/*
 * **The Platonic solids are the solids they claim**, read back through feedback.
 *
 * `glut.c` derives their faces from their vertices rather than carrying a table, which is what
 * keeps them clean-room - so what needs checking is the derivation, not a transcription. Five
 * things, and between them nothing can be wrong and look right:
 *
 *  - the face count and the vertices a face has: 4 triangles, 8, 20, and 12 pentagons;
 *  - every vertex at the radius GLUT's manual documents - 1 for the octahedron and the
 *    icosahedron, sqrt(3) for the tetrahedron and the dodecahedron;
 *  - **Euler's formula**, V - E + F = 2, with V counted from the distinct vertices that came
 *    back and E from the faces. A derivation that found one face twice, or missed one, or built
 *    a pentagon out of the wrong five vertices, fails here;
 *  - **the winding**, counter-clockwise seen from outside, or the whole solid vanishes under the
 *    default cull - the same trap the GLU quadrics have;
 *  - **the pentagons are flat**, which says the five vertices really are one face of the
 *    dodecahedron and not five that merely happen to be furthest along a direction.
 */
static void test_gl_platonic_solids_are_the_solids_they_claim(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
    void *ctx = glContextCreate(disp);

    /* The same invertible transform the quadric test uses, at twice the size: these solids reach
     * out to sqrt(3), and a vertex outside the view volume would be clipped rather than
     * reported. Object (x, y, z) arrives as (x/2 + 1, y/2 + 1, (1 - z/2) / 2). */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-2.0, 2.0, -2.0, 2.0, -2.0, 2.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, 2, 2);
    #define PLA_V 12
    #define PLA_X(p) (2.0f * ((p)[0] - 1.0f))
    #define PLA_Y(p) (2.0f * ((p)[1] - 1.0f))
    #define PLA_Z(p) (2.0f - 4.0f * (p)[2])

    static const struct {
        void (*draw)(void);
        int faces;
        int face_verts;
        float radius2;
        const char *name;
    } solids[4] = {
        {glutSolidTetrahedron, 4, 3, 3.0f, "tetrahedron"},
        {glutSolidOctahedron, 8, 3, 1.0f, "octahedron"},
        {glutSolidIcosahedron, 20, 3, 1.0f, "icosahedron"},
        {glutSolidDodecahedron, 12, 5, 3.0f, "dodecahedron"},
    };

    static GLfloat fb[16384];
    for (int s = 0; s < 4; s++) {
        glFeedbackBuffer(16384, GL_4D_COLOR_TEXTURE, fb);
        glRenderMode(GL_FEEDBACK);
        solids[s].draw();
        const GLint n = glRenderMode(GL_RENDER);
        ASSERT_TRUE(n > 0);

        float uniq[32][3];
        int edge[64][2];
        int nuniq = 0, faces = 0, nedge = 0;
        for (GLint i = 0; i < n;) {
            ASSERT_EQ(fb[i], (GLfloat)GL_POLYGON_TOKEN);
            const int verts = (int)fb[i + 1];
            i += 2;
            /* A polygon comes back as the triangles it was assembled from, so a pentagon is
             * three of them: the count checked is the face's, recovered below. */
            ASSERT_EQ(verts, 3);
            float p[3][3];
            int id[3];
            for (int v = 0; v < 3; v++, i += PLA_V) {
                ASSERT_TRUE(i + PLA_V <= n);
                p[v][0] = PLA_X(&fb[i]);
                p[v][1] = PLA_Y(&fb[i]);
                p[v][2] = PLA_Z(&fb[i]);
                const float r2 = p[v][0] * p[v][0] + p[v][1] * p[v][1] + p[v][2] * p[v][2];
                ASSERT_TRUE(r2 > solids[s].radius2 - 0.01f && r2 < solids[s].radius2 + 0.01f);
                int seen = -1;
                for (int u = 0; u < nuniq; u++) {
                    const float dx = uniq[u][0] - p[v][0], dy = uniq[u][1] - p[v][1],
                                dz = uniq[u][2] - p[v][2];
                    if (dx * dx + dy * dy + dz * dz < 1e-4f) { seen = u; break; }
                }
                if (seen < 0 && nuniq < 32) {
                    uniq[nuniq][0] = p[v][0];
                    uniq[nuniq][1] = p[v][1];
                    uniq[nuniq][2] = p[v][2];
                    seen = nuniq++;
                }
                id[v] = seen;
            }
            /* Every distinct undirected edge of every triangle. A fanned pentagon adds two
             * diagonals that are not edges of the solid, which the count below takes back. */
            for (int e = 0; e < 3; e++) {
                const int a = id[e], b = id[(e + 1) % 3];
                int known = 0;
                for (int k = 0; k < nedge; k++) {
                    if ((edge[k][0] == a && edge[k][1] == b) ||
                        (edge[k][0] == b && edge[k][1] == a)) { known = 1; break; }
                }
                if (!known && nedge < 64) {
                    edge[nedge][0] = a;
                    edge[nedge][1] = b;
                    nedge++;
                }
            }
            /* Counter-clockwise seen from outside: the triangle's own normal agrees with where
             * it sits. Every triangle of a fanned pentagon inherits the face's winding. */
            const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            const float nx = e1[1] * e2[2] - e1[2] * e2[1];
            const float ny = e1[2] * e2[0] - e1[0] * e2[2];
            const float nz = e1[0] * e2[1] - e1[1] * e2[0];
            ASSERT_TRUE(nx * p[0][0] + ny * p[0][1] + nz * p[0][2] > 0.0f);
            faces++;
        }
        /* A pentagon arrives as three triangles, a triangle as one. */
        ASSERT_EQ(faces, solids[s].faces * (solids[s].face_verts - 2));
        /* **Euler's formula on the edges that came back**, not on a count assumed from the face
         * table: V - E + F = 2, with E the distinct edges of the triangles less the two
         * diagonals each fanned pentagon contributed. A derivation that built a face out of the
         * wrong vertices fails here as well as on the planarity check below. */
        const int E = nedge - solids[s].faces * (solids[s].face_verts - 3);
        ASSERT_EQ(E, solids[s].faces * solids[s].face_verts / 2);
        ASSERT_EQ(nuniq - E + solids[s].faces, 2);
    }

    /* **The pentagons are flat.** Each of the dodecahedron's faces arrives as three triangles
     * that must share one plane; a face built from the wrong five vertices would not. Checked on
     * the first face, whose three triangles are the first three polygons back. */
    glFeedbackBuffer(16384, GL_4D_COLOR_TEXTURE, fb);
    glRenderMode(GL_FEEDBACK);
    glutSolidDodecahedron();
    {
        const GLint n = glRenderMode(GL_RENDER);
        ASSERT_TRUE(n > 0);
        float plane[3] = {0.0f, 0.0f, 0.0f}, d0 = 0.0f;
        int tri = 0;
        for (GLint i = 0; i < n && tri < 3;) {
            i += 2;
            float p[3][3];
            for (int v = 0; v < 3; v++, i += PLA_V) {
                p[v][0] = PLA_X(&fb[i]);
                p[v][1] = PLA_Y(&fb[i]);
                p[v][2] = PLA_Z(&fb[i]);
            }
            if (tri == 0) {
                const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
                const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                plane[0] = e1[1] * e2[2] - e1[2] * e2[1];
                plane[1] = e1[2] * e2[0] - e1[0] * e2[2];
                plane[2] = e1[0] * e2[1] - e1[1] * e2[0];
                const float len = (float)((double)plane[0] * plane[0] +
                                          (double)plane[1] * plane[1] +
                                          (double)plane[2] * plane[2]);
                ASSERT_TRUE(len > 1e-6f);
                d0 = plane[0] * p[0][0] + plane[1] * p[0][1] + plane[2] * p[0][2];
            }
            for (int v = 0; v < 3; v++) {
                const float d = plane[0] * p[v][0] + plane[1] * p[v][1] + plane[2] * p[v][2];
                ASSERT_TRUE(d - d0 < 0.01f && d0 - d < 0.01f);
            }
            tri++;
        }
        ASSERT_EQ(tri, 3);
    }

    #undef PLA_V
    #undef PLA_X
    #undef PLA_Y
    #undef PLA_Z
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/*
 * **The bitmap font draws the letters it is a picture of.**
 *
 * `glut_font.c` writes its glyphs as rows of `#` and `.` in source order and turns them into
 * bitmap bytes - which means the conversion is the part that can be wrong, and it can be wrong
 * in ways that still look like a font: rows upside down (a bitmap's first row is its bottom
 * one), bits mirrored, the baseline in the wrong place so every descender sits on the line.
 *
 * So this checks the picture. 'F' is the glyph to check it with: it is asymmetric both ways, so
 * a vertical flip and a horizontal mirror each break it differently. Then the baseline, through
 * a letter with a descender and one without; then the advance; then that every printable
 * character has ink and the space has none, which catches an off-by-one in the table index.
 */
static void test_gl_bitmap_font_draws_what_it_is_a_picture_of(void) {
    enum { FW = 64, FH = 32 };
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, FW, FH);
    void *ctx = glContextCreate(disp);
    uint32_t *fb = oops_display_get_framebuffer(disp);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)FW, 0.0, (GLdouble)FH, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glViewport(0, 0, FW, FH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    /* The framebuffer's row 0 is the top; GL's y counts up from the bottom. */
    #define FONT_PIX(x, y) (fb[(size_t)(FH - 1 - (y)) * FW + (size_t)(x)] & 0x00ffffffu)

    ASSERT_EQ(glutBitmapWidth(GLUT_BITMAP_8_BY_13, 'W'), 8);
    ASSERT_EQ(glutBitmapWidth(GLUT_BITMAP_9_BY_15, 'i'), 9);
    ASSERT_EQ(glutBitmapHeight(GLUT_BITMAP_8_BY_13), 13);
    ASSERT_EQ(glutBitmapHeight(GLUT_BITMAP_9_BY_15), 15);
    ASSERT_EQ(glutBitmapLength(GLUT_BITMAP_8_BY_13, (const unsigned char *)"abc"), 24);
    ASSERT_EQ(glutBitmapLength(GLUT_BITMAP_9_BY_15, (const unsigned char *)""), 0);

    /* 'F': five columns wide, seven rows tall, with the crossbar on the fourth row from the top
     * and nothing below the baseline. Drawn with the baseline at y = 8. */
    glClear(GL_COLOR_BUFFER_BIT);
    glRasterPos2i(4, 8);
    glutBitmapCharacter(GLUT_BITMAP_8_BY_13, 'F');
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* The top row of 'F' is its full bar: five pixels, six rows above the baseline. */
    for (int x = 0; x < 5; x++) ASSERT_EQ(FONT_PIX(4 + x, 8 + 6), 0x00ffffffu);
    /* The crossbar, three rows down from the top, is four wide - so the fifth column is clear,
     * which is what a horizontal mirror would fill. */
    for (int x = 0; x < 4; x++) ASSERT_EQ(FONT_PIX(4 + x, 8 + 3), 0x00ffffffu);
    ASSERT_EQ(FONT_PIX(4 + 4, 8 + 3), 0x00000000u);
    /* The bottom row is the stem alone: one pixel, on the baseline. A vertical flip would put
     * the full bar here instead. */
    ASSERT_EQ(FONT_PIX(4, 8), 0x00ffffffu);
    ASSERT_EQ(FONT_PIX(4 + 1, 8), 0x00000000u);
    ASSERT_EQ(FONT_PIX(4 + 4, 8), 0x00000000u);
    /* And nothing below the baseline. */
    for (int x = 0; x < 8; x++) {
        ASSERT_EQ(FONT_PIX(4 + x, 7), 0x00000000u);
        ASSERT_EQ(FONT_PIX(4 + x, 6), 0x00000000u);
    }

    /* **A descender goes below the baseline**, which is the whole of what the bitmap's origin
     * decides - a 'g' level with an 'F' means yorig is 0. */
    glClear(GL_COLOR_BUFFER_BIT);
    glRasterPos2i(4, 8);
    glutBitmapCharacter(GLUT_BITMAP_8_BY_13, 'g');
    {
        int below = 0;
        for (int x = 0; x < 8; x++) {
            for (int y = 6; y < 8; y++) if (FONT_PIX(4 + x, y) != 0u) below++;
        }
        ASSERT_TRUE(below > 0);
    }

    /* **The advance**: two characters, the second eight pixels along, and the raster position
     * left where the next one goes. */
    glClear(GL_COLOR_BUFFER_BIT);
    glRasterPos2i(2, 8);
    glutBitmapString(GLUT_BITMAP_8_BY_13, (const unsigned char *)"II");
    {
        GLfloat rp[4];
        glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
        ASSERT_TRUE(rp[0] == 18.0f);
        /* 'I' is a serifed bar: its top row is three pixels starting one column in. */
        for (int x = 0; x < 3; x++) {
            ASSERT_EQ(FONT_PIX(2 + 1 + x, 8 + 6), 0x00ffffffu);
            ASSERT_EQ(FONT_PIX(2 + 8 + 1 + x, 8 + 6), 0x00ffffffu);
        }
        ASSERT_EQ(FONT_PIX(2 + 5, 8 + 6), 0x00000000u); /* the gap between them */
    }

    /* **Every printable character has ink, and the space has none.** A table indexed one entry
     * out draws the wrong letter everywhere, which reads as a font until you try to read it;
     * this catches the ends of the range instead. */
    for (int c = 32; c <= 126; c++) {
        glClear(GL_COLOR_BUFFER_BIT);
        glRasterPos2i(4, 8);
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, c);
        int lit = 0;
        for (int i = 0; i < FW * FH; i++) if ((fb[i] & 0x00ffffffu) != 0u) lit++;
        if (c == ' ') {
            ASSERT_EQ(lit, 0);
        } else {
            ASSERT_TRUE(lit > 0);
        }
    }
    /* Outside the range: nothing drawn, and the position still moves - so a string with a stray
     * byte stays aligned. */
    glClear(GL_COLOR_BUFFER_BIT);
    glRasterPos2i(4, 8);
    glutBitmapCharacter(GLUT_BITMAP_8_BY_13, 1);
    {
        int lit = 0;
        for (int i = 0; i < FW * FH; i++) if ((fb[i] & 0x00ffffffu) != 0u) lit++;
        ASSERT_EQ(lit, 0);
        GLfloat rp[4];
        glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
        ASSERT_TRUE(rp[0] == 12.0f);
    }

    /* A font this does not have draws nothing and moves nothing, rather than picking one. */
    ASSERT_EQ(glutBitmapWidth((void *)0, 'A'), 0);
    ASSERT_EQ(glutBitmapHeight((void *)0), 0);

    #undef FONT_PIX
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/*
 * **The newest surface, given what a sloppy port gives it.**
 *
 * Everything added on 2026-09-20 - the extended texture targets, the occlusion queries, the
 * bitmap font - was written against code that calls it correctly. A port does not: it passes a
 * depth of zero, deletes an object while it is bound, asks for a character it does not have, and
 * runs out of query names. On a console every one of those is a fault rather than a diagnostic,
 * so what they have to do is refuse.
 */
static void test_gl_the_newest_calls_refuse_rather_than_fault(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
    void *ctx = glContextCreate(disp);
    (void)glGetError();
    GLint iv[4];

    /*
     * **A negative size is an error and a zero size is not.** GL draws that line, and a zero
     * means no image at all - which is how a program releases a level it no longer wants. This
     * library refused zero with GL_INVALID_VALUE until 2026-09-20, which was its one behavioural
     * difference from the specification.
     */
    {
        static const GLubyte vol[2 * 2 * 2 * 4] = {0};
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_3D, t);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, -1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glEnable(GL_TEXTURE_3D);
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), t);
        /* **The release**: accepted, and the level is gone, so the texture is incomplete and a
         * draw with it is untextured rather than one that reads storage nothing allocated. */
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH, iv);
        ASSERT_EQ(iv[0], 0);
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), 0u);
        glBegin(GL_TRIANGLES);
        glTexCoord3f(0.5f, 0.5f, 0.5f);
        glVertex3f(-0.5f, -0.5f, 0.0f); glVertex3f(0.5f, -0.5f, 0.0f); glVertex3f(0.0f, 0.5f, 0.0f);
        glEnd();
        /* And it can be given an image again afterwards. */
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), t);
        glDisable(GL_TEXTURE_3D);
        glDeleteTextures(1, &t);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* The same for a 2D texture and for one mip level of it, which is the shape a loader that
     * nulls levels out actually uses. */
    {
        static const GLubyte px[4 * 4 * 4] = {0};
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glEnable(GL_TEXTURE_2D);
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), t); /* chain complete */
        glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        /* Level 1 gone: a filter that reads the chain no longer has one, so the texture is
         * incomplete - which is GL's own answer and not this library's invention. */
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), 0u);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), t); /* base level alone is enough */
        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &t);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* **A cube map whose faces disagree** is incomplete, which GL draws untextured rather than
     * refusing - and which the upload must not try to build an array out of. */
    {
        static const GLubyte px[4 * 4 * 4] = {0};
        GLuint c = 0;
        glGenTextures(1, &c);
        glBindTexture(GL_TEXTURE_CUBE_MAP, c);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        for (int f = 0; f < 6; f++) {
            const GLsizei d = (f == 3) ? 4 : 2; /* one face the wrong size */
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, 0, GL_RGBA, d, d, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, px);
        }
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glEnable(GL_TEXTURE_CUBE_MAP);
        ASSERT_EQ(gl_effective_texture_id((gl_context_t *)ctx), 0u); /* incomplete: no texture */
        glBegin(GL_TRIANGLES);
        glTexCoord3f(1.0f, 0.0f, 0.0f);
        glVertex3f(-0.5f, -0.5f, 0.0f); glVertex3f(0.5f, -0.5f, 0.0f); glVertex3f(0.0f, 0.5f, 0.0f);
        glEnd();
        glDisable(GL_TEXTURE_CUBE_MAP);
        glDeleteTextures(1, &c);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* **The comparison state on a colour texture.** GL 1.4 says the mode applies to a depth
     * texture and is ignored otherwise, so setting it must be accepted and must change nothing
     * about how the colour is read. */
    {
        static const GLubyte one[4] = {10, 20, 30, 255};
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, iv);
        ASSERT_EQ(iv[0], (GLint)GL_COMPARE_R_TO_TEXTURE); /* kept, as state */
        glDeleteTextures(1, &t);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* Occlusion queries: every way a program gets the order wrong. */
    {
        GLuint q = 0, q2 = 0;
        glGenQueries(1, &q);
        glEndQuery(GL_SAMPLES_PASSED); /* no matching begin */
        ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, (GLuint *)iv); /* never begun */
        ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
        glBeginQuery(GL_SAMPLES_PASSED, q);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, (GLuint *)iv); /* still running */
        ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
        glGenQueries(1, &q2);
        glBeginQuery(GL_SAMPLES_PASSED, q2); /* one active query a target */
        ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
        /* **Deleting the active one** has to release it, or nothing can ever begin again. */
        glDeleteQueries(1, &q);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glBeginQuery(GL_SAMPLES_PASSED, q2);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glEndQuery(GL_SAMPLES_PASSED);
        glGetQueryObjectuiv(q2, GL_QUERY_RESULT, (GLuint *)iv);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        glDeleteQueries(1, &q2);
        glDeleteQueries(-1, &q2);
        ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
        glDeleteQueries(1, NULL); /* a null array is nothing to delete, not a fault */
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    /* The font, given bytes it has no glyph for. Nothing is drawn and nothing faults; the
     * position still moves, so a string with a stray byte stays aligned. */
    {
        ASSERT_EQ(glutBitmapWidth(GLUT_BITMAP_8_BY_13, 0), 8);
        ASSERT_EQ(glutBitmapWidth(GLUT_BITMAP_8_BY_13, 255), 8);
        ASSERT_EQ(glutBitmapLength(GLUT_BITMAP_8_BY_13, NULL), 0);
        glutBitmapString(GLUT_BITMAP_8_BY_13, NULL);
        glutBitmapString((void *)0, (const unsigned char *)"x");
        glRasterPos2i(0, 0);
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, 0);
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, 255);
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, -1);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
    }

    glContextDestroy(ctx);
    oops_display_close(disp);
}

static void test_gl_matrix_builders_match_their_definitions(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    GLfloat m[16];

    /* Column-major, as GL stores them. A frustum with l = -r and b = -t has zeroes in the
     * skew slots and the near/far terms in the third column. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-2.0, 2.0, -1.0, 1.0, 1.0, 101.0);
    glGetFloatv(GL_PROJECTION_MATRIX, m);
    ASSERT_FLOAT_NEAR(m[0], 0.5f, 1e-5f);   /* 2n / (r - l) = 2 / 4 */
    ASSERT_FLOAT_NEAR(m[5], 1.0f, 1e-5f);   /* 2n / (t - b) = 2 / 2 */
    ASSERT_FLOAT_NEAR(m[10], -1.02f, 1e-4f); /* -(f + n) / (f - n) = -102 / 100 */
    ASSERT_FLOAT_NEAR(m[11], -1.0f, 1e-5f);
    ASSERT_FLOAT_NEAR(m[14], -2.02f, 1e-4f); /* -2fn / (f - n) = -202 / 100 */
    ASSERT_FLOAT_NEAR(m[15], 0.0f, 1e-5f);

    /* glLoadMatrixf takes the caller's matrix verbatim; glMultMatrixf post-multiplies, so
     * loading identity and multiplying by M must give M back unchanged. */
    static const GLfloat known[16] = {
        1.0f, 2.0f, 3.0f, 0.0f,   4.0f, 5.0f, 6.0f, 0.0f,
        7.0f, 8.0f, 9.0f, 0.0f,  10.0f, 11.0f, 12.0f, 1.0f
    };
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(known);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    for (int i = 0; i < 16; i++) ASSERT_FLOAT_NEAR(m[i], known[i], 1e-5f);

    glLoadIdentity();
    glMultMatrixf(known);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    for (int i = 0; i < 16; i++) ASSERT_FLOAT_NEAR(m[i], known[i], 1e-5f);

    /* glScalef scales the basis vectors, and must leave the translation column alone - which
     * is the half of it a transposed implementation gets wrong. */
    glLoadIdentity();
    glTranslatef(5.0f, 6.0f, 7.0f);
    glScalef(2.0f, 3.0f, 4.0f);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    ASSERT_FLOAT_NEAR(m[0], 2.0f, 1e-5f);
    ASSERT_FLOAT_NEAR(m[5], 3.0f, 1e-5f);
    ASSERT_FLOAT_NEAR(m[10], 4.0f, 1e-5f);
    ASSERT_FLOAT_NEAR(m[12], 5.0f, 1e-5f);
    ASSERT_FLOAT_NEAR(m[13], 6.0f, 1e-5f);
    ASSERT_FLOAT_NEAR(m[14], 7.0f, 1e-5f);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* The vector and packed forms agree with the scalar ones they forward to.
 *
 * Cheap, and it catches the one bug this family has: a transposed or short-by-one index in a
 * forwarder, which produces colours and positions that are plausible and wrong. Each pair is
 * drawn and compared as pixels, because that is the only place the difference shows. */
#define VEC_DIM 64
static uint32_t vec_reference[VEC_DIM * VEC_DIM];

static void test_gl_vector_forms_match_their_scalar_twins(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, VEC_DIM, VEC_DIM);
    void *ctx = glContextCreate(disp);
    uint32_t *fb = oops_display_get_framebuffer(disp);
    const size_t bytes = sizeof(vec_reference);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    /* **Deliberately asymmetric, and compared whole.**
     *
     * The first version of this test used a triangle symmetric about x = y and checked one
     * centre pixel, and it passed against a glVertex3fv with x and y transposed - the swapped
     * triangle still covered the centre in the same flat colour. A forwarder bug is a
     * geometry bug, so the geometry has to be the thing that differs and the whole buffer has
     * to be what is compared. No vertex here shares a coordinate with another. */
    static const GLfloat a[3] = {-0.8f, -0.6f, 0.0f};
    static const GLfloat b[3] = { 0.7f, -0.5f, 0.0f};
    static const GLfloat c[3] = { 0.1f,  0.9f, 0.0f};
    static const GLfloat rgb[3] = {0.25f, 0.5f, 0.75f};

    /* Scalar reference. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(rgb[0], rgb[1], rgb[2]);
    glVertex3f(a[0], a[1], a[2]);
    glVertex3f(b[0], b[1], b[2]);
    glVertex3f(c[0], c[1], c[2]);
    glEnd();
    memcpy(vec_reference, fb, bytes);
    /* The reference must actually contain a triangle, or every comparison below is two empty
     * buffers agreeing with each other. */
    size_t drawn = 0;
    for (size_t i = 0; i < VEC_DIM * VEC_DIM; i++) {
        if (vec_reference[i] != 0xff000000u) drawn++;
    }
    ASSERT_TRUE(drawn > 200);

    /* The vector forms, same values, must produce the same buffer exactly. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3fv(rgb);
    glVertex3fv(a);
    glVertex3fv(b);
    glVertex3fv(c);
    glEnd();
    ASSERT_EQ(memcmp(vec_reference, fb, bytes), 0);

    /* glVertex2f defaults z to 0 and w to 1; glVertex4f with w = 1 is the same point. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor4f(rgb[0], rgb[1], rgb[2], 1.0f);
    glVertex2f(a[0], a[1]);
    glVertex4f(b[0], b[1], b[2], 1.0f);
    glVertex2f(c[0], c[1]);
    glEnd();
    ASSERT_EQ(memcmp(vec_reference, fb, bytes), 0);

    /* glColor4fv is the four-component vector twin of the same colour. */
    static const GLfloat rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor4fv(rgba);
    glVertex3fv(a);
    glVertex3fv(b);
    glVertex3fv(c);
    glEnd();
    ASSERT_EQ(memcmp(vec_reference, fb, bytes), 0);

    /* glColor4ub is the same colour in bytes. The geometry must match exactly; the colour is
     * allowed one step per channel, because the two arrive by different roundings and
     * demanding equality there would pin the rounding rather than the forwarding. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor4ub(64, 128, 191, 255);
    glVertex3fv(a);
    glVertex3fv(b);
    glVertex3fv(c);
    glEnd();
    for (size_t i = 0; i < VEC_DIM * VEC_DIM; i++) {
        GLboolean ref_bg = (GLboolean)(vec_reference[i] == 0xff000000u);
        GLboolean got_bg = (GLboolean)(fb[i] == 0xff000000u);
        ASSERT_EQ(ref_bg, got_bg); /* covered exactly where the float path covered */
        if (ref_bg) continue;
        for (int shift = 0; shift <= 16; shift += 8) {
            int lhs = (int)((fb[i] >> shift) & 0xffu);
            int rhs = (int)((vec_reference[i] >> shift) & 0xffu);
            ASSERT_TRUE(lhs - rhs <= 1 && rhs - lhs <= 1);
        }
    }

    glContextDestroy(ctx);
    oops_display_close(disp);
}
#undef VEC_DIM

/* The first error is the one kept, which is the one that says where it went wrong.
 *
 * GL is explicit: once the flag is set, nothing further is recorded until glGetError() reads
 * and clears it. Every site here used to assign the field directly, so the *last* error won -
 * a caller making several calls before one glGetError() was shown the most recent failure and
 * never the one that started it, which points at the wrong call.
 *
 * Asserted in both directions: the first of two different errors survives the second, and the
 * flag really does clear so the next error can be seen. Without the second half this would
 * also pass against an implementation that recorded one error and then stopped forever. */
static void test_gl_first_error_is_the_one_retained(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    static const GLfloat verts[9] = {
        -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.0f, 0.5f, 0.0f
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    (void)glGetError();

    /* An enum error, then a value error. The enum one came first and must be what is read.
     *
     * GL_CONVOLUTION_1D rather than GL_FOG or GL_DITHER, which have each been the generator here
     * and are accepted now - the enum has to be one this genuinely does not have, or the test
     * stops testing what it says. The imaging subset is optional, so this one stays refused. */
    glEnable((GLenum)0x8010u);          /* GL_CONVOLUTION_1D: GL_INVALID_ENUM */
    glDrawArrays(GL_TRIANGLES, 0, -4);  /* GL_INVALID_VALUE */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* Reading it cleared it, so the next error is visible - and this time the value error
     * happens first, proving the order is what decides rather than the error code. */
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDrawArrays(GL_TRIANGLES, 0, -4);  /* GL_INVALID_VALUE */
    glEnable((GLenum)0x8010u);          /* GL_CONVOLUTION_1D: GL_INVALID_ENUM */
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    glDisableClientState(GL_VERTEX_ARRAY);
    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* The scalar and vector twins that nothing else reached.
 *
 * These eight were the last entry points exercised by neither the oracle program nor this
 * suite. They are thin - most forward to a sibling that was already covered - but a forwarder
 * is exactly where a wrong argument order or a dropped parameter hides, and "it is only a
 * wrapper" is how it stays hidden. Each is checked against the twin it forwards to, so the two
 * cannot drift apart without this failing. */
static void test_gl_scalar_twins_agree_with_their_vector_forms(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    (void)glGetError();

    /* glGetBooleanv answers what glIsEnabled answers, in both states. */
    GLboolean got = GL_TRUE;
    glDisable(GL_CULL_FACE);
    glGetBooleanv(GL_CULL_FACE, &got);
    ASSERT_EQ(got, glIsEnabled(GL_CULL_FACE));
    ASSERT_EQ(got, GL_FALSE);
    glEnable(GL_CULL_FACE);
    glGetBooleanv(GL_CULL_FACE, &got);
    ASSERT_EQ(got, glIsEnabled(GL_CULL_FACE));
    ASSERT_EQ(got, GL_TRUE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The float forms reach the same validation as the integer ones they forward to: an
     * accepted target is accepted by both, a rejected one is rejected by both. */
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLfloat)GL_MODULATE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glTexEnvf((GLenum)0x1234u, GL_TEXTURE_ENV_MODE, (GLfloat)GL_MODULATE);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glTexEnvi((GLenum)0x1234u, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    static const GLfloat env_colour[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env_colour);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLfloat)GL_NEAREST);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glTexParameterf((GLenum)0x1234u, GL_TEXTURE_MIN_FILTER, (GLfloat)GL_NEAREST);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    /* The target was validated and the parameter was not: GL 3.3's GL_TEXTURE_SWIZZLE_R is a real
     * parameter this does not keep, and one like it used to be dropped in silence.
     * (GL_TEXTURE_WRAP_R, GL_TEXTURE_MIN_LOD and then GL 1.4's GL_TEXTURE_COMPARE_MODE were the
     * example until each became one this keeps - the last of GL 1.x's, 2026-09-19.) */
    glTexParameteri(GL_TEXTURE_2D, (GLenum)0x8E42u, 0); /* GL_TEXTURE_SWIZZLE_R */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glDeleteTextures(1, &tex);

    /* Same for a material property outside the six the lighting model reads. */
    static const GLfloat mat[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glMaterialfv(GL_FRONT_AND_BACK, (GLenum)0x1234u, mat);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* glLightf and glLightModelf reach the same light validation as their vector twins. */
    glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 1.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glLightf((GLenum)(GL_LIGHT0 + 64u), GL_CONSTANT_ATTENUATION, 1.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glLightModelf(GL_LIGHT_MODEL_LOCAL_VIEWER, 0.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* GL_LIGHT_MODEL_TWO_SIDE set a field nothing read, then was refused; since 2026-09-19 it is
     * kept and lights back faces (test_gl_two_sided_lighting). */
    glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, 1.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint two_side = 0;
    glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &two_side);
    ASSERT_EQ(two_side, 1);
    glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, 0.0f);
    glLightModelf((GLenum)0x1234u, 1.0f); /* the light model's refusal, still */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* glNormal3fv is glNormal3f with the components in the same order. Checked by lighting a
     * face and comparing pixels, because a transposed normal is only visible as shading.
     *
     * **The light must not point down an axis the normal is symmetric about.** The first
     * version of this put the light at (0, 0, 1), so the shading depended on the normal's z
     * alone and a glNormal3fv with x and y swapped lit identically - the test passed against
     * the bug. It points along x now, where swapping x and y changes the dot product from 0.3
     * to 0.5 and the pixel with it. */
    uint32_t *fb = oops_display_get_framebuffer(disp);
    const int centre = 240 / 2 * 320 + 320 / 2;
    static const GLfloat lit[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT0, GL_POSITION, lit);
    glDisable(GL_CULL_FACE);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glNormal3f(0.3f, 0.5f, 0.81f);
    glVertex3f(-0.8f, -0.6f, 0.0f);
    glVertex3f( 0.7f, -0.5f, 0.0f);
    glVertex3f( 0.1f,  0.9f, 0.0f);
    glEnd();
    const uint32_t scalar_normal = fb[centre];

    static const GLfloat n[3] = {0.3f, 0.5f, 0.81f};
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glNormal3fv(n);
    glVertex3f(-0.8f, -0.6f, 0.0f);
    glVertex3f( 0.7f, -0.5f, 0.0f);
    glVertex3f( 0.1f,  0.9f, 0.0f);
    glEnd();
    ASSERT_EQ(fb[centre], scalar_normal);

    /* glFlush completes an open immediate-mode block rather than discarding it - the one piece
     * of behaviour it has on a host build. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    ASSERT_EQ(fb[centre], 0xff000000u);
    glBegin(GL_TRIANGLES);
    glNormal3fv(n);
    glVertex3f(-0.8f, -0.6f, 0.0f);
    glVertex3f( 0.7f, -0.5f, 0.0f);
    glVertex3f( 0.1f,  0.9f, 0.0f);
    glFlush(); /* no glEnd() */
    ASSERT_EQ(fb[centre], scalar_normal);

    glDisable(GL_LIGHTING);
    glContextDestroy(ctx);
    oops_display_close(disp);
}
/* The bound texture's storage, found by walking the table rather than calling gl_state.c's own
 * static lookup - a test that reached in for a private helper would pin the helper as much as
 * the behaviour. */
static const gl_texture_object_t *test_find_texture(const gl_context_t *ctx, GLuint id) {
  for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
    if (ctx->textures[i].used && ctx->textures[i].id == id) return &ctx->textures[i];
  }
  return NULL;
}


/* **glTexSubImage2D updates a rectangle and leaves the rest alone.**
 *
 * Checked by reading the texture's own storage rather than by rendering, because a sampled
 * result would also pass if the update landed in the wrong place and the sampler happened to
 * read the right colour. The bytes say exactly which texels moved.
 */
static void test_gl_tex_sub_image_updates_only_its_rectangle(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  /* A 4x4 texture, every texel opaque red. */
  static GLubyte base[4 * 4 * 4];
  for (int i = 0; i < 16; i++) {
    base[i * 4 + 0] = 255; base[i * 4 + 1] = 0; base[i * 4 + 2] = 0; base[i * 4 + 3] = 255;
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, base);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A 2x2 green patch at (1,1). */
  static GLubyte patch[2 * 2 * 4];
  for (int i = 0; i < 4; i++) {
    patch[i * 4 + 0] = 0; patch[i * 4 + 1] = 255; patch[i * 4 + 2] = 0; patch[i * 4 + 3] = 255;
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, patch);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  const gl_texture_object_t *stored = test_find_texture(ctx, tex);
  ASSERT_TRUE(stored != NULL && stored->pixels != NULL);
  const GLubyte *p = (const GLubyte *)stored->pixels;
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      const GLubyte *t = p + (((size_t)y * stored->pitch) + (size_t)x) * 4u;
      GLboolean inside = (GLboolean)(x >= 1 && x <= 2 && y >= 1 && y <= 2);
      if (inside) {
        ASSERT_EQ(t[1], 255); /* green */
        ASSERT_EQ(t[0], 0);
      } else {
        ASSERT_EQ(t[0], 255); /* still red */
        ASSERT_EQ(t[1], 0);
      }
    }
  }

  glDeleteTextures(1, &tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glPixelStorei changes where the rows are, and the default is the specification's 4.
 *
 * A three-byte format at width 3 is nine bytes per row, which alignment 4 rounds to twelve and
 * alignment 1 leaves at nine - so the two readings disagree from the second row onward, and a
 * texture uploaded under one and read under the other is visibly skewed.
 */
static void test_gl_pixel_store_alignment_moves_the_rows(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  /* Two rows of three RGB texels, padded to a 4-byte boundary: 9 bytes of data, 3 of padding.
   * Row 0 is red, row 1 is blue; the padding is a value that is neither. */
  static const GLubyte padded[24] = {
    255,0,0,  255,0,0,  255,0,0,   9,9,9,
    0,0,255,  0,0,255,  0,0,255,   9,9,9,
  };

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, padded);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  const gl_texture_object_t *stored = test_find_texture(ctx, tex);
  ASSERT_TRUE(stored != NULL);
  const GLubyte *p = (const GLubyte *)stored->pixels;
  /* Under the default alignment of 4 the second row starts at byte 12 and is blue. Under
   * alignment 1 it would start at byte 9 and pick up the padding. */
  const GLubyte *row1 = p + (size_t)stored->pitch * 4u;
  ASSERT_EQ(row1[2], 255); /* blue */
  ASSERT_EQ(row1[0], 0);

  /* Now say the rows are packed tight, and the same bytes read differently. */
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, padded);
  stored = test_find_texture(ctx, tex);
  row1 = (const GLubyte *)stored->pixels + (size_t)stored->pitch * 4u;
  ASSERT_EQ(row1[0], 9); /* the padding, read as a texel */
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  /* An alignment the specification does not define is refused, not rounded. */
  glPixelStorei(GL_UNPACK_ALIGNMENT, 3);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glPixelStorei((GLenum)0x1234u, 1);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glDeleteTextures(1, &tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **A format the upload cannot convert is refused, where it used to leave garbage.**
 *
 * glTexImage2D accepted any format, converted only RGBA and RGB, and left the texture holding
 * whatever the allocation contained for everything else - sampled, with no error. The formats
 * that now convert are asserted alongside, so a build that refused them all fails this too.
 */
static void test_gl_tex_image_refuses_a_format_it_cannot_convert(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  static const GLubyte data[4 * 4 * 4] = {0};

  for (GLenum format = 0; format < 1; format++) { (void)format; }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A real GL format this does not convert - GL 3.0's GL_DEPTH_STENCIL. (GL_FLOAT was the
   * example of a type it did not convert until 2026-09-19, and then colour index; both convert
   * now.) */
  static const GLfloat one_float[4] = {1.0f, 0.5f, 0.0f, 1.0f};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_FLOAT, one_float);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, (GLenum)0x84F9u /* GL_DEPTH_STENCIL */,
               GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, (GLenum)0x1234u, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* A packed type with a format it does not fit is GL_INVALID_OPERATION, not an enum error. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_6_5, data);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* A sub-image outside the texture is an error rather than a clamp or an overrun. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  (void)glGetError();
  glTexSubImage2D(GL_TEXTURE_2D, 0, 3, 3, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glDeleteTextures(1, &tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **A compiled list replays its calls, and compiling does not draw.**
 *
 * The two halves have to be asserted together: a list that drew at compile time and did nothing
 * on replay would produce the same final framebuffer as one that worked, and only the check in
 * between tells them apart.
 */
static void test_gl_display_list_compiles_then_replays(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  const int centre = 240 / 2 * 320 + 320 / 2;

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  (void)glGetError();

  GLuint list = glGenLists(1);
  ASSERT_TRUE(list != 0u);
  ASSERT_EQ(glIsList(list), GL_FALSE); /* named, not yet compiled */

  glNewList(list, GL_COMPILE);
  glColor3f(1.0f, 0.0f, 0.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.8f, -0.6f, 0.0f);
  glVertex3f(0.7f, -0.5f, 0.0f);
  glVertex3f(0.1f, 0.9f, 0.0f);
  glEnd();
  glEndList();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  ASSERT_EQ(glIsList(list), GL_TRUE);
  ASSERT_EQ(fb[centre], 0xff000000u); /* compiling drew nothing */

  glCallList(list);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_TRUE(((fb[centre] >> 16) & 0xffu) > 200u); /* replay drew the red triangle */

  /* Replaying again over a cleared buffer gives the same picture - a list is not consumed. */
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  ASSERT_EQ(fb[centre], 0xff000000u);
  glCallList(list);
  ASSERT_TRUE(((fb[centre] >> 16) & 0xffu) > 200u);

  glDeleteLists(list, 1);
  ASSERT_EQ(glIsList(list), GL_FALSE);

  glContextDestroy(ctx);
  oops_display_close(disp);
}

/* GL_COMPILE_AND_EXECUTE draws as it records, and the recording still replays. */
static void test_gl_display_list_compile_and_execute_does_both(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  const int centre = 240 / 2 * 320 + 320 / 2;

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  (void)glGetError();

  GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE_AND_EXECUTE);
  glColor3f(0.0f, 1.0f, 0.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.8f, -0.6f, 0.0f);
  glVertex3f(0.7f, -0.5f, 0.0f);
  glVertex3f(0.1f, 0.9f, 0.0f);
  glEnd();
  glEndList();

  ASSERT_TRUE(((fb[centre] >> 8) & 0xffu) > 200u); /* it drew while recording */

  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glCallList(list);
  ASSERT_TRUE(((fb[centre] >> 8) & 0xffu) > 200u); /* and it recorded what it drew */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx);
  oops_display_close(disp);
}

/* What a list refuses, and what it deliberately does not.
 *
 * Each refusal is paired with the legal case beside it, so a build that refused everything -
 * or nothing - fails this rather than passing half of it.
 */
static void test_gl_display_list_refusals(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx = glContextCreate(disp);
  (void)glGetError();

  /* glGenLists hands out a contiguous run, and zero is never a list. */
  GLuint run = glGenLists(3);
  ASSERT_TRUE(run != 0u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGenLists(-1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* An unrecognised mode is refused before anything is recorded. */
  glNewList(run, (GLenum)0x1234u);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glNewList(0u, GL_COMPILE);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* Lists do not nest. */
  glNewList(run, GL_COMPILE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glNewList(run + 1u, GL_COMPILE);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* **The vertex-array draws compile** - as the glBegin/element/glEnd sequence they stand for,
   * read out of the arrays now. They were refused with GL_INVALID_OPERATION until 2026-09-19;
   * test_gl_display_list_records_what_gl_compiles checks that the values are the compile-time
   * ones. */
  static const GLfloat verts[9] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, verts);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glEndList();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* glEndList with nothing open is an error. */
  glEndList();
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  glDrawArrays(GL_TRIANGLES, 0, 3);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glDisableClientState(GL_VERTEX_ARRAY);

  /* **Calling a list that was never defined is ignored, not an error.** The specification is
   * explicit, and it is what lets a list reference one compiled later. */
  glCallList(run + 2u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glCallList(999999u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A type outside GL_BYTE..GL_4_BYTES is refused before it reads one. (GL_FLOAT was the
   * refused example until 2026-09-19, when glCallLists took all ten of GL 1.0's types.) */
  static const GLubyte names[2] = {1, 2};
  glCallLists(2, GL_DOUBLE, names);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCallLists(2, GL_UNSIGNED_BYTE, names);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx);
  oops_display_close(disp);
}

/* **Everything the specification compiles is compiled** - and each property that makes a list a
 * list rather than a macro is checked on its own: compiling changes nothing, pointers are copied
 * when compiled, images unpack through the pixel-store state of compile time, arrays keep the
 * values they held, the list base applies when the list runs, GL_COMPILE_AND_EXECUTE records
 * once, and a compiled call's error is raised when it runs.
 *
 * Until 2026-09-19 a list recorded 21 operations and ran everything else at compile time; the
 * first block is the gears demo's shape, which drew every gear in the last material set.
 */
static void test_gl_display_list_records_what_gl_compiles(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  const int centre = (H / 2) * W + (W / 2);
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  (void)glGetError();

  /* 1. A material per list, as the gears demo has it. Compiling sets nothing; each call sets its
   *    own. The value arrays are changed after compiling, so a list that kept the pointer would
   *    read the wrong numbers. */
  GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  GLfloat shine = 42.0f;
  GLfloat before[4], now[4];
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, before);
  const GLuint gear = glGenLists(2);
  glNewList(gear, GL_COMPILE);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, red);
  glMaterialfv(GL_FRONT, GL_SHININESS, &shine); /* one value, and only one is read */
  glEndList();
  glNewList(gear + 1u, GL_COMPILE);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, blue);
  glEndList();
  red[0] = 0.25f;
  blue[2] = 0.25f;
  shine = 1.0f;
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, now);
  ASSERT_TRUE(now[0] == before[0] && now[2] == before[2]); /* compiling set nothing */
  glCallList(gear);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, now);
  ASSERT_TRUE(now[0] == 1.0f && now[2] == 0.0f);           /* the compile-time red */
  glGetMaterialfv(GL_FRONT, GL_SHININESS, now);
  ASSERT_TRUE(now[0] == 42.0f);
  glCallList(gear + 1u);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, now);
  ASSERT_TRUE(now[0] == 0.0f && now[2] == 1.0f);           /* and its own blue */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* 2. A matrix, copied at compile time. */
  GLfloat m[16] = {2,0,0,0, 0,2,0,0, 0,0,2,0, 0,0,0,1};
  const GLuint mat = glGenLists(1);
  glNewList(mat, GL_COMPILE);
  glLoadMatrixf(m);
  glEndList();
  m[0] = 9.0f;
  GLfloat mv[16];
  glGetFloatv(GL_MODELVIEW_MATRIX, mv);
  ASSERT_TRUE(mv[0] == 1.0f);                              /* compiling loaded nothing */
  glCallList(mat);
  glGetFloatv(GL_MODELVIEW_MATRIX, mv);
  ASSERT_TRUE(mv[0] == 2.0f && mv[5] == 2.0f);
  glLoadIdentity();

  /* 3. An image, through the unpack state of *compile* time. A 2x2 corner of a 3-wide client
   *    image, compiled with GL_UNPACK_ROW_LENGTH 3; the state and the image are both changed
   *    before the list runs, and the texture must still get the corner. */
  static GLubyte img[3 * 2 * 4];
  for (int i = 0; i < 3 * 2 * 4; i++) img[i] = (GLubyte)(10 + i);
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 3);
  const GLuint upload = glGenLists(1);
  glNewList(upload, GL_COMPILE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glEndList();
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  for (int i = 0; i < 3 * 2 * 4; i++) img[i] = 0u;
  glCallList(upload);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  GLubyte got[2 * 2 * 4];
  memset(got, 0, sizeof(got));
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_EQ(got[0], 10);        /* row 0, pixel 0 */
  ASSERT_EQ(got[4], 14);        /* row 0, pixel 1 */
  ASSERT_EQ(got[8], 10 + 12);   /* row 1 starts one *client* row (3 pixels) on, not two */
  ASSERT_EQ(got[12], 10 + 16);
  glBindTexture(GL_TEXTURE_2D, 0);

  /* 4. An array draw keeps the values the arrays held when it was compiled. */
  static GLfloat verts[9] = {-1.0f, -1.0f, 0.0f, 3.0f, -1.0f, 0.0f, -1.0f, 3.0f, 0.0f};
  static GLfloat cols[12] = {1,0,0,1, 1,0,0,1, 1,0,0,1};
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, verts);
  glColorPointer(4, GL_FLOAT, 0, cols);
  const GLuint drawn = glGenLists(1);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glNewList(drawn, GL_COMPILE);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glEndList();
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0u);                /* compiling drew nothing */
  for (int i = 0; i < 3; i++) { cols[i * 4 + 0] = 0.0f; cols[i * 4 + 2] = 1.0f; }
  glDisableClientState(GL_COLOR_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glCallList(drawn);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00ff0000u);       /* red, as compiled - not blue */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* 5. glCallLists applies the list base current when the list *runs*. */
  const GLuint pair = glGenLists(3);
  glNewList(pair + 1u, GL_COMPILE); glBlendFunc(GL_ZERO, GL_ZERO); glEndList();
  glNewList(pair + 2u, GL_COMPILE); glBlendFunc(GL_ONE, GL_ONE); glEndList();
  static const GLubyte one[1] = {1};
  const GLuint caller = glGenLists(1);
  glListBase(pair);
  glNewList(caller, GL_COMPILE);
  glCallLists(1, GL_UNSIGNED_BYTE, one);
  glEndList();
  glListBase(pair + 1u);
  glCallList(caller);
  GLint src = 0;
  glGetIntegerv(GL_BLEND_SRC, &src);
  ASSERT_EQ((GLenum)src, (GLenum)GL_ONE); /* (pair + 1) + 1, not pair + 1 */
  glListBase(0u);

  /* 6. GL_COMPILE_AND_EXECUTE records a called list as one call, not as its contents, and an
   *    entry point that calls another entry point records once. */
  const GLuint cae = glGenLists(1);
  glNewList(cae, GL_COMPILE_AND_EXECUTE);
  glCallList(gear);
  glEndList();
  ASSERT_EQ(ctx->lists[cae - 1u].count, 1u);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, now);
  ASSERT_TRUE(now[0] == 1.0f);                             /* and it did execute */
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glNewList(cae, GL_COMPILE_AND_EXECUTE);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 4, 4, 0); /* its body calls glCopyTexSubImage2D */
  glEndList();
  ASSERT_EQ(ctx->lists[cae - 1u].count, 1u);
  glBindTexture(GL_TEXTURE_2D, 0);

  /* 7. A compiled call's error is raised when it runs, not when it is recorded. */
  const GLuint bad = glGenLists(1);
  (void)glGetError();
  glNewList(bad, GL_COMPILE);
  glBlendFunc(GL_TEXTURE_2D, GL_ONE);
  glEndList();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glCallList(bad);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* 8. A bitmap, as bitmap fonts in lists use it: rows packed through the unpack alignment of
   *    compile time (1), replayed after it has become 4 - which would read row 1 from byte 4. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glOrtho(0.0, (GLdouble)W, 0.0, (GLdouble)H, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glClear(GL_COLOR_BUFFER_BIT);
  static const GLubyte glyph[2] = {0xf0, 0x0f};
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  const GLuint font = glGenLists(1);
  glNewList(font, GL_COMPILE);
  glBitmap(8, 2, 0.0f, 0.0f, 9.0f, 0.0f, glyph);
  glEndList();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glColor3f(1.0f, 1.0f, 1.0f);
  glRasterPos2i(2, 2);
  glCallList(font);
  int lit = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) == 0x00ffffffu) lit++;
  ASSERT_EQ(lit, 8);
  GLfloat rp[4];
  glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
  ASSERT_TRUE(rp[0] == 11.0f); /* moved by xmove */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* 9. Redefining a list replaces its contents rather than appending to them. */
  glNewList(mat, GL_COMPILE);
  glLoadIdentity();
  glEndList();
  ASSERT_EQ(ctx->lists[mat - 1u].count, 1u);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* A list that calls itself is bounded rather than overflowing the stack. */
static void test_gl_display_list_recursion_is_bounded(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx = glContextCreate(disp);
  (void)glGetError();

  GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glCallList(list); /* recorded, not followed */
  glEndList();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glCallList(list);
  ASSERT_EQ(glGetError(), GL_STACK_OVERFLOW);

  glContextDestroy(ctx);
  oops_display_close(disp);
}

/* **glReadPixels flips the rows, because GL's origin is the bottom-left corner.**
 *
 * This is the one detail that produces a plausible wrong answer: an unflipped read gives a
 * vertically mirrored image, which still looks like a screenshot, so nothing downstream
 * notices. The framebuffer is filled with a per-row value so a flip is visible as a value
 * rather than as a picture - row 0 of memory holds 10, row 1 holds 11, and so on, so reading
 * window row 0 must give the *last* row's value.
 */
static void test_gl_read_pixels_flips_to_gl_orientation(void) {
  const int W = 8, H = 4;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  /* Row n of memory gets red = 10 + n, so every row is distinguishable. */
  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      fb[row * W + col] = 0xff000000u | ((uint32_t)(10 + row) << 16);
    }
  }

  static GLubyte got[8 * 4 * 4];
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  for (int row = 0; row < H; row++) {
    /* GL row 0 is the bottom of the window, which is the last row in memory. */
    int expected = 10 + (H - 1 - row);
    ASSERT_EQ(got[(size_t)row * (size_t)W * 4u + 0], (GLubyte)expected);
  }

  /* A single-pixel read of the bottom-left corner is the clearest statement of the same rule. */
  static GLubyte one[4];
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
  ASSERT_EQ(one[0], (GLubyte)(10 + H - 1));

  /* And of the top-left corner. */
  glReadPixels(0, H - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
  ASSERT_EQ(one[0], 10);

  glContextDestroy(ctx);
  oops_display_close(disp);
}

/* The channel order, the refusals, and what falls outside the window.
 *
 * Each refusal is paired with the case beside it that must still work, so a build that refused
 * everything would fail this too.
 */
static void test_gl_read_pixels_formats_and_bounds(void) {
  const int W = 4, H = 4;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  /* One distinguishable colour everywhere: A=0x44, R=0x11, G=0x22, B=0x33. */
  for (int i = 0; i < W * H; i++) fb[i] = 0x44112233u;

  static GLubyte rgba[4], bgra[4], rgb[3];
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  ASSERT_EQ(rgba[0], 0x11); ASSERT_EQ(rgba[1], 0x22);
  ASSERT_EQ(rgba[2], 0x33); ASSERT_EQ(rgba[3], 0x44);

  glReadPixels(0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
  ASSERT_EQ(bgra[0], 0x33); ASSERT_EQ(bgra[1], 0x22);
  ASSERT_EQ(bgra[2], 0x11); ASSERT_EQ(bgra[3], 0x44);

  glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
  ASSERT_EQ(rgb[0], 0x11); ASSERT_EQ(rgb[2], 0x33);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Floats are read back as fractions of one (GL_FLOAT was this test's refused type until
   * 2026-09-19); a colour-index read is refused rather than written as bytes - an RGBA buffer
   * holds no indices, which is GL_INVALID_OPERATION now that the format itself is known. */
  GLfloat fl[4] = {0};
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, fl);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_FLOAT_NEAR(fl[0], 17.0f / 255.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(fl[3], 68.0f / 255.0f, 1e-6f);
  glReadPixels(0, 0, 1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, rgba);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glReadPixels(0, 0, -1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* **Outside the window reads zero, not whatever followed the framebuffer.** A rectangle
   * half off the right edge fills the part that overlaps and leaves the rest cleared. */
  static GLubyte wide[8 * 4];
  memset(wide, 0xcd, sizeof(wide));
  glReadPixels(W - 2, 0, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE, wide);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(wide[0], 0x11);  /* inside */
  ASSERT_EQ(wide[4], 0x11);  /* inside */
  ASSERT_EQ(wide[8], 0x00);  /* past the edge: zero, not 0xcd and not garbage */
  ASSERT_EQ(wide[9], 0x00);

  glContextDestroy(ctx);
  oops_display_close(disp);
}

/* **The alpha test, as words in the shader.**
 *
 * Checked by reading the payload rather than by rendering, because a wrong instruction encoding
 * cannot fail loudly here: it would assemble into the shader, the GPU would do something else,
 * and the result would be a wrong frame rather than a failed build. The words below are the
 * ones `clang -target amdgcn-amd-amdhsa -mcpu=gfx1030` produced for the instruction in each
 * comment, so this test is what ties the source assembly to what ships.
 */
static void test_gl_alpha_test_patches_both_shaders(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(256) uint8_t payload[0x4000];
  memset(payload, 0, sizeof(payload));
  ctx->gpu_payload = payload;
  (void)glGetError();

  const uint32_t *untex = (const uint32_t *)((const char *)payload + OOPS_GL_PS_UNTEX_OFFSET);
  const uint32_t *tex = (const uint32_t *)((const char *)payload + OOPS_GL_PS_TEX_OFFSET);

  /* Off: four s_nop, in both shaders. */
  glDisable(GL_ALPHA_TEST);
  for (unsigned i = 0; i < 4; i++) {
    ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + i], 0xbf800000u);
    ASSERT_EQ(tex[GL_PS_ALPHA_SLOT_TEX + i], 0xbf800000u);
  }

  /* On, GL_GREATER with a reference of 0.25. */
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.25f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 0], 0x7e1802ffu); /* v_mov_b32 v12, <literal> */
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 1], 0x3e800000u); /* 0.25f */
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 2], 0x7c081907u); /* v_cmp_gt_f32 vcc_lo, v7, v12 */
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 3], 0x877e6a7eu); /* s_and_b32 exec_lo, exec_lo, vcc_lo */
  /* **Both shaders**, because a textured draw and an untextured one must agree - patching one
   * would make the test depend on whether a texture was bound. */
  for (unsigned i = 0; i < 4; i++) {
    ASSERT_EQ(tex[GL_PS_ALPHA_SLOT_TEX + i], untex[GL_PS_ALPHA_SLOT_UNTEX + i]);
  }

  /* Each comparison has its own opcode, and no two are the same. */
  const struct { GLenum func; uint32_t word; } compares[] = {
    {GL_LESS, 0x7c021907u}, {GL_EQUAL, 0x7c041907u}, {GL_LEQUAL, 0x7c061907u},
    {GL_GREATER, 0x7c081907u}, {GL_NOTEQUAL, 0x7c1a1907u}, {GL_GEQUAL, 0x7c0c1907u},
  };
  for (size_t c = 0; c < sizeof(compares) / sizeof(compares[0]); c++) {
    glAlphaFunc(compares[c].func, 0.25f);
    ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 2], compares[c].word);
  }

  /* GL_NEVER kills every lane instead of comparing. */
  glAlphaFunc(GL_NEVER, 0.25f);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 0], 0xbefe0380u); /* s_mov_b32 exec_lo, 0 */

  /* GL_ALWAYS is no test at all, even while enabled - so the shader carries no instructions
   * rather than a comparison that always passes. */
  glAlphaFunc(GL_ALWAYS, 0.25f);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 0], 0xbf800000u);

  /* A function this does not know is refused, and leaves the shader as it was. */
  glAlphaFunc((GLenum)0x1234u, 0.5f);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 0], 0xbf800000u);

  /* The reference is clamped to 0..1 as the specification says. */
  glAlphaFunc(GL_LESS, 4.0f);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 1], 0x3f800000u); /* 1.0f */
  glAlphaFunc(GL_LESS, -1.0f);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 1], 0x00000000u); /* 0.0f */

  ctx->gpu_payload = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glCopyTexSubImage2D lands the same pixels a glTexSubImage2D of them would.
 *
 * Asserted by doing both and comparing, rather than against hand-worked expectations: the two
 * paths share the read, so what this pins is that sharing - a hand-written second flip in the
 * copy path would make them disagree, and that is the mistake worth catching.
 */
static void test_gl_copy_tex_sub_image_matches_a_read_then_upload(void) {
  const int W = 8, H = 8;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  /* A framebuffer where every texel differs, so a flip or a transpose is visible. */
  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      fb[row * W + col] = 0xff000000u | ((uint32_t)(row * 16 + col) << 16);
    }
  }

  /* The copy path. */
  GLuint copied = 0;
  glGenTextures(1, &copied);
  glBindTexture(GL_TEXTURE_2D, copied);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The read-then-upload path, by hand. */
  static GLubyte scratch[8 * 8 * 4];
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int r = 0; r < H; r++) {
    glReadPixels(0, r, W, 1, GL_RGBA, GL_UNSIGNED_BYTE, scratch + (size_t)r * (size_t)W * 4u);
  }
  GLuint uploaded = 0;
  glGenTextures(1, &uploaded);
  glBindTexture(GL_TEXTURE_2D, uploaded);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  const gl_texture_object_t *a = test_find_texture(ctx, copied);
  const gl_texture_object_t *b = test_find_texture(ctx, uploaded);
  ASSERT_TRUE(a != NULL && b != NULL && a->pixels != NULL && b->pixels != NULL);
  ASSERT_EQ(memcmp(a->pixels, b->pixels, (size_t)W * (size_t)H * 4u), 0);

  /* And it is not trivially equal because both are blank: the copy really has the framebuffer
   * in it, bottom row first. */
  const GLubyte *p = (const GLubyte *)a->pixels;
  ASSERT_EQ(p[0], (GLubyte)((H - 1) * 16 + 0)); /* GL row 0 is the last framebuffer row */

  /* A rectangle wider than the scratch is refused rather than truncated. */
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 4096, 1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glDeleteTextures(1, &copied);
  glDeleteTextures(1, &uploaded);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **glPushAttrib/glPopAttrib, and the trap is which bit owns an enable.**
 *
 * GL_ENABLE_BIT carries every enable, and each buffer bit *also* carries the enable for its own
 * feature - so GL_DEPTH_BUFFER_BIT alone must restore the depth-test enable. Getting that wrong
 * gives a pop that puts back a depth function while leaving the test off, which renders
 * plausibly and wrongly. So the narrow masks are tested on their own rather than only through
 * GL_ALL_ATTRIB_BITS, which would hide it.
 */
static void test_gl_attrib_stack_saves_and_restores(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  static _Alignas(256) uint8_t payload[0x4000];
  memset(payload, 0, sizeof(payload));
  ((gl_context_t *)ctx_handle)->gpu_payload = payload;
  (void)glGetError();

  /* A narrow mask restores its own group and leaves everything else alone. */
  glDisable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDisable(GL_BLEND);

  glPushAttrib(GL_DEPTH_BUFFER_BIT_ATTRIB);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GREATER);
  glEnable(GL_BLEND); /* not in the mask, so it must survive the pop */
  glPopAttrib();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_FALSE); /* the enable came back with its buffer bit */
  ASSERT_EQ(glIsEnabled(GL_BLEND), GL_TRUE);       /* and blend was not in the mask */

  /* GL_ENABLE_BIT carries every enable, including ones no buffer bit named. */
  glDisable(GL_CULL_FACE);
  glPushAttrib(GL_ENABLE_BIT);
  glEnable(GL_CULL_FACE);
  glEnable(GL_SCISSOR_TEST);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_CULL_FACE), GL_FALSE);
  ASSERT_EQ(glIsEnabled(GL_SCISSOR_TEST), GL_FALSE);

  /* GL_ALL_ATTRIB_BITS is narrowed to what exists rather than refused - it is the ordinary
   * call, and a program saying "all" is asking for whatever there is. */
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPopAttrib();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  /* So is GL 1.0's spelling of "all", 0x000FFFFF, which this header used until 2026-09-19 and a
   * program built against an old header still passes. */
  glPushAttrib(0x000FFFFFu);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPopAttrib();

  /* The groups whose state existed without a bit to save it: line width, point size, hints and
   * the multisample state. Each was refused as a group this "does not have". */
  glLineWidth(2.0f);
  glPointSize(3.0f);
  glHint(GL_FOG_HINT, GL_NICEST);
  glSampleCoverage(0.5f, GL_TRUE);
  glPushAttrib(GL_LINE_BIT | GL_POINT_BIT | GL_HINT_BIT | GL_MULTISAMPLE_BIT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glLineWidth(7.0f);
  glPointSize(9.0f);
  glHint(GL_FOG_HINT, GL_FASTEST);
  glSampleCoverage(1.0f, GL_FALSE);
  glDisable(GL_MULTISAMPLE);
  glPopAttrib();
  GLfloat fv = 0.0f;
  glGetFloatv(GL_LINE_WIDTH, &fv);
  ASSERT_TRUE(fv == 2.0f);
  glGetFloatv(GL_POINT_SIZE, &fv);
  ASSERT_TRUE(fv == 3.0f);
  GLint iv = 0;
  glGetIntegerv(GL_FOG_HINT, &iv);
  ASSERT_EQ((GLenum)iv, (GLenum)GL_NICEST);
  glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, &fv);
  ASSERT_TRUE(fv == 0.5f);
  ASSERT_EQ(glIsEnabled(GL_MULTISAMPLE), GL_TRUE);
  glLineWidth(1.0f);
  glPointSize(1.0f);

  /* A mask naming no group is refused, because a push that saved nothing is the leak this
   * function exists to prevent.
   *
   * **GL_STENCIL_BUFFER_BIT, GL_FOG_BIT and last GL_ACCUM_BUFFER_BIT used to be the examples of
   * groups this could not save**, which is this assertion doing its job: the list shrank as each
   * feature landed, and on 2026-09-19 it emptied. The accumulation group now saves its clear
   * value; a bit no GL 1.x group uses is what is left to refuse. */
  glClearAccum(0.25f, 0.0f, 0.0f, 0.0f);
  glPushAttrib(GL_ACCUM_BUFFER_BIT);
  glClearAccum(1.0f, 1.0f, 1.0f, 1.0f);
  glPopAttrib();
  GLfloat acv[4] = {0};
  glGetFloatv(GL_ACCUM_CLEAR_VALUE, acv);
  ASSERT_TRUE(acv[0] == 0.25f && acv[1] == 0.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPushAttrib(0x00100000u);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* The fog bit saves and restores real state - including the enable, which GL_FOG_BIT carries
   * alongside GL_ENABLE_BIT. */
  glEnable(GL_FOG);
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 2.0f);
  glPushAttrib(GL_FOG_BIT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glDisable(GL_FOG);
  glFogi(GL_FOG_MODE, GL_EXP2);
  glFogf(GL_FOG_START, 9.0f);
  glPopAttrib();
  {
    GLint mode = 0;
    GLfloat start = 0.0f;
    glGetIntegerv(GL_FOG_MODE, &mode);
    glGetFloatv(GL_FOG_START, &start);
    ASSERT_EQ(mode, (GLint)GL_LINEAR);
    ASSERT_TRUE(start == 2.0f);
    ASSERT_TRUE(glIsEnabled(GL_FOG));
  }
  glDisable(GL_FOG);

  /* And the stencil bit now saves and restores real state. */
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_EQUAL, 3, 0x0f);
  glPushAttrib(GL_STENCIL_BUFFER_BIT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glStencilFunc(GL_ALWAYS, 7, 0xff);
  glDisable(GL_STENCIL_TEST);
  glPopAttrib();
  {
    GLint sf = 0, sr = 0;
    glGetIntegerv(GL_STENCIL_FUNC, &sf);
    glGetIntegerv(GL_STENCIL_REF, &sr);
    ASSERT_EQ(sf, (GLint)GL_EQUAL);
    ASSERT_EQ(sr, 3);
    ASSERT_TRUE(glIsEnabled(GL_STENCIL_TEST));
  }
  glDisable(GL_STENCIL_TEST);

  /* Popping an empty stack, and overflowing a full one, are both errors rather than silence. */
  while (glGetError() != GL_NO_ERROR) { }
  glPopAttrib();
  ASSERT_EQ(glGetError(), GL_STACK_UNDERFLOW);
  for (int i = 0; i < 16; i++) glPushAttrib(GL_ENABLE_BIT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPushAttrib(GL_ENABLE_BIT);
  ASSERT_EQ(glGetError(), GL_STACK_OVERFLOW);
  for (int i = 0; i < 16; i++) glPopAttrib();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The alpha test lives in the shader, so a pop has to rewrite it** - unlike everything else
   * here, which the next frame picks up from the context. */
  glDisable(GL_ALPHA_TEST);
  const uint32_t *untex = (const uint32_t *)((const char *)payload + OOPS_GL_PS_UNTEX_OFFSET);
  glPushAttrib(GL_COLOR_BUFFER_BIT);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.25f);
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 2], 0x7c081907u); /* the comparison is in */
  glPopAttrib();
  ASSERT_EQ(untex[GL_PS_ALPHA_SLOT_UNTEX + 0], 0xbf800000u); /* and the pop took it out again */

  ((gl_context_t *)ctx_handle)->gpu_payload = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **glRect*, and the trap is the corner order.**
 *
 * The specification's four vertices are (x1,y1), (x2,y1), (x2,y2), (x1,y2). Three of the four
 * plausible wrong orders still draw *a* rectangle covering the same pixels - the bowtie
 * (x1,y1),(x2,y1),(x1,y2),(x2,y2) does not, but a transposition or a mirror does - so the test
 * is against the explicit quad the specification names rather than against a bounding box.
 *
 * The fixture is deliberately **not square and not centred**. A rectangle symmetric about x=y
 * passes against an implementation that swaps the axes, which is the shape of mistake this is
 * here to catch, and it has been the shape of two real escapes in this file already.
 */
static void test_gl_rect_draws_the_quad_the_spec_names(void) {
  enum { W = 64, H = 64 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glColor3f(1.0f, 0.0f, 0.0f);

  static uint32_t by_rect[W * H], by_quad[W * H], transposed[W * H];

  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.75f, -0.25f, 0.25f, 0.75f);
  memcpy(by_rect, fb, sizeof by_rect);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glVertex2f(-0.75f, -0.25f);
  glVertex2f( 0.25f, -0.25f);
  glVertex2f( 0.25f,  0.75f);
  glVertex2f(-0.75f,  0.75f);
  glEnd();
  memcpy(by_quad, fb, sizeof by_quad);

  ASSERT_EQ(memcmp(by_rect, by_quad, sizeof by_rect), 0);

  /* It drew something. Without this the comparison above is satisfied by two blank frames. */
  int painted = 0;
  for (int i = 0; i < W * H; i++) {
    if (by_rect[i] != 0xff000000u) painted++;
  }
  ASSERT_TRUE(painted > 100);

  /* And the fixture can tell the axes apart: the transpose is a different picture, so an
   * implementation that swapped x and y would fail the comparison above rather than sail
   * through it. */
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.25f, -0.75f, 0.75f, 0.25f);
  memcpy(transposed, fb, sizeof transposed);
  ASSERT_TRUE(memcmp(by_rect, transposed, sizeof by_rect) != 0);

  /* The vector form takes two corners in *two* arrays. `v1` is four wide and carries decoys in
   * [2] and [3]: an implementation reading the second corner out of the first array draws the
   * decoy rectangle, which is the `transposed` picture above and not this one. */
  const GLfloat v1[4] = {-0.75f, -0.25f, -0.25f, -0.75f};
  const GLfloat v2[2] = { 0.25f,  0.75f};
  glClear(GL_COLOR_BUFFER_BIT);
  glRectfv(v1, v2);
  ASSERT_EQ(memcmp(fb, by_rect, sizeof by_rect), 0);

  /* The integer forms reach the same place after conversion. In window terms this is the whole
   * clip cube, so it fills; what matters is that it agrees with the float call. */
  static uint32_t by_int[W * H];
  glClear(GL_COLOR_BUFFER_BIT);
  glRecti(-1, -1, 1, 1);
  memcpy(by_int, fb, sizeof by_int);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(memcmp(fb, by_int, sizeof by_int), 0);

  /* A null vector draws nothing rather than dereferencing it. */
  glClear(GL_COLOR_BUFFER_BIT);
  glRectfv(NULL, v2);
  glRectdv(NULL, NULL);
  for (int i = 0; i < W * H; i++) ASSERT_EQ(fb[i], 0xff000000u);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **The client attribute stack is a separate stack, and that is the whole point of it.**
 *
 * A library that brackets its array setup with glPushClientAttrib sits inside a caller that may
 * have bracketed itself with glPushAttrib. If the two shared one stack the inner pop would take
 * the outer frame, and the caller's server state would come back as whatever the library
 * happened to be holding. So the independence is asserted directly, not just the round trip.
 */
static void test_gl_client_attrib_stack_is_its_own_stack(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  static const GLfloat verts[12] = {0};
  static const GLfloat texco[8] = {0};

  /* A buffer bound while the pointer is specified, so the saved array has a non-zero buffer to
   * restore - a frame saving 0 and restoring 0 would pass against an implementation that
   * dropped the field entirely. */
  GLuint saved_vbo = 0;
  glGenBuffers(1, &saved_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, saved_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof verts, verts, GL_STATIC_DRAW);
  glVertexPointer(3, GL_FLOAT, 0, NULL);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  ASSERT_EQ(ctx->array_vertex.buffer, saved_vbo);
  glEnableClientState(GL_VERTEX_ARRAY);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 17);
  glPixelStorei(GL_PACK_ALIGNMENT, 2);

  glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

  glVertexPointer(2, GL_FLOAT, 16, texco);
  glDisableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);

  glPopClientAttrib();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The pointer and its enable travel together.** Restoring one without the other leaves an
   * array switched on pointing at something never set for it. */
  ASSERT_EQ(ctx->array_vertex.size, 3);
  ASSERT_EQ(ctx->array_vertex.stride, 0);
  ASSERT_TRUE(ctx->array_vertex.pointer == NULL); /* offset 0 into the buffer */
  ASSERT_EQ(ctx->array_vertex.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_texcoord[0].enabled, GL_FALSE);
  /* **The buffer an array reads from is part of the array**, so it is part of what this saves.
   * gl_client_array_t carries it, which makes the struct copy enough - pinned here so a later
   * change to field-by-field copying cannot quietly drop it. Between the push and the pop the
   * pointer was respecified with no buffer bound, so this came back rather than never moving. */
  ASSERT_EQ(ctx->array_vertex.buffer, saved_vbo);
  ASSERT_EQ(ctx->unpack_alignment, 8);
  ASSERT_EQ(ctx->unpack_row_length, 17);
  ASSERT_EQ(ctx->pack_alignment, 2);

  /* A mask naming one group leaves the other alone. Without this, a pop that ignored the mask
   * and restored everything would pass every assertion above. */
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glDisableClientState(GL_VERTEX_ARRAY);
  glPopClientAttrib();
  ASSERT_EQ(ctx->unpack_alignment, 8);
  ASSERT_EQ(ctx->array_vertex.enabled, GL_FALSE); /* not named, so not restored */
  glEnableClientState(GL_VERTEX_ARRAY);

  /* The two stacks do not touch each other. The server push below is popped *after* a whole
   * client push and pop, and must still find its own frame. */
  glDepthFunc(GL_NEVER);
  glPushAttrib(GL_DEPTH_BUFFER_BIT);
  glDepthFunc(GL_ALWAYS);
  glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
  ASSERT_EQ(ctx->attrib_depth, 1u);
  ASSERT_EQ(ctx->client_attrib_depth, 1u);
  glPopClientAttrib();
  ASSERT_EQ(ctx->attrib_depth, 1u);
  ASSERT_EQ(ctx->client_attrib_depth, 0u);
  glPopAttrib();
  ASSERT_EQ(ctx->depth_func, (GLenum)GL_NEVER);
  ASSERT_EQ(ctx->attrib_depth, 0u);

  /* Underflow, overflow, and a bit that names nothing. */
  glPopClientAttrib();
  ASSERT_EQ(glGetError(), GL_STACK_UNDERFLOW);
  glPushClientAttrib(0x4u);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  for (int i = 0; i < OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY; i++) {
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  }
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  ASSERT_EQ(glGetError(), GL_STACK_OVERFLOW);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glCopyTexImage2D allocates and then copies through the path glCopyTexSubImage2D already uses,
 * so the two cannot disagree about which way up the result lands. The rectangle is deliberately
 * **not square**: a width/height swap in the allocation would otherwise be invisible. */
static void test_gl_copy_tex_image_allocates_then_copies(void) {
  enum { W = 16, H = 16, RW = 8, RH = 4 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      fb[row * W + col] = 0xff000000u | ((uint32_t)(row * 16 + col) << 16);
    }
  }

  GLuint one_call = 0, two_calls = 0;
  glGenTextures(1, &one_call);
  glBindTexture(GL_TEXTURE_2D, one_call);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, RW, RH, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glGenTextures(1, &two_calls);
  glBindTexture(GL_TEXTURE_2D, two_calls);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RW, RH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, RW, RH);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  const gl_texture_object_t *a = test_find_texture(ctx, one_call);
  const gl_texture_object_t *b = test_find_texture(ctx, two_calls);
  ASSERT_TRUE(a != NULL && b != NULL && a->pixels != NULL && b->pixels != NULL);
  ASSERT_EQ(a->width, (GLsizei)RW);
  ASSERT_EQ(a->height, (GLsizei)RH);
  ASSERT_EQ(memcmp(a->pixels, b->pixels, (size_t)RW * (size_t)RH * 4u), 0);

  /* Not blank, and flipped the same way the sub-image copy is. GL row 0 of the *window* is
   * framebuffer row H-1 - the window's height, not the rectangle's, because the read is from
   * the window at y=0 and the flip is about the window's origin. */
  const GLubyte *p = (const GLubyte *)a->pixels;
  ASSERT_EQ(p[0], (GLubyte)((H - 1) * 16 + 0));

  /* A bordered texture is refused rather than allocated one pixel short. */
  GLuint bordered = 0;
  glGenTextures(1, &bordered);
  glBindTexture(GL_TEXTURE_2D, bordered);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, RW, RH, 1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  const gl_texture_object_t *none = test_find_texture(ctx, bordered);
  ASSERT_TRUE(none == NULL || none->pixels == NULL);

  glBindTexture(GL_TEXTURE_2D, one_call);
  glCopyTexImage2D(0x0DE0 /* GL_TEXTURE_1D, which this does not have */, 0, GL_RGBA,
                   0, 0, RW, RH, 0);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glDeleteTextures(1, &one_call);
  glDeleteTextures(1, &two_calls);
  glDeleteTextures(1, &bordered);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **Every spelling of an attribute must reach the same place.**
 *
 * GL 1.x names each attribute once per C type and once per arity, and old code reaches for all
 * of them. Each one here is checked against the value it should produce rather than only
 * against its float sibling, and every fixture uses a **different number in every component**,
 * because a fixture like (1,1,1) passes against an implementation that writes the first
 * argument three times.
 */
/* The integer spellings convert, and they do not all convert the same way.
 *
 * Colours and normals are normalised onto [-1,1] or [0,1]; positions and texture coordinates are
 * the value as given. A single fixture cannot catch a mix-up, because glVertex3i(1,1,1) and
 * glColor3i(1,1,1) differ by nine orders of magnitude and both "look plausible" in isolation -
 * so each rule is asserted against the endpoint it is supposed to land on exactly.
 */
/* Multitexture on a one-unit implementation: unit 0 works, and asking for a second is refused
 * rather than silently given unit 0 again.
 *
 * The silent version is the dangerous one. A program that multitextures typically sets a
 * coordinate per unit and expects two textures combined; handed unit 0's coordinate twice it
 * draws something plausible and wrong, with GL_NO_ERROR throughout. GL_MAX_TEXTURE_UNITS
 * reporting 1 is what lets such a program take its single-texture path instead.
 */
/* Texture coordinate generation.
 *
 * The interesting case is GL_EYE_LINEAR, because it is the one whose answer depends on *when*
 * the plane was specified. An implementation that stores the caller's numbers and uses them
 * directly behaves identically to GL_OBJECT_LINEAR and passes any test that never moves the
 * modelview between specifying the plane and drawing - so this one moves it.
 */
static void test_gl_texgen_generates_and_eye_planes_are_fixed_at_specification(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  /* The initial planes are not all the same: S is (1,0,0,0) and T is (0,1,0,0). */
  GLfloat p[4] = {9.0f, 9.0f, 9.0f, 9.0f};
  glGetTexGenfv(GL_S, GL_OBJECT_PLANE, p);
  ASSERT_TRUE(p[0] == 1.0f && p[1] == 0.0f && p[2] == 0.0f && p[3] == 0.0f);
  glGetTexGenfv(GL_T, GL_OBJECT_PLANE, p);
  ASSERT_TRUE(p[0] == 0.0f && p[1] == 1.0f && p[2] == 0.0f && p[3] == 0.0f);
  GLint mode = 0;
  glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, &mode);
  ASSERT_EQ(mode, (GLint)GL_EYE_LINEAR);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Object-linear: the plane dotted with the object coordinate, and the modelview is not
   * involved at all. Plane (2,0,0,1) against x = 3 gives 7. */
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
  const GLfloat objp[4] = {2.0f, 0.0f, 0.0f, 1.0f};
  glTexGenfv(GL_S, GL_OBJECT_PLANE, objp);
  glEnable(GL_TEXTURE_GEN_S);
  ASSERT_TRUE(glIsEnabled(GL_TEXTURE_GEN_S));

  glBegin(GL_TRIANGLES);
  glVertex3f(3.0f, 0.0f, 0.0f);
  glVertex3f(3.0f, 1.0f, 0.0f);
  glVertex3f(3.0f, 0.0f, 1.0f);
  glEnd();
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] == 7.0f);

  /* Moving the modelview does not change an object-linear coordinate. */
  glTranslatef(100.0f, 0.0f, 0.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(3.0f, 0.0f, 0.0f);
  glVertex3f(3.0f, 1.0f, 0.0f);
  glVertex3f(3.0f, 0.0f, 1.0f);
  glEnd();
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] == 7.0f);

  /* Eye-linear: the plane is fixed in eye space when it is specified. Specify (1,0,0,0) under
   * an identity modelview, then translate by 10 in x. The vertex at object x = 3 is now at eye
   * x = 13, so the coordinate is 13 - **not** 3, which is what storing the plane unchanged and
   * treating it as object-linear would give. */
  glLoadIdentity();
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
  const GLfloat eyep[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  glTexGenfv(GL_S, GL_EYE_PLANE, eyep);
  glTranslatef(10.0f, 0.0f, 0.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(3.0f, 0.0f, 0.0f);
  glVertex3f(3.0f, 1.0f, 0.0f);
  glVertex3f(3.0f, 0.0f, 1.0f);
  glEnd();
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] > 12.99f && ctx->imm_verts[0].tc[0][0] < 13.01f);

  /* A coordinate whose generation is off keeps what glTexCoord set - generating s while
   * supplying t by hand is the ordinary way this is used. */
  glLoadIdentity();
  glDisable(GL_TEXTURE_GEN_T);
  glTexCoord2f(0.0f, 0.875f);
  glBegin(GL_TRIANGLES);
  glVertex3f(3.0f, 0.0f, 0.0f);
  glVertex3f(3.0f, 1.0f, 0.0f);
  glVertex3f(3.0f, 0.0f, 1.0f);
  glEnd();
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][1] == 0.875f);

  /* Sphere mapping lands inside the map. It is only defined for s and t; asking for it on r is
   * refused rather than silently generating nothing. */
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
  glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glEnable(GL_TEXTURE_GEN_T);
  glNormal3f(0.0f, 0.0f, 1.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(0.5f, 0.5f, -2.0f);
  glVertex3f(0.6f, 0.5f, -2.0f);
  glVertex3f(0.5f, 0.6f, -2.0f);
  glEnd();
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] >= 0.0f && ctx->imm_verts[0].tc[0][0] <= 1.0f);
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][1] >= 0.0f && ctx->imm_verts[0].tc[0][1] <= 1.0f);

  /* The cube-map modes were refused while there was no cube map; they are accepted for s, t and
   * r now (test_gl_cube_maps), and refused for q, which is no direction. */
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);

  /* An unknown coordinate is refused. */
  glTexGeni(GL_TEXTURE_2D, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glDisable(GL_TEXTURE_GEN_S);
  glDisable(GL_TEXTURE_GEN_T);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Fog: a per-fragment blend towards the fog colour by eye-space distance.
 *
 * Checked at known distances rather than by "something changed", because the failure modes are
 * all plausible pictures: fog from window depth instead of eye distance changes with
 * glDepthRange, fog that touches alpha changes which fragments an alpha test keeps, and a linear
 * fog with start and end swapped fogs the near objects and clears the far ones.
 */
static void test_gl_fog_blends_by_eye_distance(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -20.0, 20.0);
  glMatrixMode(GL_MODELVIEW);
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  const GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  glFogfv(GL_FOG_COLOR, blue);
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 1.0f);
  glFogf(GL_FOG_END, 3.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A small red quad centred on the axis at eye distance D, so every fragment is at ~D. */
  #define FOG_DRAW_AT(D) do {                          \
      glClear(GL_COLOR_BUFFER_BIT);                     \
      glLoadIdentity();                                 \
      glTranslatef(0.0f, 0.0f, -(D));                   \
      glColor4f(1.0f, 0.0f, 0.0f, 1.0f);                \
      glRectf(-0.1f, -0.1f, 0.1f, 0.1f);                \
    } while (0)
  const int centre = (H / 2) * W + (W / 2);

  /* Off: unchanged whatever the distance. */
  glDisable(GL_FOG);
  FOG_DRAW_AT(10.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00ff0000u);

  glEnable(GL_FOG);

  /* Nearer than the start: fully the object's own colour. */
  FOG_DRAW_AT(0.5f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00ff0000u);

  /* Beyond the end: fully the fog colour. */
  FOG_DRAW_AT(10.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x000000ffu);

  /* **Halfway between start and end is halfway between the colours** - linear fog, and the
   * direction the right way round: a swapped start and end would fog this the other way. */
  FOG_DRAW_AT(2.0f);
  {
    const uint32_t c = fb[centre];
    const int r = (int)((c >> 16) & 0xffu), b = (int)(c & 0xffu);
    ASSERT_TRUE(r > 110 && r < 145);
    ASSERT_TRUE(b > 110 && b < 145);
  }

  /* EXP with density zero is no fog at all: exp(0) = 1. */
  glFogi(GL_FOG_MODE, GL_EXP);
  glFogf(GL_FOG_DENSITY, 0.0f);
  FOG_DRAW_AT(10.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00ff0000u);

  /* EXP2 falls off faster than EXP at the same density beyond distance 1/density. */
  glFogf(GL_FOG_DENSITY, 0.5f);
  FOG_DRAW_AT(4.0f);
  const int red_exp = (int)((fb[centre] >> 16) & 0xffu);
  glFogi(GL_FOG_MODE, GL_EXP2);
  FOG_DRAW_AT(4.0f);
  const int red_exp2 = (int)((fb[centre] >> 16) & 0xffu);
  ASSERT_TRUE(red_exp2 < red_exp);

  /* **Fog does not touch alpha.** A fragment that passes the alpha test unfogged still passes
   * fully fogged - fogging alpha too would change what a cut-out texture keeps. */
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  glClear(GL_COLOR_BUFFER_BIT);
  glLoadIdentity();
  glTranslatef(0.0f, 0.0f, -10.0f); /* fully fogged */
  glColor4f(1.0f, 0.0f, 0.0f, 0.75f);
  glRectf(-0.1f, -0.1f, 0.1f, 0.1f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x000000ffu); /* drew, as fog colour */
  glDisable(GL_ALPHA_TEST);

  /* Refusals: a scalar colour, a negative density, an unknown mode. Colour-index fog is state,
   * kept and reported (it was refused until 2026-09-19). */
  glFogf(GL_FOG_COLOR, 1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glFogi(GL_FOG_INDEX, 3);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  {
    GLint fi = 0;
    glGetIntegerv(GL_FOG_INDEX, &fi);
    ASSERT_EQ(fi, 3);
  }
  glFogf(GL_FOG_DENSITY, -1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glFogi(GL_FOG_MODE, (GLint)GL_NEAREST);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  #undef FOG_DRAW_AT
  glDisable(GL_FOG);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glLogicOp: the stored bits combined with the fragment's, instead of blending.
 *
 * Checked with values whose bytes differ in every combination a truth table distinguishes, and
 * with the opcodes where getting GL's enum order backwards shows: GL_AND_REVERSE (s & ~d) and
 * GL_AND_INVERTED (~s & d) swap if the table is read the wrong way round, and GL_XOR does not -
 * so XOR alone would pass a broken mapping.
 */
static void test_gl_logic_op_combines_stored_bits(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  const int centre = (H / 2) * W + (W / 2);

  /* Destination 0x3c3c55ff-ish, source 0xf0 0x0f 0xaa 0xff (R, G, B, A). */
  #define LOGIC_DRAW() do {                                          \
      glClearColor(60.0f / 255.0f, 60.0f / 255.0f, 85.0f / 255.0f, 1.0f); \
      glClear(GL_COLOR_BUFFER_BIT);                                   \
      glColor4ub(0xf0, 0x0f, 0xaa, 0xff);                            \
      glRectf(-1.0f, -1.0f, 1.0f, 1.0f);                             \
    } while (0)

  /* Defaults: off, and GL_COPY (Mesa main/blend.c:1158-1159). */
  ASSERT_EQ(glIsEnabled(GL_COLOR_LOGIC_OP), GL_FALSE);
  GLint mode = 0;
  glGetIntegerv(GL_LOGIC_OP_MODE, &mode);
  ASSERT_EQ((GLenum)mode, GL_COPY);

  glEnable(GL_COLOR_LOGIC_OP);
  ASSERT_EQ(glIsEnabled(GL_COLOR_LOGIC_OP), GL_TRUE);

  glLogicOp(GL_XOR);
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0x00cc33ffu); /* A ff^ff, R f0^3c, G 0f^3c, B aa^55 */

  glLogicOp(GL_AND_REVERSE); /* s & ~d */
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0x00c003aau);

  glLogicOp(GL_AND_INVERTED); /* ~s & d */
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0x000c3055u);

  glLogicOp(GL_INVERT); /* ~d, whatever the source */
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0x00c3c3aau);

  glLogicOp(GL_COPY); /* identity: the source */
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0xfff00faau);

  glLogicOp(GL_NOOP); /* the destination survives */
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0xff3c3c55u);

  /* **The logic op replaces blending.** ONE + ONE would saturate every channel; XOR does not. */
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glLogicOp(GL_XOR);
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0x00cc33ffu);
  glDisable(GL_BLEND);

  /* The colour mask still applies after the logic op: red kept from the destination. The mask
   * goes on after the clear - it gates a clear as well, so set before it the clear would have
   * kept the previous draw's red too. */
  glClearColor(60.0f / 255.0f, 60.0f / 255.0f, 85.0f / 255.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
  glColor4ub(0xf0, 0x0f, 0xaa, 0xff);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[centre], 0x003c33ffu);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  /* A refusal leaves the state alone. */
  glLogicOp(GL_SET + 1u);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_LOGIC_OP_MODE, &mode);
  ASSERT_EQ((GLenum)mode, GL_XOR);

  /* GL_INDEX_LOGIC_OP is colour-index mode's: kept as state in this RGBA context and applied to
   * nothing - with only it enabled, a draw is a plain copy. */
  glDisable(GL_COLOR_LOGIC_OP);
  glEnable(GL_INDEX_LOGIC_OP);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(glIsEnabled(GL_INDEX_LOGIC_OP), GL_TRUE);
  LOGIC_DRAW();
  ASSERT_EQ(fb[centre], 0xfff00faau);
  glDisable(GL_INDEX_LOGIC_OP);
  glEnable(GL_COLOR_LOGIC_OP);

  /* GL_COLOR_BUFFER_BIT carries the enable and the opcode. */
  glPushAttrib(GL_COLOR_BUFFER_BIT);
  glLogicOp(GL_NAND);
  glDisable(GL_COLOR_LOGIC_OP);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_COLOR_LOGIC_OP), GL_TRUE);
  glGetIntegerv(GL_LOGIC_OP_MODE, &mode);
  ASSERT_EQ((GLenum)mode, GL_XOR);

  /* And GL_ENABLE_BIT carries the enable on its own. */
  glPushAttrib(GL_ENABLE_BIT);
  glDisable(GL_COLOR_LOGIC_OP);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_COLOR_LOGIC_OP), GL_TRUE);

  #undef LOGIC_DRAW
  glDisable(GL_COLOR_LOGIC_OP);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glBlendColor and the four constant factors, the blend factors refused when they are not
 * factors, and GL's own defaults for them.
 */
static void test_gl_blend_constant_and_factor_rules(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  const int centre = (H / 2) * W + (W / 2);

  /* **GL's defaults are GL_ONE and GL_ZERO** (Mesa main/blend.c:1148-1151): enabling blending
   * without choosing factors overwrites. oops-gl defaulted to alpha blending. */
  GLint f = 0;
  glGetIntegerv(GL_BLEND_SRC, &f);
  ASSERT_EQ((GLenum)f, (GLenum)GL_ONE);
  glGetIntegerv(GL_BLEND_DST, &f);
  ASSERT_EQ((GLenum)f, (GLenum)GL_ZERO);
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_BLEND);
  glColor4f(1.0f, 0.0f, 0.0f, 0.5f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00ff0000u); /* not a half-blend with the blue */

  /* The constant defaults to zero and is clamped on the way in. */
  GLfloat bc[4] = {9.0f, 9.0f, 9.0f, 9.0f};
  glGetFloatv(GL_BLEND_COLOR, bc);
  ASSERT_TRUE(bc[0] == 0.0f && bc[1] == 0.0f && bc[2] == 0.0f && bc[3] == 0.0f);
  glBlendColor(0.5f, 2.0f, -1.0f, 0.25f);
  glGetFloatv(GL_BLEND_COLOR, bc);
  ASSERT_TRUE(bc[0] == 0.5f && bc[1] == 1.0f && bc[2] == 0.0f && bc[3] == 0.25f);
  /* As integers it spans the whole range (Mesa FLOAT_TO_INT). */
  GLint bci[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_BLEND_COLOR, bci);
  ASSERT_EQ(bci[1], 2147483647);
  ASSERT_EQ(bci[2], 0);
  /* And glGetDoublev copies all four, not one. */
  GLdouble bcd[4] = {9.0, 9.0, 9.0, 9.0};
  glGetDoublev(GL_BLEND_COLOR, bcd);
  ASSERT_TRUE(bcd[3] == 0.25);

  /* GL_CONSTANT_COLOR scales the source per channel by the constant. White through
   * (0.5, 1.0, 0.0) onto black is (128, 255, 0). */
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_CONSTANT_COLOR, GL_ZERO);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x0080ff00u);

  /* GL_CONSTANT_ALPHA is the constant's alpha on every channel: 0.25 of white. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_CONSTANT_ALPHA, GL_ZERO);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00404040u);

  /* GL_ONE_MINUS_CONSTANT_ALPHA on the destination: 0.75 of what was there, plus nothing. */
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_ZERO, GL_ONE_MINUS_CONSTANT_ALPHA);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00bfbfbfu);

  /* **GL_SRC_ALPHA_SATURATE is 1 for alpha**, min(As, 1 - Ad) only for colour. Onto a
   * transparent black destination with source alpha 0.5, a ONE-less saturate that also scaled
   * alpha would write 0.25 alpha; the right answer keeps the source's 0.5. */
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_SRC_ALPHA_SATURATE, GL_ZERO);
  glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ((fb[centre] >> 24) & 0xffu, 0x80u);        /* alpha: 0.5 * 1 */
  ASSERT_EQ(fb[centre] & 0x00ffffffu, 0x00808080u);    /* colour: 1 * min(0.5, 1) */

  /* Refusals, each leaving the factors as they were. SRC_ALPHA_SATURATE is a source factor only
   * (Mesa main/blend.c:105-108 needs ARB_blend_func_extended for it as a destination). */
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  (void)glGetError();
  glBlendFunc(GL_ONE, GL_SRC_ALPHA_SATURATE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBlendFunc(GL_TEXTURE_2D, GL_ONE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_SRC_ALPHA_SATURATE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_BLEND_SRC, &f);
  ASSERT_EQ((GLenum)f, (GLenum)GL_SRC_ALPHA);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &f);
  ASSERT_EQ((GLenum)f, (GLenum)GL_ONE_MINUS_SRC_ALPHA);
  glBlendEquation(GL_ONE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_BLEND_EQUATION, &f);
  ASSERT_EQ((GLenum)f, (GLenum)GL_FUNC_ADD);

  /* GL_COLOR_BUFFER_BIT carries the constant. */
  glBlendColor(0.5f, 0.5f, 0.5f, 0.5f);
  glPushAttrib(GL_COLOR_BUFFER_BIT);
  glBlendColor(1.0f, 1.0f, 1.0f, 1.0f);
  glPopAttrib();
  glGetFloatv(GL_BLEND_COLOR, bc);
  ASSERT_TRUE(bc[0] == 0.5f && bc[3] == 0.5f);

  glDisable(GL_BLEND);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Queries that answered less than the state held: the projective texture coordinate, the
 * colour queries as integers, and the four-component vectors glGetDoublev copied one of. */
static void test_gl_vector_queries_answer_every_component(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 16, 16);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  /* r and q as set, not the constants 0 and 1. */
  glTexCoord4f(0.1f, 0.2f, 0.3f, 2.0f);
  GLfloat tc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  glGetFloatv(GL_CURRENT_TEXTURE_COORDS, tc);
  ASSERT_TRUE(tc[2] == 0.3f && tc[3] == 2.0f);

  /* Colours as integers map 1.0 to INT_MAX; these were refused outright. */
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  GLint iv[4] = {0, 0, 0, 0};
  glGetIntegerv(GL_COLOR_CLEAR_VALUE, iv);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(iv[0], 2147483647);
  ASSERT_EQ(iv[1], 0);
  glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
  glGetIntegerv(GL_CURRENT_COLOR, iv);
  ASSERT_EQ(iv[1], 2147483647);
  const GLfloat grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  glFogfv(GL_FOG_COLOR, grey);
  glGetIntegerv(GL_FOG_COLOR, iv);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(iv[3], 2147483647);

  /* glGetDoublev writes all four components of each. */
  GLdouble dv[4] = {9.0, 9.0, 9.0, 9.0};
  glGetDoublev(GL_FOG_COLOR, dv);
  ASSERT_TRUE(dv[0] == 0.5 && dv[3] == 1.0);
  dv[3] = 9.0;
  glGetDoublev(GL_CURRENT_RASTER_POSITION, dv);
  ASSERT_TRUE(dv[3] != 9.0);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Generation replaces only the coordinates it is enabled for - the rest are the vertex's own,
 * including when the vertex came from an array - and a generated q divides like a given one. */
static void test_gl_texgen_keeps_the_vertex_own_coordinates(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 16, 16);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();

  /* s generated from x; t from the texture-coordinate array, not from glTexCoord's 0.9. */
  static const GLfloat verts[9] = {0.25f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f};
  static const GLfloat tcs[6] = {0.0f, 0.1f, 0.0f, 0.2f, 0.0f, 0.3f};
  const GLfloat sx[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
  glTexGenfv(GL_S, GL_OBJECT_PLANE, sx);
  glEnable(GL_TEXTURE_GEN_S);
  glTexCoord2f(0.9f, 0.9f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, verts);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 0, tcs);
  glBegin(GL_TRIANGLES);
  glArrayElement(0);
  glArrayElement(1);
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] == 0.25f && ctx->imm_verts[0].tc[0][1] == 0.1f);
  ASSERT_TRUE(ctx->imm_verts[1].tc[0][0] == 0.5f && ctx->imm_verts[1].tc[0][1] == 0.2f);
  glArrayElement(2);
  glEnd();
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisable(GL_TEXTURE_GEN_S);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A generated q of 2 (w times 2) goes to the vertex beside the given s and t, which the
   * rasteriser then divides per fragment (it halved them at the vertex until 2026-09-19). */
  const GLfloat q2[4] = {0.0f, 0.0f, 0.0f, 2.0f};
  glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
  glTexGenfv(GL_Q, GL_OBJECT_PLANE, q2);
  glEnable(GL_TEXTURE_GEN_Q);
  glTexCoord2f(0.5f, 0.5f);
  glBegin(GL_TRIANGLES);
  glVertex3f(1.0f, 0.0f, 0.0f);
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] == 0.5f && ctx->imm_verts[0].tc[0][1] == 0.5f);
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][3] == 2.0f);
  glVertex3f(0.0f, 1.0f, 0.0f);
  glVertex3f(0.0f, 0.0f, 0.0f);
  glEnd();
  glDisable(GL_TEXTURE_GEN_Q);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glPolygonMode and edge flags - and the three things that had to be right underneath them:
 * culling and the fill offset belong to polygons, not to the quads a line becomes, and a
 * flat-shaded primitive takes its colour from the vertex the specification names.
 *
 * A 32x32 target with identity matrices, so NDC x maps to column (x + 1) * 16 and NDC y to row
 * 16 - 16y. The quad spans columns and rows 8..24; its diagonal from the first corner to the
 * third passes through the centre, which is why an unlit centre proves the diagonal is not drawn.
 */
static void test_gl_polygon_mode_draws_boundaries(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)
  #define QUAD() do { glBegin(GL_QUADS); glVertex2f(-0.5f, -0.5f); glVertex2f(0.5f, -0.5f); \
                      glVertex2f(0.5f, 0.5f); glVertex2f(-0.5f, 0.5f); glEnd(); } while (0)

  /* Defaults: both faces filled, edges flagged. */
  GLint pm[2] = {0, 0};
  glGetIntegerv(GL_POLYGON_MODE, pm);
  ASSERT_EQ((GLenum)pm[0], (GLenum)GL_FILL);
  ASSERT_EQ((GLenum)pm[1], (GLenum)GL_FILL);
  GLint ef = 0;
  glGetIntegerv(GL_EDGE_FLAG, &ef);
  ASSERT_EQ(ef, 1);

  /* GL_LINE: the four sides, and not the diagonal the quad is triangulated along. */
  glLineWidth(3.0f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glGetIntegerv(GL_POLYGON_MODE, pm);
  ASSERT_EQ((GLenum)pm[0], (GLenum)GL_LINE);
  ASSERT_EQ((GLenum)pm[1], (GLenum)GL_LINE);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1.0f, 1.0f, 1.0f);
  QUAD();
  ASSERT_EQ(PX(16, 16), 0u);          /* centre: no fill, no diagonal */
  ASSERT_EQ(PX(16, 24), 0x00ffffffu); /* bottom side */
  ASSERT_EQ(PX(24, 16), 0x00ffffffu); /* right */
  ASSERT_EQ(PX(16, 8), 0x00ffffffu);  /* top */
  ASSERT_EQ(PX(8, 16), 0x00ffffffu);  /* left */

  /* An edge flag of false before a vertex drops the edge that starts there. Triangle
   * (-0.5,-0.5) (0.5,-0.5) (0,0.5): the edge from the second vertex, whose midpoint is (20,16). */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_TRIANGLES);
  glVertex2f(-0.5f, -0.5f);
  glEdgeFlag(GL_FALSE);
  glVertex2f(0.5f, -0.5f);
  glEdgeFlag(GL_TRUE);
  glVertex2f(0.0f, 0.5f);
  glEnd();
  ASSERT_EQ(PX(16, 24), 0x00ffffffu); /* first edge */
  ASSERT_EQ(PX(20, 16), 0u);          /* the flagged-off one */
  ASSERT_EQ(PX(12, 16), 0x00ffffffu); /* third edge */

  /* The same through an edge-flag array and glDrawArrays. */
  static const GLfloat tri[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
  static const GLboolean flags[3] = {GL_TRUE, GL_FALSE, GL_TRUE};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, tri);
  glEnableClientState(GL_EDGE_FLAG_ARRAY);
  glEdgeFlagPointer(0, flags);
  ASSERT_EQ(glIsEnabled(GL_EDGE_FLAG_ARRAY), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_VERTEX_ARRAY), GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ASSERT_EQ(PX(16, 24), 0x00ffffffu);
  ASSERT_EQ(PX(20, 16), 0u);
  ASSERT_EQ(PX(12, 16), 0x00ffffffu);
  GLvoid *ptr = NULL;
  glGetPointerv(GL_EDGE_FLAG_ARRAY_POINTER, &ptr);
  ASSERT_TRUE(ptr == (const GLvoid *)flags);
  glDisableClientState(GL_EDGE_FLAG_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  /* GL_POINT: the corners, each once, and nothing between them. */
  glPointSize(3.0f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD();
  ASSERT_EQ(PX(16, 16), 0u);
  ASSERT_EQ(PX(16, 24), 0u);
  ASSERT_EQ(PX(8, 24), 0x00ffffffu);
  ASSERT_EQ(PX(24, 8), 0x00ffffffu);

  /* Culling is decided for the polygon, before its mode: a culled back face draws no outline. The
   * quad drawn clockwise is back-facing. */
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glVertex2f(-0.5f, -0.5f); glVertex2f(-0.5f, 0.5f); glVertex2f(0.5f, 0.5f); glVertex2f(0.5f, -0.5f);
  glEnd();
  ASSERT_EQ(PX(16, 24), 0u);
  glDisable(GL_CULL_FACE);

  /* A mode per face: front filled, back outlined. */
  glPolygonMode(GL_FRONT, GL_FILL);
  glPolygonMode(GL_BACK, GL_LINE);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(); /* counter-clockwise: front */
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glVertex2f(-0.5f, -0.5f); glVertex2f(-0.5f, 0.5f); glVertex2f(0.5f, 0.5f); glVertex2f(0.5f, -0.5f);
  glEnd();
  ASSERT_EQ(PX(16, 16), 0u);
  ASSERT_EQ(PX(16, 24), 0x00ffffffu);

  /* The attribute stack and a display list both carry the mode. */
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glPushAttrib(GL_POLYGON_BIT);
  glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
  glPopAttrib();
  glGetIntegerv(GL_POLYGON_MODE, pm);
  ASSERT_EQ((GLenum)pm[0], (GLenum)GL_FILL);
  const GLuint lines = glGenLists(1);
  glNewList(lines, GL_COMPILE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glEndList();
  glGetIntegerv(GL_POLYGON_MODE, pm);
  ASSERT_EQ((GLenum)pm[0], (GLenum)GL_FILL);
  glCallList(lines);
  glGetIntegerv(GL_POLYGON_MODE, pm);
  ASSERT_EQ((GLenum)pm[0], (GLenum)GL_LINE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  /* Refusals leave the mode alone. */
  glPolygonMode(GL_TEXTURE_2D, GL_LINE); /* not a face */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glPolygonMode(GL_FRONT, GL_QUADS);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_POLYGON_MODE, pm);
  ASSERT_EQ((GLenum)pm[0], (GLenum)GL_FILL);

  /* **A GL_LINES line is not culled**, whatever way its quad winds. */
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT_AND_BACK);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES);
  glVertex2f(-0.5f, 0.0f); glVertex2f(0.5f, 0.0f);
  glEnd();
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  glDisable(GL_CULL_FACE);

  /* **Nor does it take the fill offset.** A filled quad at z = 0 without offset, then a line at
   * the same depth with GL_POLYGON_OFFSET_FILL on and a large offset: GL_LEQUAL passes the line
   * only if the offset left it alone. */
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glColor3f(1.0f, 0.0f, 0.0f);
  QUAD();
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(0.0f, 1000000.0f);
  glColor3f(1.0f, 1.0f, 1.0f);
  glBegin(GL_LINES);
  glVertex2f(-0.4f, 0.0f); glVertex2f(0.4f, 0.0f);
  glEnd();
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(0.0f, 0.0f);
  glDisable(GL_DEPTH_TEST);
  ASSERT_EQ(glIsEnabled(GL_POLYGON_OFFSET_LINE), GL_FALSE);
  glEnable(GL_POLYGON_OFFSET_LINE);
  ASSERT_EQ(glIsEnabled(GL_POLYGON_OFFSET_LINE), GL_TRUE);
  glDisable(GL_POLYGON_OFFSET_LINE);

  /* **A flat quad is its fourth vertex's colour** - both triangles, not just the second. And a
   * flat polygon is its first vertex's. (18,20) is in the quad's first triangle, (12,12) in its
   * second. */
  glShadeModel(GL_FLAT);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(-0.5f, -0.5f);
  glVertex2f(0.5f, -0.5f);
  glVertex2f(0.5f, 0.5f);
  glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(-0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(PX(18, 20), 0x000000ffu);
  ASSERT_EQ(PX(12, 12), 0x000000ffu);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_POLYGON);
  glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(-0.5f, -0.5f);
  glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(0.5f, -0.5f);
  glVertex2f(0.5f, 0.5f);
  glVertex2f(-0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(PX(18, 20), 0x0000ff00u);
  ASSERT_EQ(PX(12, 12), 0x0000ff00u);
  glShadeModel(GL_SMOOTH);

  #undef QUAD
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* State an RGBA context keeps and never draws with - the colour index, the index mask and clear
 * value, the index array, the multisample enables and coverage - plus the point and line
 * queries, which were declared and answered nowhere. Defaults are Mesa's (main/context.c:269,
 * main/blend.c:1139-1141, main/multisample.c:69-75).
 */
static void test_gl_rgba_context_keeps_index_and_sample_state(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  GLint iv[2] = {0, 0};
  GLfloat fv[2] = {0.0f, 0.0f};
  glGetIntegerv(GL_CURRENT_INDEX, iv);      ASSERT_EQ(iv[0], 1);
  glGetIntegerv(GL_INDEX_WRITEMASK, iv);    ASSERT_EQ(iv[0], -1); /* all ones */
  glGetIntegerv(GL_INDEX_CLEAR_VALUE, iv);  ASSERT_EQ(iv[0], 0);
  glGetIntegerv(GL_INDEX_MODE, iv);         ASSERT_EQ(iv[0], 0);
  glGetIntegerv(GL_RGBA_MODE, iv);          ASSERT_EQ(iv[0], 1);
  glGetIntegerv(GL_SAMPLE_BUFFERS, iv);     ASSERT_EQ(iv[0], 0);
  ASSERT_EQ(glIsEnabled(GL_MULTISAMPLE), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_SAMPLE_COVERAGE), GL_FALSE);
  glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, fv); ASSERT_TRUE(fv[0] == 1.0f);

  /* Every spelling of glIndex is a cast, not a normalisation. */
  glIndexub(200);
  glGetFloatv(GL_CURRENT_INDEX, fv);        ASSERT_TRUE(fv[0] == 200.0f);
  glIndexs(-3);
  glGetFloatv(GL_CURRENT_INDEX, fv);        ASSERT_TRUE(fv[0] == -3.0f);
  glIndexd(2.6);
  glGetIntegerv(GL_CURRENT_INDEX, iv);      ASSERT_EQ(iv[0], 3); /* rounded, not truncated */
  const GLint seven = 7;
  glIndexiv(&seven);
  glGetIntegerv(GL_CURRENT_INDEX, iv);      ASSERT_EQ(iv[0], 7);

  /* The coverage value clamps (main/multisample.c:49). */
  glSampleCoverage(2.0f, GL_TRUE);
  glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, fv); ASSERT_TRUE(fv[0] == 1.0f);
  glGetIntegerv(GL_SAMPLE_COVERAGE_INVERT, iv); ASSERT_EQ(iv[0], 1);
  glSampleCoverage(-1.0f, GL_FALSE);
  glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, fv); ASSERT_TRUE(fv[0] == 0.0f);
  glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  ASSERT_EQ(glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE), GL_TRUE);
  glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The index array: its types are the specification's, and an element compiled into a list
   * becomes the glIndex call it stands for. */
  static const GLubyte idx[1] = {42};
  static const GLfloat pos[3] = {0.0f, 0.0f, 0.0f};
  glIndexPointer(GL_UNSIGNED_INT, 0, idx);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glIndexPointer(GL_UNSIGNED_BYTE, 0, idx);
  glEnableClientState(GL_INDEX_ARRAY);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, pos);
  ASSERT_EQ(glIsEnabled(GL_INDEX_ARRAY), GL_TRUE);
  glGetIntegerv(GL_INDEX_ARRAY_TYPE, iv);   ASSERT_EQ((GLenum)iv[0], (GLenum)GL_UNSIGNED_BYTE);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glBegin(GL_POINTS);
  glArrayElement(0);
  glEnd();
  glIndexMask(0x0fu);
  glClearIndex(5.0f);
  glEndList();
  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_INDEX_ARRAY);
  glGetIntegerv(GL_CURRENT_INDEX, iv);      ASSERT_EQ(iv[0], 7);  /* compiling set nothing */
  glCallList(list);
  glGetIntegerv(GL_CURRENT_INDEX, iv);      ASSERT_EQ(iv[0], 42);
  glGetIntegerv(GL_INDEX_WRITEMASK, iv);    ASSERT_EQ(iv[0], 0x0f);
  glGetIntegerv(GL_INDEX_CLEAR_VALUE, iv);  ASSERT_EQ(iv[0], 5);

  /* The attribute stack carries them: the index with GL_CURRENT_BIT, the mask and clear value
   * with GL_COLOR_BUFFER_BIT. */
  glPushAttrib(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT);
  glIndexf(1.0f);
  glIndexMask(0u);
  glClearIndex(0.0f);
  glPopAttrib();
  glGetIntegerv(GL_CURRENT_INDEX, iv);      ASSERT_EQ(iv[0], 42);
  glGetIntegerv(GL_INDEX_WRITEMASK, iv);    ASSERT_EQ(iv[0], 0x0f);

  /* Point size and line width, and their ranges: queried as set, drawn rounded and clamped. */
  glLineWidth(2.4f);
  glGetFloatv(GL_LINE_WIDTH, fv);           ASSERT_TRUE(fv[0] == 2.4f);
  glGetIntegerv(GL_LINE_WIDTH, iv);         ASSERT_EQ(iv[0], 2);
  glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == (GLfloat)OOPS_GL_MAX_POINT_LINE_SIZE);
  glGetIntegerv(GL_POINT_SIZE_RANGE, iv);
  ASSERT_EQ(iv[0], 1);
  ASSERT_EQ(iv[1], OOPS_GL_MAX_POINT_LINE_SIZE);
  /* The smooth sizes' step - an eighth since smoothing landed; 1 before, when nothing smoothed. */
  glGetFloatv(GL_POINT_SIZE_GRANULARITY, fv); ASSERT_TRUE(fv[0] == OOPS_GL_SMOOTH_GRANULARITY);

  /* **A sub-pixel width draws a one-pixel line**, as an aliased line of any width below 1.5
   * does. The raw 0.3 used to be the quad's width, which can miss every pixel centre. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glLineWidth(0.3f);
  glColor3f(1.0f, 1.0f, 1.0f);
  glBegin(GL_LINES);
  glVertex2f(-0.9f, 0.03f); glVertex2f(0.9f, 0.03f);
  glEnd();
  int lit = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) lit++;
  ASSERT_TRUE(lit >= 10 && lit <= 20);
  glLineWidth(1.0f);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Mipmap levels, completeness, the default texture, the filters, and perspective-correct
 * interpolation - the texturing a real 1.x program depends on, each against GL's own numbers.
 *
 * The first assertion is the one that matters most: uploading levels 1 and 2 must leave level 0
 * alone. `level` was ignored until 2026-09-19, so a mip chain uploaded level by level ended as its
 * own 1x1 level on the base image.
 */
static void test_gl_mipmaps_completeness_and_filtering(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)

  /* A three-level chain: 4x4 red, 2x2 green, 1x1 blue. */
  GLubyte red[4 * 4 * 4], green[2 * 2 * 4], blue[4] = {0, 0, 255, 255};
  for (int i = 0; i < 16; i++) { red[i * 4] = 255; red[i * 4 + 1] = 0; red[i * 4 + 2] = 0; red[i * 4 + 3] = 255; }
  for (int i = 0; i < 4; i++) { green[i * 4] = 0; green[i * 4 + 1] = 255; green[i * 4 + 2] = 0; green[i * 4 + 3] = 255; }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
  glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Each level is its own image, and the base one is untouched. */
  GLint lw = 0;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &lw); ASSERT_EQ(lw, 4);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &lw); ASSERT_EQ(lw, 2);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &lw); ASSERT_EQ(lw, 1);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &lw); ASSERT_EQ(lw, 0);
  GLubyte back[4 * 4 * 4];
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(back[0], 255); ASSERT_EQ(back[1], 0);   /* still red */
  glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(back[0], 0); ASSERT_EQ(back[1], 255);   /* green */
  glPixelStorei(GL_PACK_ALIGNMENT, 4);

  glEnable(GL_TEXTURE_2D);
  glColor3f(1.0f, 1.0f, 1.0f);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  #define QUAD(h) do { glBegin(GL_QUADS); \
      glTexCoord2f(0.0f, 0.0f); glVertex2f(-(h), -(h)); glTexCoord2f(1.0f, 0.0f); glVertex2f((h), -(h)); \
      glTexCoord2f(1.0f, 1.0f); glVertex2f((h), (h));   glTexCoord2f(0.0f, 1.0f); glVertex2f(-(h), (h)); \
      glEnd(); } while (0)

  /* Magnified - 4 texels over 16 pixels - the base level. */
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(0.5f);
  ASSERT_EQ(PX(16, 16), 0x00ff0000u);
  /* 4 texels over 2 pixels: lod 1, the second level. */
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(0.0625f);
  ASSERT_EQ(PX(16, 16), 0x0000ff00u);
  /* 4 texels over 1 pixel: lod 2, the third. The one-pixel quad is centred on a pixel corner,
   * all four of whose neighbouring centres lie on its edges; under the rasteriser's tie rule
   * the upper-left one is its pixel (it drew all four until 2026-09-19). */
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(0.03125f);
  ASSERT_EQ(PX(15, 15), 0x000000ffu);
  ASSERT_EQ(PX(16, 16), 0x00000000u);

  /* **Incomplete draws untextured.** A second level of the wrong size breaks the chain, and the
   * quad takes the plain vertex colour - white - as it would on any GL. */
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(0.5f);
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  /* A filter that reads no mipmaps needs only the base level. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(0.5f);
  ASSERT_EQ(PX(16, 16), 0x00ff0000u);

  /* **Nothing bound means the default texture.** Uploaded with no bind, drawn with no bind. */
  glBindTexture(GL_TEXTURE_2D, 0);
  const GLubyte yellow[4] = {255, 255, 0, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(0.5f);
  ASSERT_EQ(PX(16, 16), 0x00ffff00u);

  /* Linear against nearest magnification: a black-then-white 2x1 texture across the screen. At
   * the centre column the linear sample is a blend; the nearest one is the white texel. */
  const GLubyte bw[8] = {0, 0, 0, 255, 255, 255, 255, 255};
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, bw);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(1.0f);
  const uint32_t mid_linear = (fb[16 * W + 16] >> 16) & 0xffu;
  ASSERT_TRUE(mid_linear > 100u && mid_linear < 170u);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(1.0f);
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  glDisable(GL_TEXTURE_2D);

  /* **Perspective-correct interpolation.** Black at the bottom with w = 1, white at the top
   * with w = 3, filling the screen. The middle row is NDC y = 0, which in clip space is a quarter
   * of the way up the edge ((-1 + 4t) / (1 + 2t) = 0 at t = 1/4): colour 0.25, about 64. Affine
   * interpolation, which this was, gives the screen halfway point's 128. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glColor3f(0.0f, 0.0f, 0.0f); glVertex4f(-1.0f, -1.0f, 0.0f, 1.0f);
  glColor3f(0.0f, 0.0f, 0.0f); glVertex4f(1.0f, -1.0f, 0.0f, 1.0f);
  glColor3f(1.0f, 1.0f, 1.0f); glVertex4f(3.0f, 3.0f, 0.0f, 3.0f);
  glColor3f(1.0f, 1.0f, 1.0f); glVertex4f(-3.0f, 3.0f, 0.0f, 3.0f);
  glEnd();
  const uint32_t mid = (fb[16 * W + 16] >> 16) & 0xffu;
  ASSERT_TRUE(mid > 52u && mid < 76u);

  /* Level bounds. */
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, OOPS_GL_MAX_TEXTURE_LEVELS, GL_RGBA, 1, 1, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, blue);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, OOPS_GL_MAX_TEXTURE_SIZE, 1, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* larger than level 1 can be */
  glTexSubImage2D(GL_TEXTURE_2D, 5, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* no level 5 to update */

  #undef QUAD
  #undef PX
  glDeleteTextures(1, &tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Evaluators: every number here is the Bezier arithmetic worked by hand.
 *
 * The first drawing check is the one that tells a Bezier curve from a curve *through* its control
 * points: the quadratic below has its middle control point at y = 0.5, and the curve's middle is
 * at y = 0 - so the point lands on the centre row, not a quarter of the way up.
 */
static void test_gl_evaluators(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)

  /* Defaults: every map order 1 over 0..1 holding its initial point, nothing enabled, a 1x1 grid. */
  GLint iv[4] = {0, 0, 0, 0};
  GLfloat fv[16] = {0}; /* GL_COEFF writes every point: 12 values for the 2x2 patch below */
  glGetIntegerv(GL_MAX_EVAL_ORDER, iv);
  ASSERT_TRUE(iv[0] >= 8);
  ASSERT_EQ(glIsEnabled(GL_MAP2_VERTEX_3), GL_FALSE);
  ASSERT_EQ(glIsEnabled(GL_AUTO_NORMAL), GL_FALSE);
  glGetMapfv(GL_MAP1_COLOR_4, GL_COEFF, fv);
  ASSERT_FLOAT_NEAR(fv[0], 1.0f, 0.0f); ASSERT_FLOAT_NEAR(fv[3], 1.0f, 0.0f);
  glGetMapfv(GL_MAP2_NORMAL, GL_COEFF, fv);
  ASSERT_FLOAT_NEAR(fv[0], 0.0f, 0.0f); ASSERT_FLOAT_NEAR(fv[2], 1.0f, 0.0f);
  glGetMapiv(GL_MAP2_VERTEX_4, GL_ORDER, iv);
  ASSERT_EQ(iv[0], 1); ASSERT_EQ(iv[1], 1);
  glGetMapfv(GL_MAP1_VERTEX_3, GL_DOMAIN, fv);
  ASSERT_FLOAT_NEAR(fv[0], 0.0f, 0.0f); ASSERT_FLOAT_NEAR(fv[1], 1.0f, 0.0f);
  glGetIntegerv(GL_MAP2_GRID_SEGMENTS, iv);
  ASSERT_EQ(iv[0], 1); ASSERT_EQ(iv[1], 1);
  GLdouble dv[4] = {9, 9, 9, 9};
  glGetDoublev(GL_MAP2_GRID_DOMAIN, dv); /* four values, all of them */
  ASSERT_TRUE(dv[0] == 0.0 && dv[1] == 1.0 && dv[2] == 0.0 && dv[3] == 1.0);
  /* An enable answers every glGet, not only glGetBooleanv. */
  glGetIntegerv(GL_AUTO_NORMAL, iv);
  ASSERT_EQ(iv[0], 0);
  glEnable(GL_DEPTH_TEST);
  glGetIntegerv(GL_DEPTH_TEST, iv);
  ASSERT_EQ(iv[0], 1);
  glDisable(GL_DEPTH_TEST);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A quadratic through a stride wider than its points - the fourth value of each is padding. */
  const GLfloat curve[12] = {-0.5f, -0.5f, 0.0f, 99.0f,
                              0.0f,  0.5f, 0.0f, 99.0f,
                              0.5f, -0.5f, 0.0f, 99.0f};
  glMap1f(GL_MAP1_VERTEX_3, 2.0f, 4.0f, 4, 3, curve); /* domain 2..4: u = 3 is the middle */
  glGetMapiv(GL_MAP1_VERTEX_3, GL_ORDER, iv);
  ASSERT_EQ(iv[0], 3);
  glGetMapfv(GL_MAP1_VERTEX_3, GL_COEFF, fv); /* packed: the padding is gone */
  ASSERT_FLOAT_NEAR(fv[3], 0.0f, 0.0f); ASSERT_FLOAT_NEAR(fv[4], 0.5f, 0.0f);
  glGetMapfv(GL_MAP1_VERTEX_3, GL_DOMAIN, fv);
  ASSERT_FLOAT_NEAR(fv[0], 2.0f, 0.0f); ASSERT_FLOAT_NEAR(fv[1], 4.0f, 0.0f);

  glPointSize(3.0f);
  glColor3f(1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_POINTS);
  glEvalCoord1f(3.0f); /* map not enabled: no vertex */
  glEnd();
  ASSERT_EQ(PX(16, 16), 0u);
  glEnable(GL_MAP1_VERTEX_3);
  glBegin(GL_POINTS);
  glEvalCoord1f(3.0f);
  glEnd();
  ASSERT_EQ(PX(16, 16), 0x00ffffffu); /* (0, 0): the curve's middle */
  ASSERT_EQ(PX(16, 8), 0u);           /* not the middle control point */

  /* **An evaluated colour is that vertex's only.** Order 2 from red to green: u = 2 is red, and
   * the current colour is still white afterwards. */
  const GLfloat red_green[8] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
  glMap1f(GL_MAP1_COLOR_4, 2.0f, 4.0f, 4, 2, red_green);
  glEnable(GL_MAP1_COLOR_4);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_POINTS);
  glEvalCoord1d(2.0);
  glEnd();
  ASSERT_EQ(PX(8, 24), 0x00ff0000u); /* (-0.5, -0.5), red */
  glGetFloatv(GL_CURRENT_COLOR, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == 1.0f && fv[2] == 1.0f);
  glDisable(GL_MAP1_COLOR_4);

  /* **GL_MAP1_VERTEX_4 wins over _3, and is projected.** (-1, 0, 0, 2) is (-0.5, 0). */
  const GLfloat rational[8] = {-1.0f, 0.0f, 0.0f, 2.0f, 0.5f, 0.0f, 0.0f, 1.0f};
  glMap1f(GL_MAP1_VERTEX_4, 0.0f, 1.0f, 4, 2, rational);
  glEnable(GL_MAP1_VERTEX_4);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_POINTS);
  glEvalCoord1f(0.0f);
  glEnd();
  ASSERT_EQ(PX(8, 16), 0x00ffffffu);
  ASSERT_EQ(PX(8, 24), 0u); /* where the VERTEX_3 map's u = 0 would have been */
  glDisable(GL_MAP1_VERTEX_4);

  /* The grid: glEvalMesh1 as a line strip and glEvalPoint1 at the end of it. */
  glMapGrid1f(2, 2.0f, 4.0f);
  glGetFloatv(GL_MAP1_GRID_DOMAIN, fv);
  ASSERT_FLOAT_NEAR(fv[0], 2.0f, 0.0f); ASSERT_FLOAT_NEAR(fv[1], 4.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glLineWidth(3.0f);
  glEvalMesh1(GL_LINE, 0, 2);
  ASSERT_EQ(PX(12, 20), 0x00ffffffu); /* (-0.25, -0.25), on the curve's first chord */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_POINTS);
  glEvalPoint1(2); /* u = 4 exactly */
  glEnd();
  ASSERT_EQ(PX(24, 24), 0x00ffffffu);
  glDisable(GL_MAP1_VERTEX_3);
  glLineWidth(1.0f);

  /* A flat 2x2 patch over x, y in -0.5..0.5, u along x and v along y: points u-major. */
  const GLfloat patch[12] = {-0.5f, -0.5f, 0.0f,  -0.5f, 0.5f, 0.0f,
                              0.5f, -0.5f, 0.0f,   0.5f, 0.5f, 0.0f};
  glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 6, 2, 0.0f, 1.0f, 3, 2, patch);
  glEnable(GL_MAP2_VERTEX_3);
  glMapGrid2f(2, 0.0f, 1.0f, 2, 0.0f, 1.0f);
  glGetIntegerv(GL_MAP2_GRID_SEGMENTS, iv);
  ASSERT_EQ(iv[0], 2); ASSERT_EQ(iv[1], 2);
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 0, 2, 0, 2);
  ASSERT_EQ(PX(12, 12), 0x00ffffffu);
  ASSERT_EQ(PX(20, 20), 0x00ffffffu);
  ASSERT_EQ(PX(4, 4), 0u);
  /* GL_LINE: the grid lines only - a cell's inside stays dark, a row across it crosses three. */
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_LINE, 0, 2, 0, 2);
  ASSERT_EQ(PX(12, 12), 0u);
  int lit = 0;
  for (int x = 0; x < W; x++) if (PX(x, 12) != 0u) lit++;
  ASSERT_TRUE(lit >= 3 && lit <= 6);
  /* GL_POINT: the nine grid points and nothing between. */
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_POINT, 0, 2, 0, 2);
  ASSERT_EQ(PX(8, 8), 0x00ffffffu);
  ASSERT_EQ(PX(24, 24), 0x00ffffffu);
  ASSERT_EQ(PX(12, 12), 0u);
  /* A sub-range of the grid: only the cell at i 1..2, j 0..1 - the lower right one. */
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 1, 2, 0, 1);
  ASSERT_EQ(PX(20, 20), 0x00ffffffu);
  ASSERT_EQ(PX(12, 20), 0u);
  ASSERT_EQ(PX(20, 12), 0u);

  /* **GL_AUTO_NORMAL** lights the patch with its own normal, du x dv = +z, facing the default
   * light; the current normal, pointing away, is left as it was. Turning the u domain round turns
   * the normal round with it. */
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glNormal3f(0.0f, 0.0f, -1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 0, 2, 0, 2);
  ASSERT_TRUE((PX(16, 16) & 0xffu) < 40u); /* the current normal faces away: ambient only */
  glEnable(GL_AUTO_NORMAL);
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 0, 2, 0, 2);
  ASSERT_TRUE((PX(16, 16) & 0xffu) > 150u);
  glGetFloatv(GL_CURRENT_NORMAL, fv);
  ASSERT_FLOAT_NEAR(fv[2], -1.0f, 0.0f);
  glMap2f(GL_MAP2_VERTEX_3, 1.0f, 0.0f, 6, 2, 0.0f, 1.0f, 3, 2, patch);
  glMapGrid2f(2, 1.0f, 0.0f, 2, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 0, 2, 0, 2);
  ASSERT_TRUE((PX(16, 16) & 0xffu) < 40u);
  glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 6, 2, 0.0f, 1.0f, 3, 2, patch);
  glMapGrid2f(2, 0.0f, 1.0f, 2, 0.0f, 1.0f);
  glDisable(GL_AUTO_NORMAL);
  glDisable(GL_LIGHTING);
  glDisable(GL_LIGHT0);

  /* **The highest texture-coordinate map wins.** Order-1 maps are constants: s = 0.25 from
   * _COORD_1 would sample the red texel, s = 0.75 from _COORD_2 the green one. */
  const GLubyte texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  glEnable(GL_TEXTURE_2D);
  const GLfloat s1 = 0.25f;
  const GLfloat st2[2] = {0.75f, 0.5f};
  glMap2f(GL_MAP2_TEXTURE_COORD_1, 0.0f, 1.0f, 1, 1, 0.0f, 1.0f, 1, 1, &s1);
  glMap2f(GL_MAP2_TEXTURE_COORD_2, 0.0f, 1.0f, 2, 1, 0.0f, 1.0f, 2, 1, st2);
  glEnable(GL_MAP2_TEXTURE_COORD_1);
  glEnable(GL_MAP2_TEXTURE_COORD_2);
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 0, 2, 0, 2);
  ASSERT_EQ(PX(16, 16), 0x0000ff00u);
  glDisable(GL_MAP2_TEXTURE_COORD_2);
  glClear(GL_COLOR_BUFFER_BIT);
  glEvalMesh2(GL_FILL, 0, 2, 0, 2);
  ASSERT_EQ(PX(16, 16), 0x00ff0000u);
  glDisable(GL_MAP2_TEXTURE_COORD_1);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);

  /* **A list keeps the points it was compiled with**, and GL_COMPILE changes nothing until it
   * is called. The mesh is compiled too, and draws when the list runs. */
  GLfloat moving[12];
  for (int i = 0; i < 12; i++) moving[i] = patch[i] * 0.5f; /* a patch half the size */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 6, 2, 0.0f, 1.0f, 3, 2, moving);
  glMapGrid2f(1, 0.0f, 1.0f, 1, 0.0f, 1.0f);
  glEvalMesh2(GL_FILL, 0, 1, 0, 1);
  glEndList();
  for (int i = 0; i < 12; i++) moving[i] = 0.0f;
  glGetMapfv(GL_MAP2_VERTEX_3, GL_COEFF, fv);
  ASSERT_FLOAT_NEAR(fv[0], -0.5f, 0.0f); /* not yet the compiled patch */
  glGetIntegerv(GL_MAP2_GRID_SEGMENTS, iv);
  ASSERT_EQ(iv[0], 2);
  glClear(GL_COLOR_BUFFER_BIT);
  glCallList(list);
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  ASSERT_EQ(PX(10, 10), 0u); /* inside the full patch, outside the half one */
  glGetMapfv(GL_MAP2_VERTEX_3, GL_COEFF, fv);
  ASSERT_FLOAT_NEAR(fv[0], -0.25f, 0.0f);
  glDeleteLists(list, 1);

  /* The attribute stack: GL_EVAL_BIT carries the enables and the grid, GL_ENABLE_BIT the enables. */
  glPushAttrib(GL_EVAL_BIT);
  glDisable(GL_MAP2_VERTEX_3);
  glEnable(GL_AUTO_NORMAL);
  glMapGrid2f(7, 0.0f, 1.0f, 7, 0.0f, 1.0f);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_MAP2_VERTEX_3), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_AUTO_NORMAL), GL_FALSE);
  glGetIntegerv(GL_MAP2_GRID_SEGMENTS, iv);
  ASSERT_EQ(iv[0], 1);
  glPushAttrib(GL_ENABLE_BIT);
  glDisable(GL_MAP2_VERTEX_3);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_MAP2_VERTEX_3), GL_TRUE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Refusals, each leaving the map as it was. */
  glMap1f(GL_MAP1_VERTEX_3, 1.0f, 1.0f, 3, 2, curve);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* empty domain */
  glMap1f(GL_MAP1_VERTEX_3, 0.0f, 1.0f, 3, 0, curve);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* order 0 */
  glMap1f(GL_MAP1_VERTEX_3, 0.0f, 1.0f, 3, 31, curve);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* past GL_MAX_EVAL_ORDER */
  glMap1f(GL_MAP1_VERTEX_3, 0.0f, 1.0f, 2, 2, curve);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* stride shorter than a point */
  glMap1f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 3, 2, curve);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);  /* a 2D target for glMap1 */
  glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 6, 2, 0.0f, 1.0f, 2, 2, patch);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* vstride */
  glGetMapiv(GL_MAP1_VERTEX_3, GL_ORDER, iv);
  ASSERT_EQ(iv[0], 3);
  glGetMapfv(GL_TEXTURE_2D, GL_COEFF, fv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetMapfv(GL_MAP1_VERTEX_3, GL_TEXTURE_2D, fv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glMapGrid1f(0, 0.0f, 1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glEvalMesh1(GL_FILL, 0, 1);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glEvalMesh2(GL_TRIANGLES, 0, 1, 0, 1);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBegin(GL_TRIANGLES);
  glEvalMesh2(GL_FILL, 0, 1, 0, 1);
  glEnd();
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  /* A compiled glMap with a bad stride fails when it runs, as the call would have. */
  const GLuint bad = glGenLists(1);
  glNewList(bad, GL_COMPILE);
  glMap1f(GL_MAP1_VERTEX_3, 0.0f, 1.0f, 2, 2, curve);
  glEndList();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glCallList(bad);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glGetMapiv(GL_MAP1_VERTEX_3, GL_ORDER, iv);
  ASSERT_EQ(iv[0], 3);
  glDeleteLists(bad, 1);

  glDisable(GL_MAP2_VERTEX_3);
  glPointSize(1.0f);
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Selection and feedback, against numbers worked by hand. With identity matrices a vertex's z
 * is its NDC z, so window z is z/2 + 1/2 and a hit record's depth is that times 2^32-1: z = 0 is
 * 2147483648 after rounding. */
static void test_gl_selection_and_feedback(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)
  #define QUAD(x0, y0, x1, y1) do { glBegin(GL_QUADS); glVertex2f(x0, y0); glVertex2f(x1, y0); \
                                    glVertex2f(x1, y1); glVertex2f(x0, y1); glEnd(); } while (0)
  const GLuint Z_HALF = 2147483648u;

  GLint iv[4] = {0};
  glGetIntegerv(GL_RENDER_MODE, iv);
  ASSERT_EQ((GLenum)iv[0], (GLenum)GL_RENDER);
  glGetIntegerv(GL_MAX_NAME_STACK_DEPTH, iv);
  ASSERT_TRUE(iv[0] >= 64);

  /* GL_SELECT before glSelectBuffer is refused and changes nothing. */
  ASSERT_EQ(glRenderMode(GL_SELECT), 0);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glGetIntegerv(GL_RENDER_MODE, iv);
  ASSERT_EQ((GLenum)iv[0], (GLenum)GL_RENDER);

  /* **Nothing is drawn in GL_SELECT, glClear included.** */
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  GLuint sel[64];
  for (int i = 0; i < 64; i++) sel[i] = 0xdeadbeefu;
  glSelectBuffer(64, sel);
  ASSERT_EQ(glRenderMode(GL_SELECT), 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glInitNames();
  glPushName(0);
  glLoadName(1);
  glColor3f(1.0f, 1.0f, 1.0f);
  QUAD(-0.5f, -0.5f, 0.5f, 0.5f);                /* hit, flat at z = 0 */
  glLoadName(2);
  QUAD(2.0f, 2.0f, 3.0f, 3.0f);                  /* wholly outside: no hit */
  glLoadName(3);
  glBegin(GL_TRIANGLES);                          /* hit, z from -0.5 to 0.5 */
  glVertex3f(-0.5f, -0.5f, -0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();
  glLoadName(4);
  glBegin(GL_TRIANGLES);                          /* clipped at the far plane: zmax is 1 */
  glVertex3f(-0.5f, -0.5f, 0.0f); glVertex3f(0.5f, -0.5f, 0.0f); glVertex3f(0.0f, 0.5f, 2.0f);
  glEnd();
  glGetIntegerv(GL_NAME_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], 1);
  ASSERT_EQ(glRenderMode(GL_RENDER), 3);
  ASSERT_EQ(PX(16, 16), 0x00ff0000u); /* the red clear survived the whole pass */
  ASSERT_EQ(sel[0], 1u); ASSERT_EQ(sel[1], Z_HALF); ASSERT_EQ(sel[2], Z_HALF); ASSERT_EQ(sel[3], 1u);
  ASSERT_EQ(sel[4], 1u); ASSERT_EQ(sel[5], 1073741824u); ASSERT_EQ(sel[6], 3221225471u);
  ASSERT_EQ(sel[7], 3u);
  ASSERT_EQ(sel[8], 1u); ASSERT_EQ(sel[9], Z_HALF); ASSERT_EQ(sel[10], 0xffffffffu);
  ASSERT_EQ(sel[11], 4u);
  ASSERT_EQ(sel[12], 0xdeadbeefu);
  glGetIntegerv(GL_NAME_STACK_DEPTH, iv); /* leaving GL_SELECT empties the stack */
  ASSERT_EQ(iv[0], 0);

  /* **A culled polygon is no hit**; lines, points and a valid raster position are hits; names
   * nest, bottom first. */
  glRenderMode(GL_SELECT);
  glInitNames();
  glPushName(5);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glBegin(GL_QUADS);                              /* clockwise: back-facing, culled */
  glVertex2f(-0.5f, -0.5f); glVertex2f(-0.5f, 0.5f); glVertex2f(0.5f, 0.5f); glVertex2f(0.5f, -0.5f);
  glEnd();
  glDisable(GL_CULL_FACE);
  glPushName(6);
  glBegin(GL_LINES);
  glVertex2f(-2.0f, 0.0f); glVertex2f(0.0f, 0.0f); /* half outside: still a hit */
  glEnd();
  glPopName();
  glPushName(7);
  glBegin(GL_POINTS);
  glVertex3f(0.0f, 0.0f, 0.5f);
  glEnd();
  glPopName();
  glPushName(8);
  glRasterPos3f(0.0f, 0.0f, -0.5f);
  glPopName();
  ASSERT_EQ(glRenderMode(GL_RENDER), 3);
  ASSERT_EQ(sel[0], 2u); ASSERT_EQ(sel[3], 5u); ASSERT_EQ(sel[4], 6u);
  ASSERT_EQ(sel[5], 2u); ASSERT_EQ(sel[6], 3221225471u); ASSERT_EQ(sel[9], 7u);
  ASSERT_EQ(sel[10], 2u); ASSERT_EQ(sel[11], 1073741824u); ASSERT_EQ(sel[14], 8u);

  /* **The pick matrix**: a 4x4 pixel box around window (8, 8) - the lower left quarter - picks
   * the quad there and not the one in the upper right. */
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPickMatrix(8.0, 8.0, 4.0, 4.0, vp);
  glMatrixMode(GL_MODELVIEW);
  glRenderMode(GL_SELECT);
  glInitNames();
  glPushName(0);
  glLoadName(10);
  QUAD(-1.0f, -1.0f, 0.0f, 0.0f);
  glLoadName(11);
  QUAD(0.0f, 0.0f, 1.0f, 1.0f);
  ASSERT_EQ(glRenderMode(GL_RENDER), 1);
  ASSERT_EQ(sel[3], 10u);
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);

  /* A list carries the name-stack calls; they run when it does. */
  const GLuint named = glGenLists(1);
  glNewList(named, GL_COMPILE);
  glPushName(21);
  QUAD(-0.5f, -0.5f, 0.5f, 0.5f);
  glPopName();
  glEndList();
  glRenderMode(GL_SELECT);
  glInitNames();
  glCallList(named);
  ASSERT_EQ(glRenderMode(GL_RENDER), 1);
  ASSERT_EQ(sel[0], 1u); ASSERT_EQ(sel[3], 21u);
  glDeleteLists(named, 1);

  /* **Overflow**: as much as fits is written, and glRenderMode answers -1. */
  GLuint small[3] = {0, 0, 0};
  glSelectBuffer(3, small);
  glRenderMode(GL_SELECT);
  glInitNames();
  glPushName(9);
  QUAD(-0.5f, -0.5f, 0.5f, 0.5f);
  ASSERT_EQ(glRenderMode(GL_RENDER), -1);
  ASSERT_EQ(small[0], 1u); ASSERT_EQ(small[1], Z_HALF);
  glSelectBuffer(64, sel);

  /* The name stack's refusals - and outside GL_SELECT, no refusal at all. */
  glPopName();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glRenderMode(GL_SELECT);
  glInitNames();
  glPopName();
  ASSERT_EQ(glGetError(), GL_STACK_UNDERFLOW);
  glLoadName(1);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  for (int i = 0; i < 64; i++) glPushName((GLuint)i);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPushName(64);
  ASSERT_EQ(glGetError(), GL_STACK_OVERFLOW);
  glSelectBuffer(64, sel);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* not while selecting */
  glBegin(GL_TRIANGLES);
  ASSERT_EQ(glRenderMode(GL_RENDER), 0);
  glEnd();
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* not inside glBegin */
  ASSERT_EQ(glRenderMode(GL_RENDER), 0);          /* no hits */

  /* Feedback, GL_2D: every token in order. A point at the centre is (16, 16); a line from x = -2
   * is clipped at the edge, x = 0; the second segment of a strip is a plain GL_LINE_TOKEN. */
  GLfloat fbk[128];
  for (int i = 0; i < 128; i++) fbk[i] = -99.0f;
  ASSERT_EQ(glRenderMode(GL_FEEDBACK), 0);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* no feedback buffer yet */
  glFeedbackBuffer(128, GL_2D, fbk);
  glRenderMode(GL_FEEDBACK);
  glBegin(GL_POINTS); glVertex2f(0.0f, 0.0f); glEnd();
  glPassThrough(7.0f);
  glBegin(GL_LINES); glVertex2f(-2.0f, 0.0f); glVertex2f(0.0f, 0.0f); glEnd();
  glBegin(GL_LINE_STRIP);
  glVertex2f(-0.5f, 0.0f); glVertex2f(0.0f, 0.0f); glVertex2f(0.5f, 0.0f);
  glEnd();
  glBegin(GL_TRIANGLES);
  glVertex2f(-0.5f, -0.5f); glVertex2f(0.5f, -0.5f); glVertex2f(0.0f, 0.5f);
  glEnd();
  glRasterPos2f(0.0f, 0.0f);
  glBitmap(0, 0, 0.0f, 0.0f, 4.0f, 0.0f, NULL);
  const GLfloat want2d[] = {
      (GLfloat)GL_POINT_TOKEN, 16, 16,
      (GLfloat)GL_PASS_THROUGH_TOKEN, 7,
      (GLfloat)GL_LINE_RESET_TOKEN, 0, 16, 16, 16,
      (GLfloat)GL_LINE_RESET_TOKEN, 8, 16, 16, 16,
      (GLfloat)GL_LINE_TOKEN, 16, 16, 24, 16,
      (GLfloat)GL_POLYGON_TOKEN, 3, 8, 8, 24, 8, 16, 24,
      (GLfloat)GL_BITMAP_TOKEN, 16, 16,
  };
  const int n2d = (int)(sizeof(want2d) / sizeof(want2d[0]));
  ASSERT_EQ(glRenderMode(GL_RENDER), n2d);
  for (int i = 0; i < n2d; i++) ASSERT_FLOAT_NEAR(fbk[i], want2d[i], 1e-4f);
  ASSERT_FLOAT_NEAR(fbk[n2d], -99.0f, 0.0f);
  GLfloat rp[4];
  glGetFloatv(GL_CURRENT_RASTER_POSITION, rp); /* the bitmap still moved it */
  ASSERT_FLOAT_NEAR(rp[0], 20.0f, 1e-4f);

  /* GL_3D_COLOR and GL_4D_COLOR_TEXTURE: z, the colour, w (the clip w), the texture coordinate. */
  glFeedbackBuffer(128, GL_4D_COLOR_TEXTURE, fbk);
  glGetIntegerv(GL_FEEDBACK_BUFFER_TYPE, iv);
  ASSERT_EQ((GLenum)iv[0], (GLenum)GL_4D_COLOR_TEXTURE);
  glRenderMode(GL_FEEDBACK);
  glColor4f(1.0f, 0.5f, 0.25f, 1.0f);
  glTexCoord2f(0.25f, 0.75f);
  glBegin(GL_POINTS); glVertex4f(0.0f, 0.0f, 1.0f, 2.0f); glEnd(); /* NDC z 0.5: window 0.75 */
  ASSERT_EQ(glRenderMode(GL_RENDER), 13);
  const GLfloat want4d[] = {(GLfloat)GL_POINT_TOKEN, 16, 16, 0.75f, 2.0f,
                            1.0f, 0.5f, 0.25f, 1.0f, 0.25f, 0.75f, 0.0f, 1.0f};
  for (int i = 0; i < 13; i++) ASSERT_FLOAT_NEAR(fbk[i], want4d[i], 1e-5f);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glTexCoord2f(0.0f, 0.0f);

  /* GL_POLYGON_MODE GL_LINE reports the polygon's own sides, the first as a reset. */
  glFeedbackBuffer(128, GL_2D, fbk);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glRenderMode(GL_FEEDBACK);
  QUAD(-0.5f, -0.5f, 0.5f, 0.5f);
  ASSERT_EQ(glRenderMode(GL_RENDER), 20); /* four sides of five values: no diagonal */
  ASSERT_FLOAT_NEAR(fbk[0], (GLfloat)GL_LINE_RESET_TOKEN, 0.0f);
  ASSERT_FLOAT_NEAR(fbk[5], (GLfloat)GL_LINE_TOKEN, 0.0f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  /* Overflow in feedback, then refusals. */
  GLfloat tiny[2];
  glFeedbackBuffer(2, GL_2D, tiny);
  glRenderMode(GL_FEEDBACK);
  glBegin(GL_POINTS); glVertex2f(0.0f, 0.0f); glEnd();
  ASSERT_EQ(glRenderMode(GL_RENDER), -1);
  ASSERT_FLOAT_NEAR(tiny[0], (GLfloat)GL_POINT_TOKEN, 0.0f);
  glFeedbackBuffer(8, GL_RGBA, fbk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glFeedbackBuffer(-1, GL_2D, fbk);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glRenderMode(GL_POINTS);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  GLvoid *ptr = NULL;
  glGetPointerv(GL_SELECTION_BUFFER_POINTER, &ptr);
  ASSERT_TRUE(ptr == (GLvoid *)sel);

  /* Back in GL_RENDER, drawing draws again. */
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD(-0.5f, -0.5f, 0.5f, 0.5f);
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);

  #undef QUAD
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Pixel transfer and the pixel maps, on every path a colour rectangle takes: drawn, copied,
 * uploaded, copied into a texture, and read back. A byte through a scale of 0.5 is 127.5, which
 * rounds to 128 (0x80). */
static void test_gl_pixel_transfer_and_maps(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  /* The raster position (0, 0) is window (16, 16): framebuffer row 15. */
  #define RP() (fb[15 * W + 16] & 0x00ffffffu)
  const GLubyte white[4] = {255, 255, 255, 255};

  /* Defaults: nothing changes a pixel. */
  GLfloat f = -1.0f;
  GLint iv = -1;
  glGetFloatv(GL_RED_SCALE, &f);    ASSERT_FLOAT_NEAR(f, 1.0f, 0.0f);
  glGetFloatv(GL_ALPHA_BIAS, &f);   ASSERT_FLOAT_NEAR(f, 0.0f, 0.0f);
  glGetIntegerv(GL_MAP_COLOR, &iv); ASSERT_EQ(iv, 0);
  glGetIntegerv(GL_PIXEL_MAP_R_TO_R_SIZE, &iv); ASSERT_EQ(iv, 1);
  glGetIntegerv(GL_MAX_PIXEL_MAP_TABLE, &iv);   ASSERT_TRUE(iv >= 32);
  GLfloat map[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  glGetPixelMapfv(GL_PIXEL_MAP_R_TO_R, map);
  ASSERT_FLOAT_NEAR(map[0], 0.0f, 0.0f);
  ASSERT_FLOAT_NEAR(map[1], 9.0f, 0.0f); /* one entry, and only one written */

  /* **glDrawPixels**: red halved, green biased away. */
  glRasterPos2f(0.0f, 0.0f);
  glPixelTransferf(GL_RED_SCALE, 0.5f);
  glPixelTransferf(GL_GREEN_BIAS, -1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
  ASSERT_EQ(RP(), 0x008000ffu);
  glPixelTransferf(GL_GREEN_BIAS, 0.0f);

  /* **glReadPixels** applies it too: white read back with red halved. */
  glPixelTransferf(GL_RED_SCALE, 1.0f);
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glPixelTransferf(GL_RED_SCALE, 0.5f);
  GLubyte px[4] = {0, 0, 0, 0};
  glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ASSERT_EQ(px[0], 128); ASSERT_EQ(px[1], 255);

  /* **A texture copy is transferred once**, not once reading and again uploading: 128, not 64. */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 1, 1, 0);
  glPixelTransferf(GL_RED_SCALE, 1.0f);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); /* not transferred itself */
  ASSERT_EQ(px[0], 128); ASSERT_EQ(px[1], 255);

  /* **An upload**: red scaled to nothing on the way in, so the texture is cyan. */
  glPixelTransferf(GL_RED_SCALE, 0.0f);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glPixelTransferf(GL_RED_SCALE, 1.0f);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ASSERT_EQ(px[0], 0); ASSERT_EQ(px[1], 255); ASSERT_EQ(px[2], 255);
  glDeleteTextures(1, &tex);

  /* **glCopyPixels**: the white at (16, 16) copied one to the right with blue biased off. */
  glPixelTransferf(GL_BLUE_BIAS, -1.0f);
  glRasterPos2f(1.0f / 16.0f, 0.0f); /* window (17, 16) */
  glCopyPixels(16, 16, 1, 1, GL_COLOR);
  ASSERT_EQ(fb[15 * W + 17] & 0x00ffffffu, 0x00ffff00u);
  glPixelTransferf(GL_BLUE_BIAS, 0.0f);

  /* **GL_MAP_COLOR**: red through a two-entry inverting table, the others through identity -
   * magenta becomes blue. Every map starts as a single 0, so leaving one unset would zero it. */
  const GLfloat invert[2] = {1.0f, 0.0f}, identity[2] = {0.0f, 1.0f};
  glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 2, invert);
  glPixelMapfv(GL_PIXEL_MAP_G_TO_G, 2, identity);
  glPixelMapfv(GL_PIXEL_MAP_B_TO_B, 2, identity);
  glPixelMapfv(GL_PIXEL_MAP_A_TO_A, 2, identity);
  glPixelTransferi(GL_MAP_COLOR, GL_TRUE);
  const GLubyte magenta[4] = {255, 0, 255, 255};
  glRasterPos2f(0.0f, 0.0f);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, magenta);
  ASSERT_EQ(RP(), 0x000000ffu);
  glPixelTransferi(GL_MAP_COLOR, GL_FALSE);

  /* **Luminance read from colour is R + G + B**: pure blue is 255, not 0. */
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  GLubyte lum = 7;
  glReadPixels(16, 16, 1, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, &lum);
  ASSERT_EQ(lum, 255);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  /* The maps: stored clamped, converted by the integer forms, read back as they were set. */
  const GLfloat wide[2] = {2.0f, -1.0f};
  glPixelMapfv(GL_PIXEL_MAP_G_TO_G, 2, wide);
  glGetPixelMapfv(GL_PIXEL_MAP_G_TO_G, map);
  ASSERT_FLOAT_NEAR(map[0], 1.0f, 0.0f); ASSERT_FLOAT_NEAR(map[1], 0.0f, 0.0f);
  const GLuint ui[2] = {0u, 0xffffffffu};
  glPixelMapuiv(GL_PIXEL_MAP_B_TO_B, 2, ui);
  glGetPixelMapfv(GL_PIXEL_MAP_B_TO_B, map);
  ASSERT_FLOAT_NEAR(map[1], 1.0f, 1e-6f);
  GLushort us[2] = {9, 9};
  glGetPixelMapusv(GL_PIXEL_MAP_B_TO_B, us);
  ASSERT_EQ(us[0], 0); ASSERT_EQ(us[1], 65535);
  const GLuint indices[2] = {5u, 7u};
  glPixelMapuiv(GL_PIXEL_MAP_I_TO_I, 2, indices); /* an index map: taken as integers */
  GLuint back[2] = {0, 0};
  glGetPixelMapuiv(GL_PIXEL_MAP_I_TO_I, back);
  ASSERT_EQ(back[0], 5u); ASSERT_EQ(back[1], 7u);
  glGetIntegerv(GL_PIXEL_MAP_I_TO_I_SIZE, &iv);
  ASSERT_EQ(iv, 2);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Refusals, each leaving the map as it was. */
  const GLfloat three[3] = {0.0f, 0.5f, 1.0f};
  glPixelMapfv(GL_PIXEL_MAP_I_TO_I, 3, three);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* index maps are powers of two - I_TO_I too */
  glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 3, three);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);      /* colour maps are any size */
  glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 0, three);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 257, three);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glPixelMapfv(GL_TEXTURE_2D, 2, three);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetPixelMapfv(GL_TEXTURE_2D, map);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glPixelTransferf(GL_TEXTURE_2D, 1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_PIXEL_MAP_I_TO_I_SIZE, &iv);
  ASSERT_EQ(iv, 2);

  /* The state only colour-index and stencil pixels would use: kept and answered. */
  glPixelTransferi(GL_INDEX_SHIFT, 3);
  glPixelTransferi(GL_MAP_STENCIL, GL_TRUE);
  glGetIntegerv(GL_INDEX_SHIFT, &iv);  ASSERT_EQ(iv, 3);
  glGetIntegerv(GL_MAP_STENCIL, &iv);  ASSERT_EQ(iv, 1);

  /* GL_PIXEL_MODE_BIT carries the transfer state and the zoom, not the maps. */
  glPushAttrib(GL_PIXEL_MODE_BIT);
  glPixelTransferf(GL_RED_SCALE, 0.25f);
  glPixelTransferi(GL_INDEX_SHIFT, 0);
  glPixelZoom(2.0f, 2.0f);
  glPixelMapfv(GL_PIXEL_MAP_A_TO_A, 2, invert);
  glPopAttrib();
  glGetFloatv(GL_RED_SCALE, &f);    ASSERT_FLOAT_NEAR(f, 1.0f, 0.0f);
  glGetIntegerv(GL_INDEX_SHIFT, &iv); ASSERT_EQ(iv, 3);
  glGetFloatv(GL_ZOOM_X, &f);       ASSERT_FLOAT_NEAR(f, 1.0f, 0.0f);
  glGetPixelMapfv(GL_PIXEL_MAP_A_TO_A, map);
  ASSERT_FLOAT_NEAR(map[0], 1.0f, 0.0f);

  /* **A list keeps the map it was compiled with**, and applies nothing until it runs. */
  GLfloat moving[2] = {0.25f, 0.75f};
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 2, moving);
  glPixelTransferf(GL_RED_SCALE, 3.0f);
  glEndList();
  moving[0] = 0.0f;
  glGetFloatv(GL_RED_SCALE, &f);    ASSERT_FLOAT_NEAR(f, 1.0f, 0.0f);
  glCallList(list);
  glGetFloatv(GL_RED_SCALE, &f);    ASSERT_FLOAT_NEAR(f, 3.0f, 0.0f);
  glGetPixelMapfv(GL_PIXEL_MAP_R_TO_R, map);
  ASSERT_FLOAT_NEAR(map[0], 0.25f, 0.0f);
  glDeleteLists(list, 1);

  #undef RP
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Line and polygon stipple. A horizontal line across the 32-pixel window is 32 fragments, one
 * per column, so fragment s is column s - which makes the dash pattern readable pixel by pixel.
 * Width 3 keeps a row of it on window row 15 (framebuffer row 16) whatever the rounding. */
static void test_gl_line_and_polygon_stipple(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glColor3f(1.0f, 1.0f, 1.0f);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)
  #define LIT(x) (PX(x, 16) != 0u)

  GLint iv = 0;
  ASSERT_EQ(glIsEnabled(GL_LINE_STIPPLE), GL_FALSE);
  ASSERT_EQ(glIsEnabled(GL_POLYGON_STIPPLE), GL_FALSE);
  glGetIntegerv(GL_LINE_STIPPLE_PATTERN, &iv); ASSERT_EQ(iv, 0xffff);
  glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &iv);  ASSERT_EQ(iv, 1);
  GLubyte back[32 * 8];
  glGetPolygonStipple(back);
  ASSERT_EQ(back[0], 0xff); ASSERT_EQ(back[127], 0xff);

  /* Four on, four off. */
  glLineWidth(3.0f);
  glEnable(GL_LINE_STIPPLE);
  glLineStipple(1, 0x0f0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES); glVertex2f(-1.0f, 0.0f); glVertex2f(1.0f, 0.0f); glEnd();
  ASSERT_TRUE(LIT(1)); ASSERT_TRUE(!LIT(5)); ASSERT_TRUE(LIT(9)); ASSERT_TRUE(!LIT(13));
  ASSERT_TRUE(LIT(18)); ASSERT_TRUE(!LIT(30));
  /* The factor stretches each bit: two on-bits of 0x0003 at factor 4 are eight columns. */
  glLineStipple(4, 0x0003);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES); glVertex2f(-1.0f, 0.0f); glVertex2f(1.0f, 0.0f); glEnd();
  ASSERT_TRUE(LIT(7)); ASSERT_TRUE(!LIT(8)); ASSERT_TRUE(!LIT(31));

  /* **The count carries across a strip and starts again for each separate line.** 0x00ff at
   * factor 1 is eight on, eight off; the joint is at column 12. Carried on, column 13 is s = 13,
   * an off bit; started again it is s = 1, an on one. */
  glLineStipple(1, 0x00ff);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINE_STRIP);
  glVertex2f(-1.0f, 0.0f); glVertex2f(-0.25f, 0.0f); glVertex2f(1.0f, 0.0f);
  glEnd();
  ASSERT_TRUE(LIT(3)); ASSERT_TRUE(!LIT(13)); ASSERT_TRUE(LIT(17)); ASSERT_TRUE(!LIT(26));
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES);
  glVertex2f(-1.0f, 0.0f); glVertex2f(-0.25f, 0.0f);
  glVertex2f(-0.25f, 0.0f); glVertex2f(1.0f, 0.0f);
  glEnd();
  ASSERT_TRUE(LIT(13)); ASSERT_TRUE(!LIT(21));

  /* Off again: solid. */
  glDisable(GL_LINE_STIPPLE);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES); glVertex2f(-1.0f, 0.0f); glVertex2f(1.0f, 0.0f); glEnd();
  ASSERT_TRUE(LIT(13)); ASSERT_TRUE(LIT(21));
  glLineWidth(1.0f);

  /* The factor is clamped, not refused. */
  glLineStipple(0, 0x1234);
  glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &iv); ASSERT_EQ(iv, 1);
  glLineStipple(1000, 0x1234);
  glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &iv); ASSERT_EQ(iv, 256);
  glGetIntegerv(GL_LINE_STIPPLE_PATTERN, &iv); ASSERT_EQ(iv, 0x1234);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The polygon stipple**: a checkerboard, the mask's first row the window's bottom row, its
   * first byte's top bit column 0. Unpacked at an alignment of 8, so each row's last four bytes
   * are padding that must not be read. */
  GLubyte mask[32 * 8];
  for (int r = 0; r < 32; r++) {
    for (int c = 0; c < 8; c++) mask[r * 8 + c] = (GLubyte)(c < 4 ? ((r & 1) ? 0x55 : 0xaa) : 0x00);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
  glPolygonStipple(mask);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glEnable(GL_POLYGON_STIPPLE);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(PX(0, 31), 0x00ffffffu); ASSERT_EQ(PX(1, 31), 0u);  /* window row 0 */
  ASSERT_EQ(PX(0, 30), 0u);          ASSERT_EQ(PX(1, 30), 0x00ffffffu); /* window row 1 */
  ASSERT_EQ(PX(20, 0), 0u);          ASSERT_EQ(PX(21, 0), 0x00ffffffu); /* row 31: odd */
  /* Lines are not polygons, and nor is a polygon's outline. */
  glClear(GL_COLOR_BUFFER_BIT);
  glLineWidth(3.0f);
  glBegin(GL_LINES); glVertex2f(-1.0f, 0.0f); glVertex2f(1.0f, 0.0f); glEnd();
  ASSERT_TRUE(LIT(4)); ASSERT_TRUE(LIT(5));
  glLineWidth(1.0f);
  /* Read back at the pack alignment - here 1, so tight. */
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetPolygonStipple(back);
  ASSERT_EQ(back[0], 0xaa); ASSERT_EQ(back[4], 0x55); ASSERT_EQ(back[127], 0x55);

  /* The attribute stack: GL_POLYGON_STIPPLE_BIT carries the mask, GL_POLYGON_BIT the enable,
   * GL_LINE_BIT the pattern and its enable. */
  GLubyte ones[128];
  for (int i = 0; i < 128; i++) ones[i] = 0xff;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPushAttrib(GL_POLYGON_STIPPLE_BIT | GL_POLYGON_BIT | GL_LINE_BIT);
  glPolygonStipple(ones);
  glDisable(GL_POLYGON_STIPPLE);
  glEnable(GL_LINE_STIPPLE);
  glLineStipple(2, 0x0001);
  glPopAttrib();
  glGetPolygonStipple(back);
  ASSERT_EQ(back[0], 0xaa);
  ASSERT_EQ(glIsEnabled(GL_POLYGON_STIPPLE), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_LINE_STIPPLE), GL_FALSE);
  glGetIntegerv(GL_LINE_STIPPLE_PATTERN, &iv); ASSERT_EQ(iv, 0x1234);

  /* **A list keeps the mask it was compiled with.** */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glPolygonStipple(ones);
  glLineStipple(3, 0x00f0);
  glEndList();
  for (int i = 0; i < 128; i++) ones[i] = 0x00;
  glGetPolygonStipple(back);
  ASSERT_EQ(back[0], 0xaa); /* not yet */
  glCallList(list);
  glGetPolygonStipple(back);
  ASSERT_EQ(back[0], 0xff); ASSERT_EQ(back[127], 0xff);
  glGetIntegerv(GL_LINE_STIPPLE_REPEAT, &iv); ASSERT_EQ(iv, 3);
  glDeleteLists(list, 1);
  glDisable(GL_POLYGON_STIPPLE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);

  #undef LIT
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The accumulation buffer, the way programs use it - frames added in at a weight each, then
 * returned - and each operation on its own. 0.5 of full intensity lands on 128, 0.25 on 64 and
 * 0.75 on 191, allowing one step for the 16-bit rounding in between. */
static void test_gl_accumulation_buffer(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)
  #define CH(v, s) ((int)(((v) >> (s)) & 0xffu))
  #define NEAR(v, want) ((v) >= (want) - 1 && (v) <= (want) + 1)

  GLint iv = 0;
  glGetIntegerv(GL_ACCUM_RED_BITS, &iv); ASSERT_EQ(iv, 16);
  glGetIntegerv(GL_RED_BITS, &iv);       ASSERT_EQ(iv, 8);
  glGetIntegerv(GL_DEPTH_BITS, &iv);     ASSERT_EQ(iv, 32);
  GLfloat cv[4] = {9, 9, 9, 9};
  glGetFloatv(GL_ACCUM_CLEAR_VALUE, cv);
  ASSERT_TRUE(cv[0] == 0.0f && cv[3] == 0.0f);

  /* **Two frames, half each**: red and blue make half-red, half-blue. */
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_ACCUM_BUFFER_BIT);
  glAccum(GL_ACCUM, 0.5f);
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_ACCUM, 0.5f);
  glAccum(GL_RETURN, 1.0f);
  uint32_t p = PX(5, 5);
  ASSERT_TRUE(NEAR(CH(p, 16), 128) && CH(p, 8) == 0 && NEAR(CH(p, 0), 128));

  /* GL_LOAD replaces, GL_MULT scales, GL_ADD offsets. */
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_LOAD, 1.0f);
  glAccum(GL_MULT, 0.5f);
  glAccum(GL_ADD, 0.25f);
  glAccum(GL_RETURN, 1.0f);
  p = PX(5, 5);
  ASSERT_TRUE(NEAR(CH(p, 16), 64) && NEAR(CH(p, 8), 191) && NEAR(CH(p, 0), 64));
  /* GL_RETURN scales too, and clamps. */
  glAccum(GL_RETURN, 4.0f);
  ASSERT_EQ(PX(5, 5), 0x00ffffffu);

  /* **The colour mask applies to GL_RETURN**: green stays as the colour buffer had it. */
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE);
  glAccum(GL_RETURN, 4.0f);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  ASSERT_EQ(PX(5, 5), 0x00ff00ffu);

  /* **Past full scale it clamps, it does not wrap.** White loaded, then white added again: the
   * sum is 2, held at 1 - where 16-bit arithmetic that wrapped would come back near -1, black. */
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_LOAD, 1.0f);
  glAccum(GL_ACCUM, 1.0f);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_RETURN, 1.0f);
  ASSERT_EQ(PX(5, 5), 0x00ffffffu);

  /* **The scissor box bounds a clear and an operation**: only the left half is cleared to white.
   * The window's left half is framebuffer columns 0..15. */
  glClearAccum(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_ACCUM_BUFFER_BIT);
  glClearAccum(1.0f, 1.0f, 1.0f, 1.0f);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, 16, 32);
  glClear(GL_ACCUM_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  glClear(GL_COLOR_BUFFER_BIT);
  glAccum(GL_RETURN, 1.0f);
  ASSERT_EQ(PX(5, 5), 0x00ffffffu);
  ASSERT_EQ(PX(20, 5), 0u);

  /* The clear value is clamped to -1..1 as it is set; a list carries both calls. */
  glClearAccum(2.0f, -2.0f, 0.5f, 0.0f);
  glGetFloatv(GL_ACCUM_CLEAR_VALUE, cv);
  ASSERT_TRUE(cv[0] == 1.0f && cv[1] == -1.0f && cv[2] == 0.5f);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glClearAccum(0.0f, 0.0f, 0.0f, 0.0f);
  glAccum(GL_RETURN, 1.0f);
  glEndList();
  glGetFloatv(GL_ACCUM_CLEAR_VALUE, cv);
  ASSERT_TRUE(cv[0] == 1.0f); /* not yet */
  glCallList(list);
  glGetFloatv(GL_ACCUM_CLEAR_VALUE, cv);
  ASSERT_TRUE(cv[0] == 0.0f);
  glDeleteLists(list, 1);

  /* Refusals. */
  glAccum(GL_TEXTURE_2D, 1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBegin(GL_TRIANGLES);
  glAccum(GL_RETURN, 1.0f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glClear(0x00100000u);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  #undef NEAR
  #undef CH
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glClear through the scissor box and the write masks - all of which it ignored until
 * 2026-09-19 - and unmoved by any per-fragment state it is not subject to. */
static void test_gl_clear_keeps_to_scissor_and_masks(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)
  #define DEPTH(x, y) (ctx->depth_buffer[(y) * W + (x)])
  /* Window (4, 4) is framebuffer (4, 27); window (20, 20) is framebuffer (20, 11). */

  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClearDepth(1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  ASSERT_EQ(PX(4, 27), 0x00ff0000u);
  ASSERT_EQ(PX(20, 11), 0x00ff0000u);

  /* **The scissor box**: green and depth 0.25 in the lower-left 16x16 only. */
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, 16, 16);
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glClearDepth(0.25);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  ASSERT_EQ(PX(4, 27), 0x0000ff00u);
  ASSERT_EQ(PX(20, 11), 0x00ff0000u);
  ASSERT_FLOAT_NEAR(DEPTH(4, 27), 0.25f, 1e-6f);
  ASSERT_FLOAT_NEAR(DEPTH(20, 11), 1.0f, 1e-6f);

  /* **The colour mask**: blue written, red kept. */
  glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  ASSERT_EQ(PX(20, 11), 0x00ff00ffu);
  ASSERT_EQ(PX(4, 27), 0x000000ffu); /* green cleared to 0 - it was not masked */

  /* **The depth mask** drops the depth clear. */
  glDepthMask(GL_FALSE);
  glClearDepth(0.5);
  glClear(GL_DEPTH_BUFFER_BIT);
  glDepthMask(GL_TRUE);
  ASSERT_FLOAT_NEAR(DEPTH(20, 11), 1.0f, 1e-6f);

  /* **None of the per-fragment state a clear is not subject to touches it** - blending, an
   * alpha test that rejects everything, a clip plane that cuts the whole window, a depth test
   * that would fail, culling of the quad's winding, a texture - and all of it is back afterwards,
   * with the matrices, viewport and depth range the clear set aside. */
  glEnable(GL_BLEND);
  glBlendFunc(GL_ZERO, GL_ONE);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_NEVER, 0.0f);
  const GLdouble cut[4] = {0.0, 0.0, 0.0, -1.0};
  glClipPlane(GL_CLIP_PLANE0, cut);
  glEnable(GL_CLIP_PLANE0);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_NEVER);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_FRONT_AND_BACK);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTranslatef(5.0f, 0.0f, 0.0f);
  glViewport(0, 0, 8, 8);
  glDepthRange(0.5, 0.75);
  glColor3f(0.25f, 0.5f, 0.75f);
  glEnable(GL_SCISSOR_TEST);
  glScissor(16, 16, 16, 16);
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClearDepth(0.125);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  ASSERT_EQ(PX(20, 11), 0x00ffffffu);
  ASSERT_FLOAT_NEAR(DEPTH(20, 11), 0.125f, 1e-6f);
  ASSERT_EQ(PX(4, 27), 0x000000ffu); /* outside the box */
  ASSERT_EQ(glIsEnabled(GL_BLEND), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_ALPHA_TEST), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_CLIP_PLANE0), GL_TRUE);
  ASSERT_EQ(glIsEnabled(GL_CULL_FACE), GL_TRUE);
  GLint iv[4];
  glGetIntegerv(GL_DEPTH_FUNC, iv);   ASSERT_EQ((GLenum)iv[0], (GLenum)GL_NEVER);
  glGetIntegerv(GL_VIEWPORT, iv);     ASSERT_EQ(iv[2], 8);
  GLfloat fv[16];
  glGetFloatv(GL_DEPTH_RANGE, fv);    ASSERT_FLOAT_NEAR(fv[0], 0.5f, 0.0f);
  glGetFloatv(GL_MODELVIEW_MATRIX, fv); ASSERT_FLOAT_NEAR(fv[12], 5.0f, 0.0f);
  glGetFloatv(GL_CURRENT_COLOR, fv);  ASSERT_FLOAT_NEAR(fv[2], 0.75f, 0.0f);
  glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_CLIP_PLANE0);
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
  glLoadIdentity(); glViewport(0, 0, W, H); glDepthRange(0.0, 1.0);

  /* A box wholly off the surface clears nothing; a clear inside glBegin is refused. */
  glEnable(GL_SCISSOR_TEST);
  glScissor(100, 100, 8, 8);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  ASSERT_EQ(PX(20, 11), 0x00ffffffu);
  glBegin(GL_TRIANGLES);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnd();
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  ASSERT_EQ(PX(20, 11), 0x00ffffffu);

  #undef DEPTH
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* 3D textures on the software rasteriser. A 2x2x2 volume whose slice 0 is red and slice 1 green:
 * slice centres sit at r = 0.25 and 0.75, so nearest filtering picks the slice and linear
 * filtering at r = 0.5 is half of each. The quad carries one (s, t, r) at every corner, so the
 * whole window samples the same point. */
static void test_gl_texture_3d(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glColor3f(1.0f, 1.0f, 1.0f);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)
  #define DRAW_AT(s_, t_, r_) do { glClear(GL_COLOR_BUFFER_BIT); glTexCoord3f(s_, t_, r_); \
      glRectf(-1.0f, -1.0f, 1.0f, 1.0f); } while (0)

  GLint iv = 0;
  glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &iv); ASSERT_TRUE(iv >= 16);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_3D), GL_FALSE);

  GLubyte vol[2 * 2 * 2 * 4];
  for (int i = 0; i < 8; i++) {
    const int slice = i / 4;
    vol[i * 4 + 0] = slice ? 0 : 255; vol[i * 4 + 1] = slice ? 255 : 0;
    vol[i * 4 + 2] = 0; vol[i * 4 + 3] = 255;
  }
  GLuint t3 = 0;
  glGenTextures(1, &t3);
  glBindTexture(GL_TEXTURE_3D, t3);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH, &iv); ASSERT_EQ(iv, 2);
  glGetIntegerv(GL_TEXTURE_BINDING_3D, &iv); ASSERT_EQ((GLuint)iv, t3);

  /* **r picks the slice.** And a 2D texture enabled alongside - blue - loses to the volume. */
  const GLubyte blue[4] = {0, 0, 255, 255};
  GLuint t2 = 0;
  glGenTextures(1, &t2);
  glBindTexture(GL_TEXTURE_2D, t2);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_TEXTURE_3D);
  DRAW_AT(0.5f, 0.5f, 0.25f);
  ASSERT_EQ(PX(16, 16), 0x00ff0000u);
  DRAW_AT(0.5f, 0.5f, 0.75f);
  ASSERT_EQ(PX(16, 16), 0x0000ff00u);
  glDisable(GL_TEXTURE_3D);
  DRAW_AT(0.5f, 0.5f, 0.75f);
  ASSERT_EQ(PX(16, 16), 0x000000ffu); /* 2D again */
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_TEXTURE_3D);

  /* **Linear between slices**: r = 0.5 is half red, half green. */
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  DRAW_AT(0.5f, 0.5f, 0.5f);
  uint32_t p = PX(16, 16);
  ASSERT_TRUE(((p >> 16) & 0xffu) >= 126u && ((p >> 16) & 0xffu) <= 129u);
  ASSERT_TRUE(((p >> 8) & 0xffu) >= 126u && ((p >> 8) & 0xffu) <= 129u);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

  /* **r wraps by GL_TEXTURE_WRAP_R**: 1.25 repeats to 0.25 (red), or clamps to the last slice. */
  DRAW_AT(0.5f, 0.5f, 1.25f);
  ASSERT_EQ(PX(16, 16), 0x00ff0000u);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glGetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, &iv);
  ASSERT_EQ((GLenum)iv, (GLenum)GL_CLAMP_TO_EDGE);
  DRAW_AT(0.5f, 0.5f, 1.25f);
  ASSERT_EQ(PX(16, 16), 0x0000ff00u);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);

  /* **The texture matrix moves r too**: 0.25 translated by a half samples the green slice. */
  glMatrixMode(GL_TEXTURE);
  glTranslatef(0.0f, 0.0f, 0.5f);
  glMatrixMode(GL_MODELVIEW);
  DRAW_AT(0.5f, 0.5f, 0.25f);
  ASSERT_EQ(PX(16, 16), 0x0000ff00u);
  glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);

  /* glTexSubImage3D replaces slice 1 with blue; glGetTexImage reads the volume back in order. */
  const GLubyte blues[2 * 2 * 4] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
  glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 1, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, blues);
  DRAW_AT(0.5f, 0.5f, 0.75f);
  ASSERT_EQ(PX(16, 16), 0x000000ffu);
  GLubyte back[2 * 2 * 3 * 4];
  glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(back[0], 255); ASSERT_EQ(back[16 + 2], 255); ASSERT_EQ(back[16 + 0], 0);

  /* **GL_UNPACK_IMAGE_HEIGHT**: the caller's slices three rows apart, the third row padding. */
  GLubyte padded[2 * 3 * 2 * 4];
  memset(padded, 7, sizeof(padded));
  for (int z = 0; z < 2; z++) {
    for (int y = 0; y < 2; y++) {
      for (int x = 0; x < 2; x++) {
        GLubyte *q = &padded[((z * 3 + y) * 2 + x) * 4];
        q[0] = (GLubyte)(z ? 10 : 20); q[1] = 0; q[2] = 0; q[3] = 255;
      }
    }
  }
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 3);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, padded);
  glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
  glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(back[0], 20); ASSERT_EQ(back[12], 20); ASSERT_EQ(back[16], 10); ASSERT_EQ(back[28], 10);

  /* glCopyTexSubImage3D: a white framebuffer pixel into texel (1, 1) of slice 1. */
  glDisable(GL_TEXTURE_3D);
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glCopyTexSubImage3D(GL_TEXTURE_3D, 0, 1, 1, 1, 5, 5, 1, 1);
  glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(back[28], 255); ASSERT_EQ(back[28 + 1], 255); ASSERT_EQ(back[16], 10);
  glEnable(GL_TEXTURE_3D);

  /* **Completeness counts depth**: a mipmapping filter wants level 1 at 1x1x1. Without it the
   * volume samples as none - the vertex colour, white; with it, level 1's yellow. */
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glPushMatrix();
  glScalef(0.07f, 0.07f, 1.0f); /* a quad about two pixels across over the centre */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glTexCoord3f(0.0f, 0.0f, 0.25f); glVertex2f(-1.0f, -1.0f);
  glTexCoord3f(1.0f, 0.0f, 0.25f); glVertex2f(1.0f, -1.0f);
  glTexCoord3f(1.0f, 1.0f, 0.25f); glVertex2f(1.0f, 1.0f);
  glTexCoord3f(0.0f, 1.0f, 0.25f); glVertex2f(-1.0f, 1.0f);
  glEnd();
  ASSERT_EQ(PX(16, 16), 0x00ffffffu);
  const GLubyte yellow[4] = {255, 255, 0, 255};
  glTexImage3D(GL_TEXTURE_3D, 1, GL_RGBA, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glTexCoord3f(0.0f, 0.0f, 0.25f); glVertex2f(-1.0f, -1.0f);
  glTexCoord3f(4.0f, 0.0f, 0.25f); glVertex2f(1.0f, -1.0f);
  glTexCoord3f(4.0f, 4.0f, 0.25f); glVertex2f(1.0f, 1.0f);
  glTexCoord3f(0.0f, 4.0f, 0.25f); glVertex2f(-1.0f, 1.0f);
  glEnd();
  glPopMatrix();
  ASSERT_EQ(PX(16, 16), 0x00ffff00u);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  /* The attribute stack carries the binding and the enable; a list keeps the volume it compiled. */
  glPushAttrib(GL_TEXTURE_BIT | GL_ENABLE_BIT);
  glBindTexture(GL_TEXTURE_3D, 0);
  glDisable(GL_TEXTURE_3D);
  glPopAttrib();
  glGetIntegerv(GL_TEXTURE_BINDING_3D, &iv); ASSERT_EQ((GLuint)iv, t3);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_3D), GL_TRUE);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  glEndList();
  vol[0] = 1;
  glCallList(list);
  glGetTexImage(GL_TEXTURE_3D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(back[0], 255); /* compiled red, not the 1 written after */
  glDeleteLists(list, 1);

  /* Refusals. */
  glTexImage3D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* A depth of zero is **not** a refusal since 2026-09-20: it releases the level, which is what
   * GL says a zero size means. A negative one is still an error. */
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, -2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 1024, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 2, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* slice 2 of 2 */

  /* Deleting it unbinds it. */
  glDeleteTextures(1, &t3);
  glGetIntegerv(GL_TEXTURE_BINDING_3D, &iv); ASSERT_EQ(iv, 0);
  glDisable(GL_TEXTURE_3D);
  glDeleteTextures(1, &t2);

  #undef DRAW_AT
  #undef PX
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Texture parameters: values checked, the float ones kept as floats, priority and residency, the
 * border colour - and the wrap modes as sampled: GL_CLAMP_TO_BORDER reaching the border,
 * GL_CLAMP clamping the coordinate first, GL_MIRRORED_REPEAT reflecting. */
static void test_gl_texture_parameters_and_wrap_modes(void) {
  const int W = 64, H = 4;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  GLint iv[4] = {0, 0, 0, 0};
  GLfloat fv[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  /* **Values are checked**: a wrap mode or filter GL does not have is refused and changes
   * nothing. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x1234);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_REPEAT);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, iv);
  ASSERT_EQ(iv[0], (GLint)GL_REPEAT);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_LINEAR);

  /* **Priority**: 1 by default, a float, clamped; glPrioritizeTextures sets the same field. */
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, fv);
  ASSERT_TRUE(fv[0] == 1.0f);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 0.5f);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, fv);
  ASSERT_TRUE(fv[0] == 0.5f);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, iv);
  ASSERT_EQ(iv[0], 1073741823); /* FLOAT_TO_INT(0.5) */
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 7.0f);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, fv);
  ASSERT_TRUE(fv[0] == 1.0f);
  const GLclampf quarter = 0.25f;
  glPrioritizeTextures(1, &tex, &quarter);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, fv);
  ASSERT_TRUE(fv[0] == 0.25f);
  /* Residency is a query; everything is resident. */
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_RESIDENT, iv);
  ASSERT_EQ(iv[0], GL_TRUE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_RESIDENT, GL_TRUE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* **Border colour**: four floats, clamped; integers converted by range; refused as a scalar. */
  const GLfloat wild[4] = {0.25f, 2.0f, -1.0f, 0.5f};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, wild);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.25f && fv[1] == 1.0f && fv[2] == 0.0f && fv[3] == 0.5f);
  const GLint ib[4] = {2147483647, 0, 0, 2147483647};
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, ib);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, iv);
  ASSERT_TRUE(iv[0] == 2147483647 && iv[1] == 0 && iv[2] == 0 && iv[3] == 2147483647);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, 0);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* An environment colour of 1 read back as an integer is INT_MAX - it overflowed to INT_MIN. */
  const GLfloat one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, one);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, iv);
  ASSERT_EQ(iv[0], 2147483647);

  /* **The wrap modes, sampled.** A red | green texture across a quad whose s runs from -1 at the
   * left edge to 2 at the right: pixel x samples s = -1 + 3 (x + 0.5) / 64. Columns 10, 26, 37,
   * 48 and 58 are s = -0.51, 0.24, 0.76, 1.27 and 1.74. */
  const GLubyte rg[8] = {255, 0, 0, 255, 0, 255, 0, 255};
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rg);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  const GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, blue);
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  #define DRAW_S(mode) do { \
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode); \
      glBegin(GL_QUADS); \
      glTexCoord2f(-1.0f, 0.5f); glVertex2f(-1.0f, -1.0f); \
      glTexCoord2f(2.0f, 0.5f);  glVertex2f(1.0f, -1.0f); \
      glTexCoord2f(2.0f, 0.5f);  glVertex2f(1.0f, 1.0f); \
      glTexCoord2f(-1.0f, 0.5f); glVertex2f(-1.0f, 1.0f); \
      glEnd(); } while (0)
  #define AT(x) (fb[2 * W + (x)] & 0x00ffffffu)
  DRAW_S(GL_CLAMP_TO_BORDER);
  ASSERT_EQ(AT(10), 0x0000ffu); /* the border */
  ASSERT_EQ(AT(26), 0xff0000u);
  ASSERT_EQ(AT(37), 0x00ff00u);
  ASSERT_EQ(AT(48), 0x0000ffu);
  DRAW_S(GL_CLAMP); /* nearest never leaves the image */
  ASSERT_EQ(AT(10), 0xff0000u);
  ASSERT_EQ(AT(48), 0x00ff00u);
  DRAW_S(GL_CLAMP_TO_EDGE);
  ASSERT_EQ(AT(10), 0xff0000u);
  ASSERT_EQ(AT(48), 0x00ff00u);
  DRAW_S(GL_REPEAT);
  ASSERT_EQ(AT(48), 0xff0000u);
  ASSERT_EQ(AT(58), 0x00ff00u);
  DRAW_S(GL_MIRRORED_REPEAT); /* the second repetition reflected */
  ASSERT_EQ(AT(48), 0x00ff00u);
  ASSERT_EQ(AT(58), 0xff0000u);
  /* GL_CLAMP under a linear filter: s clamped to 0 puts the sample on the texel's edge, half
   * red and half border. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  DRAW_S(GL_CLAMP);
  const uint32_t half = AT(10);
  ASSERT_TRUE(((half >> 16) & 0xffu) >= 126u && ((half >> 16) & 0xffu) <= 129u);
  ASSERT_TRUE((half & 0xffu) >= 126u && (half & 0xffu) <= 129u);
  ASSERT_EQ((half >> 8) & 0xffu, 0u);
  /* A luminance texture's border is expanded as its texels are: red 0.5 is grey. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rg);
  const GLfloat half_red[4] = {0.5f, 0.0f, 0.0f, 1.0f};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, half_red);
  DRAW_S(GL_CLAMP_TO_BORDER);
  ASSERT_EQ(AT(10), 0x808080u);
  #undef AT
  #undef DRAW_S

  /* Compiled into a list, the float and vector forms keep their values. */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 0.75f);
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, wild);
  glEndList();
  glCallList(list);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, fv);
  ASSERT_TRUE(fv[0] == 0.75f);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.25f && fv[1] == 1.0f && fv[3] == 0.5f);
  glDeleteLists(list, 1);

  glDisable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Vertex arrays of every type GL 1.1 allows, read as their type (and checked), and GL 1.4's
 * glWindowPos. */
static void test_gl_array_types_and_window_pos(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  #define CENTRE (fb[(H / 2) * W + W / 2] & 0x00ffffffu)
  GLint iv[4] = {0, 0, 0, 0};

  /* **A GL_SHORT position array** - pixel coordinates - with a GL_UNSIGNED_SHORT colour array,
   * normalised: red. The positions were read as floats before, and drew nothing. */
  static const GLshort sq[8] = {4, 4, 12, 4, 12, 12, 4, 12};
  static const GLushort red16[16] = {65535, 0, 0, 65535, 65535, 0, 0, 65535,
                                     65535, 0, 0, 65535, 65535, 0, 0, 65535};
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(2, GL_SHORT, 0, sq);
  glColorPointer(4, GL_UNSIGNED_SHORT, 0, red16);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(CENTRE, 0xff0000u);
  /* GL_DOUBLE positions and GL_BYTE colours, converted as glColor3b converts: (2c + 1) / 255, so
   * 127 is 1 and -128 is -1, clamped to 0 (0 would be 1/255). */
  static const GLdouble sqd[12] = {4, 4, 0, 12, 4, 0, 12, 12, 0, 4, 12, 0};
  static const GLbyte green8[12] = {-128, 127, -128, -128, 127, -128,
                                    -128, 127, -128, -128, 127, -128};
  glVertexPointer(3, GL_DOUBLE, 0, sqd);
  glColorPointer(3, GL_BYTE, 0, green8);
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  /* A strided GL_INT array: the stride in bytes, two ints of padding per element. */
  static const GLint sqi[16] = {4, 4, -1, -1, 12, 4, -1, -1, 12, 12, -1, -1, 4, 12, -1, -1};
  glVertexPointer(2, GL_INT, 4 * (GLsizei)sizeof(GLint), sqi);
  glDisableClientState(GL_COLOR_ARRAY);
  glColor3f(0.0f, 0.0f, 1.0f);
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(CENTRE, 0x0000ffu);

  /* **The checks**, each leaving the array as it was. */
  glVertexPointer(1, GL_FLOAT, 0, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glVertexPointer(2, GL_UNSIGNED_BYTE, 0, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glVertexPointer(2, GL_FLOAT, -4, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glGetIntegerv(GL_VERTEX_ARRAY_SIZE, iv);
  ASSERT_EQ(iv[0], 2);
  glGetIntegerv(GL_VERTEX_ARRAY_TYPE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_INT);
  glColorPointer(2, GL_FLOAT, 0, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glNormalPointer(GL_UNSIGNED_BYTE, 0, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexCoordPointer(2, GL_BYTE, 0, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexCoordPointer(5, GL_FLOAT, 0, sq);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glDisableClientState(GL_VERTEX_ARRAY);

  /* **glWindowPos**: the raster position as given, valid, w 1, z through the depth range. */
  glWindowPos2i(3, 5);
  GLfloat rp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
  ASSERT_TRUE(rp[0] == 3.0f && rp[1] == 5.0f && rp[2] == 0.0f && rp[3] == 1.0f);
  glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, iv);
  ASSERT_EQ(iv[0], 1);
  glDepthRange(0.25, 0.75);
  glWindowPos3f(1.0f, 2.0f, 0.5f);
  glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
  ASSERT_TRUE(rp[2] == 0.5f);
  glWindowPos3f(1.0f, 2.0f, 7.0f); /* clamped to 1 first */
  glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
  ASSERT_TRUE(rp[2] == 0.75f);
  glDepthRange(0.0, 1.0);
  /* Off-screen is still valid - there is no clip test - and the colour is the current one,
   * unlit though lighting is on. */
  glEnable(GL_LIGHTING);
  glColor3f(1.0f, 0.5f, 0.0f);
  glWindowPos2i(-100, 1000);
  glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, iv);
  ASSERT_EQ(iv[0], 1);
  GLfloat rc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  glGetFloatv(GL_CURRENT_RASTER_COLOR, rc);
  ASSERT_TRUE(rc[0] == 1.0f && rc[1] == 0.5f && rc[2] == 0.0f);
  glDisable(GL_LIGHTING);
  /* A bitmap drawn there lands there; and compiled into a list, the call replays. */
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1.0f, 1.0f, 1.0f);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glWindowPos2i(3, 5);
  glEndList();
  glWindowPos2i(0, 0);
  glCallList(list);
  const GLubyte bit = 0x80u;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glBitmap(1, 1, 0.0f, 0.0f, 0.0f, 0.0f, &bit);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  ASSERT_EQ(fb[(H - 1 - 5) * W + 3] & 0x00ffffffu, 0xffffffu);
  glDeleteLists(list, 1);
  #undef CENTRE

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.4's secondary colour: the current one and its array, the colour sum that adds it after
 * texturing, lighting's precedence over it, and the lists and attribute groups that carry it. */
static void test_gl_secondary_color_and_color_sum(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  #define CENTRE (fb[(H / 2) * W + W / 2] & 0x00ffffffu)
  #define QUAD() do { glBegin(GL_QUADS); glTexCoord2f(0.5f, 0.5f); glVertex2f(4, 4); \
    glVertex2f(12, 4); glVertex2f(12, 12); glVertex2f(4, 12); glEnd(); } while (0)
  GLint iv[4] = {0, 0, 0, 0};
  GLfloat fv[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  /* **The initial state**: (0, 0, 0, 1), the sum off, the array off and three GL_FLOATs - and
   * the other arrays' sizes and types, which answered 0 until 2026-09-19. */
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.0f && fv[1] == 0.0f && fv[2] == 0.0f && fv[3] == 1.0f);
  ASSERT_EQ(glIsEnabled(GL_COLOR_SUM), GL_FALSE);
  ASSERT_EQ(glIsEnabled(GL_SECONDARY_COLOR_ARRAY), GL_FALSE);
  glGetIntegerv(GL_SECONDARY_COLOR_ARRAY_SIZE, iv);  ASSERT_EQ(iv[0], 3);
  glGetIntegerv(GL_SECONDARY_COLOR_ARRAY_TYPE, iv);  ASSERT_EQ(iv[0], (GLint)GL_FLOAT);
  glGetIntegerv(GL_VERTEX_ARRAY_SIZE, iv);           ASSERT_EQ(iv[0], 4);
  glGetIntegerv(GL_VERTEX_ARRAY_TYPE, iv);           ASSERT_EQ(iv[0], (GLint)GL_FLOAT);
  glGetIntegerv(GL_COLOR_ARRAY_SIZE, iv);            ASSERT_EQ(iv[0], 4);
  glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, iv);    ASSERT_EQ(iv[0], 4);
  glGetIntegerv(GL_NORMAL_ARRAY_TYPE, iv);           ASSERT_EQ(iv[0], (GLint)GL_FLOAT);

  /* Normalised as glColor is; alpha stays 1; the integer query is FLOAT_TO_INT's. */
  glSecondaryColor3ub(255, 0, 51);
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == 0.0f && fv[2] == 0.2f && fv[3] == 1.0f);
  glGetIntegerv(GL_CURRENT_SECONDARY_COLOR, iv);
  ASSERT_EQ(iv[0], 2147483647);
  glSecondaryColor3b(127, -128, 127);
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == -1.0f && fv[2] == 1.0f);

  /* **The sum, unlit**: red plus a blue secondary is red while GL_COLOR_SUM is off, magenta
   * when on. */
  glSecondaryColor3f(0.0f, 0.0f, 1.0f);
  glColor3f(1.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD();
  ASSERT_EQ(CENTRE, 0xff0000u);
  glEnable(GL_COLOR_SUM);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetIntegerv(GL_COLOR_SUM, iv);
  ASSERT_EQ(iv[0], 1);
  QUAD();
  ASSERT_EQ(CENTRE, 0xff00ffu);

  /* **After texturing**: white modulated by a red texel is red, and the green secondary then
   * makes it yellow. Summed before the texture it would be red again. */
  GLuint tex = 0;
  const GLubyte red_texel[4] = {255, 0, 0, 255};
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red_texel);
  glEnable(GL_TEXTURE_2D);
  glColor3f(1.0f, 1.0f, 1.0f);
  glSecondaryColor3f(0.0f, 1.0f, 0.0f);
  QUAD();
  ASSERT_EQ(CENTRE, 0xffff00u);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);

  /* **Clamped like the primary colour**: a negative secondary adds nothing rather than
   * darkening. */
  glSecondaryColor3f(-1.0f, -1.0f, -1.0f);
  QUAD();
  ASSERT_EQ(CENTRE, 0xffffffu);

  /* **Lighting takes the secondary colour over**: under GL_SINGLE_COLOR it is zero whatever
   * glSecondaryColor said, so the blue below is not added to the dim ambient-lit grey. */
  glSecondaryColor3f(0.0f, 0.0f, 1.0f);
  glEnable(GL_LIGHTING);
  glNormal3f(0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD();
  ASSERT_TRUE((CENTRE & 0xffu) < 0x20u);
  ASSERT_TRUE(CENTRE != 0u);
  glDisable(GL_LIGHTING);

  /* **Flat shading** takes the provoking vertex's secondary colour: a quad's fourth. */
  glShadeModel(GL_FLAT);
  glColor3f(1.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glSecondaryColor3f(0.0f, 0.0f, 0.0f);
  glVertex2f(4, 4); glVertex2f(12, 4); glVertex2f(12, 12);
  glSecondaryColor3f(0.0f, 0.0f, 1.0f);
  glVertex2f(4, 12);
  glEnd();
  ASSERT_EQ(CENTRE, 0xff00ffu);
  glShadeModel(GL_SMOOTH);

  /* **The array**: unsigned bytes, normalised, over a red current colour. */
  static const GLshort sq[8] = {4, 4, 12, 4, 12, 12, 4, 12};
  static const GLubyte blue8[12] = {0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0, 255};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_SHORT, 0, sq);
  glSecondaryColorPointer(3, GL_UNSIGNED_BYTE, 0, blue8);
  glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(glIsEnabled(GL_SECONDARY_COLOR_ARRAY), GL_TRUE);
  glSecondaryColor3f(0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(CENTRE, 0xff00ffu);
  glGetIntegerv(GL_SECONDARY_COLOR_ARRAY_TYPE, iv);  ASSERT_EQ(iv[0], (GLint)GL_UNSIGNED_BYTE);
  GLvoid *ptr = NULL;
  glGetPointerv(GL_SECONDARY_COLOR_ARRAY_POINTER, &ptr);
  ASSERT_TRUE(ptr == (const GLvoid *)blue8);
  /* Checked as Mesa checks it: 3 or 4 components, any of the eight types. */
  glSecondaryColorPointer(2, GL_FLOAT, 0, blue8);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glSecondaryColorPointer(3, (GLenum)0x1407u /* GL_2_BYTES */, 0, blue8);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_SECONDARY_COLOR_ARRAY_SIZE, iv);  ASSERT_EQ(iv[0], 3);

  /* **Compiled**: the draw records the array's values as glSecondaryColor calls, so the list
   * replays blue after the array has changed and been switched off. */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glDrawArrays(GL_QUADS, 0, 4);
  glEndList();
  glDisableClientState(GL_SECONDARY_COLOR_ARRAY);
  glClear(GL_COLOR_BUFFER_BIT);
  glCallList(list);
  ASSERT_EQ(CENTRE, 0xff00ffu);
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv); /* the replayed call set it */
  ASSERT_TRUE(fv[2] == 1.0f);
  glDeleteLists(list, 1);
  glDisableClientState(GL_VERTEX_ARRAY);

  /* **The attribute groups**: the colour with GL_CURRENT_BIT, the sum with GL_FOG_BIT and with
   * GL_ENABLE_BIT, the array with the client's vertex-array bit. */
  glSecondaryColor3f(0.0f, 1.0f, 0.0f);
  glPushAttrib(GL_CURRENT_BIT);
  glSecondaryColor3f(1.0f, 1.0f, 1.0f);
  glPopAttrib();
  glGetFloatv(GL_CURRENT_SECONDARY_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.0f && fv[1] == 1.0f && fv[2] == 0.0f);
  glPushAttrib(GL_FOG_BIT);
  glDisable(GL_COLOR_SUM);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_COLOR_SUM), GL_TRUE);
  glPushAttrib(GL_ENABLE_BIT);
  glDisable(GL_COLOR_SUM);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_COLOR_SUM), GL_TRUE);
  glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
  glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
  glSecondaryColorPointer(4, GL_FLOAT, 0, sq);
  glPopClientAttrib();
  ASSERT_EQ(glIsEnabled(GL_SECONDARY_COLOR_ARRAY), GL_FALSE);
  glGetIntegerv(GL_SECONDARY_COLOR_ARRAY_TYPE, iv);  ASSERT_EQ(iv[0], (GLint)GL_UNSIGNED_BYTE);

  /* And the sum stays out of the way once disabled. */
  glDisable(GL_COLOR_SUM);
  glClear(GL_COLOR_BUFFER_BIT);
  QUAD();
  ASSERT_EQ(CENTRE, 0xff0000u);
  #undef QUAD
  #undef CENTRE

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.4's fog coordinate: fog from a per-vertex value in place of the eye distance, from
 * glFogCoord and from its array, interpolated, compiled, saved - and the raster distance. */
static void test_gl_fog_coordinates(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  /* Centred on the eye, so the eye distance of a small quad in the middle is about 1. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(-8.0, 8.0, -8.0, 8.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  #define PIX(x, y) (fb[(H - 1 - (y)) * W + (x)] & 0x00ffffffu)
  #define QUAD() do { glBegin(GL_QUADS); glVertex2f(-8, -8); glVertex2f(8, -8); \
    glVertex2f(8, 8); glVertex2f(-8, 8); glEnd(); } while (0)
  GLint iv[1] = {0};
  GLfloat fv[1] = {0.0f};

  /* The initial state: the eye distance, a coordinate of 0, the array off and one GL_FLOAT. */
  glGetIntegerv(GL_FOG_COORD_SRC, iv);  ASSERT_EQ(iv[0], (GLint)GL_FRAGMENT_DEPTH);
  glGetFloatv(GL_CURRENT_FOG_COORD, fv); ASSERT_TRUE(fv[0] == 0.0f);
  ASSERT_EQ(glIsEnabled(GL_FOG_COORD_ARRAY), GL_FALSE);
  glGetIntegerv(GL_FOG_COORD_ARRAY_TYPE, iv); ASSERT_EQ(iv[0], (GLint)GL_FLOAT);

  /* Linear fog from 0 to 10 towards blue, over red. By the eye distance - at most 1.5 for this
   * small quad around the eye - it is mostly red, whatever the coordinate says. */
  const GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  glFogfv(GL_FOG_COLOR, blue);
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 0.0f);
  glFogf(GL_FOG_END, 10.0f);
  glEnable(GL_FOG);
  glColor3f(1.0f, 0.0f, 0.0f);
  glFogCoordf(10.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_TRUE(((PIX(8, 8) >> 16) & 0xffu) > 0xc0u);

  /* **From the coordinate**: 10 is fully fogged, 0 not at all. */
  glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(PIX(8, 8), 0x0000ffu);
  QUAD();
  ASSERT_EQ(PIX(8, 8), 0x0000ffu);
  glFogCoordf(0.0f);
  QUAD();
  ASSERT_EQ(PIX(8, 8), 0xff0000u);
  /* Used as given, not its absolute value: -10 is beyond the start, unfogged too. */
  glFogCoordd(-10.0);
  QUAD();
  ASSERT_EQ(PIX(8, 8), 0xff0000u);

  /* **Interpolated**: 0 on the left edge, 10 on the right - red to blue across. */
  glBegin(GL_QUADS);
  glFogCoordf(0.0f);  glVertex2f(-8, -8);
  glFogCoordf(10.0f); glVertex2f(8, -8);
  glFogCoordf(10.0f); glVertex2f(8, 8);
  glFogCoordf(0.0f);  glVertex2f(-8, 8);
  glEnd();
  ASSERT_TRUE((PIX(1, 8) & 0xffu) < 0x30u);
  ASSERT_TRUE(((PIX(14, 8) >> 16) & 0xffu) < 0x30u);
  {
    const uint32_t c = PIX(8, 8);
    const int r = (int)((c >> 16) & 0xffu), b = (int)(c & 0xffu);
    ASSERT_TRUE(r > 100 && r < 155 && b > 100 && b < 155);
  }

  /* **The array**: GL_DOUBLE, fully fogged; and the checks - one value of GL_FLOAT or
   * GL_DOUBLE. */
  static const GLshort sq[8] = {-8, -8, 8, -8, 8, 8, -8, 8};
  static const GLdouble far4[4] = {10.0, 10.0, 10.0, 10.0};
  glFogCoordf(0.0f);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_SHORT, 0, sq);
  glFogCoordPointer(GL_DOUBLE, 0, far4);
  glEnableClientState(GL_FOG_COORD_ARRAY);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(PIX(8, 8), 0x0000ffu);
  GLvoid *ptr = NULL;
  glGetPointerv(GL_FOG_COORD_ARRAY_POINTER, &ptr);
  ASSERT_TRUE(ptr == (const GLvoid *)far4);
  glFogCoordPointer(GL_INT, 0, far4);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glFogCoordPointer(GL_FLOAT, -1, far4);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glGetIntegerv(GL_FOG_COORD_ARRAY_TYPE, iv); ASSERT_EQ(iv[0], (GLint)GL_DOUBLE);

  /* **Compiled**, from the array and by hand: the values of now replay later. */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glDrawArrays(GL_QUADS, 0, 4);
  glFogCoordf(3.0f);
  glEndList();
  glDisableClientState(GL_FOG_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);
  glClear(GL_COLOR_BUFFER_BIT);
  glCallList(list);
  ASSERT_EQ(PIX(8, 8), 0x0000ffu);
  glGetFloatv(GL_CURRENT_FOG_COORD, fv); ASSERT_TRUE(fv[0] == 3.0f);
  glDeleteLists(list, 1);

  /* **The raster distance** is the coordinate under GL_FOG_COORD - glRasterPos and glWindowPos
   * both - and the eye distance (or glWindowPos's 0) otherwise. */
  glRasterPos2f(4.0f, 3.0f);
  glGetFloatv(GL_CURRENT_RASTER_DISTANCE, fv); ASSERT_TRUE(fv[0] == 3.0f);
  glWindowPos2i(1, 1);
  glGetFloatv(GL_CURRENT_RASTER_DISTANCE, fv); ASSERT_TRUE(fv[0] == 3.0f);
  glFogi(GL_FOG_COORD_SRC, GL_FRAGMENT_DEPTH);
  glRasterPos2f(4.0f, 3.0f);
  glGetFloatv(GL_CURRENT_RASTER_DISTANCE, fv); ASSERT_TRUE(fv[0] == 5.0f);
  glWindowPos2i(1, 1);
  glGetFloatv(GL_CURRENT_RASTER_DISTANCE, fv); ASSERT_TRUE(fv[0] == 0.0f);

  /* The source takes the two values only. */
  glFogi(GL_FOG_COORD_SRC, (GLint)GL_LINEAR);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* **The attribute groups**: the coordinate with GL_CURRENT_BIT, the source with GL_FOG_BIT
   * (which Mesa's pop leaves out), the array with the client vertex-array bit. */
  glPushAttrib(GL_CURRENT_BIT | GL_FOG_BIT);
  glFogCoordf(7.0f);
  glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
  glPopAttrib();
  glGetFloatv(GL_CURRENT_FOG_COORD, fv); ASSERT_TRUE(fv[0] == 3.0f);
  glGetIntegerv(GL_FOG_COORD_SRC, iv);  ASSERT_EQ(iv[0], (GLint)GL_FRAGMENT_DEPTH);
  glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
  glFogCoordPointer(GL_FLOAT, 0, sq);
  glEnableClientState(GL_FOG_COORD_ARRAY);
  glPopClientAttrib();
  ASSERT_EQ(glIsEnabled(GL_FOG_COORD_ARRAY), GL_FALSE);
  glGetIntegerv(GL_FOG_COORD_ARRAY_TYPE, iv); ASSERT_EQ(iv[0], (GLint)GL_DOUBLE);
  #undef QUAD
  #undef PIX

  glDisable(GL_FOG);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Pixel rectangles are fragments: glDrawPixels, glBitmap and glCopyPixels meet the scissor, the
 * alpha, stencil and depth tests, blending, the logic op, the masks, the texture environment and
 * fog. They were written straight into the colour buffer until 2026-09-19. And glCopyPixels reads
 * its whole source first, and the zoom maps pixels to window columns by GL's rule. */
static void test_gl_pixel_rectangles_are_fragments(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  #define PIX(x, y) (fb[(H - 1 - (y)) * W + (x)] & 0x00ffffffu)
  static GLubyte img[4 * 4 * 4];
  #define FILL(r, g, b, a) do { for (int i_ = 0; i_ < 16; i_++) { img[i_ * 4] = (r); \
    img[i_ * 4 + 1] = (g); img[i_ * 4 + 2] = (b); img[i_ * 4 + 3] = (a); } } while (0)
  static const GLubyte ones[4] = {0xf0, 0xf0, 0xf0, 0xf0}; /* a 4x4 bitmap, all set */

  /* **The scissor**: a 4x4 red image at (2, 2), a box from (4, 0) - only its right half lands. */
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  FILL(255, 0, 0, 255);
  glEnable(GL_SCISSOR_TEST);
  glScissor(4, 0, 12, 16);
  glWindowPos2i(2, 2);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glDisable(GL_SCISSOR_TEST);
  ASSERT_EQ(PIX(2, 2), 0x000000u);
  ASSERT_EQ(PIX(4, 2), 0xff0000u);

  /* **Blending**: half-alpha red over blue. */
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  FILL(255, 0, 0, 128);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glDisable(GL_BLEND);
  {
    const uint32_t c = PIX(3, 3);
    ASSERT_TRUE(((c >> 16) & 0xffu) > 0x70u && ((c >> 16) & 0xffu) < 0x90u);
    ASSERT_TRUE((c & 0xffu) > 0x70u && (c & 0xffu) < 0x90u);
  }

  /* **The alpha test** drops the transparent image; **the colour mask** keeps red out of a white
   * one. */
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  FILL(255, 255, 255, 0);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glDisable(GL_ALPHA_TEST);
  ASSERT_EQ(PIX(3, 3), 0x000000u);
  glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  ASSERT_EQ(PIX(3, 3), 0x00ffffu);

  /* **The logic op**: white XOR cyan is red. */
  glEnable(GL_COLOR_LOGIC_OP);
  glLogicOp(GL_XOR);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glDisable(GL_COLOR_LOGIC_OP);
  ASSERT_EQ(PIX(3, 3), 0xff0000u);

  /* **The depth test, at the raster z, and the depth write**: a bitmap behind a cleared 0.5 is
   * dropped, one in front drawn - and its 0.25 then hides a later one at 0.375. */
  glClear(GL_COLOR_BUFFER_BIT);
  glClearDepth(0.5);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glColor3f(0.0f, 1.0f, 0.0f);
  glWindowPos3f(2.0f, 2.0f, 0.75f);
  glBitmap(4, 4, 0.0f, 0.0f, 0.0f, 0.0f, ones);
  ASSERT_EQ(PIX(3, 3), 0x000000u);
  glWindowPos3f(2.0f, 2.0f, 0.25f);
  glBitmap(4, 4, 0.0f, 0.0f, 0.0f, 0.0f, ones);
  ASSERT_EQ(PIX(3, 3), 0x00ff00u);
  glColor3f(1.0f, 0.0f, 0.0f);
  glWindowPos3f(2.0f, 2.0f, 0.375f);
  glBitmap(4, 4, 0.0f, 0.0f, 0.0f, 0.0f, ones);
  ASSERT_EQ(PIX(3, 3), 0x00ff00u);
  glDisable(GL_DEPTH_TEST);
  glClearDepth(1.0);

  /* **The stencil test**: a bitmap writes 1 where it lands, and a full-screen red rectangle kept
   * to stencil 1 then fills exactly that square. */
  glClearStencil(0);
  glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 1, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glColor3f(0.0f, 0.0f, 0.0f);
  glWindowPos2i(8, 8);
  glBitmap(4, 4, 0.0f, 0.0f, 0.0f, 0.0f, ones);
  glStencilFunc(GL_EQUAL, 1, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glColor3f(1.0f, 0.0f, 0.0f);
  glRectf(0.0f, 0.0f, 16.0f, 16.0f);
  glDisable(GL_STENCIL_TEST);
  ASSERT_EQ(PIX(9, 9), 0xff0000u);
  ASSERT_EQ(PIX(3, 3), 0x000000u);

  /* **Texturing**, at the raster position's texture coordinate: white modulated by green. */
  GLuint tex = 0;
  const GLubyte green_texel[4] = {0, 255, 0, 255};
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green_texel);
  glEnable(GL_TEXTURE_2D);
  glClear(GL_COLOR_BUFFER_BIT);
  FILL(255, 255, 255, 255);
  glTexCoord2f(0.5f, 0.5f);
  glWindowPos2i(2, 2);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(PIX(3, 3), 0x00ff00u);

  /* **Fog**, by the raster distance - here the fog coordinate glWindowPos takes under
   * GL_FOG_COORD: 10 is fully the fog colour. */
  const GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  glFogfv(GL_FOG_COLOR, blue);
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 0.0f);
  glFogf(GL_FOG_END, 10.0f);
  glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
  glFogCoordf(10.0f);
  glEnable(GL_FOG);
  glWindowPos2i(2, 2);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glDisable(GL_FOG);
  glFogi(GL_FOG_COORD_SRC, GL_FRAGMENT_DEPTH);
  ASSERT_EQ(PIX(3, 3), 0x0000ffu);

  /* **glCopyPixels reads before it writes**: three rows - red, green, blue from the bottom -
   * and the lower two copied up by one. Row 2 is green, not the red a copy that read its own
   * writes smeared up. */
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1.0f, 0.0f, 0.0f); glRectf(0.0f, 0.0f, 16.0f, 1.0f);
  glColor3f(0.0f, 1.0f, 0.0f); glRectf(0.0f, 1.0f, 16.0f, 2.0f);
  glColor3f(0.0f, 0.0f, 1.0f); glRectf(0.0f, 2.0f, 16.0f, 3.0f);
  glWindowPos2i(0, 1);
  glCopyPixels(0, 0, 16, 2, GL_COLOR);
  ASSERT_EQ(PIX(5, 1), 0xff0000u);
  ASSERT_EQ(PIX(5, 2), 0x00ff00u);
  /* And it is fragments too: through a mask keeping red as it is, green copied onto red is
   * yellow. */
  glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
  glWindowPos2i(0, 0);
  glCopyPixels(0, 2, 16, 1, GL_COLOR);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  ASSERT_EQ(PIX(5, 0), 0xffff00u);

  /* **A mirrored image lands where GL puts it**: pixel 0 of a zoom of -1 from x 8 covers the
   * column whose centre is in [7, 8) - column 7, not 8. */
  glClear(GL_COLOR_BUFFER_BIT);
  img[0] = 255; img[1] = 255; img[2] = 0; img[3] = 255; /* pixel 0 yellow, the rest white */
  glPixelZoom(-1.0f, 1.0f);
  glWindowPos2i(8, 2);
  glDrawPixels(4, 1, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glPixelZoom(1.0f, 1.0f);
  ASSERT_EQ(PIX(7, 2), 0xffff00u);
  ASSERT_EQ(PIX(4, 2), 0xffffffu);
  ASSERT_EQ(PIX(8, 2), 0x000000u);

  /* **A bitmap in a negative raster colour is black**, clamped as a fragment's colour is - by
   * glRasterPos, which keeps the colour unclamped (glWindowPos clamps it). */
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(-1.0f, -1.0f, -1.0f);
  glRasterPos2i(2, 2);
  glBitmap(4, 4, 0.0f, 0.0f, 0.0f, 0.0f, ones);
  ASSERT_EQ(PIX(3, 3), 0x000000u);
  #undef FILL
  #undef PIX

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **Every CPU pixel operation leaves nothing outstanding.**
 *
 * On the scanout path the colour buffer is the display's own memory, mapped write-combined, and
 * a WC store is not ordered against a later load - so the CP's DMA in `gl_hw_flush` can read a
 * store that has not drained. `gl_color_cpu_drain` is the barrier.
 *
 * **It is not why six of gl1-probe's eight hardware failures failed**, though this test was
 * written believing it was. The console returned all eight pixels byte for byte unchanged with
 * the barrier in place, and that is what ruled the ordering out; the cause was
 * `glGetFrameReadbackSampled` handing back the CP's copy as of the last submit, which a CPU
 * pixel operation never produces. The barrier stays because the hazard is real, and this test
 * stays because the bookkeeping under it is the half that rots silently - a new pixel operation
 * that forgets `gl_raster_wrote`, or a fragment path that stops calling `gl_color_cpu_touched`,
 * would leave the span behind and nothing else here would notice.
 *
 * So: the span grows when a fragment is written, and every public operation that writes one
 * ends with it empty. The host build compiles the `sfence` out - a host framebuffer is ordinary
 * memory - so the bookkeeping is all this can check, and it is checked deliberately.
 */
static void test_gl_cpu_pixel_ops_drain_what_they_wrote(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  static GLubyte img[4 * 4 * 4];
  for (int i = 0; i < 16; i++) {
    img[i * 4] = 255; img[i * 4 + 1] = 0; img[i * 4 + 2] = 0; img[i * 4 + 3] = 255;
  }
  static const GLubyte ones[4] = {0xf0, 0xf0, 0xf0, 0xf0};

  /* A fresh context owes nothing. */
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  /* **The span grows with the fragments written into it.** Driven directly, because every
   * public entry point drains on its way out and so can never be caught holding one. */
  gl_pixel_frags_t pf;
  gl_pixel_frags_begin(ctx, &pf);
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  gl_pixel_fragment(ctx, &pf, 4, 4, red);
  ASSERT_TRUE(ctx->cpu_color_lo < ctx->cpu_color_hi);
  ASSERT_EQ(ctx->cpu_color_hi - ctx->cpu_color_lo, (size_t)1);
  ASSERT_TRUE(ctx->cpu_color_buf == ctx->framebuffer);
  gl_pixel_fragment(ctx, &pf, 9, 4, red);
  /* Same row, five columns along: one span covering both, not two records. */
  ASSERT_EQ(ctx->cpu_color_hi - ctx->cpu_color_lo, (size_t)6);
  gl_color_cpu_drain(ctx);
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  /* **And each public operation drains its own.** These are the four that write colour with the
   * CPU, which is the whole of the failing set. */
  glWindowPos2i(2, 2);
  glDrawPixels(4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  glColor3f(1.0f, 1.0f, 1.0f);
  glWindowPos2i(8, 8);
  glBitmap(4, 4, 0.0f, 0.0f, 0.0f, 0.0f, ones);
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  glWindowPos2i(2, 10);
  glCopyPixels(2, 2, 4, 4, GL_COLOR);
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  glAccum(GL_LOAD, 1.0f);
  glAccum(GL_RETURN, 1.0f);
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  /* A depth rectangle colours fragments too, and takes the early return out of glDrawPixels -
   * the path that needs its own drain rather than the one at the end of the function. */
  static GLfloat depths[4 * 4];
  for (int i = 0; i < 16; i++) depths[i] = 0.5f;
  glWindowPos2i(2, 2);
  glDrawPixels(4, 4, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
  ASSERT_TRUE(ctx->cpu_color_lo >= ctx->cpu_color_hi);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The pixels of a buffer that are not black. */
static int count_lit_pixels(const uint32_t *fb, int n) {
  int lit = 0;
  for (int i = 0; i < n; i++) lit += (fb[i] & 0x00ffffffu) != 0u;
  return lit;
}

/* GL 1.4's point parameters - the size clamp and the distance attenuation - and its
 * multi-draws. */
static void test_gl_point_parameters_and_multi_draw(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  /* One unit a pixel, the eye at the centre. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(-16.0, 16.0, -16.0, 16.0, -10.0, 10.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  GLfloat fv[3] = {0.0f, 0.0f, 0.0f};
  #define LIT() count_lit_pixels(fb, W * H)
  #define POINT_AT(z) do { glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_POINTS); \
    glVertex3f(0.0f, 0.0f, (z)); glEnd(); } while (0)

  /* The initial values: no attenuation, a clamp from 0 to the largest size, a threshold of 1. */
  glGetFloatv(GL_POINT_SIZE_MIN, fv);  ASSERT_TRUE(fv[0] == 0.0f);
  glGetFloatv(GL_POINT_SIZE_MAX, fv);  ASSERT_TRUE(fv[0] == (float)OOPS_GL_MAX_POINT_LINE_SIZE);
  glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, fv); ASSERT_TRUE(fv[0] == 1.0f);
  glGetFloatv(GL_POINT_DISTANCE_ATTENUATION, fv);
  ASSERT_TRUE(fv[0] == 1.0f && fv[1] == 0.0f && fv[2] == 0.0f);

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glColor3f(1.0f, 1.0f, 1.0f);
  glPointSize(8.0f);
  POINT_AT(-4.0f);
  ASSERT_EQ(LIT(), 64);

  /* **The clamp, unattenuated**: GL_POINT_SIZE_MAX 2 draws the size-8 point as 2 pixels a side. */
  glPointParameterf(GL_POINT_SIZE_MAX, 2.0f);
  POINT_AT(-4.0f);
  ASSERT_EQ(LIT(), 4);
  glPointParameterf(GL_POINT_SIZE_MAX, (float)OOPS_GL_MAX_POINT_LINE_SIZE);

  /* **Attenuation**: (0, 0, 1) divides by the distance - 16 at distance 4 is 4 a side, at
   * distance 2 is 8 - and GL_POINT_SIZE_MIN holds it from below. */
  const GLfloat quad_atten[3] = {0.0f, 0.0f, 1.0f};
  glPointSize(16.0f);
  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, quad_atten);
  POINT_AT(-4.0f);
  ASSERT_EQ(LIT(), 16);
  POINT_AT(-2.0f);
  ASSERT_EQ(LIT(), 64);
  glPointParameteri(GL_POINT_SIZE_MIN, 6);
  POINT_AT(-4.0f);
  ASSERT_EQ(LIT(), 36);
  {
    GLint iv[3] = {0, 0, 0};
    glGetIntegerv(GL_POINT_DISTANCE_ATTENUATION, iv);
    ASSERT_TRUE(iv[0] == 0 && iv[1] == 0 && iv[2] == 1);
  }

  /* **GL_POINT_BIT** saves and restores them; **a list** records them. */
  glPushAttrib(GL_POINT_BIT);
  const GLint one_atten[3] = {1, 0, 0};
  glPointParameteriv(GL_POINT_DISTANCE_ATTENUATION, one_atten);
  glPointParameterf(GL_POINT_SIZE_MIN, 0.0f);
  glPopAttrib();
  glGetFloatv(GL_POINT_SIZE_MIN, fv); ASSERT_TRUE(fv[0] == 6.0f);
  glGetFloatv(GL_POINT_DISTANCE_ATTENUATION, fv); ASSERT_TRUE(fv[0] == 0.0f && fv[2] == 1.0f);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, 3.0f);
  glEndList();
  glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, fv); ASSERT_TRUE(fv[0] == 1.0f);
  glCallList(list);
  glGetFloatv(GL_POINT_FADE_THRESHOLD_SIZE, fv); ASSERT_TRUE(fv[0] == 3.0f);
  glDeleteLists(list, 1);

  /* The checks: a negative clamp or threshold, and GL 2.0's sprite origin. */
  glPointParameterf(GL_POINT_SIZE_MAX, -1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glPointParameterf(GL_POINT_FADE_THRESHOLD_SIZE, -1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  /* **The sprite origin is a parameter now, not a refusal** (2026-09-22, with GL_POINT_SPRITE
     itself). Both corners are values and anything else is still an enum error, which is the half
     of this assertion worth keeping. */
  glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, (GLint)GL_LOWER_LEFT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetFloatv(GL_POINT_SPRITE_COORD_ORIGIN, fv);
  ASSERT_TRUE(fv[0] == (GLfloat)GL_LOWER_LEFT);
  glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, (GLint)GL_FASTEST);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, (GLint)GL_UPPER_LEFT);
  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (const GLfloat[3]){1.0f, 0.0f, 0.0f});
  glPointParameterf(GL_POINT_SIZE_MIN, 0.0f);
  glPointSize(1.0f);

  /* **glMultiDrawArrays**: two 2x2 quads from one array in one call, an empty third skipped. */
  static const GLfloat quads[16] = {-8, -8, -6, -8, -6, -6, -8, -6,   6, 6, 8, 6, 8, 8, 6, 8};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, quads);
  const GLint firsts[3] = {0, 4, 0};
  const GLsizei counts[3] = {4, 4, 0};
  glClear(GL_COLOR_BUFFER_BIT);
  glMultiDrawArrays(GL_QUADS, firsts, counts, 3);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(LIT(), 8);
  /* **Checked before anything is drawn**: a negative count at the end draws neither quad. */
  const GLsizei bad_counts[2] = {4, -1};
  glClear(GL_COLOR_BUFFER_BIT);
  glMultiDrawArrays(GL_QUADS, firsts, bad_counts, 2);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  ASSERT_EQ(LIT(), 0);
  glMultiDrawArrays(GL_QUADS, firsts, counts, -1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glMultiDrawArrays((GLenum)0x7fu, firsts, counts, 2);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* **glMultiDrawElements**: the same two quads by index, bytes, and a bad type refused. */
  static const GLubyte idx_a[4] = {0, 1, 2, 3}, idx_b[4] = {4, 5, 6, 7};
  const GLvoid *const idx[2] = {idx_a, idx_b};
  glClear(GL_COLOR_BUFFER_BIT);
  glMultiDrawElements(GL_QUADS, counts, GL_UNSIGNED_BYTE, idx, 2);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(LIT(), 8);
  glMultiDrawElements(GL_QUADS, counts, GL_FLOAT, idx, 2);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* **Compiled**: the draws a list records are the arrays' values of now. */
  const GLuint list2 = glGenLists(1);
  glNewList(list2, GL_COMPILE);
  glMultiDrawArrays(GL_QUADS, firsts, counts, 2);
  glEndList();
  glDisableClientState(GL_VERTEX_ARRAY);
  glClear(GL_COLOR_BUFFER_BIT);
  glCallList(list2);
  ASSERT_EQ(LIT(), 8);
  glDeleteLists(list2, 1);
  #undef POINT_AT
  #undef LIT

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.4's level-of-detail bias - the texture's and the unit's - and GL_GENERATE_MIPMAP. */
static void test_gl_lod_bias_and_generate_mipmap(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  #define PIX(x, y) (fb[(H - 1 - (y)) * W + (x)] & 0x00ffffffu)
  /* A 4x4 texture drawn 4x4 pixels: a level of detail of about 0. */
  #define QUAD() do { glBegin(GL_QUADS); glTexCoord2f(0, 0); glVertex2f(4, 4); \
    glTexCoord2f(1, 0); glVertex2f(8, 4); glTexCoord2f(1, 1); glVertex2f(8, 8); \
    glTexCoord2f(0, 1); glVertex2f(4, 8); glEnd(); } while (0)
  GLint iv[1] = {0};
  GLfloat fv[1] = {0.0f};
  GLubyte img[4 * 4 * 4];

  /* **GL_GENERATE_MIPMAP**: set before the upload, a 4x4 base of red columns and blue columns
   * gives a 2x2 level of the same and a 1x1 level of their mean - 127.5 rounded to even, 128. */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, iv);
  ASSERT_EQ(iv[0], GL_FALSE);
  glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, iv);
  ASSERT_EQ(iv[0], GL_TRUE);
  for (int i = 0; i < 16; i++) {
    const int col = i % 4;
    img[i * 4] = (col < 2) ? 255 : 0; img[i * 4 + 1] = 0;
    img[i * 4 + 2] = (col < 2) ? 0 : 255; img[i * 4 + 3] = 255;
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, iv);
  ASSERT_EQ(iv[0], 1);
  GLubyte got[4 * 4 * 4];
  glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_TRUE(got[0] == 255 && got[2] == 0 && got[4] == 0 && got[6] == 255);
  glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_TRUE(got[0] == 128 && got[1] == 0 && got[2] == 128 && got[3] == 255);
  /* The texture is complete through them: minified to one pixel, it samples the 1x1 level. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0); glVertex2f(1, 1); glTexCoord2f(1, 0); glVertex2f(2, 1);
  glTexCoord2f(1, 1); glVertex2f(2, 2); glTexCoord2f(0, 1); glVertex2f(1, 2);
  glEnd();
  ASSERT_EQ(PIX(1, 1), 0x800080u);
  /* **A change to the base regenerates**: all green through glTexSubImage2D. */
  for (int i = 0; i < 16; i++) { img[i * 4] = 0; img[i * 4 + 1] = 255; img[i * 4 + 2] = 0; }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_TRUE(got[0] == 0 && got[1] == 255 && got[2] == 0);
  /* **Not past GL_TEXTURE_MAX_LEVEL**, and not from a level that is not the base: with the
   * maximum at 1, a new base rebuilds level 1 alone and level 2 stays green. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
  for (int i = 0; i < 16; i++) { img[i * 4] = 255; img[i * 4 + 1] = 255; img[i * 4 + 2] = 255; }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_TRUE(got[0] == 255 && got[1] == 255 && got[2] == 255);
  glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, got);
  ASSERT_TRUE(got[0] == 0 && got[1] == 255 && got[2] == 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
  glDeleteTextures(1, &tex);

  /* **The level-of-detail bias**: level 0 red, level 1 green, drawn at a level of detail of about
   * 0 - red; biased by 1 through the texture, green; the unit's -1 cancelling it, red again. */
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  for (int i = 0; i < 16; i++) { img[i * 4] = 255; img[i * 4 + 1] = 0; img[i * 4 + 2] = 0; img[i * 4 + 3] = 255; }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
  for (int i = 0; i < 16; i++) { img[i * 4] = 0; img[i * 4 + 1] = 255; }
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  QUAD();
  ASSERT_EQ(PIX(5, 5), 0xff0000u);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 1.0f);
  QUAD();
  ASSERT_EQ(PIX(5, 5), 0x00ff00u);
  glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, -1.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  QUAD();
  ASSERT_EQ(PIX(5, 5), 0xff0000u);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, fv); ASSERT_TRUE(fv[0] == 1.0f);
  glGetTexEnvfv(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, fv); ASSERT_TRUE(fv[0] == -1.0f);
  glGetTexEnviv(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, iv); ASSERT_EQ(iv[0], -1);
  glGetFloatv(GL_MAX_TEXTURE_LOD_BIAS, fv); ASSERT_TRUE(fv[0] == 14.0f);
  /* GL_TEXTURE_FILTER_CONTROL has the one parameter. */
  glTexEnvi(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* **GL_TEXTURE_BIT** keeps both. */
  glPushAttrib(GL_TEXTURE_BIT);
  glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 3.0f);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 3.0f);
  glPopAttrib();
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, fv); ASSERT_TRUE(fv[0] == 1.0f);
  glGetTexEnvfv(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, fv); ASSERT_TRUE(fv[0] == -1.0f);
  glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 0.0f);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);

  /* GL_GENERATE_MIPMAP_HINT is a hint like the others. */
  glHint(GL_GENERATE_MIPMAP_HINT, GL_NICEST);
  glGetIntegerv(GL_GENERATE_MIPMAP_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NICEST);
  #undef QUAD
  #undef PIX

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.0's depth and stencil pixel rectangles - glReadPixels, glDrawPixels and glCopyPixels of
 * GL_DEPTH_COMPONENT and GL_STENCIL_INDEX - and GL 1.4's depth textures and shadow comparison. */
static void test_gl_depth_stencil_pixels_and_depth_textures(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  #define PIX(x, y) (fb[(H - 1 - (y)) * W + (x)] & 0x00ffffffu)
  GLfloat zf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  GLubyte sv[4] = {0, 0, 0, 0};

  /* **glReadPixels of depth**, as floats and as unsigned shorts, and through GL_DEPTH_SCALE. */
  glClearDepth(0.25);
  glClear(GL_DEPTH_BUFFER_BIT);
  glReadPixels(3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_TRUE(zf[0] == 0.25f);
  GLushort zs = 0;
  glReadPixels(3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, &zs);
  ASSERT_EQ(zs, 16384);
  glPixelTransferf(GL_DEPTH_SCALE, 2.0f);
  glReadPixels(3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  glPixelTransferf(GL_DEPTH_SCALE, 1.0f);
  ASSERT_TRUE(zf[0] == 0.5f);
  /* A packed type is no depth type. */
  glReadPixels(3, 3, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT_5_6_5, &zs);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* **glDrawPixels of depth**: fragments at their own z in the raster colour, through the depth
   * test - GL_ALWAYS writes each; with the test off no depth is written at all. */
  static const GLfloat zimg[4] = {0.125f, 0.375f, 0.625f, 0.875f};
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_ALWAYS);
  glColor3f(0.0f, 1.0f, 0.0f);
  glWindowPos2i(0, 0);
  glDrawPixels(2, 2, GL_DEPTH_COMPONENT, GL_FLOAT, zimg);
  glReadPixels(0, 0, 2, 2, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  ASSERT_TRUE(zf[0] == 0.125f && zf[1] == 0.375f && zf[2] == 0.625f && zf[3] == 0.875f);
  ASSERT_EQ(PIX(1, 1), 0x00ff00u);
  glDisable(GL_DEPTH_TEST);
  static const GLfloat zmid[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  glDrawPixels(2, 2, GL_DEPTH_COMPONENT, GL_FLOAT, zmid);
  glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  ASSERT_TRUE(zf[0] == 0.125f);
  /* **glCopyPixels of depth**, to (4, 4): the depth test on again, GL_ALWAYS. */
  glEnable(GL_DEPTH_TEST);
  glWindowPos2i(4, 4);
  glCopyPixels(0, 0, 2, 2, GL_DEPTH);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glReadPixels(4, 4, 2, 2, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  ASSERT_TRUE(zf[0] == 0.125f && zf[3] == 0.875f);
  glDisable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);

  /* **Stencil indices**: drawn straight into the stencil buffer, through the write mask and the
   * index offset, read back, and copied. */
  static const GLubyte simg[4] = {1, 2, 3, 4};
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glWindowPos2i(8, 8);
  glDrawPixels(2, 2, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, simg);
  glReadPixels(8, 8, 2, 2, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sv);
  ASSERT_TRUE(sv[0] == 1 && sv[1] == 2 && sv[2] == 3 && sv[3] == 4);
  glPixelTransferi(GL_INDEX_OFFSET, 10);
  glReadPixels(8, 8, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sv);
  glPixelTransferi(GL_INDEX_OFFSET, 0);
  ASSERT_EQ(sv[0], 11);
  glStencilMask(0x01u);
  static const GLubyte sff[4] = {0xff, 0xff, 0xff, 0xff};
  glDrawPixels(1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sff); /* 1 | (0xfe of 1) = 1 */
  glWindowPos2i(9, 8);
  glDrawPixels(1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sff); /* 2 -> 3 */
  glStencilMask(0xffu);
  glReadPixels(8, 8, 2, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sv);
  ASSERT_TRUE(sv[0] == 1 && sv[1] == 3);
  glWindowPos2i(12, 12);
  glCopyPixels(8, 8, 2, 2, GL_STENCIL);
  glReadPixels(12, 12, 2, 2, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sv);
  ASSERT_TRUE(sv[0] == 1 && sv[1] == 3 && sv[2] == 3 && sv[3] == 4);
  glCopyPixels(0, 0, 1, 1, GL_STENCIL_INDEX); /* a format; the buffer is GL_STENCIL */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* **A depth texture**: 2x1, 0.25 and 0.75, read as luminance - (64, 64, 64) and (191, ...). */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  static const GLfloat dtex[2] = {0.25f, 0.75f};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 2, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, dtex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  GLint iv[1] = {0};
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_DEPTH_SIZE, iv);
  ASSERT_EQ(iv[0], 32);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_LUMINANCE);
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  #define DQUAD() do { glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_QUADS); \
    glTexCoord3f(0, 0, 0.5f); glVertex2f(0, 0); glTexCoord3f(1, 0, 0.5f); glVertex2f(16, 0); \
    glTexCoord3f(1, 1, 0.5f); glVertex2f(16, 16); glTexCoord3f(0, 1, 0.5f); glVertex2f(0, 16); \
    glEnd(); } while (0)
  DQUAD();
  ASSERT_EQ(PIX(4, 8), 0x404040u);
  ASSERT_EQ(PIX(12, 8), 0xbfbfbfu);
  /* **The comparison**: r = 0.5 against 0.25 and 0.75 under GL_LEQUAL - 0 where 0.5 > 0.25, 1
   * where 0.5 <= 0.75 - and the other way round under GL_GEQUAL. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
  DQUAD();
  ASSERT_EQ(PIX(4, 8), 0x000000u);
  ASSERT_EQ(PIX(12, 8), 0xffffffu);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_GEQUAL);
  DQUAD();
  ASSERT_EQ(PIX(4, 8), 0xffffffu);
  ASSERT_EQ(PIX(12, 8), 0x000000u);
  /* GL_ALPHA reads the result as alpha alone: under GL_REPLACE the fragment's own green stays on
   * both sides, as an alpha texture's does. */
  glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_ALPHA);
  DQUAD();
  ASSERT_EQ(PIX(4, 8), 0x00ff00u);
  ASSERT_EQ(PIX(12, 8), 0x00ff00u);
  glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  /* Read back as depth only. */
  glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  ASSERT_TRUE(zf[0] == 0.25f && zf[1] == 0.75f);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, sv);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  /* **Copied from the depth buffer**: glCopyTexImage2D with a depth format reads depth. */
  glClearDepth(0.5);
  glClear(GL_DEPTH_BUFFER_BIT);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 0, 0, 2, 1, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, zf);
  ASSERT_TRUE(zf[0] == 0.5f && zf[1] == 0.5f);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);

  /* The checks: depth data for a depth format and only for one, a 1D or 2D target, the
   * parameters' values. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, sv);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, dtex);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sv);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_DEPTH_COMPONENT, 2, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, dtex);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_LEQUAL);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_RGBA);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  #undef DQUAD
  #undef PIX

  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.0's colour-index images in an RGBA context: glDrawPixels and a texture upload through the
 * index shift and offset and the I_TO_R/G/B/A maps; never read back. */
static void test_gl_colour_index_images(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  #define PIX(x, y) (fb[(H - 1 - (y)) * W + (x)] & 0x00ffffffu)

  /* The default maps are one entry of 0: every index is transparent black. */
  static const GLubyte idx[2] = {0, 1};
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glWindowPos2i(2, 2);
  glDrawPixels(2, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(PIX(2, 2), 0x000000u);

  /* Two-entry maps: index 0 blue, index 1 red. */
  static const GLfloat r[2] = {0.0f, 1.0f}, g[2] = {0.0f, 0.0f}, b[2] = {1.0f, 0.0f},
                       a[2] = {1.0f, 1.0f};
  glPixelMapfv(GL_PIXEL_MAP_I_TO_R, 2, r);
  glPixelMapfv(GL_PIXEL_MAP_I_TO_G, 2, g);
  glPixelMapfv(GL_PIXEL_MAP_I_TO_B, 2, b);
  glPixelMapfv(GL_PIXEL_MAP_I_TO_A, 2, a);
  glDrawPixels(2, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
  ASSERT_EQ(PIX(2, 2), 0x0000ffu);
  ASSERT_EQ(PIX(3, 2), 0xff0000u);
  /* **Through the offset**, and not the RGBA scale: index 0 + 1 is red, and a red scale of 0
   * leaves it red - the maps stand in for the colour transfer. */
  glPixelTransferi(GL_INDEX_OFFSET, 1);
  glPixelTransferf(GL_RED_SCALE, 0.0f);
  glDrawPixels(1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
  glPixelTransferi(GL_INDEX_OFFSET, 0);
  glPixelTransferf(GL_RED_SCALE, 1.0f);
  ASSERT_EQ(PIX(2, 2), 0xff0000u);
  /* Masked to the map's size: index 3 is index 1. */
  static const GLshort idx3[1] = {3};
  glDrawPixels(1, 1, GL_COLOR_INDEX, GL_SHORT, idx3);
  ASSERT_EQ(PIX(2, 2), 0xff0000u);

  /* **A texture uploaded from indices** is the colours they map to. */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, idx);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  GLubyte texels[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  ASSERT_TRUE(texels[2] == 255 && texels[0] == 0 && texels[4] == 255 && texels[6] == 0);

  /* **Never read back**: an RGBA buffer holds no indices, and a texture is no index format. */
  GLubyte out[4] = {0, 0, 0, 0};
  glReadPixels(0, 0, 1, 1, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, out);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE, out);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* No packed type holds an index. */
  glDrawPixels(1, 1, GL_COLOR_INDEX, GL_UNSIGNED_SHORT_5_6_5, idx);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* **GL_BITMAP**: an index a bit, most significant first - 1010 0000 is red, blue, red, blue
   * - for glDrawPixels, a texture upload and a compiled list; and only for index formats. */
  static const GLubyte bits[1] = {0xa0};
  glClear(GL_COLOR_BUFFER_BIT);
  glWindowPos2i(4, 4);
  glDrawPixels(4, 1, GL_COLOR_INDEX, GL_BITMAP, bits);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_TRUE(PIX(4, 4) == 0xff0000u && PIX(5, 4) == 0x0000ffu && PIX(6, 4) == 0xff0000u);
  glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE); /* 0xa0 read from bit 0: 0, 0, 0, 0, 0, 1 */
  glWindowPos2i(4, 6);
  glDrawPixels(6, 1, GL_COLOR_INDEX, GL_BITMAP, bits);
  glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
  ASSERT_TRUE(PIX(4, 6) == 0x0000ffu && PIX(9, 6) == 0xff0000u);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_COLOR_INDEX, GL_BITMAP, bits);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  ASSERT_TRUE(texels[0] == 255 && texels[6] == 255 && texels[4] == 0);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glWindowPos2i(4, 8);
  glDrawPixels(4, 1, GL_COLOR_INDEX, GL_BITMAP, bits);
  glEndList();
  glCallList(list);
  ASSERT_TRUE(PIX(4, 8) == 0xff0000u && PIX(5, 8) == 0x0000ffu);
  glDeleteLists(list, 1);
  glDrawPixels(4, 1, GL_RGBA, GL_BITMAP, bits);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* Stencil indices as bits, drawn and read back. */
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glWindowPos2i(0, 0);
  glDrawPixels(4, 1, GL_STENCIL_INDEX, GL_BITMAP, bits);
  GLubyte sread[4] = {9, 9, 9, 9};
  glReadPixels(0, 0, 4, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sread);
  ASSERT_TRUE(sread[0] == 1 && sread[1] == 0 && sread[2] == 1 && sread[3] == 0);
  GLubyte sbits[1] = {0x0f};
  glReadPixels(0, 0, 4, 1, GL_STENCIL_INDEX, GL_BITMAP, sbits);
  ASSERT_EQ(sbits[0], 0xafu); /* the four bits written, the rest of the byte left alone */
  glDeleteTextures(1, &tex);
  #undef PIX

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Projective texturing: q divides s, t and r per fragment, after interpolation, so a q that
 * varies across a primitive bends the mapping rather than being averaged away at the vertices.
 *
 * A 32-pixel-wide strip with (s, q) = (0, 1) on its left edge and (3, 3) on its right: s/q runs
 * 0 to 1 either way, but interpolated first it is 3f / (1 + 2f) at fraction f of the width, which
 * reaches the second texel of a two-texel texture at f = 1/4 - column 8 - where a per-vertex
 * divide would have reached it only at column 16. */
static void test_gl_projective_texcoords_divide_per_fragment(void) {
  const int W = 32, H = 8;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 32.0, 0.0, 8.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  #define PIX(x, y) (fb[(H - 1 - (y)) * W + (x)] & 0x00ffffffu)

  static const GLubyte texels[2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255}; /* red, green */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glEnable(GL_TEXTURE_2D);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  glTexCoord4f(0.0f, 0.0f, 0.0f, 1.0f); glVertex2f(0.0f, 2.0f);
  glTexCoord4f(3.0f, 0.0f, 0.0f, 3.0f); glVertex2f(32.0f, 2.0f);
  glTexCoord4f(3.0f, 0.0f, 0.0f, 3.0f); glVertex2f(32.0f, 6.0f);
  glTexCoord4f(0.0f, 0.0f, 0.0f, 1.0f); glVertex2f(0.0f, 6.0f);
  glEnd();
  for (int y = 2; y < 6; y++) {
    int first_green = -1;
    for (int x = 0; x < W; x++) {
      const uint32_t c = PIX(x, y);
      ASSERT_TRUE(c == 0xff0000u || c == 0x00ff00u);
      if (c == 0x00ff00u && first_green < 0) first_green = x;
      if (first_green >= 0) ASSERT_EQ(c, 0x00ff00u); /* one boundary, no stray texels past it */
    }
    ASSERT_EQ(first_green, 8);
  }
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);
  #undef PIX

  /* Feedback returns the coordinate as the vertex kept it: all four components, undivided
   * (mesa/src/mesa/main/feedback.c:136-139). */
  GLfloat fbuf[16];
  for (int i = 0; i < 16; i++) fbuf[i] = -1.0f;
  glFeedbackBuffer(16, GL_4D_COLOR_TEXTURE, fbuf);
  glRenderMode(GL_FEEDBACK);
  glBegin(GL_POINTS);
  glTexCoord4f(2.0f, 4.0f, 6.0f, 2.0f);
  glVertex2f(1.0f, 1.0f);
  glEnd();
  ASSERT_EQ(glRenderMode(GL_RENDER), 13);
  ASSERT_TRUE(fbuf[0] == (GLfloat)GL_POINT_TOKEN);
  ASSERT_TRUE(fbuf[9] == 2.0f && fbuf[10] == 4.0f && fbuf[11] == 6.0f && fbuf[12] == 2.0f);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **What an audit of the specification's state tables found** (2026-09-19): every name GL 1.5's
 * tables give glGet or glIsEnabled was queried through every getter, and every GL 1.0-1.5 enum
 * in Mesa's headers checked against gl.h. Each gap it turned up is pinned here. */
/* **The front buffer** (GL 1.0, 4.2.1; since 2026-09-19): a surface of its own beside the back,
 * drawn into, cleared and read by name. GL_FRONT_AND_BACK and GL_LEFT draw into both and read
 * the front (Mesa main/buffers.c:145-170, :209-226). A swap makes it the back's picture, and it
 * waits to be presented while drawn into. The host has no screen, so the front starts as the
 * back. */
static void test_gl_front_buffer(void) {
  const int W = 8, H = 8;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *back = oops_display_get_framebuffer(disp);
  (void)glGetError();
  GLint iv[1];
  GLubyte px[4];
  const size_t mid = (size_t)(H / 2) * W + W / 2;
  #define RGB(p) ((p) & 0x00ffffffu)

  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ASSERT_TRUE(ctx->front_fb == NULL); /* none until a program names it */
  glDrawBuffer(GL_FRONT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_FRONT);
  ASSERT_TRUE(ctx->front_fb != NULL && ctx->front_fb != back);
  ASSERT_EQ(RGB(ctx->front_fb[mid]), 0x0000ffu);

  /* Drawn into, the front changes and the back does not. */
  glColor3f(1.0f, 0.0f, 0.0f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(RGB(ctx->front_fb[mid]), 0xff0000u);
  ASSERT_EQ(RGB(back[mid]), 0x0000ffu);

  /* Read by name. */
  glReadBuffer(GL_BACK);
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ASSERT_TRUE(px[0] == 0u && px[2] == 255u);
  glReadBuffer(GL_FRONT);
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ASSERT_TRUE(px[0] == 255u && px[2] == 0u);
  glReadBuffer(GL_BACK);
  glReadBuffer(GL_FRONT_AND_BACK);
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ASSERT_TRUE(px[0] == 255u && px[2] == 0u);
  glReadBuffer(GL_LEFT);
  glGetIntegerv(GL_READ_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_LEFT);
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
  ASSERT_TRUE(px[0] == 255u && px[2] == 0u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* glCopyPixels from the read buffer into the draw buffer: the front's red into the back's
   * bottom-left pixel. */
  glReadBuffer(GL_FRONT);
  glDrawBuffer(GL_BACK);
  glWindowPos2i(0, 0);
  glCopyPixels(W / 2, H / 2, 1, 1, GL_COLOR);
  ASSERT_EQ(RGB(back[(size_t)(H - 1) * W]), 0xff0000u);
  ASSERT_EQ(RGB(ctx->front_fb[(size_t)(H - 1) * W]), 0xff0000u);

  /* Both: drawn into together, each blended against its own pixel - red plus green in the
   * front, blue plus green in the back. */
  glDrawBuffer(GL_FRONT_AND_BACK);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glColor3f(0.0f, 1.0f, 0.0f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  glDisable(GL_BLEND);
  ASSERT_EQ(RGB(ctx->front_fb[mid]), 0xffff00u);
  ASSERT_EQ(RGB(back[mid]), 0x00ffffu);
  /* GL_LEFT is the same pair, and a clear reaches both. */
  glDrawBuffer(GL_LEFT);
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ASSERT_EQ(RGB(ctx->front_fb[mid]), 0xffffffu);
  ASSERT_EQ(RGB(back[mid]), 0xffffffu);
  /* And the accumulation buffer's return. */
  glClearAccum(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_ACCUM_BUFFER_BIT);
  glAccum(GL_RETURN, 1.0f);
  ASSERT_EQ(RGB(ctx->front_fb[mid]), 0x000000u);
  ASSERT_EQ(RGB(back[mid]), 0x000000u);

  /* A swap: the front becomes the picture just presented, the back's. */
  glDrawBuffer(GL_BACK);
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glSwapBuffers();
  ASSERT_EQ(RGB(ctx->front_fb[mid]), 0x00ff00u);
  ASSERT_TRUE(ctx->framebuffer == back && ctx->fb_also == NULL);

  /* Waiting to be presented once drawn into, until glFlush puts it on screen. */
  ASSERT_EQ(ctx->front_pending, GL_FALSE);
  glDrawBuffer(GL_FRONT);
  ASSERT_EQ(ctx->front_pending, GL_TRUE);
  glDrawBuffer(GL_BACK);
  ASSERT_EQ(ctx->front_pending, GL_TRUE);
  glFlush();
  ASSERT_EQ(ctx->front_pending, GL_FALSE);

  /* Saved with GL_COLOR_BUFFER_BIT, and the buffer drawn into follows it back. */
  glDrawBuffer(GL_FRONT);
  glPushAttrib(GL_COLOR_BUFFER_BIT);
  glDrawBuffer(GL_BACK);
  ASSERT_TRUE(ctx->framebuffer == back);
  glPopAttrib();
  ASSERT_TRUE(ctx->framebuffer == ctx->front_fb);
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  #undef RGB

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_gl_state_table_audit_findings(void) {
  const int W = 8, H = 8;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  GLint iv[16];
  GLfloat fv[16];

  /* Float-valued state through glGetIntegerv: normalised ones mapped linearly (GL 1.5, 6.1.2),
   * the rest rounded. Every one of these was GL_INVALID_ENUM. */
  glAlphaFunc(GL_GREATER, 0.5f);
  glGetIntegerv(GL_ALPHA_TEST_REF, iv);
  ASSERT_EQ(iv[0], 1073741823);
  glClearDepth(1.0);
  glGetIntegerv(GL_DEPTH_CLEAR_VALUE, iv);
  ASSERT_EQ(iv[0], 2147483647);
  glGetIntegerv(GL_CURRENT_NORMAL, iv);
  ASSERT_TRUE(iv[0] == 0 && iv[1] == 0 && iv[2] == 2147483647);
  glPixelZoom(2.5f, -1.0f);
  glGetIntegerv(GL_ZOOM_X, iv);
  ASSERT_EQ(iv[0], 3);
  glGetIntegerv(GL_ZOOM_Y, iv);
  ASSERT_EQ(iv[0], -1);
  glPixelZoom(1.0f, 1.0f);
  glPolygonOffset(1.4f, 2.6f);
  glGetIntegerv(GL_POLYGON_OFFSET_FACTOR, iv);
  ASSERT_EQ(iv[0], 1);
  glGetIntegerv(GL_POLYGON_OFFSET_UNITS, iv);
  ASSERT_EQ(iv[0], 3);
  glGetIntegerv(GL_CURRENT_RASTER_POSITION, iv);
  ASSERT_TRUE(iv[0] == 0 && iv[3] == 1);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetIntegerv(0x1234, iv); /* and a name nothing knows still stops, rather than recursing */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* The texture matrix stack's depth, per unit; the transposed matrices. */
  glMatrixMode(GL_TEXTURE);
  glGetIntegerv(GL_TEXTURE_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], 1);
  glPushMatrix();
  glGetIntegerv(GL_TEXTURE_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], 2);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], 1);
  glActiveTexture(GL_TEXTURE0);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTranslatef(1.0f, 2.0f, 3.0f);
  glGetFloatv(GL_TRANSPOSE_MODELVIEW_MATRIX, fv);
  ASSERT_TRUE(fv[3] == 1.0f && fv[7] == 2.0f && fv[11] == 3.0f && fv[12] == 0.0f);
  glGetFloatv(GL_MODELVIEW_MATRIX, fv);
  ASSERT_TRUE(fv[12] == 1.0f && fv[3] == 0.0f);
  glLoadIdentity();

  /* The ten table names that were not even declared. */
  glGetIntegerv(GL_MAX_LIST_NESTING, iv);  ASSERT_EQ(iv[0], 64);
  glGetIntegerv(GL_AUX_BUFFERS, iv);       ASSERT_EQ(iv[0], 0);
  glGetIntegerv(GL_DOUBLEBUFFER, iv);      ASSERT_EQ(iv[0], 1);
  glGetIntegerv(GL_STEREO, iv);            ASSERT_EQ(iv[0], 0);
  glGetIntegerv(GL_SUBPIXEL_BITS, iv);     ASSERT_EQ(iv[0], 4);
  glGetIntegerv(GL_CURRENT_RASTER_INDEX, iv); ASSERT_EQ(iv[0], 1);
  glGetIntegerv(GL_MAX_ELEMENTS_VERTICES, iv); ASSERT_TRUE(iv[0] > 0);
  glGetIntegerv(GL_MAX_ELEMENTS_INDICES, iv);  ASSERT_TRUE(iv[0] > 0);
  glGetIntegerv(GL_LIST_INDEX, iv);        ASSERT_EQ(iv[0], 0);
  glNewList(5, GL_COMPILE);
  glGetIntegerv(GL_LIST_INDEX, iv);        ASSERT_EQ(iv[0], 5); /* a get is not compiled */
  glGetIntegerv(GL_LIST_MODE, iv);         ASSERT_EQ(iv[0], (GLint)GL_COMPILE);
  glEndList();
  glGetIntegerv(GL_LIST_MODE, iv);         ASSERT_EQ(iv[0], 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* glCallLists' ten types (GL 1.0) - three were accepted. Lists 10 and 11 each set a colour. */
  glNewList(10, GL_COMPILE); glColor3f(1.0f, 0.0f, 0.0f); glEndList();
  glNewList(11, GL_COMPILE); glColor3f(0.0f, 1.0f, 0.0f); glEndList();
  glListBase(12);
  static const GLbyte back2[1] = {-2};                /* 12 - 2 = 10 */
  static const GLubyte two[2] = {0xff, 0xff};         /* 12 + 65535, which does not exist */
  static const GLubyte two11[2] = {0x00, 0x00};       /* 12 + 0: none - so nothing changes */
  static const GLfloat fl[1] = {-0.7f};               /* truncated to 0: list 12, none */
  static const GLubyte four[4] = {0xff, 0xff, 0xff, 0xff}; /* -1 as a 32-bit name: 11 */
  glColor3f(0.0f, 0.0f, 1.0f);
  glCallLists(1, GL_BYTE, back2);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == 0.0f);
  glCallLists(1, GL_4_BYTES, four);
  ASSERT_TRUE(ctx->cur_color[0] == 0.0f && ctx->cur_color[1] == 1.0f);
  glCallLists(1, GL_2_BYTES, two);
  glCallLists(1, GL_2_BYTES, two11);
  glCallLists(1, GL_FLOAT, fl);
  ASSERT_TRUE(ctx->cur_color[1] == 1.0f);
  static const GLubyte three[3] = {0x00, 0x00, 0x00};
  glListBase(10);
  glCallLists(1, GL_3_BYTES, three);                 /* 10 + 0 */
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f);
  glListBase(0);
  glDeleteLists(10, 2);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* glDrawBuffer and glReadBuffer: GL_NONE draws no colour, GL_BACK_LEFT is the back buffer,
   * the absent buffers are GL_INVALID_OPERATION, and a name that is no buffer is
   * GL_INVALID_ENUM. The front has its own test, test_gl_front_buffer. */
  glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawBuffer(GL_NONE);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NONE);
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(0.0f, 1.0f, 0.0f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[(H / 2) * W + W / 2] & 0x00ffffffu, 0x0000ffu); /* still the first clear */
  glDrawBuffer(GL_BACK_LEFT);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK_LEFT);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(fb[(H / 2) * W + W / 2] & 0x00ffffffu, 0x00ff00u);
  glDrawBuffer(GL_AUX0);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glDrawBuffer(GL_RIGHT);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glDrawBuffer(GL_TEXTURE_2D);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK_LEFT); /* the refusals changed nothing */
  glReadBuffer(GL_NONE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glReadBuffer(GL_BACK_LEFT);
  glGetIntegerv(GL_READ_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK_LEFT);
  /* Saved with their groups: the draw buffer with the colour buffer bit, the read buffer with
   * the pixel mode bit. */
  glPushAttrib(GL_COLOR_BUFFER_BIT | GL_PIXEL_MODE_BIT);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_BACK);
  glPopAttrib();
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK_LEFT);
  glGetIntegerv(GL_READ_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK_LEFT);
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);

  /* GL_CURRENT_BIT carries the raster position (table 6.5), which the push left out. */
  glRasterPos2f(0.5f, 0.5f);
  glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
  const float saved_x = fv[0];
  glPushAttrib(GL_CURRENT_BIT);
  glRasterPos2f(-0.5f, -0.5f);
  glPopAttrib();
  glGetFloatv(GL_CURRENT_RASTER_POSITION, fv);
  ASSERT_TRUE(fv[0] == saved_x);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Two texture-enable fixes that came with the per-unit state (2026-09-19): the drawn clear sets
 * aside every texture target, not just 1D and 2D, and GL_ENABLE_BIT carries the texture
 * generation enables (GL 1.3, table 6.20, "texture/enable"; Mesa main/attrib.c:189-192). */
static void test_gl_drawn_clear_and_enable_bit_cover_every_texture_enable(void) {
  const int W = 8, H = 8;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  /* A red 3D texture, enabled. A scissored clear is drawn as a quad at the clear colour; were it
   * textured, red modulating green would clear to black. */
  static const GLubyte red[2 * 2 * 2 * 4] = {
    255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
    255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
  };
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_3D, tex);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
  glEnable(GL_TEXTURE_3D);
  glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
  glEnable(GL_SCISSOR_TEST);
  glScissor(2, 2, 4, 4);
  glClear(GL_COLOR_BUFFER_BIT);
  ASSERT_EQ(fb[4 * W + 4] & 0x00ffffffu, 0x00ff00u);
  glDisable(GL_SCISSOR_TEST);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_3D), GL_TRUE); /* and the enable came back after the clear */
  glDisable(GL_TEXTURE_3D);
  glDeleteTextures(1, &tex);

  /* GL_ENABLE_BIT alone restores a generation enable. */
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_GEN_S), GL_FALSE);
  glPushAttrib(GL_ENABLE_BIT);
  glEnable(GL_TEXTURE_GEN_S);
  glEnable(GL_TEXTURE_2D);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_GEN_S), GL_FALSE);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_2D), GL_FALSE);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.5's buffer mapping and read back, its full set of usages, and its occlusion queries. */
static void test_gl_buffer_mapping_and_occlusion_queries(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0.0, 16.0, 0.0, 16.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  GLint iv[1] = {0};

  /* **Buffers**: a _READ usage, refused until 2026-09-19; mapped, written through the pointer,
   * and read back once unmapped. */
  static const GLfloat quad0[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  GLuint buf = 0;
  glGenBuffers(1, &buf);
  glBindBuffer(GL_ARRAY_BUFFER, buf);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad0), quad0, GL_STATIC_READ);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS, iv); ASSERT_EQ(iv[0], (GLint)GL_READ_WRITE);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, iv); ASSERT_EQ(iv[0], GL_FALSE);
  GLfloat *p = (GLfloat *)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
  ASSERT_TRUE(p != NULL);
  static const GLfloat quad[8] = {4, 4, 8, 4, 8, 8, 4, 8};
  memcpy(p, quad, sizeof(quad));
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, iv); ASSERT_EQ(iv[0], GL_TRUE);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS, iv); ASSERT_EQ(iv[0], (GLint)GL_WRITE_ONLY);
  GLvoid *mp = NULL;
  glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &mp);
  ASSERT_TRUE(mp == (GLvoid *)p);
  /* **While mapped**: no second map, no update, no read back, no draw from it. */
  ASSERT_TRUE(glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY) == NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glBufferSubData(GL_ARRAY_BUFFER, 0, 4, quad);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  GLfloat back[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)sizeof(back), back);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, (const GLvoid *)0);
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  ASSERT_EQ(glUnmapBuffer(GL_ARRAY_BUFFER), GL_TRUE);
  ASSERT_EQ(glUnmapBuffer(GL_ARRAY_BUFFER), GL_FALSE);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glGetBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)sizeof(back), back);
  ASSERT_TRUE(back[0] == 4.0f && back[5] == 8.0f && back[7] == 8.0f);
  glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &mp);
  ASSERT_TRUE(mp == NULL);
  /* The checks: an access, a pointer name, a range. */
  ASSERT_TRUE(glMapBuffer(GL_ARRAY_BUFFER, GL_STATIC_DRAW) == NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &mp);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetBufferSubData(GL_ARRAY_BUFFER, 4, (GLsizeiptr)sizeof(back), back);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  /* A new store unmaps. */
  (void)glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(quad), quad, GL_DYNAMIC_COPY);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, iv); ASSERT_EQ(iv[0], GL_FALSE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **Occlusion queries.** Names from glGenQueries are query objects only once begun. */
  GLuint q[2] = {0, 0};
  glGenQueries(2, q);
  ASSERT_TRUE(q[0] != 0u && q[1] != 0u && q[0] != q[1]);
  ASSERT_EQ(glIsQuery(q[0]), GL_FALSE);
  glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS, iv); ASSERT_EQ(iv[0], 32);
  glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, iv);      ASSERT_EQ(iv[0], 0);
  /* The 4x4 quad from the buffer: sixteen samples. */
  glBeginQuery(GL_SAMPLES_PASSED, q[0]);
  glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, iv); ASSERT_EQ(iv[0], (GLint)q[0]);
  glGetQueryObjectiv(q[0], GL_QUERY_RESULT, iv);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* active */
  glDrawArrays(GL_QUADS, 0, 4);
  glEndQuery(GL_SAMPLES_PASSED);
  ASSERT_EQ(glIsQuery(q[0]), GL_TRUE);
  GLuint uv = 0;
  glGetQueryObjectuiv(q[0], GL_QUERY_RESULT_AVAILABLE, &uv); ASSERT_EQ(uv, (GLuint)GL_TRUE);
  glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &uv);           ASSERT_EQ(uv, 16u);
  glDisableClientState(GL_VERTEX_ARRAY);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  /* **Only what passes the depth test**: a quad behind a nearer one counts nothing; and only
   * what passes the alpha test. */
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glBegin(GL_QUADS);
  glVertex3f(0, 0, 0.5f); glVertex3f(16, 0, 0.5f); glVertex3f(16, 16, 0.5f); glVertex3f(0, 16, 0.5f);
  glEnd();
  glBeginQuery(GL_SAMPLES_PASSED, q[1]);
  glBegin(GL_QUADS); /* z -0.5 is further under this orthographic projection */
  glVertex3f(2, 2, -0.5f); glVertex3f(6, 2, -0.5f); glVertex3f(6, 6, -0.5f); glVertex3f(2, 6, -0.5f);
  glEnd();
  glEndQuery(GL_SAMPLES_PASSED);
  glGetQueryObjectuiv(q[1], GL_QUERY_RESULT, &uv); ASSERT_EQ(uv, 0u);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  glColor4f(1.0f, 1.0f, 1.0f, 0.25f);
  glBeginQuery(GL_SAMPLES_PASSED, q[1]);
  glRectf(0, 0, 4, 4);
  glEndQuery(GL_SAMPLES_PASSED);
  glGetQueryObjectuiv(q[1], GL_QUERY_RESULT, &uv); ASSERT_EQ(uv, 0u);
  glDisable(GL_ALPHA_TEST);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  /* **Pixel rectangles are fragments too**: a 2x2 glDrawPixels, four samples - and a new name
   * at glBeginQuery is made a query object, as a compatibility context allows. */
  static const GLubyte px4[16] = {255, 255, 255, 255, 255, 255, 255, 255,
                                  255, 255, 255, 255, 255, 255, 255, 255};
  glBeginQuery(GL_SAMPLES_PASSED, 50u);
  glWindowPos2i(10, 10);
  glDrawPixels(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px4);
  glEndQuery(GL_SAMPLES_PASSED);
  glGetQueryObjectuiv(50u, GL_QUERY_RESULT, &uv); ASSERT_EQ(uv, 4u);
  /* **Compiled**: the begin and end replay around the draw between them. */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glBeginQuery(GL_SAMPLES_PASSED, q[0]);
  glRectf(0, 0, 2, 2);
  glEndQuery(GL_SAMPLES_PASSED);
  glEndList();
  glCallList(list);
  glGetQueryObjectuiv(q[0], GL_QUERY_RESULT, &uv); ASSERT_EQ(uv, 4u);
  glDeleteLists(list, 1);
  /* The checks. */
  glBeginQuery(GL_SAMPLES_PASSED, 0u);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glEndQuery(GL_SAMPLES_PASSED);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glBeginQuery((GLenum)0x8C2Fu /* GL 3.3's GL_ANY_SAMPLES_PASSED */, q[0]);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBeginQuery(GL_SAMPLES_PASSED, q[0]);
  glBeginQuery(GL_SAMPLES_PASSED, q[1]);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glGetQueryObjectuiv(q[0], GL_QUERY_COUNTER_BITS, &uv);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* still active: that is checked first */
  /* Deleting the active query ends it. */
  glDeleteQueries(1, &q[0]);
  glGetQueryiv(GL_SAMPLES_PASSED, GL_CURRENT_QUERY, iv); ASSERT_EQ(iv[0], 0);
  ASSERT_EQ(glIsQuery(q[0]), GL_FALSE);
  glGetQueryObjectuiv(q[1], GL_QUERY_COUNTER_BITS, &uv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glDeleteQueries(1, &q[1]);
  GLuint q50 = 50u;
  glDeleteQueries(1, &q50);
  glDeleteBuffers(1, &buf);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL_COLOR_MATERIAL writes the current colour into the material it tracks - so the material reads
 * back as the colour, glMaterial cannot set a tracked property, and the last tracked colour stays
 * when the enable goes off. */
static void test_gl_color_material_writes_the_material(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 16, 16);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();
  GLfloat m[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const GLfloat green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  #define IS(v, r, g, b) ASSERT_TRUE((v)[0] == (r) && (v)[1] == (g) && (v)[2] == (b))

  /* Enabled with the defaults - both sides, ambient and diffuse: the current colour now, and
   * each colour after it. */
  glColor3f(1.0f, 0.0f, 0.0f);
  glEnable(GL_COLOR_MATERIAL);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, m);  IS(m, 1.0f, 0.0f, 0.0f);
  glGetMaterialfv(GL_BACK, GL_AMBIENT, m);   IS(m, 1.0f, 0.0f, 0.0f);
  glColor3f(0.0f, 0.0f, 1.0f);
  glGetMaterialfv(GL_FRONT, GL_AMBIENT, m);  IS(m, 0.0f, 0.0f, 1.0f);

  /* A tracked property is the colour's: glMaterial leaves it and sets the rest. */
  glMaterialfv(GL_FRONT, GL_DIFFUSE, green);
  glMaterialfv(GL_FRONT, GL_SPECULAR, green);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, m);  IS(m, 0.0f, 0.0f, 1.0f);
  glGetMaterialfv(GL_FRONT, GL_SPECULAR, m); IS(m, 0.0f, 1.0f, 0.0f);

  /* **Off, the material keeps the last colour it tracked**, and later colours leave it. */
  glDisable(GL_COLOR_MATERIAL);
  glColor3f(1.0f, 1.0f, 1.0f);
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, m);  IS(m, 0.0f, 0.0f, 1.0f);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, green); /* and glMaterial sets it again */
  glGetMaterialfv(GL_FRONT, GL_DIFFUSE, m);  IS(m, 0.0f, 1.0f, 0.0f);

  /* **glColorMaterial while on** tracks the new property at once, on its side only. */
  glColor3f(1.0f, 0.0f, 1.0f);
  glColorMaterial(GL_FRONT, GL_EMISSION);
  glEnable(GL_COLOR_MATERIAL);
  glGetMaterialfv(GL_FRONT, GL_EMISSION, m); IS(m, 1.0f, 0.0f, 1.0f);
  glGetMaterialfv(GL_BACK, GL_EMISSION, m);  IS(m, 0.0f, 0.0f, 0.0f);
  glColorMaterial(GL_FRONT, GL_SPECULAR);
  glGetMaterialfv(GL_FRONT, GL_SPECULAR, m); IS(m, 1.0f, 0.0f, 1.0f);
  glDisable(GL_COLOR_MATERIAL);
  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
  #undef IS

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Antialiasing: a smooth point, line and polygon each weigh their fragments' alpha by the fraction
 * of the pixel they cover. Read as alpha, over a transparent black clear with blending off. */
static void test_gl_smooth_points_lines_polygons(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  GLubyte img[16 * 16 * 4];
  /* Alpha of window pixel (x, y), y counting up from the bottom as glReadPixels does. */
  #define ALPHA(x, y) (img[((y) * W + (x)) * 4 + 3])
  #define GRAB() glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img)

  /* The enables, their groups, and the step a smooth size takes. */
  ASSERT_EQ(glIsEnabled(GL_POINT_SMOOTH), GL_FALSE);
  glPushAttrib(GL_POINT_BIT | GL_LINE_BIT | GL_POLYGON_BIT);
  glEnable(GL_POINT_SMOOTH); glEnable(GL_LINE_SMOOTH); glEnable(GL_POLYGON_SMOOTH);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(glIsEnabled(GL_LINE_SMOOTH), GL_TRUE);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_LINE_SMOOTH), GL_FALSE);
  ASSERT_EQ(glIsEnabled(GL_POLYGON_SMOOTH), GL_FALSE);
  GLfloat fv[2] = {0.0f, 0.0f};
  glGetFloatv(GL_SMOOTH_POINT_SIZE_GRANULARITY, fv);
  ASSERT_TRUE(fv[0] == OOPS_GL_SMOOTH_GRANULARITY);

  /* **A smooth point** of size 6 at the centre, (8, 8) in window pixels: a disc of radius 3.
   * Its centre pixel is covered, one 2.5 across and 0.5 up almost all of it, and the corner
   * of the 6x6 square an aliased point fills - 3.5 away both ways - not at all. */
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_POINT_SMOOTH);
  glPointSize(6.0f);
  glBegin(GL_POINTS); glVertex2f(0.0f, 0.0f); glEnd();
  glDisable(GL_POINT_SMOOTH);
  GRAB();
  ASSERT_EQ(ALPHA(7, 7), 255);
  ASSERT_TRUE(ALPHA(10, 8) > 230 && ALPHA(10, 8) < 255);
  ASSERT_EQ(ALPHA(10, 10), 0);
  ASSERT_EQ(ALPHA(4, 7), 0); /* 3.5 from the centre along x: outside */

  /* **A smooth line** 3 wide along y = 8 from x = 2 to 14: rows 7 and 8 lie inside it, rows 6
   * and 9 half inside (their centres 1.5 from the axis, the edge at 1.5); and it stops at its
   * ends - column 1 is outside. */
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_LINE_SMOOTH);
  glLineWidth(3.0f);
  glBegin(GL_LINES); glVertex2f(-0.75f, 0.0f); glVertex2f(0.75f, 0.0f); glEnd();
  glDisable(GL_LINE_SMOOTH);
  glLineWidth(1.0f);
  GRAB();
  ASSERT_EQ(ALPHA(6, 7), 255);
  ASSERT_EQ(ALPHA(6, 8), 255);
  ASSERT_TRUE(ALPHA(6, 6) > 118 && ALPHA(6, 6) < 138);
  ASSERT_TRUE(ALPHA(6, 9) > 118 && ALPHA(6, 9) < 138);
  ASSERT_EQ(ALPHA(6, 10), 0);
  ASSERT_EQ(ALPHA(1, 7), 0);

  /* **A smooth polygon**, a square from 4.5 to 11.5: its edge columns are half covered, its
   * inside is whole - along the diagonal where its two triangles meet too, which must not
   * fade. */
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_POLYGON_SMOOTH);
  glRectf(-0.4375f, -0.4375f, 0.4375f, 0.4375f);
  glDisable(GL_POLYGON_SMOOTH);
  GRAB();
  ASSERT_TRUE(ALPHA(4, 8) > 118 && ALPHA(4, 8) < 138);
  ASSERT_TRUE(ALPHA(11, 8) > 118 && ALPHA(11, 8) < 138);
  ASSERT_TRUE(ALPHA(8, 4) > 118 && ALPHA(8, 4) < 138);
  ASSERT_EQ(ALPHA(3, 8), 0);
  for (int i = 5; i <= 10; i++) {
    ASSERT_EQ(ALPHA(i, i), 255);
    ASSERT_EQ(ALPHA(i, 15 - i), 255);
  }
  /* A corner pixel is covered a quarter - the two edges' halves multiplied - where both edges
   * are one triangle's: the bottom right and top left of glRectf's quad. **At the two corners
   * the diagonal leaves, each triangle knows one of the corner's edges**, and the pixel comes
   * out half covered: the approximation of fading each triangle across its own boundary edges. */
  ASSERT_TRUE(ALPHA(11, 4) > 54 && ALPHA(11, 4) < 74);
  ASSERT_TRUE(ALPHA(4, 11) > 54 && ALPHA(4, 11) < 74);
  ASSERT_TRUE(ALPHA(4, 4) > 118 && ALPHA(4, 4) < 138);
  #undef GRAB
  #undef ALPHA

  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL_COMBINE (GL 1.3): each colour and alpha function, operands, sources, scales, the DOT3
 * functions, and the parameters' checks. One 1x1 texture over a full-screen quad, read at the
 * centre. */
static void test_gl_texture_combine(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  GLubyte out[4];
  GLint iv = 0;
  #define TEXEL(r, g, b, a) do { const GLubyte tx[4] = {r, g, b, a}; \
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx); } while (0)
  #define DRAW(r, g, b, a) do { glColor4f(r, g, b, a); glRectf(-1.0f, -1.0f, 1.0f, 1.0f); \
      glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out); } while (0)
  #define NEAR2(v, want) ((int)(v) >= (want) - 2 && (int)(v) <= (want) + 2)
  #define RGBA_NEAR(r, g, b, a) ASSERT_TRUE(NEAR2(out[0], r) && NEAR2(out[1], g) && \
                                            NEAR2(out[2], b) && NEAR2(out[3], a))

  /* The defaults: GL_MODULATE of the texture by the previous colour, for both. */
  glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &iv);    ASSERT_EQ(iv, (GLint)GL_MODULATE);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE1_RGB, &iv);    ASSERT_EQ(iv, (GLint)GL_PREVIOUS);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND2_RGB, &iv);   ASSERT_EQ(iv, (GLint)GL_SRC_ALPHA);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_RGB_SCALE, &iv);      ASSERT_EQ(iv, 1);
  TEXEL(128, 128, 128, 255);
  DRAW(0.5f, 1.0f, 0.0f, 1.0f);
  RGBA_NEAR(64, 128, 0, 255);
  /* **GL_RGB_SCALE** doubles it; the alpha has its own scale. */
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f);
  DRAW(0.5f, 1.0f, 0.0f, 1.0f);
  RGBA_NEAR(128, 255, 0, 255);
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);

  /* **GL_ADD_SIGNED**: 0.5 + 0.75 - 0.5. **GL_SUBTRACT**: 1 - 0.25. */
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD_SIGNED);
  DRAW(0.75f, 0.75f, 0.75f, 1.0f);
  RGBA_NEAR(191, 191, 191, 255);
  TEXEL(255, 255, 255, 255);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_SUBTRACT);
  DRAW(0.25f, 0.25f, 0.25f, 1.0f);
  RGBA_NEAR(191, 191, 191, 255);

  /* **GL_INTERPOLATE**: red texture towards blue fragment by the constant's alpha, 0.25. */
  TEXEL(255, 0, 0, 255);
  const GLfloat k[4] = {0.0f, 0.0f, 0.0f, 0.25f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, k);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
  DRAW(0.0f, 0.0f, 1.0f, 1.0f);
  RGBA_NEAR(64, 0, 191, 255);

  /* **Operands and sources**: GL_REPLACE of one minus the primary colour; of the constant's
   * alpha; the alpha function on its own - GL_REPLACE of the constant alpha. */
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
  DRAW(1.0f, 0.25f, 0.0f, 1.0f);
  RGBA_NEAR(0, 191, 255, 255);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_CONSTANT);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_ALPHA);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_CONSTANT);
  DRAW(1.0f, 1.0f, 1.0f, 1.0f);
  RGBA_NEAR(64, 64, 64, 64);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);

  /* **GL_DOT3_RGB**: a +x normal map texel (1, 0.5, 0.5) against a +x light vector in the
   * primary colour is 4 * 0.25 = 1, white; against +y, 0. GL_DOT3_RGBA puts it in alpha too. */
  TEXEL(255, 128, 128, 255);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_DOT3_RGB);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
  DRAW(1.0f, 0.5f, 0.5f, 0.5f);
  RGBA_NEAR(255, 255, 255, 128); /* alpha from its own function: 1 * 0.5 */
  DRAW(0.5f, 1.0f, 0.5f, 1.0f);
  RGBA_NEAR(0, 0, 0, 255);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_DOT3_RGBA);
  DRAW(0.5f, 1.0f, 0.5f, 1.0f);
  RGBA_NEAR(0, 0, 0, 0);

  /* **The checks**: DOT3 is a colour function only; an alpha operand takes no colour; a source
   * must be one GL names - a unit there is, GL_TEXTURE0 or GL_TEXTURE1 (GL_TEXTURE1 was refused
   * while there was one unit), not GL_TEXTURE2; a scale is 1, 2 or 4. */
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_DOT3_RGB);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_COLOR);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE1);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE2);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE0);
  glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.5f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 3.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_RGB_SCALE, &iv); ASSERT_EQ(iv, 1);

  /* **On the attribute stack** with the rest of the environment, and **in a list**. */
  glPushAttrib(GL_TEXTURE_BIT);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD);
  glPopAttrib();
  glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &iv); ASSERT_EQ(iv, (GLint)GL_DOT3_RGBA);
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 4.0f);
  glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_ONE_MINUS_SRC_ALPHA);
  glEndList();
  glCallList(list);
  GLfloat fv = 0.0f;
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, &fv);  ASSERT_TRUE(fv == 4.0f);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND1_RGB, &iv); ASSERT_EQ(iv, (GLint)GL_ONE_MINUS_SRC_ALPHA);
  glDeleteLists(list, 1);
  #undef RGBA_NEAR
  #undef NEAR2
  #undef DRAW
  #undef TEXEL

  glDisable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Cube maps (GL 1.3): six faces through their own targets, cube completeness, the lookup by
 * direction, the two generation modes that make directions, the proxy, and the targets each
 * call does and does not take. */
static void test_gl_cube_maps(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  GLint iv = 0;
  glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &iv);
  ASSERT_TRUE(iv >= 16);

  GLuint cube = 0;
  glGenTextures(1, &cube);
  glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
  glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &iv); ASSERT_EQ(iv, (GLint)cube);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* +X red, -X green, +Y blue, -Y yellow, +Z magenta, -Z cyan - 1x1 each. */
  static const GLubyte colours[6][4] = {
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
    {255, 255, 0, 255}, {255, 0, 255, 255}, {0, 255, 255, 255},
  };
  static const uint32_t argb[6] = {0xff0000u, 0x00ff00u, 0x0000ffu, 0xffff00u, 0xff00ffu, 0x00ffffu};
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int f = 0; f < 5; f++) {
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, colours[f]);
  }
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **Refusals**: the cube itself is not an image target, a face not an object target, and a
   * face must be square. */
  glTexImage2D(GL_TEXTURE_CUBE_MAP, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, colours[0]);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBindTexture(GL_TEXTURE_CUBE_MAP_POSITIVE_X, cube);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexParameteri(GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, colours[0]);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* **Cube complete only with all six**: five faces draw untextured, in the vertex colour. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glEnable(GL_TEXTURE_CUBE_MAP);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_CUBE_MAP), GL_TRUE);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glColor3f(1.0f, 1.0f, 1.0f);
  #define CENTRE (fb[(H / 2) * W + W / 2] & 0x00ffffffu)
  #define DRAW(s, t, r) do { glTexCoord3f(s, t, r); glRectf(-0.5f, -0.5f, 0.5f, 0.5f); } while (0)
  DRAW(1.0f, 0.0f, 0.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               colours[5]);

  /* **The lookup by direction**: the major axis names the face. */
  DRAW(1.0f, 0.1f, 0.2f);   ASSERT_EQ(CENTRE, argb[0]);
  DRAW(-1.0f, 0.2f, 0.1f);  ASSERT_EQ(CENTRE, argb[1]);
  DRAW(0.1f, 1.0f, -0.2f);  ASSERT_EQ(CENTRE, argb[2]);
  DRAW(0.2f, -1.0f, 0.1f);  ASSERT_EQ(CENTRE, argb[3]);
  DRAW(0.1f, 0.2f, 1.0f);   ASSERT_EQ(CENTRE, argb[4]);
  DRAW(-0.1f, 0.1f, -1.0f); ASSERT_EQ(CENTRE, argb[5]);

  /* **The place on a face**, GL 1.3's table 3.19: on +X, s runs with -z and t with -y. A 2x2 +X
   * face - red, green over blue, yellow in memory order - at (1, 0.5, -0.5) is s 0.75, t 0.25:
   * the second texel of the first row. The other faces go 2x2 too, to stay complete. */
  static const GLubyte quad4[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
  for (int f = 0; f < 6; f++) {
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, quad4);
  }
  DRAW(1.0f, 0.5f, -0.5f);  ASSERT_EQ(CENTRE, 0x00ff00u);
  DRAW(1.0f, -0.5f, 0.5f);  ASSERT_EQ(CENTRE, 0x0000ffu); /* s 0.25, t 0.75 */
  /* And on -Z, s runs with -x: (0.5, -0.5, -1) is s 0.25, t 0.75, blue again. */
  DRAW(0.5f, -0.5f, -1.0f); ASSERT_EQ(CENTRE, 0x0000ffu);
  for (int f = 0; f < 6; f++) {
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, colours[f]);
  }

  /* **The generation modes that make directions**: GL_NORMAL_MAP looks up by the eye-space
   * normal - (0, 0, -1) is -Z, cyan - and GL_REFLECTION_MAP by the eye vector reflected in it:
   * looking down -z at a face whose normal is +z reflects back up +z, magenta. */
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
  glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
  glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);
  glEnable(GL_TEXTURE_GEN_S); glEnable(GL_TEXTURE_GEN_T); glEnable(GL_TEXTURE_GEN_R);
  glNormal3f(0.0f, 0.0f, -1.0f);
  glRectf(-0.5f, -0.5f, 0.5f, 0.5f);
  ASSERT_EQ(CENTRE, argb[5]);
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
  glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
  glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);
  glMatrixMode(GL_PROJECTION); glOrtho(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
  glMatrixMode(GL_MODELVIEW); glTranslatef(0.0f, 0.0f, -5.0f);
  glNormal3f(0.0f, 0.0f, 1.0f);
  glRectf(-0.5f, -0.5f, 0.5f, 0.5f);
  ASSERT_EQ(CENTRE, argb[4]);
  glLoadIdentity();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T); glDisable(GL_TEXTURE_GEN_R);

  /* **A cube map outranks 2D**: both enabled, the cube samples. */
  GLuint flat = 0;
  const GLubyte white[4] = {255, 255, 255, 255};
  glGenTextures(1, &flat);
  glBindTexture(GL_TEXTURE_2D, flat);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glEnable(GL_TEXTURE_2D);
  DRAW(0.0f, 1.0f, 0.0f);
  ASSERT_EQ(CENTRE, argb[2]);
  glDisable(GL_TEXTURE_2D);

  /* **Each face read back and asked about through its own target**, and written by a
   * sub-image. */
  GLubyte back[4] = {0, 0, 0, 0};
  glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_TRUE(back[0] == 255 && back[1] == 0 && back[2] == 255);
  glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_TRUE(back[0] == 255 && back[1] == 255 && back[2] == 255);
  glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 0, GL_TEXTURE_WIDTH, &iv);
  ASSERT_EQ(iv, 1);
  glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP, 0, GL_TEXTURE_WIDTH, &iv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The proxy**: square and within the limit fits; otherwise zeros, no error. */
  glTexImage2D(GL_PROXY_TEXTURE_CUBE_MAP, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_CUBE_MAP, 0, GL_TEXTURE_WIDTH, &iv); ASSERT_EQ(iv, 64);
  glTexImage2D(GL_PROXY_TEXTURE_CUBE_MAP, 0, GL_RGBA, 64, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_CUBE_MAP, 0, GL_TEXTURE_WIDTH, &iv); ASSERT_EQ(iv, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **On the attribute stack**: the binding and the enable come back. */
  glPushAttrib(GL_TEXTURE_BIT);
  glDisable(GL_TEXTURE_CUBE_MAP);
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
  glPopAttrib();
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_CUBE_MAP), GL_TRUE);
  glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &iv); ASSERT_EQ(iv, (GLint)cube);
  #undef DRAW
  #undef CENTRE

  glDisable(GL_TEXTURE_CUBE_MAP);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glDeleteTextures(1, &cube);
  glDeleteTextures(1, &flat);
  glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &iv); ASSERT_EQ(iv, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Two-sided lighting: a polygon facing away lit with the back material and its normal reversed;
 * glColorMaterial's face; a polygon's outline lit from its own side; lines from the front. */
static void test_gl_two_sided_lighting(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  const GLfloat dir[4] = {0.0f, 0.0f, 1.0f, 0.0f}, none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const GLfloat white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const GLfloat green[4] = {0.0f, 1.0f, 0.0f, 1.0f}, red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  glLightfv(GL_LIGHT0, GL_POSITION, dir);
  glLightfv(GL_LIGHT0, GL_AMBIENT, none);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, none);
  glEnable(GL_LIGHT0);
  glEnable(GL_LIGHTING);
  #define CENTRE (fb[(H / 2) * W + W / 2] & 0x00ffffffu)
  /* A quad wound counter-clockwise (front) or clockwise (back), normal along z = `nz`. */
  #define QUAD(cw, nz) do { glNormal3f(0.0f, 0.0f, nz); glBegin(GL_QUADS); \
      if (cw) { glVertex2f(-0.5f, -0.5f); glVertex2f(-0.5f, 0.5f); \
                glVertex2f(0.5f, 0.5f); glVertex2f(0.5f, -0.5f); } \
      else    { glVertex2f(-0.5f, -0.5f); glVertex2f(0.5f, -0.5f); \
                glVertex2f(0.5f, 0.5f); glVertex2f(-0.5f, 0.5f); } \
      glEnd(); } while (0)

  /* **The back material**: green emission in front, red behind, no diffuse. One-sided lighting
   * lights a back face with the front material. */
  glMaterialfv(GL_FRONT, GL_EMISSION, green);
  glMaterialfv(GL_BACK, GL_EMISSION, red);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, none);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, none);
  QUAD(1, 1.0f);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
  QUAD(1, 1.0f);
  ASSERT_EQ(CENTRE, 0xff0000u);
  QUAD(0, 1.0f); /* a front face is still the front */
  ASSERT_EQ(CENTRE, 0x00ff00u);
  glFrontFace(GL_CW); /* and the facing follows glFrontFace */
  QUAD(1, 1.0f);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  glFrontFace(GL_CCW);

  /* **The normal reversed**: a back face whose normal points away from the light is lit on its
   * visible side - where one-sided lighting leaves it dark. */
  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, none);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, white);
  QUAD(1, -1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
  QUAD(1, -1.0f);
  ASSERT_EQ(CENTRE, 0x000000u);

  /* **glColorMaterial's face**: tracking the back only leaves one-sided lighting - which lights
   * with the front material - alone. It used to change the front material whatever it said. */
  glEnable(GL_COLOR_MATERIAL);
  glColorMaterial(GL_BACK, GL_DIFFUSE);
  glColor3f(1.0f, 0.0f, 0.0f);
  QUAD(0, 1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
  QUAD(0, 1.0f);
  ASSERT_EQ(CENTRE, 0xff0000u);
  glColorMaterial(GL_FRONT, (GLenum)0x1234u);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  GLint iv = 0;
  glGetIntegerv(GL_COLOR_MATERIAL_FACE, &iv);      ASSERT_EQ(iv, (GLint)GL_FRONT_AND_BACK);
  glGetIntegerv(GL_COLOR_MATERIAL_PARAMETER, &iv); ASSERT_EQ(iv, (GLint)GL_DIFFUSE);
  glDisable(GL_COLOR_MATERIAL);
  glColor3f(1.0f, 1.0f, 1.0f);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, white);

  /* **An outline takes its polygon's side**: the back face's outline under GL_LINE is red, the
   * back emission. A GL_LINES line across the middle is lit from the front whatever way it runs:
   * green. */
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, none);
  glMaterialfv(GL_FRONT, GL_EMISSION, green);
  glMaterialfv(GL_BACK, GL_EMISSION, red);
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  QUAD(1, 1.0f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  GLboolean saw_red = GL_FALSE, saw_green = GL_FALSE;
  for (int i = 0; i < W * H; i++) {
    if ((fb[i] & 0x00ffffffu) == 0xff0000u) saw_red = GL_TRUE;
    if ((fb[i] & 0x00ffffffu) == 0x00ff00u) saw_green = GL_TRUE;
  }
  ASSERT_TRUE(saw_red && !saw_green);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES);
  glVertex2f(0.9f, 0.0f); glVertex2f(-0.9f, 0.0f);
  glEnd();
  saw_red = GL_FALSE; saw_green = GL_FALSE;
  for (int i = 0; i < W * H; i++) {
    if ((fb[i] & 0x00ffffffu) == 0xff0000u) saw_red = GL_TRUE;
    if ((fb[i] & 0x00ffffffu) == 0x00ff00u) saw_green = GL_TRUE;
  }
  ASSERT_TRUE(saw_green && !saw_red);

  /* On the attribute stack. */
  glPushAttrib(GL_LIGHTING_BIT);
  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
  glColorMaterial(GL_BACK, GL_EMISSION);
  glPopAttrib();
  glGetIntegerv(GL_LIGHT_MODEL_TWO_SIDE, &iv);     ASSERT_EQ(iv, 1);
  glGetIntegerv(GL_COLOR_MATERIAL_FACE, &iv);      ASSERT_EQ(iv, (GLint)GL_FRONT_AND_BACK);
  #undef QUAD
  #undef CENTRE

  glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
  glDisable(GL_LIGHTING);
  glDisable(GL_LIGHT0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.2's separate specular colour and GL_RESCALE_NORMAL, and the lighting details under them:
 * a normal lights at the length it has unless the program asks otherwise, and a shininess of 0
 * is full specular. */
static void test_gl_separate_specular_and_rescale_normal(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  GLint iv = 0;

  /* State: the default, the two values, a refusal, the enable - and the light model's scalars
   * on the attribute stack, the local viewer included. */
  glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &iv); ASSERT_EQ(iv, (GLint)GL_SINGLE_COLOR);
  glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
  glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &iv); ASSERT_EQ(iv, (GLint)GL_SEPARATE_SPECULAR_COLOR);
  glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_MODULATE);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_EQ(glIsEnabled(GL_RESCALE_NORMAL), GL_FALSE);
  glEnable(GL_RESCALE_NORMAL);
  ASSERT_EQ(glIsEnabled(GL_RESCALE_NORMAL), GL_TRUE);
  glDisable(GL_RESCALE_NORMAL);
  glPushAttrib(GL_LIGHTING_BIT);
  glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
  glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
  glGetIntegerv(GL_LIGHT_MODEL_LOCAL_VIEWER, &iv); ASSERT_EQ(iv, 1);
  glPopAttrib();
  glGetIntegerv(GL_LIGHT_MODEL_COLOR_CONTROL, &iv); ASSERT_EQ(iv, (GLint)GL_SEPARATE_SPECULAR_COLOR);
  glGetIntegerv(GL_LIGHT_MODEL_LOCAL_VIEWER, &iv); ASSERT_EQ(iv, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* One directional light along +z, the viewer along +z: n.l = n.h = 1 for a +z normal. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  const GLfloat dir[4] = {0.0f, 0.0f, 1.0f, 0.0f}, zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const GLfloat white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  glLightfv(GL_LIGHT0, GL_POSITION, dir);
  glLightfv(GL_LIGHT0, GL_AMBIENT, zero);
  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
  glEnable(GL_LIGHT0);
  glEnable(GL_LIGHTING);
  #define CENTRE (fb[(H / 2) * W + W / 2] & 0x00ffffffu)
  #define RECT(n) do { glNormal3f(0.0f, 0.0f, n); \
      glBegin(GL_QUADS); glVertex2f(-0.5f, -0.5f); glVertex2f(0.5f, -0.5f); \
      glVertex2f(0.5f, 0.5f); glVertex2f(-0.5f, 0.5f); glEnd(); } while (0)

  /* **Specular only, shininess 0**: (n.h)^0 is 1, a full highlight - which was skipped. Under a
   * black GL_MODULATE texture it survives only when kept apart: GL_SEPARATE_SPECULAR_COLOR adds
   * it after texturing; GL_SINGLE_COLOR lets the texture black it out. */
  glLightfv(GL_LIGHT0, GL_DIFFUSE, zero);
  glLightfv(GL_LIGHT0, GL_SPECULAR, white);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, zero);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, white);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 0.0f);
  glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
  RECT(1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  GLuint tex = 0;
  const GLubyte black[4] = {0, 0, 0, 255};
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
  glEnable(GL_TEXTURE_2D);
  glTexCoord2f(0.5f, 0.5f);
  RECT(1.0f);
  ASSERT_EQ(CENTRE, 0x000000u);
  glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
  RECT(1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glDisable(GL_TEXTURE_2D);
  glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SINGLE_COLOR);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);

  /* **A normal lights at its own length** unless GL_NORMALIZE or GL_RESCALE_NORMAL says
   * otherwise: (0, 0, 2) under a light of diffuse 0.25 is 0.5 - it was normalised to 0.25. */
  const GLfloat quarter[4] = {0.25f, 0.25f, 0.25f, 1.0f};
  glLightfv(GL_LIGHT0, GL_DIFFUSE, quarter);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, white);
  RECT(2.0f);
  ASSERT_EQ(CENTRE, 0x808080u);
  glEnable(GL_NORMALIZE);
  RECT(2.0f);
  ASSERT_EQ(CENTRE, 0x404040u);
  glDisable(GL_NORMALIZE);

  /* **GL_RESCALE_NORMAL** undoes a uniform modelview scale: under glScalef(2) the unit normal
   * comes out half as long, a quarter-bright 0.5 * 0.25; rescaled, it is unit again. */
  glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
  glScalef(2.0f, 2.0f, 2.0f);
  RECT(1.0f);
  ASSERT_EQ(CENTRE, 0x808080u);
  glEnable(GL_RESCALE_NORMAL);
  RECT(1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glDisable(GL_RESCALE_NORMAL);
  glLoadIdentity();
  #undef RECT
  #undef CENTRE

  glDisable(GL_LIGHTING);
  glDisable(GL_LIGHT0);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL 1.2's level-of-detail parameters, sampled: the base level sampling starts from, the maximum
 * level that makes a short chain complete, and the clamp on the level of detail. */
static void test_gl_texture_lod_parameters(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  GLint iv = 0;
  GLfloat fv = 0.0f;

  /* Defaults, and the value rules. */
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &iv); ASSERT_EQ(iv, 0);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &iv);  ASSERT_EQ(iv, 1000);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, &iv);    ASSERT_EQ(iv, -1000);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, &fv);    ASSERT_TRUE(fv == 1000.0f);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, -1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, -2);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 0.625f);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, &fv);    ASSERT_TRUE(fv == 0.625f);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, &iv);    ASSERT_EQ(iv, 1); /* rounded */
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Level 0 red 4x4, level 1 green 2x2, level 2 blue 1x1 - but only level 0 for now. */
  uint32_t red[16], green[4], blue[1];
  for (int i = 0; i < 16; i++) red[i] = 0xff0000ffu; /* bytes R,G,B,A = ff,00,00,ff */
  for (int i = 0; i < 4; i++) green[i] = 0xff00ff00u;
  blue[0] = 0xffff0000u;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glColor3f(1.0f, 1.0f, 1.0f);
  /* A full-screen quad with s and t running 0..`reps`: at 16 pixels across, `reps` 16 puts four
   * texels of the 4x4 base under each pixel (level of detail 2), and `reps` 1 a quarter of one
   * (magnified). */
  #define QUAD(reps) do { \
      glBegin(GL_QUADS); \
      glTexCoord2f(0.0f, 0.0f);   glVertex2f(-1.0f, -1.0f); \
      glTexCoord2f(reps, 0.0f);   glVertex2f(1.0f, -1.0f); \
      glTexCoord2f(reps, reps);   glVertex2f(1.0f, 1.0f); \
      glTexCoord2f(0.0f, reps);   glVertex2f(-1.0f, 1.0f); \
      glEnd(); } while (0)
  #define CENTRE (fb[(H / 2) * W + W / 2] & 0x00ffffffu)

  /* **GL_TEXTURE_MAX_LEVEL makes a short chain complete**: one level and a mipmap filter is
   * incomplete - the quad in its own white - until the maximum level says one level is all. */
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0xff0000u);
  QUAD(16.0f); /* minified, but level 0 is the last */
  ASSERT_EQ(CENTRE, 0xff0000u);

  /* The full chain, and minified sampling reaches level 2. */
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
  glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
  QUAD(16.0f);
  ASSERT_EQ(CENTRE, 0x0000ffu);
  /* **GL_TEXTURE_MAX_LOD** holds it at level 1, then at level 0. */
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1.0f);
  QUAD(16.0f);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 0.25f);
  QUAD(16.0f);
  ASSERT_EQ(CENTRE, 0xff0000u);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1000.0f);
  /* **GL_TEXTURE_MIN_LOD** makes a magnified quad minify, from level 1. */
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0xff0000u);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 1.0f);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, -1000.0f);

  /* **GL_TEXTURE_BASE_LEVEL** is where sampling starts: a magnified quad is level 1's green,
   * level 2's blue - and a base level with no image is incomplete. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  QUAD(16.0f); /* minified from level 1: 2x2 under 1/16 of the quad a pixel is lod 1: level 2 */
  ASSERT_EQ(CENTRE, 0x0000ffu);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 2);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0x0000ffu);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 3);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  /* And a maximum level below the base leaves a mipmapped texture incomplete, a non-mipmapped
   * one not. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0xffffffu);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  QUAD(1.0f);
  ASSERT_EQ(CENTRE, 0x00ff00u);
  #undef CENTRE
  #undef QUAD

  /* **GL_TEXTURE_BIT carries the bound texture's parameters**, not only the binding. */
  glPushAttrib(GL_TEXTURE_BIT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 2);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 0.5f);
  glPopAttrib();
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &iv); ASSERT_EQ(iv, 1);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &iv);     ASSERT_EQ(iv, (GLint)GL_REPEAT);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, &fv);   ASSERT_TRUE(fv == 1.0f);

  glDisable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Internal formats: what each keeps, how glGetTexImage and the size queries report it, how the
 * texture environment combines it (Mesa's calculate_derived_texenv), that a mip chain of mixed
 * formats is incomplete - and the proxy targets. */
static void test_gl_internal_formats_and_proxies(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  GLubyte out[4];
  GLint iv = 0;
  #define NEAR2(v, want) ((int)(v) >= (want) - 2 && (int)(v) <= (want) + 2)
  #define RGBA_NEAR(p, r, g, b, a) ASSERT_TRUE(NEAR2((p)[0], r) && NEAR2((p)[1], g) && \
                                               NEAR2((p)[2], b) && NEAR2((p)[3], a))

  /* **What each keeps**, read back as GL_RGBA: luminance and intensity in red alone, alpha 1
   * but for GL_LUMINANCE_ALPHA (Mesa, main/texgetimage.c:289-301). */
  const GLubyte src[4] = {10, 20, 30, 40};
  struct { GLint ifmt; GLubyte r, g, b, a; } keep[] = {
    {GL_ALPHA8, 0, 0, 0, 40},           {GL_LUMINANCE, 10, 0, 0, 255},
    {GL_LUMINANCE_ALPHA, 10, 0, 0, 40}, {GL_INTENSITY, 10, 0, 0, 255},
    {GL_RGB5, 10, 20, 30, 255},         {GL_RGBA4, 10, 20, 30, 40},
    {1, 10, 0, 0, 255},                 {3, 10, 20, 30, 255},
  };
  for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
    glTexImage2D(GL_TEXTURE_2D, 0, keep[i].ifmt, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    ASSERT_TRUE(out[0] == keep[i].r && out[1] == keep[i].g && out[2] == keep[i].b &&
                out[3] == keep[i].a);
  }
  /* A luminance texture read back as luminance is its own luminance. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
  GLubyte lum = 0;
  glGetTexImage(GL_TEXTURE_2D, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, &lum);
  ASSERT_EQ(lum, 10);
  /* A sub-image into it is reduced the same way. */
  const GLubyte src2[4] = {99, 1, 2, 3};
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, src2);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(out[0] == 99 && out[1] == 0 && out[2] == 0 && out[3] == 255);

  /* **The queries**: the format as named, a generic compressed one as its base format, and eight
   * bits for each component the base format keeps. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8_ALPHA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &iv);
  ASSERT_EQ(iv, (GLint)GL_LUMINANCE8_ALPHA8);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_LUMINANCE_SIZE, &iv); ASSERT_EQ(iv, 8);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &iv);     ASSERT_EQ(iv, 8);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &iv);       ASSERT_EQ(iv, 0);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTENSITY_SIZE, &iv); ASSERT_EQ(iv, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &iv);
  ASSERT_EQ(iv, (GLint)GL_RGBA);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &iv);
  ASSERT_EQ(iv, GL_FALSE);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_INTERNAL_FORMAT, &iv); /* never specified */
  ASSERT_EQ(iv, (GLint)GL_RGBA);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The environment**, per base format: a full-screen quad, read at the centre. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glEnable(GL_TEXTURE_2D);
  glTexCoord2f(0.5f, 0.5f);
  const GLubyte half[4] = {128, 128, 128, 128};
  #define DRAW_READ(ifmt, mode, cr, cg, cb, ca) do { \
      glTexImage2D(GL_TEXTURE_2D, 0, ifmt, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, half); \
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode); \
      glColor4f(cr, cg, cb, ca); \
      glRectf(-1.0f, -1.0f, 1.0f, 1.0f); \
      glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out); } while (0)
  /* An alpha texture leaves the colour alone and modulates alpha. */
  DRAW_READ(GL_ALPHA, GL_MODULATE, 1.0f, 0.0f, 0.0f, 1.0f); RGBA_NEAR(out, 255, 0, 0, 128);
  /* ... and replaces alpha - colour still the fragment's. */
  DRAW_READ(GL_ALPHA, GL_REPLACE, 0.0f, 1.0f, 0.0f, 1.0f);  RGBA_NEAR(out, 0, 255, 0, 128);
  /* Luminance and RGB replace the colour and keep the fragment's alpha. */
  DRAW_READ(GL_LUMINANCE, GL_REPLACE, 1.0f, 0.0f, 0.0f, 0.25f); RGBA_NEAR(out, 128, 128, 128, 64);
  DRAW_READ(GL_RGB, GL_REPLACE, 1.0f, 0.0f, 0.0f, 0.25f);       RGBA_NEAR(out, 128, 128, 128, 64);
  /* RGBA replaces both. */
  DRAW_READ(GL_RGBA, GL_REPLACE, 1.0f, 0.0f, 0.0f, 0.25f);      RGBA_NEAR(out, 128, 128, 128, 128);
  /* Intensity is all four. */
  DRAW_READ(GL_INTENSITY, GL_MODULATE, 1.0f, 1.0f, 1.0f, 1.0f); RGBA_NEAR(out, 128, 128, 128, 128);
  /* GL_ADD: colour added and alpha multiplied - but for intensity, alpha added too. */
  DRAW_READ(GL_RGBA, GL_ADD, 0.25f, 0.0f, 1.0f, 1.0f);          RGBA_NEAR(out, 192, 128, 255, 128);
  DRAW_READ(GL_INTENSITY, GL_ADD, 0.0f, 0.0f, 0.0f, 0.25f);     RGBA_NEAR(out, 128, 128, 128, 192);
  /* GL_BLEND: the fragment towards the environment colour by the texel - which was drawn as
   * GL_MODULATE until 2026-09-19. Intensity blends alpha the same way. */
  const GLfloat env[4] = {0.0f, 0.0f, 1.0f, 0.0f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
  DRAW_READ(GL_RGBA, GL_BLEND, 1.0f, 0.0f, 0.0f, 1.0f);         RGBA_NEAR(out, 128, 0, 128, 128);
  DRAW_READ(GL_INTENSITY, GL_BLEND, 1.0f, 0.0f, 0.0f, 1.0f);    RGBA_NEAR(out, 128, 0, 128, 128);
  DRAW_READ(GL_LUMINANCE, GL_BLEND, 1.0f, 0.0f, 0.0f, 1.0f);    RGBA_NEAR(out, 128, 0, 128, 255);
  /* GL_DECAL: an RGB texture replaces the colour; a luminance one leaves it (Mesa's reading of an
   * undefined case); alpha is always the fragment's. */
  DRAW_READ(GL_RGB, GL_DECAL, 1.0f, 0.0f, 0.0f, 0.25f);         RGBA_NEAR(out, 128, 128, 128, 64);
  DRAW_READ(GL_LUMINANCE, GL_DECAL, 1.0f, 0.0f, 0.0f, 0.25f);   RGBA_NEAR(out, 255, 0, 0, 64);
  DRAW_READ(GL_RGBA, GL_DECAL, 1.0f, 0.0f, 0.0f, 0.25f);        RGBA_NEAR(out, 191, 64, 64, 64);
  #undef DRAW_READ

  /* A mode GL does not have is refused and changes nothing. (GL_COMBINE was the example until the
   * combiner landed - test_gl_texture_combine.) */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x1234);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &iv);
  ASSERT_EQ(iv, (GLint)GL_MODULATE);

  /* **A mipmapped texture is one format**: level 1 as RGB under an RGBA base is incomplete, and
   * the quad draws in its own colour. */
  const GLubyte red4[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, red4);
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGB, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red4);
  glColor4f(0.0f, 1.0f, 0.0f, 1.0f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  RGBA_NEAR(out, 0, 255, 0, 255);
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red4);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  RGBA_NEAR(out, 0, 0, 0, 255); /* complete now: red modulating green */
  glDisable(GL_TEXTURE_2D);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **Refusals**: an internal format GL 1.x does not have is a value error; a border too. A copy
   * refuses the legacy 1 to 4, as an enum error. */
  glTexImage2D(GL_TEXTURE_2D, 0, 0x1234, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexImage2D(GL_TEXTURE_2D, 0, 5, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, src);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, 3, 0, 0, 1, 1, 0);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 0, 0, 1, 1, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &iv);
  ASSERT_EQ(iv, (GLint)GL_LUMINANCE);

  /* **Proxies**: a size that fits is reported, one that does not is zeros and no error. Nothing is
   * allocated - the bound texture keeps its image. */
  glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, 64, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &iv);  ASSERT_EQ(iv, 64);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &iv); ASSERT_EQ(iv, 32);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &iv);
  ASSERT_EQ(iv, (GLint)GL_RGBA8);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &iv);        ASSERT_EQ(iv, 1);
  glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA8, 4096, 4096, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &iv);  ASSERT_EQ(iv, 0);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &iv); ASSERT_EQ(iv, 0);
  glTexImage3D(GL_PROXY_TEXTURE_3D, 0, GL_RGBA, 16, 16, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_3D, 0, GL_TEXTURE_DEPTH, &iv);  ASSERT_EQ(iv, 0);
  glTexImage1D(GL_PROXY_TEXTURE_1D, 2, GL_INTENSITY8, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_1D, 2, GL_TEXTURE_INTENSITY_SIZE, &iv);
  ASSERT_EQ(iv, 8);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  /* Enum and level errors are still errors. */
  glTexImage2D(GL_PROXY_TEXTURE_2D, 0, 0x1234, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glTexImage2D(GL_PROXY_TEXTURE_2D, -1, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  /* A proxy is not a binding and not a sub-image target. */
  glBindTexture(GL_PROXY_TEXTURE_2D, tex);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexSubImage2D(GL_PROXY_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, src);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* And never compiled: inside glNewList it runs at once. */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &iv);
  glEndList();
  ASSERT_EQ(iv, 8);
  glDeleteLists(list, 1);
  #undef RGBA_NEAR
  #undef NEAR2

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Pixel formats, types and every glPixelStorei parameter, through a 1x1 texture upload read
 * back as RGBA bytes - the unpack side - and glReadPixels - the pack side. Each packed layout's
 * expected value is its bit pattern worked by hand from GL 1.2's tables. */
static void test_gl_pixel_types_and_store(void) {
  const int W = 16, H = 16;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  GLubyte out[16];
  #define UP(fmt, typ, data) do { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, fmt, typ, data); \
      ASSERT_EQ(glGetError(), GL_NO_ERROR); \
      glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out); } while (0)
  #define RGBA_IS(r, g, b, a) ASSERT_TRUE(out[0] == (r) && out[1] == (g) && out[2] == (b) && out[3] == (a))

  GLint iv = 0;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &iv); ASSERT_EQ(iv, 1);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &iv); ASSERT_EQ(iv, 0);

  /* **Packed types**: the format's first component in the top bits, or the bottom for _REV. */
  const GLushort r565 = 0xF800u, r565rev = 0x001Fu, rgba4 = 0xF00Fu, bgra1555rev = 0x801Fu;
  UP(GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &r565);          RGBA_IS(255, 0, 0, 255);
  UP(GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, &r565rev);   RGBA_IS(255, 0, 0, 255);
  UP(GL_BGR, GL_UNSIGNED_SHORT_5_6_5, &r565);          RGBA_IS(0, 0, 255, 255); /* B first */
  UP(GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, &rgba4);      RGBA_IS(255, 0, 0, 255);
  UP(GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, &bgra1555rev); RGBA_IS(0, 0, 255, 255);
  const GLuint i8888 = 0x11223344u, i8888rev = 0x44332211u, i1010102rev = 0xC00003FFu;
  UP(GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, &i8888);        RGBA_IS(0x11, 0x22, 0x33, 0x44);
  UP(GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, &i8888rev); RGBA_IS(0x11, 0x22, 0x33, 0x44);
  UP(GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &i1010102rev); RGBA_IS(255, 0, 0, 255);
  const GLubyte b332 = 0xE0u;
  UP(GL_RGB, GL_UNSIGNED_BYTE_3_3_2, &b332);           RGBA_IS(255, 0, 0, 255);

  /* **Plain types**: unsigned over 2^b - 1, signed as (2c + 1) / (2^b - 1) and clamped. */
  const GLushort us[4] = {65535u, 32768u, 0u, 65535u};
  UP(GL_RGBA, GL_UNSIGNED_SHORT, us);                  RGBA_IS(255, 128, 0, 255);
  const GLshort ss[4] = {32767, -32768, 16383, 32767}; /* 32767 / 65535 */
  UP(GL_RGBA, GL_SHORT, ss);                           RGBA_IS(255, 0, 127, 255);
  const GLbyte sb[4] = {127, -128, 63, 127};           /* 127 / 255 */
  UP(GL_RGBA, GL_BYTE, sb);                            RGBA_IS(255, 0, 127, 255);
  const GLuint ui[4] = {0xFFFFFFFFu, 0u, 0u, 0xFFFFFFFFu};
  UP(GL_RGBA, GL_UNSIGNED_INT, ui);                    RGBA_IS(255, 0, 0, 255);
  const GLfloat fs[4] = {0.25f, 2.0f, -1.0f, 1.0f};
  UP(GL_RGBA, GL_FLOAT, fs);                           RGBA_IS(64, 255, 0, 255);

  /* **Single-channel formats**: the rest is 0, alpha 1. */
  const GLubyte one = 200u;
  UP(GL_RED, GL_UNSIGNED_BYTE, &one);                  RGBA_IS(200, 0, 0, 255);
  UP(GL_GREEN, GL_UNSIGNED_BYTE, &one);                RGBA_IS(0, 200, 0, 255);
  UP(GL_BLUE, GL_UNSIGNED_BYTE, &one);                 RGBA_IS(0, 0, 200, 255);

  /* **Swap bytes**: R stored as bytes 00 FF is 0xFF00 read natively and 0x00FF swapped. */
  const GLubyte swap_src[8] = {0x00, 0xFF, 0, 0, 0, 0, 0xFF, 0xFF};
  UP(GL_RGBA, GL_UNSIGNED_SHORT, swap_src);            ASSERT_EQ(out[0], 254);
  glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
  UP(GL_RGBA, GL_UNSIGNED_SHORT, swap_src);            ASSERT_EQ(out[0], 1);
  glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);

  /* **Skips**: the texel at (1, 1) of a 3x3 image, rows three pixels long. */
  GLubyte img[3 * 3 * 4];
  for (int i = 0; i < 9; i++) { img[i * 4] = (GLubyte)(i * 10); img[i * 4 + 1] = 0; img[i * 4 + 2] = 0; img[i * 4 + 3] = 255; }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 3);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
  UP(GL_RGBA, GL_UNSIGNED_BYTE, img);                  ASSERT_EQ(out[0], 40); /* pixel 4 */
  /* A list keeps what the skips selected; the replay does not skip again. */
  const GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
  glEndList();
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  UP(GL_RGBA, GL_UNSIGNED_BYTE, img);                  ASSERT_EQ(out[0], 0);
  glCallList(list);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_EQ(out[0], 40);
  glDeleteLists(list, 1);

  /* The client attribute stack carries every parameter. */
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 5);
  glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
  glPopClientAttrib();
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &iv); ASSERT_EQ(iv, 0);
  glGetIntegerv(GL_PACK_SWAP_BYTES, &iv);  ASSERT_EQ(iv, 0);

  /* **The pack side**: A=44 R=11 G=22 B=33 everywhere, read as packed types, floats, through a
   * skip and with bytes swapped. */
  for (int i = 0; i < W * H; i++) fb[i] = 0x44112233u;
  GLuint word = 0;
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, &word);
  ASSERT_EQ(word, 0x11223344u);
  GLubyte bytes[8] = {0};
  glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, bytes);
  glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
  ASSERT_TRUE(bytes[0] == 0x11 && bytes[1] == 0x22 && bytes[2] == 0x33 && bytes[3] == 0x44);
  for (int i = 0; i < W * H; i++) fb[i] = 0xffff0000u; /* opaque red */
  GLushort p565 = 0;
  glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &p565);
  ASSERT_EQ(p565, 0xF800u);
  /* Four bytes of slack past the pixel, left at 0xcd: a skip moves where the pixel lands, it
   * does not widen what is written. Sized to exactly the twelve bytes the skip and the pixel
   * need, this is correct but a pack that ran long would have nowhere to land and nothing to
   * fail on. */
  GLubyte skipped[16];
  memset(skipped, 0xcd, sizeof(skipped));
  glPixelStorei(GL_PACK_SKIP_PIXELS, 2);
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, skipped);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  ASSERT_EQ(skipped[0], 0xcd);  /* the two skipped pixels untouched */
  ASSERT_EQ(skipped[8], 255);   /* red, third pixel */
  ASSERT_EQ(skipped[12], 0xcd); /* and nothing past it */
  GLshort sred[3] = {0, 0, 0};
  glReadPixels(0, 0, 1, 1, GL_RGB, GL_SHORT, sred);
  ASSERT_EQ(sred[0], 32767);   /* ((2^16 - 1) * 1 - 1) / 2 */
  ASSERT_EQ(sred[1], 0);       /* 0 packs to -1/2: toward zero, as Mesa's FLOAT_TO_SHORT */

  /* **GL_UNPACK_LSB_FIRST** turns a bitmap byte round: 0x01 is the leftmost pixel. */
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1.0f, 1.0f, 1.0f);
  glRasterPos2f(-1.0f, -1.0f); /* window (0, 0): framebuffer row 15 */
  const GLubyte bit0 = 0x01u;
  glBitmap(8, 1, 0.0f, 0.0f, 0.0f, 0.0f, &bit0);
  ASSERT_EQ(fb[15 * W + 7] & 0x00ffffffu, 0x00ffffffu);
  ASSERT_EQ(fb[15 * W + 0] & 0x00ffffffu, 0u);
  glClear(GL_COLOR_BUFFER_BIT);
  glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE);
  glBitmap(8, 1, 0.0f, 0.0f, 0.0f, 0.0f, &bit0);
  glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
  ASSERT_EQ(fb[15 * W + 0] & 0x00ffffffu, 0x00ffffffu);
  ASSERT_EQ(fb[15 * W + 7] & 0x00ffffffu, 0u);

  /* Refusals: a packed type with the wrong format is an operation error; a negative skip a
   * value error. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4, &rgba4);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, -1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  #undef RGBA_IS
  #undef UP
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The texture matrix, and the raster position processed as the vertex it is. */
static void test_gl_texture_matrix_and_raster_vertex(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  #define PX(x, y) (fb[(y) * W + (x)] & 0x00ffffffu)

  /* Red | green, and a quad whose s runs 0..0.5 - all red, until the texture matrix moves s on
   * by a half. */
  const GLubyte texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
  glEnable(GL_TEXTURE_2D);
  glColor3f(1.0f, 1.0f, 1.0f);
  #define TQUAD() do { glBegin(GL_QUADS); \
      glTexCoord2f(0.0f, 0.0f); glVertex2f(-0.5f, -0.5f); glTexCoord2f(0.49f, 0.0f); glVertex2f(0.5f, -0.5f); \
      glTexCoord2f(0.49f, 1.0f); glVertex2f(0.5f, 0.5f); glTexCoord2f(0.0f, 1.0f); glVertex2f(-0.5f, 0.5f); \
      glEnd(); } while (0)
  glClear(GL_COLOR_BUFFER_BIT);
  TQUAD();
  ASSERT_EQ(PX(16, 16), 0x00ff0000u);
  glMatrixMode(GL_TEXTURE);
  glTranslatef(0.5f, 0.0f, 0.0f);
  glMatrixMode(GL_MODELVIEW);
  glClear(GL_COLOR_BUFFER_BIT);
  TQUAD();
  ASSERT_EQ(PX(16, 16), 0x0000ff00u);
  glDisable(GL_TEXTURE_2D);

  /* The raster position's coordinate goes through the same matrix. */
  glTexCoord2f(0.25f, 0.0f);
  glRasterPos2f(0.0f, 0.0f);
  GLfloat v[4];
  glGetFloatv(GL_CURRENT_RASTER_TEXTURE_COORDS, v);
  ASSERT_FLOAT_NEAR(v[0], 0.75f, 1e-6f);
  glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);

  /* Its colour is lit: the default light and material facing the viewer give 0.2*0.2 + 0.8. */
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glNormal3f(0.0f, 0.0f, 1.0f);
  glRasterPos2f(0.0f, 0.0f);
  glGetFloatv(GL_CURRENT_RASTER_COLOR, v);
  ASSERT_FLOAT_NEAR(v[0], 0.84f, 1e-4f);
  glDisable(GL_LIGHTING);
  glDisable(GL_LIGHT0);

  /* And its w is the clip w, not its reciprocal. */
  glRasterPos4f(0.0f, 0.0f, 0.0f, 2.0f);
  glGetFloatv(GL_CURRENT_RASTER_POSITION, v);
  ASSERT_FLOAT_NEAR(v[3], 2.0f, 1e-6f);

  #undef TQUAD
  #undef PX
  glDeleteTextures(1, &tex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Points and lines, drawn as triangles.
 *
 * The geometry engine will not take a one- or two-vertex primitive - measured, five sweeps, one of
 * them on this stage. So a line is a screen-width quad and a point is a square. What has to be
 * checked is that the *width is in screen space*: an expansion done in object or eye space gives a
 * line that thins with distance, which passes any test drawn at a single depth.
 */
static void test_gl_points_and_lines_expand_to_triangles(void) {
  const int W = 64, H = 64;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  /* A horizontal line across the middle, one pixel wide. */
  glClear(GL_COLOR_BUFFER_BIT);
  glLineWidth(1.0f);
  glColor3f(1.0f, 1.0f, 1.0f);
  glBegin(GL_LINES);
  glVertex3f(-0.8f, 0.0f, 0.0f);
  glVertex3f( 0.8f, 0.0f, 0.0f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  int thin = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) thin++;
  ASSERT_TRUE(thin > 0);

  /* **Wider means more pixels, and roughly proportionally.** This is the assertion that fails
   * for an expansion done in the wrong space or with the perpendicular in NDC rather than
   * pixels - a 64x64 viewport hides the aspect error, so the ratio is what is checked. */
  glClear(GL_COLOR_BUFFER_BIT);
  glLineWidth(6.0f);
  glBegin(GL_LINES);
  glVertex3f(-0.8f, 0.0f, 0.0f);
  glVertex3f( 0.8f, 0.0f, 0.0f);
  glEnd();
  int thick = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) thick++;
  /* Substantially more, not exactly six times: a one-pixel line whose centre falls on a row
   * boundary lights two rows, so the ratio here is 6 rows against 2 rather than 6 against 1.
   * Asserting the exact factor would be asserting a fill rule. */
  ASSERT_TRUE(thick > thin * 2);
  ASSERT_TRUE(thick < thin * 8);

  /* A vertical line of the same width covers a similar area - if the perpendicular were taken
   * in NDC without the viewport scale, a non-square viewport would make these differ. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES);
  glVertex3f(0.0f, -0.8f, 0.0f);
  glVertex3f(0.0f,  0.8f, 0.0f);
  glEnd();
  int vertical = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) vertical++;
  ASSERT_TRUE(vertical > thick / 2 && vertical < thick * 2);

  /* A point is a square of glPointSize pixels: four times the size is about sixteen times the
   * area, so the count must grow with the square rather than linearly. */
  glClear(GL_COLOR_BUFFER_BIT);
  glPointSize(2.0f);
  glBegin(GL_POINTS);
  glVertex3f(0.0f, 0.0f, 0.0f);
  glEnd();
  int small = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) small++;
  ASSERT_TRUE(small > 0);

  glClear(GL_COLOR_BUFFER_BIT);
  glPointSize(8.0f);
  glBegin(GL_POINTS);
  glVertex3f(0.0f, 0.0f, 0.0f);
  glEnd();
  int big = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) big++;
  ASSERT_TRUE(big > small * 4);

  /* **A loop closes and a strip does not**, which is the whole difference between them: three
   * vertices give two segments as a strip and three as a loop. */
  glLineWidth(1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINE_STRIP);
  glVertex3f(-0.8f, -0.8f, 0.0f);
  glVertex3f( 0.8f, -0.8f, 0.0f);
  glVertex3f( 0.0f,  0.8f, 0.0f);
  glEnd();
  int strip = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) strip++;

  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINE_LOOP);
  glVertex3f(-0.8f, -0.8f, 0.0f);
  glVertex3f( 0.8f, -0.8f, 0.0f);
  glVertex3f( 0.0f,  0.8f, 0.0f);
  glEnd();
  int loop = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) loop++;
  ASSERT_TRUE(loop > strip);

  /* Zero-width and zero-size are errors, not thin lines. */
  glLineWidth(0.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glPointSize(-1.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* A degenerate segment has no direction to be perpendicular to, and must not divide by zero. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_LINES);
  glVertex3f(0.25f, 0.25f, 0.0f);
  glVertex3f(0.25f, 0.25f, 0.0f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* GL_POLYGON and GL_QUAD_STRIP triangulate. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_POLYGON);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f( 0.5f, -0.5f, 0.0f);
  glVertex3f( 0.5f,  0.5f, 0.0f);
  glVertex3f(-0.5f,  0.5f, 0.0f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  int poly = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) poly++;
  ASSERT_TRUE(poly > 500);

  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUAD_STRIP);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(-0.5f,  0.5f, 0.0f);
  glVertex3f( 0.5f, -0.5f, 0.0f);
  glVertex3f( 0.5f,  0.5f, 0.0f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  int qstrip = 0;
  for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) qstrip++;
  ASSERT_TRUE(qstrip > 500);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Compressed textures: none supported, and the whole point is that this is said out loud.
 *
 * The specification allows an empty set of compressed formats. A program using them is supposed
 * to query the count first, so answering 0 is what sends it down its uncompressed path; a program
 * that does not query gets an error it can read. What it must never get is a call to address
 * zero, which is what an absent entry point gives under this link.
 */
static void test_gl_compressed_textures_are_refused_not_absent(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 32, 32);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  /* The count is zero, and the list query writes nothing rather than scribbling. */
  GLint n = 99;
  glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
  ASSERT_EQ(n, 0);
  GLint formats[4] = {11, 22, 33, 44};
  glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats);
  ASSERT_EQ(formats[0], 11); /* untouched: there are no formats to write */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Every upload is refused, and refused the same way whichever arity is used. */
  static const GLubyte junk[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA, 4, 4, 0, 8, junk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCompressedTexImage1D(GL_TEXTURE_1D, 0, GL_COMPRESSED_RGB, 4, 0, 8, junk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCompressedTexImage3D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA, 4, 4, 1, 0, 8, junk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA, 8, junk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCompressedTexSubImage1D(GL_TEXTURE_1D, 0, 0, 4, GL_COMPRESSED_RGB, 8, junk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCompressedTexSubImage3D(GL_TEXTURE_2D, 0, 0, 0, 0, 4, 4, 1, GL_COMPRESSED_RGBA, 8, junk);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* Reading back a compressed image is GL_INVALID_OPERATION, not GL_INVALID_ENUM: the target is
   * fine, the texture simply is not compressed. A bad target still gets the enum error. */
  GLubyte back[64];
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  static const GLubyte one[4] = {255, 255, 255, 255};
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetCompressedTexImage(GL_TEXTURE_2D, 0, back);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  /* A target that names no image - GL_TEXTURE_CUBE_MAP, whose images are its faces. (GL_TEXTURE_3D
   * was the example until 3D textures landed.) */
  glGetCompressedTexImage(GL_TEXTURE_CUBE_MAP, 0, back);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* And the per-level queries answer rather than erroring: no texture here is compressed, which
   * is a fact about all of them. */
  GLint compressed = 99, size = 99;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
  ASSERT_EQ(compressed, (GLint)GL_FALSE);
  ASSERT_EQ(size, 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL_TEXTURE_1D is a target, not a shape.
 *
 * Both bindings are live at once, each has its own enable, and 2D wins when both are on. An
 * implementation that treated 1D as "a 2D texture of height 1" would pass a test that only ever
 * uses one of them - so this keeps both bound with different contents and checks which one a
 * draw actually samples.
 */
static void test_gl_texture_1d_is_its_own_binding_point(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  GLuint t1 = 0, t2 = 0;
  glGenTextures(1, &t1);
  glGenTextures(1, &t2);
  ASSERT_TRUE(t1 != 0 && t2 != 0 && t1 != t2);

  /* A red 1D texture and a blue 2D one, bound at the same time. */
  static const GLubyte red[4]  = {255, 0, 0, 255};
  static const GLubyte blue[4] = {0, 0, 255, 255};
  glBindTexture(GL_TEXTURE_1D, t1);
  glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glBindTexture(GL_TEXTURE_2D, t2);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **Both bindings are live**, and each reports its own. */
  GLint b1 = 0, b2 = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_1D, &b1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &b2);
  ASSERT_EQ(b1, (GLint)t1);
  ASSERT_EQ(b2, (GLint)t2);

  /* 1D alone: the draw samples red. */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glColor3f(1.0f, 1.0f, 1.0f);
  glEnable(GL_TEXTURE_1D);
  glDisable(GL_TEXTURE_2D);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.8f, -0.8f, 0.8f, 0.8f);
  ASSERT_EQ(fb[(H / 2) * W + (W / 2)] & 0x00ffffffu, 0x00ff0000u);

  /* 2D alone: blue. */
  glDisable(GL_TEXTURE_1D);
  glEnable(GL_TEXTURE_2D);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.8f, -0.8f, 0.8f, 0.8f);
  ASSERT_EQ(fb[(H / 2) * W + (W / 2)] & 0x00ffffffu, 0x000000ffu);

  /* **Both enabled: the higher dimensionality wins**, so blue. This is the assertion an
   * implementation that collapsed the two bindings could not pass. */
  glEnable(GL_TEXTURE_1D);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.8f, -0.8f, 0.8f, 0.8f);
  ASSERT_EQ(fb[(H / 2) * W + (W / 2)] & 0x00ffffffu, 0x000000ffu);

  /* Turning 2D off falls back to the 1D texture, which was never disturbed. */
  glDisable(GL_TEXTURE_2D);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.8f, -0.8f, 0.8f, 0.8f);
  ASSERT_EQ(fb[(H / 2) * W + (W / 2)] & 0x00ffffffu, 0x00ff0000u);
  glDisable(GL_TEXTURE_1D);

  /* **An object belongs to the target it was first bound to.** */
  glBindTexture(GL_TEXTURE_2D, t1);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glBindTexture(GL_TEXTURE_1D, t2);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* The 2D entry points refuse a 1D target and vice versa - a target is not a shorthand. */
  glTexImage2D(GL_TEXTURE_1D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexImage1D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* A border is refused rather than ignored: a program that asks for one and is given a texture
   * without it samples the wrong texels at the edges. */
  glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* glTexSubImage1D updates in place. */
  glBindTexture(GL_TEXTURE_1D, t1);
  glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  static const GLubyte green[4] = {0, 255, 0, 255};
  glTexSubImage1D(GL_TEXTURE_1D, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Raster position, glDrawPixels and glBitmap.
 *
 * Three behaviours here are invisible in a "did it draw something" check and each breaks a real
 * program: an invalid raster position must draw **nothing** rather than clamping to the edge,
 * glBitmap must **move** the position or a string of glyphs piles up on the first one, and the
 * bitmap rows are most-significant-bit first.
 */
static void test_gl_raster_position_and_pixel_ops(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  /* NDC (0,0) is the middle of the viewport. */
  glRasterPos2f(0.0f, 0.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  GLfloat rp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  glGetFloatv(GL_CURRENT_RASTER_POSITION, rp);
  ASSERT_TRUE(rp[0] > 15.0f && rp[0] < 17.0f);
  ASSERT_TRUE(rp[1] > 15.0f && rp[1] < 17.0f);
  GLint valid = 0;
  glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
  ASSERT_EQ(valid, 1);

  /* A 2x2 RGBA block lands at the position, bottom-up: GL row 0 is the lowest. */
  static const GLubyte px2[16] = {
    255, 0, 0, 255,   0, 255, 0, 255,
    0, 0, 255, 255,   255, 255, 0, 255,
  };
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glDrawPixels(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px2);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  {
    const int rowbot = H - 1 - 16;          /* GL y 16 */
    ASSERT_EQ(fb[rowbot * W + 16] & 0x00ffffffu, 0x00ff0000u); /* red at (16,16) */
    ASSERT_EQ(fb[(rowbot - 1) * W + 16] & 0x00ffffffu, 0x000000ffu); /* blue one row up */
  }

  /* **An invalid position draws nothing.** Put the raster position off the far side of the
   * clip volume and draw: the framebuffer must not change anywhere. */
  glClear(GL_COLOR_BUFFER_BIT);
  glRasterPos2f(5.0f, 5.0f); /* outside the clip volume */
  glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
  ASSERT_EQ(valid, 0);
  glDrawPixels(2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px2);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  {
    int lit = 0;
    for (int i = 0; i < W * H; i++) if ((fb[i] & 0x00ffffffu) != 0u) lit++;
    ASSERT_EQ(lit, 0);
  }

  /* glBitmap draws in the raster colour, most significant bit first, and then moves the
   * position. A row of 8 bits 0xA0 is pixels 0 and 2. */
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(0.0f, 1.0f, 0.0f);
  glRasterPos2f(-0.5f, 0.0f);
  GLfloat before[4];
  glGetFloatv(GL_CURRENT_RASTER_POSITION, before);
  static const GLubyte bits[1] = {0xA0u};
  glBitmap(8, 1, 0.0f, 0.0f, 10.0f, 0.0f, bits);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  {
    const int bx = (int)before[0];
    const int by = (int)before[1];
    const int row = H - 1 - by;
    ASSERT_EQ(fb[row * W + bx + 0] & 0x00ffffffu, 0x0000ff00u); /* bit 7 set */
    ASSERT_EQ(fb[row * W + bx + 1] & 0x00ffffffu, 0u);          /* bit 6 clear */
    ASSERT_EQ(fb[row * W + bx + 2] & 0x00ffffffu, 0x0000ff00u); /* bit 5 set */
  }

  /* **The move happened**, which is what lays out a string. */
  GLfloat after[4];
  glGetFloatv(GL_CURRENT_RASTER_POSITION, after);
  ASSERT_TRUE(after[0] > before[0] + 9.0f && after[0] < before[0] + 11.0f);

  /* A null bitmap still moves - that is how a space is drawn. */
  glBitmap(0, 0, 0.0f, 0.0f, 5.0f, 0.0f, NULL);
  GLfloat after2[4];
  glGetFloatv(GL_CURRENT_RASTER_POSITION, after2);
  ASSERT_TRUE(after2[0] > after[0] + 4.0f && after2[0] < after[0] + 6.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The raster colour is latched at glRasterPos, not read at draw time: changing glColor
   * afterwards must not change what a later glBitmap draws. */
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1.0f, 0.0f, 0.0f);
  glRasterPos2f(-0.5f, -0.5f);
  glColor3f(0.0f, 0.0f, 1.0f); /* after the latch */
  glBitmap(8, 1, 0.0f, 0.0f, 0.0f, 0.0f, bits);
  {
    GLfloat p[4];
    glGetFloatv(GL_CURRENT_RASTER_POSITION, p);
    const int row = H - 1 - (int)p[1];
    ASSERT_EQ(fb[row * W + (int)p[0]] & 0x00ffffffu, 0x00ff0000u); /* red, not blue */
  }

  /* Refusals: a format GL 1.x does not have - GL 3.0's GL_DEPTH_STENCIL; GL_FLOAT and then
   * GL_COLOR_INDEX were the example until each converted - and a negative extent. */
  glDrawPixels(2, 2, (GLenum)0x84F9u /* GL_DEPTH_STENCIL */, GL_UNSIGNED_BYTE, px2);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glDrawPixels(-1, 2, GL_RGBA, GL_UNSIGNED_BYTE, px2);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glCopyPixels(0, 0, 4, 4, GL_DEPTH_COMPONENT); /* a format; the buffer is GL_DEPTH */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Stencil: the test, the three operations, and the two orderings that go wrong silently.
 *
 * The buffer is written **even when the stencil test fails** - that is how a mask gets built in
 * the first place - and a fragment the **alpha test** discards must not touch stencil at all.
 * Both are invisible in a simple pass/fail check and both break every technique stencil exists
 * for, so each gets its own assertion.
 */
static void test_gl_stencil_test_and_operations(void) {
  const int W = 32, H = 32;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  GLint bits = 0;
  glGetIntegerv(GL_STENCIL_BITS, &bits);
  ASSERT_EQ(bits, 8);

  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_ALPHA_TEST);

  /* Clear to 0, then draw with GL_ALWAYS / GL_REPLACE to stamp 1 into the left half. */
  glClearStencil(0);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  ASSERT_EQ(ctx->stencil_buffer[0], 0u);

  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 1, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glColor3f(1.0f, 1.0f, 1.0f);
  glRectf(-1.0f, -1.0f, 0.0f, 1.0f); /* left half */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 1u);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W * 3 / 4)], 0u);

  /* Now draw everywhere, keeping only where stencil == 1. The right half must stay black. */
  glClear(GL_COLOR_BUFFER_BIT);
  glStencilFunc(GL_EQUAL, 1, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  glColor3f(0.0f, 1.0f, 0.0f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  int lit_left = 0, lit_right = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if ((fb[y * W + x] & 0x00ffffffu) == 0u) continue;
      if (x < W / 2) lit_left++; else lit_right++;
    }
  }
  ASSERT_TRUE(lit_left > 50);
  ASSERT_EQ(lit_right, 0);

  /* **The fail operation writes.** Stencil is 0 on the right, the test demands 1, so every
   * fragment there fails - and GL_INCR on the fail path must still raise it to 1. */
  glStencilFunc(GL_EQUAL, 1, 0xff);
  glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W * 3 / 4)], 1u);

  /* GL_INCR saturates at 255 rather than wrapping to 0. */
  glStencilFunc(GL_ALWAYS, 0, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glStencilFunc(GL_ALWAYS, 255, 0xff);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 255u);
  glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 255u);

  /* The write mask gates the write, and it masks the **result**: GL_ZERO under mask 0x0f
   * clears only the low four bits. */
  glStencilFunc(GL_ALWAYS, 0xff, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glStencilMask(0xff);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 255u);
  glStencilMask(0x0f);
  glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0xf0u);
  glStencilMask(0xff);

  /* **A mask of zero makes every stencil write a no-op**, which is the state a program leaves
   * behind when it masks stencil off and forgets. */
  glStencilMask(0x00);
  glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0xf0u);

  /* **And it gates a clear too.** This asserted the opposite until 2026-09-19 - that glClear
   * ignores the stencil write mask - and GL applies every buffer's write mask to a clear; Mesa
   * clears a masked stencil buffer through a quad whose stencil writemask is the program's
   * (state_tracker/st_cb_clear.c, clear_with_quad). Masked off, the clear changes nothing;
   * masked to the low half, it changes the low half. */
  glClearStencil(0x11);
  glClear(GL_STENCIL_BUFFER_BIT);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0xf0u);
  glStencilMask(0x0f);
  glClear(GL_STENCIL_BUFFER_BIT);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0xf1u);
  glStencilMask(0xff);
  glClear(GL_STENCIL_BUFFER_BIT);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0x11u);

  /* **Alpha test comes first.** A fragment alpha discards must not reach stencil at all - not
   * even the fail operation. With alpha rejecting everything, the buffer must not move. */
  glClearStencil(0x05);
  glClear(GL_STENCIL_BUFFER_BIT);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  glColor4f(0.0f, 1.0f, 0.0f, 0.25f); /* below the reference: discarded */
  glStencilFunc(GL_NEVER, 0, 0xff);   /* would fail, and GL_INCR would bump it */
  glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0x05u);

  /* With alpha passing, the same draw does bump it - so the check above was the ordering and
   * not simply a draw that never happened. */
  glColor4f(0.0f, 1.0f, 0.0f, 0.75f);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0x06u);
  glDisable(GL_ALPHA_TEST);

  /* **GL 1.4's wrapping operations** (refused until 2026-09-19): from 255 GL_INCR_WRAP goes round
   * to 0 and GL_DECR_WRAP back to 255, where GL_INCR holds at the end. The function is still
   * GL_NEVER, so each is the fail operation. */
  glClearStencil(0xff);
  glClear(GL_STENCIL_BUFFER_BIT);
  glStencilOp(GL_INCR_WRAP, GL_KEEP, GL_KEEP);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0x00u);
  glStencilOp(GL_DECR_WRAP, GL_KEEP, GL_KEEP);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0xffu);
  glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);
  glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
  ASSERT_EQ(ctx->stencil_buffer[(H / 2) * W + (W / 4)], 0xffu);

  /* Refusals. */
  glStencilOp(GL_KEEP, GL_KEEP, (GLenum)0x8509u); /* past GL_DECR_WRAP */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glStencilFunc((GLenum)0x1E00u, 0, 0xff);        /* GL_KEEP is not a function */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glDisable(GL_STENCIL_TEST);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* User clip planes cut geometry, and the plane is fixed in eye space when it is specified.
 *
 * The fixture draws a full-viewport rectangle and cuts it with the plane x >= 0, then counts how
 * much of each half survives. Counting rather than sampling one pixel, because a clip plane that
 * does nothing and a clip plane that discards everything both pass a single-point check depending
 * where the point is.
 */
static void test_gl_clip_planes_cut_geometry(void) {
  const int W = 64, H = 64;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  GLint planes = 0;
  glGetIntegerv(GL_MAX_CLIP_PLANES, &planes);
  ASSERT_EQ(planes, 6);

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  /* Unclipped: the whole rectangle is drawn. */
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1.0f, 1.0f, 1.0f);
  glRectf(-0.9f, -0.9f, 0.9f, 0.9f);
  int left_before = 0, right_before = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if ((fb[y * W + x] & 0x00ffffffu) == 0u) continue;
      if (x < W / 2) left_before++; else right_before++;
    }
  }
  ASSERT_TRUE(left_before > 100 && right_before > 100);

  /* Keep x >= 0: the plane (1,0,0,0) against eye coordinates. The left half goes. */
  const GLdouble keep_positive_x[4] = {1.0, 0.0, 0.0, 0.0};
  glClipPlane(GL_CLIP_PLANE0, keep_positive_x);
  glEnable(GL_CLIP_PLANE0);
  ASSERT_TRUE(glIsEnabled(GL_CLIP_PLANE0));
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.9f, -0.9f, 0.9f, 0.9f);
  int left_after = 0, right_after = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if ((fb[y * W + x] & 0x00ffffffu) == 0u) continue;
      if (x < W / 2) left_after++; else right_after++;
    }
  }
  ASSERT_EQ(left_after, 0);
  ASSERT_TRUE(right_after > 100);

  /* Disabling brings it back - the plane is still stored, the enable is what decides. */
  glDisable(GL_CLIP_PLANE0);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(-0.9f, -0.9f, 0.9f, 0.9f);
  int left_again = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W / 2; x++) {
      if ((fb[y * W + x] & 0x00ffffffu) != 0u) left_again++;
    }
  }
  ASSERT_TRUE(left_again > 100);

  /* glGetClipPlane returns the eye-space plane, which for an identity modelview is what went
   * in. Specified under a translation it is not, which is the point of the next block. */
  GLdouble got[4] = {9.0, 9.0, 9.0, 9.0};
  glGetClipPlane(GL_CLIP_PLANE0, got);
  ASSERT_TRUE(got[0] == 1.0 && got[1] == 0.0 && got[2] == 0.0 && got[3] == 0.0);

  /* **Fixed at specification.**
   *
   * Specify "keep x >= 0" while the modelview is translated +0.5 in x. An object point at
   * x_o = 0 sits at x_e = 0.5, so the plane in eye space is x_e >= 0.5 - stored as
   * (1, 0, 0, -0.5), because the equation is p.xyz . v + p.w >= 0.
   *
   * Then put the modelview back to the identity and draw across the boundary. The half below
   * eye x = 0.5 is cut. An implementation that stored the caller's numbers unchanged would keep
   * the whole rectangle, because (1,0,0,0) passes everything with x >= 0. */
  glLoadIdentity();
  glTranslatef(0.5f, 0.0f, 0.0f);
  glClipPlane(GL_CLIP_PLANE1, keep_positive_x);
  glLoadIdentity();
  glGetClipPlane(GL_CLIP_PLANE1, got);
  ASSERT_TRUE(got[0] == 1.0);
  ASSERT_TRUE(got[3] > -0.51 && got[3] < -0.49);

  glEnable(GL_CLIP_PLANE1);
  glClear(GL_COLOR_BUFFER_BIT);
  glRectf(0.1f, -0.9f, 0.9f, 0.9f); /* eye x from 0.1 to 0.9, straddling the plane at 0.5 */
  int below = 0, above = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if ((fb[y * W + x] & 0x00ffffffu) == 0u) continue;
      /* Eye x = 0.5 lands at screen x = (0.5 + 1) / 2 * 64 = 48. */
      if (x < 46) below++; else if (x > 50) above++;
    }
  }
  ASSERT_EQ(below, 0);
  ASSERT_TRUE(above > 100);
  glDisable(GL_CLIP_PLANE1);

  /* An out-of-range plane is refused. */
  glClipPlane(GL_CLIP_PLANE0 + 6, keep_positive_x);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetClipPlane(GL_CLIP_PLANE0 + 9, got);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glClipPlane(GL_CLIP_PLANE0, NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The 4x4 inverse, checked against the one property that matters: M * M^-1 is the identity. */
static void test_gl_mat4_invert_round_trips(void) {
  gl_mat4_t m, inv, prod;
  mat4_identity(&m);
  ASSERT_TRUE(mat4_invert(&inv, &m));
  for (int i = 0; i < 16; i++) ASSERT_TRUE(inv.m[i] == m.m[i]);

  /* A translate-rotate-scale composition, which is what a modelview normally is.
   *
   * **mat4_translate and its siblings post-multiply onto `out`**, the way glTranslatef composes
   * onto the current matrix - they do not build a fresh matrix. Starting from anything other
   * than the identity here silently tests a different matrix than the one written down. */
  mat4_identity(&m);
  mat4_translate(&m, 3.0f, -4.0f, 5.0f);
  mat4_rotate(&m, 37.0f, 0.3f, 0.5f, 0.8f);
  mat4_scale(&m, 2.0f, 0.5f, 1.5f);

  ASSERT_TRUE(mat4_invert(&inv, &m));
  mat4_mult(&prod, &m, &inv);
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      const float want = (col == row) ? 1.0f : 0.0f;
      const float got = prod.m[col * 4 + row];
      ASSERT_TRUE(got > want - 1e-4f && got < want + 1e-4f);
    }
  }

  /* A singular matrix is refused rather than filled with infinities, and the caller's buffer is
   * left as it was - a stale answer beats a poisoned one. */
  mat4_identity(&m);
  mat4_scale(&m, 1.0f, 0.0f, 1.0f); /* a flattened axis: determinant zero */
  gl_mat4_t before;
  mat4_identity(&before);
  gl_mat4_t out = before;
  ASSERT_TRUE(!mat4_invert(&out, &m));
  for (int i = 0; i < 16; i++) ASSERT_TRUE(out.m[i] == before.m[i]);
}

/* Two texture units' state (GL 1.3; one unit until 2026-09-19, which GL 1.3 does not allow -
 * section 2.6). Each server call acts on the active unit, each client-array call on the client
 * active unit, glTexCoord on unit 0 always, and the attribute groups and lists carry all of it. */
static void test_gl_multitexture_state_is_per_unit(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();
  GLint iv[4] = {0, 0, 0, 0};
  GLfloat fv[16];

  glGetIntegerv(GL_MAX_TEXTURE_UNITS, iv);
  ASSERT_EQ(iv[0], 2);
  glGetIntegerv(GL_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE0);
  glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE0);

  /* Current coordinates: each unit its own, defaults filled; glTexCoord is unit 0's whichever
   * unit is active; GL_CURRENT_TEXTURE_COORDS answers for the active unit. */
  glMultiTexCoord2f(GL_TEXTURE0, 0.25f, 0.75f);
  glMultiTexCoord2f(GL_TEXTURE1, 3.0f, 4.0f);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.25f && ctx->cur_texcoord[0][1] == 0.75f);
  ASSERT_TRUE(ctx->cur_texcoord[1][0] == 3.0f && ctx->cur_texcoord[1][1] == 4.0f);
  ASSERT_TRUE(ctx->cur_texcoord[1][2] == 0.0f && ctx->cur_texcoord[1][3] == 1.0f);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE1);
  glTexCoord2f(0.5f, 0.125f);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.5f && ctx->cur_texcoord[1][0] == 3.0f);
  glGetFloatv(GL_CURRENT_TEXTURE_COORDS, fv);
  ASSERT_TRUE(fv[0] == 3.0f && fv[1] == 4.0f && fv[3] == 1.0f);
  /* The ARB spelling is the same function, not a stub. */
  glMultiTexCoord4fARB(GL_TEXTURE1, 1.0f, 2.0f, 3.0f, 2.0f);
  ASSERT_TRUE(ctx->cur_texcoord[1][2] == 3.0f && ctx->cur_texcoord[1][3] == 2.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* A unit past the maximum is refused and writes nothing - a refusal that still wrote some unit
   * would be worse than none. Null vectors are ignored rather than dereferenced. */
  glMultiTexCoord2f(GL_TEXTURE2, 9.0f, 9.0f);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.5f && ctx->cur_texcoord[1][0] == 1.0f);
  glActiveTexture(GL_TEXTURE2);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE1);
  glClientActiveTexture(GL_TEXTURE3);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glMultiTexCoord4fv(GL_TEXTURE1, NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Server state follows the active unit: the enables, the bindings, the environment and the
   * texture matrix. */
  GLuint tex[2] = {0, 0};
  glGenTextures(2, tex);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, tex[1]);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glMatrixMode(GL_TEXTURE);
  glScalef(2.0f, 2.0f, 2.0f);
  glActiveTexture(GL_TEXTURE0);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_2D), GL_FALSE);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, iv);
  ASSERT_EQ(iv[0], 0);
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_MODULATE);
  glGetFloatv(GL_TEXTURE_MATRIX, fv);
  ASSERT_TRUE(fv[0] == 1.0f);
  glActiveTexture(GL_TEXTURE1);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_2D), GL_TRUE);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, iv);
  ASSERT_EQ(iv[0], (GLint)tex[1]);
  glGetFloatv(GL_TEXTURE_MATRIX, fv);
  ASSERT_TRUE(fv[0] == 2.0f);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);

  /* Client arrays follow the client active unit. */
  glClientActiveTexture(GL_TEXTURE1);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_COORD_ARRAY), GL_TRUE);
  glClientActiveTexture(GL_TEXTURE0);
  ASSERT_EQ(glIsEnabled(GL_TEXTURE_COORD_ARRAY), GL_FALSE);
  /* The client attribute group carries every unit's array and the selector (table 6.6). */
  glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
  glClientActiveTexture(GL_TEXTURE1);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glPopClientAttrib();
  glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE0);
  ASSERT_EQ(ctx->array_texcoord[1].enabled, GL_TRUE);
  glClientActiveTexture(GL_TEXTURE1);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glClientActiveTexture(GL_TEXTURE0);

  /* GL_TEXTURE_BIT saves every unit and the active selector (table 6.20). */
  glActiveTexture(GL_TEXTURE1);
  glPushAttrib(GL_TEXTURE_BIT);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glActiveTexture(GL_TEXTURE0);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
  glPopAttrib();
  glGetIntegerv(GL_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE1);
  ASSERT_EQ(ctx->tex_unit[1].tex_env_mode, (GLenum)GL_ADD);
  ASSERT_EQ(ctx->tex_unit[0].tex_env_mode, (GLenum)GL_MODULATE);
  /* GL_CURRENT_BIT, every unit's current coordinate. */
  glPushAttrib(GL_CURRENT_BIT);
  glMultiTexCoord2f(GL_TEXTURE1, 7.0f, 7.0f);
  glPopAttrib();
  ASSERT_TRUE(ctx->cur_texcoord[1][0] == 1.0f);

  /* A list records the unit a coordinate went to and the unit it made active. */
  GLuint list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glMultiTexCoord2f(GL_TEXTURE1, 5.0f, 6.0f);
  glActiveTexture(GL_TEXTURE0);
  glEndList();
  ASSERT_TRUE(ctx->cur_texcoord[1][0] == 1.0f);
  glGetIntegerv(GL_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE1);
  glCallList(list);
  ASSERT_TRUE(ctx->cur_texcoord[1][0] == 5.0f && ctx->cur_texcoord[1][1] == 6.0f);
  glGetIntegerv(GL_ACTIVE_TEXTURE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_TEXTURE0);
  glDeleteLists(list, 1);

  /* Deleting a texture unbinds it from every unit. */
  glActiveTexture(GL_TEXTURE1);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(2, tex);
  ASSERT_EQ(ctx->tex_unit[1].bound_texture_2d, 0u);
  glActiveTexture(GL_TEXTURE0);

  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **Two units drawing** - the software rasteriser applies unit 0's environment to the fragment's
 * colour and unit 1's to what unit 0 left, each unit sampling its own coordinate (current, array,
 * after its own texture matrix), and GL_COMBINE telling GL_PREVIOUS, GL_PRIMARY_COLOR and GL 1.4's
 * GL_TEXTURE0 apart. 8x8 target, one quad over all of it; colours read at the centre. */
static void test_gl_two_texture_units_draw(void) {
  const int W = 8, H = 8;
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();
  #define CENTRE() (fb[(H / 2) * W + (W / 2)] & 0x00ffffffu)
  #define QUAD() do { glBegin(GL_QUADS); glVertex2f(-1, -1); glVertex2f(1, -1); \
    glVertex2f(1, 1); glVertex2f(-1, 1); glEnd(); } while (0)
  #define NEAR1(x, want) ((int)(x) - (want) <= 2 && (want) - (int)(x) <= 2)
  #define NEAR(c, r, g, b) (NEAR1(((c) >> 16) & 0xffu, r) && NEAR1(((c) >> 8) & 0xffu, g) && \
                            NEAR1((c) & 0xffu, b))

  /* Unit 0: red | green, replacing. Unit 1: blue | black, added. */
  static const GLubyte red_green[2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255};
  static const GLubyte blue_black[2 * 4] = {0, 0, 255, 255, 0, 0, 0, 255};
  GLuint tex[2] = {0, 0};
  glGenTextures(2, tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex[0]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red_green);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glEnable(GL_TEXTURE_2D);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, tex[1]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue_black);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  glEnable(GL_TEXTURE_2D);

  /* Each unit its own coordinate: unit 0 at s 0.25 (red), unit 1 at s 0.75 (black) - red. Read
   * unit 0's coordinate for unit 1 and it would be red + blue. */
  glTexCoord2f(0.25f, 0.5f);
  glMultiTexCoord2f(GL_TEXTURE1, 0.75f, 0.5f);
  QUAD();
  ASSERT_EQ(CENTRE(), 0xff0000u);
  /* Unit 1 at s 0.25 (blue): red + blue. */
  glMultiTexCoord2f(GL_TEXTURE1, 0.25f, 0.5f);
  QUAD();
  ASSERT_EQ(CENTRE(), 0xff00ffu);
  /* Unit 1's own texture matrix moves its coordinate to 0.75 and leaves unit 0's alone. */
  glMatrixMode(GL_TEXTURE);
  glTranslatef(0.5f, 0.0f, 0.0f);
  glMatrixMode(GL_MODELVIEW);
  QUAD();
  ASSERT_EQ(CENTRE(), 0xff0000u);
  glMatrixMode(GL_TEXTURE);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  /* Unit 1 off: unit 0 alone. */
  glDisable(GL_TEXTURE_2D);
  glTexCoord2f(0.75f, 0.5f);
  QUAD();
  ASSERT_EQ(CENTRE(), 0x00ff00u);
  glEnable(GL_TEXTURE_2D);

  /* From arrays: unit 1's coordinate array through the client active unit. */
  static const GLfloat pos[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
  static const GLfloat tc0[8] = {0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f};
  static const GLfloat tc1[8] = {0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, pos);
  glClientActiveTexture(GL_TEXTURE0);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 0, tc0);
  glClientActiveTexture(GL_TEXTURE1);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glTexCoordPointer(2, GL_FLOAT, 0, tc1);
  glMultiTexCoord2f(GL_TEXTURE1, 0.75f, 0.5f); /* the array, not this, is what unit 1 reads */
  glDrawArrays(GL_QUADS, 0, 4);
  ASSERT_EQ(CENTRE(), 0xff00ffu);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glClientActiveTexture(GL_TEXTURE0);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  /* GL_COMBINE on unit 1, fragment colour 0.5 grey, unit 0 now modulating red: unit 0 leaves
   * (0.5, 0, 0). GL_REPLACE of GL_PREVIOUS is that; of GL_PRIMARY_COLOR the grey; of GL 1.4's
   * GL_TEXTURE0 unit 0's texel, red. With one unit these three were indistinguishable. */
  glActiveTexture(GL_TEXTURE0);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glActiveTexture(GL_TEXTURE1);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
  glColor3f(0.5f, 0.5f, 0.5f);
  glTexCoord2f(0.25f, 0.5f);
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
  QUAD();
  ASSERT_TRUE(NEAR(CENTRE(), 128, 0, 0));
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
  QUAD();
  ASSERT_TRUE(NEAR(CENTRE(), 128, 128, 128));
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
  QUAD();
  ASSERT_EQ(CENTRE(), 0xff0000u);
  /* A crossbar source naming a unit past the last is refused. */
  glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE2);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glDisable(GL_TEXTURE_2D);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(2, tex);
  #undef CENTRE
  #undef QUAD
  #undef NEAR
  #undef NEAR1
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_gl_integer_spellings_normalise_only_what_should_be(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  /* Signed types put both extremes exactly on -1 and 1. */
  glColor4b(127, -128, 127, -128);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == -1.0f);
  ASSERT_TRUE(ctx->cur_color[2] == 1.0f && ctx->cur_color[3] == -1.0f);
  glColor4s(32767, -32768, 32767, -32768);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == -1.0f);

  /* Unsigned types put 0 on 0 and the maximum exactly on 1 - 255 divides, it does not shift. */
  glColor4ub(255, 0, 255, 0);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == 0.0f);
  ASSERT_TRUE(ctx->cur_color[2] == 1.0f && ctx->cur_color[3] == 0.0f);
  glColor4us(65535, 0, 65535, 0);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == 0.0f);

  /* A three-component colour sets alpha to 1.0 exactly, not to the converted maximum of its
   * own argument type - the two agree for ubyte and disagree for everything signed. */
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glColor3b(127, 127, 127);
  ASSERT_TRUE(ctx->cur_color[3] == 1.0f);
  glColor4f(0.0f, 0.0f, 0.0f, 0.0f);
  glColor3i(0, 0, 0);
  ASSERT_TRUE(ctx->cur_color[3] == 1.0f);

  /* Normals normalise like colours, **not** like positions: this is the mix-up that produces a
   * lit scene which is merely wrong. glNormal3b(127,..) is the axis; glNormal3f(127,..) would
   * be a normal 127 units long. */
  glNormal3b(127, 0, 0);
  ASSERT_TRUE(ctx->cur_normal[0] == 1.0f);
  ASSERT_TRUE(ctx->cur_normal[1] < 0.01f && ctx->cur_normal[1] > 0.0f);
  glNormal3s(0, 32767, 0);
  ASSERT_TRUE(ctx->cur_normal[1] == 1.0f);

  /* Positions do not convert. glVertex3i(1,2,3) is the point (1,2,3) - if this ever reads as
   * near-zero, an integer position has been put through a colour conversion. */
  glBegin(GL_TRIANGLES);
  glVertex3i(1, 2, 3);
  glVertex3s(4, 5, 6);
  glVertex4i(7, 8, 9, 1);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Texture coordinates do not convert either, and the components the call does not name take
   * the specification's defaults rather than whatever the previous call left behind. */
  glTexCoord4f(0.5f, 0.5f, 0.5f, 0.5f);
  glTexCoord2s(3, 4);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 3.0f && ctx->cur_texcoord[0][1] == 4.0f);
  ASSERT_TRUE(ctx->cur_texcoord[0][2] == 0.0f && ctx->cur_texcoord[0][3] == 1.0f);

  /* q is a projective divide, not a fourth coordinate to ignore - kept with the vertex, undivided,
   * for the rasteriser to divide per fragment (the vertex divided until 2026-09-19); the current
   * coordinate keeps what was passed, which is what GL_CURRENT_TEXTURE_COORDS reports. */
  glTexCoord4f(2.0f, 4.0f, 6.0f, 2.0f);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 2.0f && ctx->cur_texcoord[0][1] == 4.0f);
  ASSERT_TRUE(ctx->cur_texcoord[0][2] == 6.0f && ctx->cur_texcoord[0][3] == 2.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(0.0f, 0.0f, 0.0f);
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][0] == 2.0f && ctx->imm_verts[0].tc[0][1] == 4.0f);
  ASSERT_TRUE(ctx->imm_verts[0].tc[0][2] == 6.0f && ctx->imm_verts[0].tc[0][3] == 2.0f);

  /* A q of zero is kept as given too; the divide that meets it leaves the coordinate alone
   * rather than producing infinities (gl_q_inv). */
  glTexCoord4f(1.0f, 2.0f, 3.0f, 0.0f);
  glVertex3f(1.0f, 0.0f, 0.0f);
  ASSERT_TRUE(ctx->imm_verts[1].tc[0][0] == 1.0f && ctx->imm_verts[1].tc[0][1] == 2.0f);
  ASSERT_TRUE(ctx->imm_verts[1].tc[0][3] == 0.0f);
  glVertex3f(0.0f, 1.0f, 0.0f);
  glEnd();

  /* The vector forms reach the same place as the scalar ones. */
  const GLbyte nb[3] = {127, 0, 0};
  glNormal3f(0.0f, 0.0f, 0.0f);
  glNormal3bv(nb);
  ASSERT_TRUE(ctx->cur_normal[0] == 1.0f);
  const GLushort cu[4] = {65535, 0, 65535, 0};
  glColor4usv(cu);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == 0.0f);

  /* A null vector draws nothing rather than being dereferenced. */
  glColor4usv(NULL);
  glNormal3bv(NULL);
  glTexCoord4sv(NULL);
  glVertex4iv(NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_gl_every_spelling_reaches_the_same_state(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  /* Colours. The double and float forms are the same numbers; the unsigned-byte form is the
   * one with a conversion in it. */
  glColor3d(0.125, 0.25, 0.5);
  ASSERT_TRUE(ctx->cur_color[0] == 0.125f && ctx->cur_color[1] == 0.25f &&
              ctx->cur_color[2] == 0.5f && ctx->cur_color[3] == 1.0f);
  glColor4d(0.25, 0.5, 0.75, 0.125);
  ASSERT_TRUE(ctx->cur_color[0] == 0.25f && ctx->cur_color[1] == 0.5f &&
              ctx->cur_color[2] == 0.75f && ctx->cur_color[3] == 0.125f);

  const GLdouble c3[3] = {0.5, 0.125, 0.75};
  glColor3dv(c3);
  ASSERT_TRUE(ctx->cur_color[0] == 0.5f && ctx->cur_color[1] == 0.125f &&
              ctx->cur_color[2] == 0.75f && ctx->cur_color[3] == 1.0f);
  const GLdouble c4[4] = {0.75, 0.5, 0.25, 0.125};
  glColor4dv(c4);
  ASSERT_TRUE(ctx->cur_color[0] == 0.75f && ctx->cur_color[3] == 0.125f);

  /* **255 must land on exactly 1.0.** A shift by 8 instead of a divide by 255 leaves full white
   * one step short of white - invisible on its own, wrong in a blend, and the reason this is
   * asserted for equality rather than nearness. */
  glColor3ub(255, 0, 128);
  ASSERT_TRUE(ctx->cur_color[0] == 1.0f && ctx->cur_color[1] == 0.0f &&
              ctx->cur_color[3] == 1.0f);
  ASSERT_TRUE(ctx->cur_color[2] > 0.501f && ctx->cur_color[2] < 0.503f);
  const GLubyte u3[3] = {0, 255, 64};
  glColor3ubv(u3);
  ASSERT_TRUE(ctx->cur_color[0] == 0.0f && ctx->cur_color[1] == 1.0f &&
              ctx->cur_color[3] == 1.0f);
  const GLubyte u4[4] = {64, 0, 255, 32};
  glColor4ubv(u4);
  ASSERT_TRUE(ctx->cur_color[2] == 1.0f && ctx->cur_color[1] == 0.0f);
  ASSERT_TRUE(ctx->cur_color[3] > 0.125f && ctx->cur_color[3] < 0.126f);

  /* Normals and texture coordinates. */
  glNormal3d(0.25, 0.5, 0.75);
  ASSERT_TRUE(ctx->cur_normal[0] == 0.25f && ctx->cur_normal[1] == 0.5f &&
              ctx->cur_normal[2] == 0.75f);
  const GLdouble n3[3] = {0.75, 0.25, 0.5};
  glNormal3dv(n3);
  ASSERT_TRUE(ctx->cur_normal[0] == 0.75f && ctx->cur_normal[1] == 0.25f &&
              ctx->cur_normal[2] == 0.5f);

  glTexCoord2d(0.25, 0.75);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.25f && ctx->cur_texcoord[0][1] == 0.75f);
  glTexCoord2i(3, 7);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 3.0f && ctx->cur_texcoord[0][1] == 7.0f);
  /* The one-coordinate form leaves t at zero, so it has to clear a t that was set. */
  glTexCoord1f(0.5f);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.5f && ctx->cur_texcoord[0][1] == 0.0f);
  const GLfloat t2f[2] = {0.125f, 0.875f};
  glTexCoord2fv(t2f);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.125f && ctx->cur_texcoord[0][1] == 0.875f);
  const GLdouble t2d[2] = {0.875, 0.125};
  glTexCoord2dv(t2d);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 0.875f && ctx->cur_texcoord[0][1] == 0.125f);
  const GLint t2i[2] = {5, 9};
  glTexCoord2iv(t2i);
  ASSERT_TRUE(ctx->cur_texcoord[0][0] == 5.0f && ctx->cur_texcoord[0][1] == 9.0f);

  /* Vertices, read out of the immediate-mode buffer. The two-coordinate forms must also set
   * z to 0 and w to 1 rather than leaving whatever the previous vertex had. */
  glBegin(GL_TRIANGLES);
  glVertex3d(0.125, 0.25, 0.5);
  glVertex2d(0.75, 0.875);
  glVertex4d(1.5, 2.5, 3.5, 4.5);
  glVertex3i(1, 2, 3);
  glVertex2i(4, 5);
  const GLdouble v3d[3] = {9.0, 8.0, 7.0};
  glVertex3dv(v3d);
  const GLdouble v2d[2] = {6.0, 5.0};
  glVertex2dv(v2d);
  const GLint v3i[3] = {11, 12, 13};
  glVertex3iv(v3i);
  const GLint v2i[2] = {14, 15};
  glVertex2iv(v2i);
  const GLfloat v2f[2] = {16.0f, 17.0f};
  glVertex2fv(v2f);
  const GLfloat v4f[4] = {18.0f, 19.0f, 20.0f, 21.0f};
  glVertex4fv(v4f);
  const gl_vertex_t *v = ctx->imm_verts;
  ASSERT_EQ(ctx->imm_count, 11);
  ASSERT_TRUE(v[0].x == 0.125f && v[0].y == 0.25f && v[0].z == 0.5f && v[0].w == 1.0f);
  ASSERT_TRUE(v[1].x == 0.75f && v[1].y == 0.875f && v[1].z == 0.0f && v[1].w == 1.0f);
  ASSERT_TRUE(v[2].x == 1.5f && v[2].y == 2.5f && v[2].z == 3.5f && v[2].w == 4.5f);
  ASSERT_TRUE(v[3].x == 1.0f && v[3].y == 2.0f && v[3].z == 3.0f && v[3].w == 1.0f);
  ASSERT_TRUE(v[4].x == 4.0f && v[4].y == 5.0f && v[4].z == 0.0f && v[4].w == 1.0f);
  ASSERT_TRUE(v[5].x == 9.0f && v[5].y == 8.0f && v[5].z == 7.0f);
  ASSERT_TRUE(v[6].x == 6.0f && v[6].y == 5.0f && v[6].z == 0.0f);
  ASSERT_TRUE(v[7].x == 11.0f && v[7].y == 12.0f && v[7].z == 13.0f);
  ASSERT_TRUE(v[8].x == 14.0f && v[8].y == 15.0f && v[8].z == 0.0f);
  ASSERT_TRUE(v[9].x == 16.0f && v[9].y == 17.0f && v[9].z == 0.0f && v[9].w == 1.0f);
  ASSERT_TRUE(v[10].x == 18.0f && v[10].y == 19.0f && v[10].z == 20.0f && v[10].w == 21.0f);
  glEnd();

  /* A null vector is ignored rather than dereferenced. */
  glBegin(GL_TRIANGLES);
  glVertex3dv(NULL);
  glVertex2iv(NULL);
  glColor3dv(NULL);
  glTexCoord2fv(NULL);
  glNormal3dv(NULL);
  ASSERT_EQ(ctx->imm_count, 0);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The double matrix spellings. The trap is glLoadMatrixd: casting the pointer instead of
 * narrowing element by element reads eight doubles as sixteen floats, which is noise - so the
 * fixture is sixteen *distinct* values and every one is checked. */
static void test_gl_double_matrix_forms_narrow_element_by_element(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  glMatrixMode(GL_MODELVIEW);

  GLdouble md[16];
  GLfloat mf[16];
  for (int i = 0; i < 16; i++) {
    md[i] = (GLdouble)(i + 1) * 0.25;
    mf[i] = (GLfloat)md[i];
  }

  glLoadMatrixd(md);
  gl_mat4_t after_d = ctx->modelview_stack[ctx->modelview_depth];
  glLoadMatrixf(mf);
  ASSERT_EQ(memcmp(&after_d, &ctx->modelview_stack[ctx->modelview_depth], sizeof after_d), 0);
  /* Not vacuously equal to a blank matrix: the values really arrived. */
  ASSERT_TRUE(((const float *)&after_d)[15] == 4.0f);
  ASSERT_TRUE(((const float *)&after_d)[1] == 0.5f);

  /* glTranslated / glRotated / glScaled, each against its float twin from the same start.
   * The arguments differ in every component, so an implementation dropping or duplicating one
   * shows up rather than cancelling out. */
  glLoadIdentity();
  glTranslated(1.5, 2.5, 3.5);
  glRotated(30.0, 0.25, 0.5, 0.75);
  glScaled(2.0, 3.0, 4.0);
  glMultMatrixd(md);
  gl_mat4_t chain_d = ctx->modelview_stack[ctx->modelview_depth];

  glLoadIdentity();
  glTranslatef(1.5f, 2.5f, 3.5f);
  glRotatef(30.0f, 0.25f, 0.5f, 0.75f);
  glScalef(2.0f, 3.0f, 4.0f);
  glMultMatrixf(mf);
  ASSERT_EQ(memcmp(&chain_d, &ctx->modelview_stack[ctx->modelview_depth], sizeof chain_d), 0);

  /* And the chain is not the identity, so the comparison above has something in it. */
  glLoadIdentity();
  ASSERT_TRUE(memcmp(&chain_d, &ctx->modelview_stack[ctx->modelview_depth],
                     sizeof chain_d) != 0);

  /* A null matrix pointer leaves the stack alone rather than being dereferenced. */
  glLoadMatrixd(NULL);
  glMultMatrixd(NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **The query family, and the trap is how many elements it writes.**
 *
 * The caller sizes its buffer from the pname, so a query that writes four values into the
 * `GLint[1]` a program allocated for GL_DEPTH_FUNC corrupts whatever sits after it - and does
 * so only for the pnames nobody happened to test. Every single-valued query below is therefore
 * given a buffer with a **guard element after it** that must still hold its sentinel.
 */
static void test_gl_queries_report_and_refuse(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  /* The limits a program reads to size its own work. Zero here makes a correct program give
   * up before it draws anything, which is why these are asserted against the real capacities
   * rather than merely "not an error". */
  GLint iv[18];
  const GLint GUARD = 0x5eed;

  iv[1] = GUARD;
  glGetIntegerv(GL_MAX_LIGHTS, iv);
  ASSERT_EQ(iv[0], OOPS_GL_LIGHT_COUNT);
  ASSERT_EQ(iv[1], GUARD);

  iv[1] = GUARD;
  glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], OOPS_GL_MODELVIEW_STACK_CAPACITY);
  ASSERT_EQ(iv[1], GUARD);

  iv[1] = GUARD;
  glGetIntegerv(GL_MAX_CLIENT_ATTRIB_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY);
  ASSERT_EQ(iv[1], GUARD);

  iv[1] = GUARD;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, iv);
  ASSERT_TRUE(iv[0] >= 64);
  ASSERT_EQ(iv[1], GUARD);

  /* Two elements, and the third must be untouched. */
  iv[2] = GUARD;
  glGetIntegerv(GL_MAX_VIEWPORT_DIMS, iv);
  ASSERT_EQ(iv[0], 320);
  ASSERT_EQ(iv[1], 240);
  ASSERT_EQ(iv[2], GUARD);

  /* State, not limits. Set it, then read it back through the query. */
  glDepthFunc(GL_GEQUAL);
  glCullFace(GL_FRONT);
  glFrontFace(GL_CW);
  glShadeModel(GL_FLAT);
  glListBase(37);
  iv[1] = GUARD;
  glGetIntegerv(GL_DEPTH_FUNC, iv);
  ASSERT_EQ(iv[0], (GLint)GL_GEQUAL);
  ASSERT_EQ(iv[1], GUARD);
  glGetIntegerv(GL_CULL_FACE_MODE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_FRONT);
  glGetIntegerv(GL_FRONT_FACE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_CW);
  glGetIntegerv(GL_SHADE_MODEL, iv);
  ASSERT_EQ(iv[0], (GLint)GL_FLAT);
  glGetIntegerv(GL_LIST_BASE, iv);
  ASSERT_EQ(iv[0], 37);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The stack depths move with the stacks. GL counts the current matrix, so an untouched
   * modelview stack reports 1 rather than 0. */
  glMatrixMode(GL_MODELVIEW);
  glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, iv);
  const GLint depth_before = iv[0];
  glPushMatrix();
  glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], depth_before + 1);
  glPopMatrix();

  glPushAttrib(GL_DEPTH_BUFFER_BIT);
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glGetIntegerv(GL_ATTRIB_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], 1);
  glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, iv);
  ASSERT_EQ(iv[0], 1);
  glPopClientAttrib();
  glPopAttrib();

  /* The float side, including the matrices. Three elements for a normal, and the fourth must
   * be untouched - GL_CURRENT_NORMAL is the one query in this port that is neither 1, 2, 4 nor
   * 16 wide, so it is the one a single hardcoded width gets wrong. */
  GLfloat fv[18];
  const GLfloat FGUARD = -12345.0f;
  glNormal3f(0.25f, 0.5f, 0.75f);
  fv[3] = FGUARD;
  glGetFloatv(GL_CURRENT_NORMAL, fv);
  ASSERT_TRUE(fv[0] == 0.25f && fv[1] == 0.5f && fv[2] == 0.75f);
  ASSERT_TRUE(fv[3] == FGUARD);

  glColor4f(0.125f, 0.25f, 0.5f, 0.75f);
  fv[4] = FGUARD;
  glGetFloatv(GL_CURRENT_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.125f && fv[3] == 0.75f && fv[4] == FGUARD);

  glPolygonOffset(2.0f, 3.0f);
  fv[1] = FGUARD;
  glGetFloatv(GL_POLYGON_OFFSET_FACTOR, fv);
  ASSERT_TRUE(fv[0] == 2.0f && fv[1] == FGUARD);
  glGetFloatv(GL_POLYGON_OFFSET_UNITS, fv);
  ASSERT_TRUE(fv[0] == 3.0f);

  glDepthRange(0.25, 0.75);
  fv[2] = FGUARD;
  glGetFloatv(GL_DEPTH_RANGE, fv);
  ASSERT_TRUE(fv[0] == 0.25f && fv[1] == 0.75f && fv[2] == FGUARD);

  /* An integer-valued pname asked in float, which is the cross-type conversion GL requires. */
  fv[1] = FGUARD;
  glGetFloatv(GL_DEPTH_FUNC, fv);
  ASSERT_TRUE(fv[0] == (GLfloat)GL_GEQUAL && fv[1] == FGUARD);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Doubles go through the float table, so they agree by construction - but the widths have to
   * survive the trip, which is what the guard after the matrix checks. */
  GLdouble dv[18];
  dv[16] = -1.0;
  glGetDoublev(GL_MODELVIEW_MATRIX, dv);
  glGetFloatv(GL_MODELVIEW_MATRIX, fv);
  for (int i = 0; i < 16; i++) ASSERT_TRUE(dv[i] == (GLdouble)fv[i]);
  ASSERT_TRUE(dv[16] == -1.0);
  dv[1] = -1.0;
  glGetDoublev(GL_DEPTH_FUNC, dv);
  ASSERT_TRUE(dv[0] == (GLdouble)GL_GEQUAL && dv[1] == -1.0);
  /* Three, which is the width a single hardcoded number gets wrong. Only the double form
   * consults the width table - glGetFloatv answers this pname from its own loop - so this is
   * the assertion that holds the table honest. */
  dv[3] = -1.0;
  glGetDoublev(GL_CURRENT_NORMAL, dv);
  ASSERT_TRUE(dv[0] == 0.25 && dv[1] == 0.5 && dv[2] == 0.75 && dv[3] == -1.0);

  /* **glIsEnabled must answer from the same list glEnable accepts.** These two were absent, so
   * a capability that had just been switched on read back as off. */
  glEnable(GL_ALPHA_TEST);
  ASSERT_EQ(glIsEnabled(GL_ALPHA_TEST), GL_TRUE);
  glEnable(GL_POLYGON_OFFSET_FILL);
  ASSERT_EQ(glIsEnabled(GL_POLYGON_OFFSET_FILL), GL_TRUE);
  glDisable(GL_ALPHA_TEST);
  ASSERT_EQ(glIsEnabled(GL_ALPHA_TEST), GL_FALSE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Booleans: a mask is not an enable. Forwarding GL_COLOR_WRITEMASK to glIsEnabled reported
   * all four channels off while all four were on. */
  GLboolean bv[5];
  glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
  bv[4] = 42;
  glGetBooleanv(GL_COLOR_WRITEMASK, bv);
  ASSERT_EQ(bv[0], GL_TRUE);
  ASSERT_EQ(bv[1], GL_FALSE);
  ASSERT_EQ(bv[2], GL_TRUE);
  ASSERT_EQ(bv[3], GL_FALSE);
  ASSERT_EQ(bv[4], 42);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  glDepthMask(GL_FALSE);
  bv[1] = 42;
  glGetBooleanv(GL_DEPTH_WRITEMASK, bv);
  ASSERT_EQ(bv[0], GL_FALSE);
  ASSERT_EQ(bv[1], 42);
  glDepthMask(GL_TRUE);

  glEnable(GL_BLEND);
  glGetBooleanv(GL_BLEND, bv);
  ASSERT_EQ(bv[0], GL_TRUE);
  glDisable(GL_BLEND);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **Refused, not ignored.** A pname nothing here answers must raise rather than leave the
   * caller's buffer as it found it - the sentinel proves the buffer was not touched, and the
   * error proves the caller can tell. */
  iv[0] = GUARD;
  glGetIntegerv(0x8000u /* GL_FOG_HINT: real GL, absent here */, iv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_EQ(iv[0], GUARD);

  fv[0] = FGUARD;
  glGetFloatv(0x8000u, fv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_TRUE(fv[0] == FGUARD);

  dv[0] = -1.0;
  glGetDoublev(0x8000u, dv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_TRUE(dv[0] == -1.0);

  bv[0] = 42;
  glGetBooleanv(0x8000u, bv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  ASSERT_EQ(bv[0], 42);

  /* A null buffer is ignored rather than dereferenced. */
  glGetIntegerv(GL_MAX_LIGHTS, NULL);
  glGetFloatv(GL_CURRENT_COLOR, NULL);
  glGetDoublev(GL_MODELVIEW_MATRIX, NULL);
  glGetBooleanv(GL_COLOR_WRITEMASK, NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **glGetTexImage does not flip, and glReadPixels does.**
 *
 * They look like the same operation and share the conversion, which is exactly why this is
 * asserted rather than assumed: glReadPixels turns the framebuffer over because GL's window
 * origin is the bottom-left corner, and a texture has no window. Row 0 of a texture image is
 * row 0 of what was uploaded. Reusing glReadPixels' loop here would have been the tidy-looking
 * mistake, so the fixture makes the two orientations different pictures.
 */
static void test_gl_get_tex_image_reads_back_unflipped(void) {
  enum { W = 4, H = 3 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  /* Every texel distinct, and W != H so a transpose cannot hide either. */
  GLubyte src[W * H * 4];
  for (int row = 0; row < H; row++) {
    for (int col = 0; col < W; col++) {
      GLubyte *p = src + ((size_t)row * W + (size_t)col) * 4u;
      p[0] = (GLubyte)(10 + row * 16 + col); /* r */
      p[1] = (GLubyte)(60 + row * 16 + col); /* g */
      p[2] = (GLubyte)(110 + row * 16 + col);/* b */
      p[3] = (GLubyte)(160 + row * 16 + col);/* a */
    }
  }

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  GLubyte back[W * H * 4];
  memset(back, 0xcd, sizeof back);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(memcmp(back, src, sizeof src), 0);

  /* And the fixture would notice a flip: reversing the rows is a different picture. */
  GLubyte flipped[W * H * 4];
  for (int row = 0; row < H; row++) {
    memcpy(flipped + (size_t)row * W * 4u, src + (size_t)(H - 1 - row) * W * 4u,
           (size_t)W * 4u);
  }
  ASSERT_TRUE(memcmp(back, flipped, sizeof back) != 0);

  /* A narrower format drops the channels it has no room for, through the same packer
   * glReadPixels uses - so GL_RGB is three bytes a texel with the alpha gone. */
  GLubyte rgb[W * H * 3];
  memset(rgb, 0xcd, sizeof rgb);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
  for (int i = 0; i < W * H; i++) {
    ASSERT_EQ(rgb[i * 3 + 0], src[i * 4 + 0]);
    ASSERT_EQ(rgb[i * 3 + 1], src[i * 4 + 1]);
    ASSERT_EQ(rgb[i * 3 + 2], src[i * 4 + 2]);
  }

  /* GL_BGRA swaps red and blue. The fixture has r != b in every texel, so a packer that
   * forwarded the channels unchanged fails here rather than passing on a grey image. */
  GLubyte bgra[W * H * 4];
  glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
  for (int i = 0; i < W * H; i++) {
    ASSERT_EQ(bgra[i * 4 + 0], src[i * 4 + 2]);
    ASSERT_EQ(bgra[i * 4 + 2], src[i * 4 + 0]);
  }
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Refusals: a target this does not have, a format it cannot convert, and a query with no
   * image under it. */
  /* GL_TEXTURE_1D used to be the example here, then GL_TEXTURE_3D; both are real targets now.
   * GL_TEXTURE_CUBE_MAP is a real texture too, but not an image target - a cube's images are
   * its six faces, each read through its own - so it is refused here as GL says it is. */
  glGetTexImage(GL_TEXTURE_CUBE_MAP, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetTexImage(GL_TEXTURE_2D, 0, (GLenum)0x1900u /* GL_COLOR_INDEX */, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  GLuint empty = 0;
  glGenTextures(1, &empty);
  glBindTexture(GL_TEXTURE_2D, empty);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glDeleteTextures(1, &tex);
  glDeleteTextures(1, &empty);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The per-object queries, each read back against the setter that wrote it. A query answering
 * from anywhere but the field its setter writes is how glIsEnabled came to disagree with
 * glEnable, so every one of these sets first and asks afterwards. */
static void test_gl_per_object_queries_match_their_setters(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  GLint iv[6];
  GLfloat fv[6];
  const GLint GUARD = 0x5eed;
  const GLfloat FGUARD = -12345.0f;

  /* Texture parameters. */
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  iv[1] = GUARD;
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, iv);
  ASSERT_EQ(iv[0], (GLint)GL_CLAMP_TO_EDGE);
  ASSERT_EQ(iv[1], GUARD);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NEAREST);
  /* Untouched, so still the default a fresh object was given - and that default has to be the
   * one gl_find_or_create_texture actually assigns, not a plausible-looking other one. */
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, iv);
  ASSERT_EQ(iv[0], (GLint)GL_REPEAT);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, fv);
  ASSERT_TRUE(fv[0] == (GLfloat)GL_CLAMP_TO_EDGE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Per-level: the image, not the sampler. Width and height differ so a swap shows. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, iv);
  ASSERT_EQ(iv[0], 8);
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, iv);
  ASSERT_EQ(iv[0], 4);
  /* **What was asked for**, as Mesa reports it, now that the internal format means something:
   * GL_RGB went in and is kept as RGB - alpha 1 however it was uploaded. This reported GL_RGBA
   * while every texture was RGBA. */
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_RGB);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Lights. Four-element colours and a three-element direction, each with a guard after it. */
  const GLfloat amb[4] = {0.125f, 0.25f, 0.5f, 0.75f};
  glLightfv(GL_LIGHT2, GL_AMBIENT, amb);
  glLightf(GL_LIGHT2, GL_SPOT_EXPONENT, 3.5f);
  glLightf(GL_LIGHT2, GL_QUADRATIC_ATTENUATION, 0.75f);
  fv[4] = FGUARD;
  glGetLightfv(GL_LIGHT2, GL_AMBIENT, fv);
  ASSERT_TRUE(fv[0] == 0.125f && fv[1] == 0.25f && fv[2] == 0.5f && fv[3] == 0.75f);
  ASSERT_TRUE(fv[4] == FGUARD);
  fv[1] = FGUARD;
  glGetLightfv(GL_LIGHT2, GL_SPOT_EXPONENT, fv);
  ASSERT_TRUE(fv[0] == 3.5f && fv[1] == FGUARD);
  glGetLightfv(GL_LIGHT2, GL_QUADRATIC_ATTENUATION, fv);
  ASSERT_TRUE(fv[0] == 0.75f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **Eye coordinates**, which is the question GL asks. glLightfv puts the position through
   * the modelview matrix on the way in, so a translated modelview means the value read back is
   * deliberately not the one passed. */
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  glTranslatef(10.0f, 0.0f, 0.0f);
  const GLfloat pos[4] = {1.0f, 2.0f, 3.0f, 1.0f};
  glLightfv(GL_LIGHT3, GL_POSITION, pos);
  glGetLightfv(GL_LIGHT3, GL_POSITION, fv);
  ASSERT_TRUE(fv[0] == 11.0f); /* 1 + the translate */
  ASSERT_TRUE(fv[1] == 2.0f && fv[2] == 3.0f && fv[3] == 1.0f);
  glPopMatrix();

  /* Materials. GL_FRONT and GL_BACK are separate, and asking for both at once has no single
   * answer, so it is refused although glMaterialfv accepts it. */
  const GLfloat emis[4] = {0.75f, 0.5f, 0.25f, 0.125f};
  glMaterialfv(GL_FRONT, GL_EMISSION, emis);
  glMaterialf(GL_FRONT, GL_SHININESS, 17.0f);
  fv[4] = FGUARD;
  glGetMaterialfv(GL_FRONT, GL_EMISSION, fv);
  ASSERT_TRUE(fv[0] == 0.75f && fv[3] == 0.125f && fv[4] == FGUARD);
  fv[1] = FGUARD;
  glGetMaterialfv(GL_FRONT, GL_SHININESS, fv);
  ASSERT_TRUE(fv[0] == 17.0f && fv[1] == FGUARD);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, fv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* The integer forms narrow the same answers, and write the same number of them. */
  iv[4] = GUARD;
  glGetMaterialiv(GL_FRONT, GL_SHININESS, iv);
  ASSERT_EQ(iv[0], 17);
  glGetLightiv(GL_LIGHT2, GL_SPOT_EXPONENT, iv);
  ASSERT_EQ(iv[0], 3);
  ASSERT_EQ(iv[4], GUARD);

  /* Pointers. */
  static const GLfloat buf[16] = {0};
  glVertexPointer(3, GL_FLOAT, 0, buf);
  GLvoid *p = NULL;
  glGetPointerv(GL_VERTEX_ARRAY_POINTER, &p);
  ASSERT_TRUE(p == (const void *)buf);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Refusals across the family. */
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WIDTH, iv); /* a level query, not a parameter */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetLightfv(GL_LIGHT0 + 99, GL_AMBIENT, fv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetPointerv(GL_TEXTURE_WIDTH, &p);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  glDeleteTextures(1, &tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **glInterleavedArrays: fourteen formats, each with its own offsets.**
 *
 * The offsets in the implementation were checked against Mesa's `_mesa_get_interleaved_layout`
 * rather than derived, so what this test is for is the arithmetic *around* them - the default
 * stride, and which arrays end up enabled. The formats below are chosen to cover every array
 * being present, a byte colour, and formats that leave arrays out.
 *
 * Note the C4UB alignment rule is not observable here: four bytes rounded up to a 4-byte float
 * is four bytes. It is written out in the source anyway, but no assertion can pin it down on
 * this platform and none below pretends to.
 */
static void test_gl_interleaved_arrays_sets_the_pointers_the_spec_names(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  static const GLubyte buf[256] = {0};
  const GLubyte *base = buf;
  const size_t f = sizeof(GLfloat);

  /* GL_T2F_C4F_N3F_V3F: every array present, so every offset is exercised at once. */
  glInterleavedArrays(GL_T2F_C4F_N3F_V3F, 0, buf);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(ctx->array_texcoord[0].enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_color.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_normal.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_vertex.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_texcoord[0].size, 2);
  ASSERT_EQ(ctx->array_color.size, 4);
  ASSERT_EQ(ctx->array_vertex.size, 3);
  ASSERT_TRUE(ctx->array_texcoord[0].pointer == (const void *)base);
  ASSERT_TRUE(ctx->array_color.pointer == (const void *)(base + 2 * f));
  ASSERT_TRUE(ctx->array_normal.pointer == (const void *)(base + 6 * f));
  ASSERT_TRUE(ctx->array_vertex.pointer == (const void *)(base + 9 * f));
  /* stride 0 means the format's own size: 2 + 4 + 3 + 3 floats. */
  ASSERT_EQ(ctx->array_vertex.stride, (GLsizei)(12 * f));

  /* GL_T2F_C4UB_V3F: the byte colour. Two floats of texture coordinate, then the colour in four
   * bytes, then the position. Mesa's table gives voffset = c + 2f and defstride = c + 5f, which
   * with c = 4 is 3f and 6f. */
  glInterleavedArrays(GL_T2F_C4UB_V3F, 0, buf);
  ASSERT_EQ(ctx->array_color.type, (GLenum)GL_UNSIGNED_BYTE);
  ASSERT_EQ(ctx->array_color.size, 4);
  ASSERT_TRUE(ctx->array_color.pointer == (const void *)(base + 2 * f));
  ASSERT_TRUE(ctx->array_vertex.pointer == (const void *)(base + 3 * f));
  ASSERT_EQ(ctx->array_vertex.stride, (GLsizei)(6 * f));
  ASSERT_EQ(ctx->array_normal.enabled, GL_FALSE);

  /* **The arrays a format does not name are disabled, not left alone.** The normal array was
   * on a moment ago; after a format without one it must be off, or the draw keeps reading
   * normals through a pointer nothing set for this buffer. */
  glInterleavedArrays(GL_C3F_V3F, 0, buf);
  ASSERT_EQ(ctx->array_normal.enabled, GL_FALSE);
  ASSERT_EQ(ctx->array_texcoord[0].enabled, GL_FALSE);
  ASSERT_EQ(ctx->array_color.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_color.size, 3);
  ASSERT_EQ(ctx->array_color.type, (GLenum)GL_FLOAT);
  ASSERT_TRUE(ctx->array_color.pointer == (const void *)base);
  ASSERT_TRUE(ctx->array_vertex.pointer == (const void *)(base + 3 * f));

  /* The simplest format: position only, and everything else off. */
  glInterleavedArrays(GL_V2F, 0, buf);
  ASSERT_EQ(ctx->array_vertex.size, 2);
  ASSERT_EQ(ctx->array_color.enabled, GL_FALSE);
  ASSERT_TRUE(ctx->array_vertex.pointer == (const void *)base);
  ASSERT_EQ(ctx->array_vertex.stride, (GLsizei)(2 * f));

  /* The widest one, where a miscounted offset has the furthest to travel. */
  glInterleavedArrays(GL_T4F_C4F_N3F_V4F, 0, buf);
  ASSERT_EQ(ctx->array_texcoord[0].size, 4);
  ASSERT_EQ(ctx->array_vertex.size, 4);
  ASSERT_TRUE(ctx->array_color.pointer == (const void *)(base + 4 * f));
  ASSERT_TRUE(ctx->array_normal.pointer == (const void *)(base + 8 * f));
  ASSERT_TRUE(ctx->array_vertex.pointer == (const void *)(base + 11 * f));
  ASSERT_EQ(ctx->array_vertex.stride, (GLsizei)(15 * f));

  /* An explicit stride overrides the packed size but not the offsets. */
  glInterleavedArrays(GL_N3F_V3F, 64, buf);
  ASSERT_EQ(ctx->array_vertex.stride, 64);
  ASSERT_EQ(ctx->array_normal.stride, 64);
  ASSERT_TRUE(ctx->array_vertex.pointer == (const void *)(base + 3 * f));
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Refusals. */
  glInterleavedArrays(GL_TRIANGLES, 0, buf);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glInterleavedArrays(GL_V3F, -1, buf);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **Buffer objects, and the design decision worth testing is *when* the address is worked out.**
 *
 * An array remembers the buffer's *name*, not an address. Resolving at glVertexPointer time
 * would be simpler and would work right up until a program called glBufferData again - which is
 * an ordinary thing to do every frame - and then read through a freed pointer. The test below
 * forces a reallocation between specifying the pointer and drawing, and makes the second
 * allocation much larger so it cannot land back on the first address and pass by luck.
 *
 * The other trap is that `pointer` becomes a byte *offset* when a buffer is bound, and offset
 * zero is a legitimate value that arrives as NULL - which the array reader's old "is the
 * pointer non-null" guard would have read as "no array bound" and silently drawn defaults.
 */
static void test_gl_buffer_objects_resolve_at_draw_time(void) {
  enum { W = 64, H = 64 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glColor3f(1.0f, 0.0f, 0.0f);

  /* Two different triangles, neither symmetric about x=y. */
  static const GLfloat tri_a[9] = {
    -0.8f, -0.6f, 0.0f,   0.4f, -0.6f, 0.0f,   0.4f, 0.7f, 0.0f,
  };
  static const GLfloat tri_b[9] = {
    -0.3f, -0.9f, 0.0f,   0.9f, -0.2f, 0.0f,  -0.1f, 0.5f, 0.0f,
  };

  static uint32_t want_a[W * H], want_b[W * H], got[W * H];

  /* The reference pictures, drawn from client memory through the path that already worked. */
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, tri_a);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  memcpy(want_a, fb, sizeof want_a);
  glVertexPointer(3, GL_FLOAT, 0, tri_b);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  memcpy(want_b, fb, sizeof want_b);
  ASSERT_TRUE(memcmp(want_a, want_b, sizeof want_a) != 0); /* the fixtures differ */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Now the same thing out of a buffer object, at offset zero - the NULL that means zero. */
  GLuint vbo = 0;
  glGenBuffers(1, &vbo);
  ASSERT_TRUE(vbo != 0u);
  ASSERT_EQ(glIsBuffer(vbo), GL_TRUE);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof tri_a, tri_a, GL_STATIC_DRAW);
  glVertexPointer(3, GL_FLOAT, 0, NULL);
  ASSERT_EQ(ctx->array_vertex.buffer, vbo);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  memcpy(got, fb, sizeof got);
  ASSERT_EQ(memcmp(got, want_a, sizeof got), 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The reallocation.** glBufferData respecifies the store; the pointer was set before this
   * happened and is not set again. A far larger allocation makes reuse of the old address
   * implausible, so an implementation that captured the address at glVertexPointer time reads
   * freed memory here rather than passing by coincidence. */
  static GLfloat big[9 + 4096];
  memcpy(big, tri_b, sizeof tri_b);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof big, big, GL_DYNAMIC_DRAW);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  memcpy(got, fb, sizeof got);
  ASSERT_EQ(memcmp(got, want_b, sizeof got), 0);

  /* A non-zero offset reaches into the buffer. tri_b sits at the start of `big`, so putting
   * tri_a one triangle further in and pointing there must draw tri_a again. */
  glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)sizeof tri_a, (GLsizeiptr)sizeof tri_a, tri_a);
  glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)(uintptr_t)sizeof tri_a);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  memcpy(got, fb, sizeof got);
  ASSERT_EQ(memcmp(got, want_a, sizeof got), 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The parameters read back. */
  GLint iv[2];
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, iv);
  ASSERT_EQ(iv[0], (GLint)sizeof big);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_USAGE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_DYNAMIC_DRAW);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, iv);
  ASSERT_EQ(iv[0], (GLint)vbo);

  /* **The binding at pointer-specification time is what the array keeps**, so two arrays can
   * read from two different buffers at once. */
  GLuint second = 0;
  glGenBuffers(1, &second);
  glBindBuffer(GL_ARRAY_BUFFER, second);
  glBufferData(GL_ARRAY_BUFFER, 64, NULL, GL_STATIC_DRAW);
  glColorPointer(4, GL_FLOAT, 0, NULL);
  ASSERT_EQ(ctx->array_color.buffer, second);
  ASSERT_EQ(ctx->array_vertex.buffer, vbo); /* set earlier, and unchanged by the rebind */

  /* Back to client memory. */
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexPointer(3, GL_FLOAT, 0, tri_a);
  ASSERT_EQ(ctx->array_vertex.buffer, 0u);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ASSERT_EQ(memcmp(fb, want_a, sizeof want_a), 0);

  /* **Deleting a bound buffer reverts the binding to 0**, or the next glBufferData looks up a
   * name nothing owns. */
  glBindBuffer(GL_ARRAY_BUFFER, second);
  glDeleteBuffers(1, &second);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, iv);
  ASSERT_EQ(iv[0], 0);
  ASSERT_EQ(glIsBuffer(second), GL_FALSE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Refusals. */
  glBindBuffer(GL_TEXTURE_2D, vbo);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glBindBuffer(GL_ARRAY_BUFFER, 4242u);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBufferData(GL_ARRAY_BUFFER, 16, NULL, GL_STATIC_DRAW);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION); /* nothing bound to fill */
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, 16, NULL, GL_TRIANGLES);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM); /* not a usage */
  glBufferData(GL_ARRAY_BUFFER, -1, NULL, GL_STATIC_DRAW);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glBufferData(GL_ARRAY_BUFFER, 32, NULL, GL_STATIC_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 16, 32, tri_a); /* runs off the end */
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  glBufferSubData(GL_ARRAY_BUFFER, -4, 8, tri_a);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  glDisableClientState(GL_VERTEX_ARRAY);
  glDeleteBuffers(1, &vbo);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glDrawElements out of an element-array buffer, where `indices` is an offset too - and where
 * offset zero is again the NULL that means something. */
static void test_gl_element_array_buffer_draws_from_its_offsets(void) {
  enum { W = 64, H = 64 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glColor3f(0.0f, 1.0f, 0.0f);

  /* Four corners, and two index sets picking different triangles out of them. */
  static const GLfloat quad[12] = {
    -0.8f, -0.7f, 0.0f,   0.7f, -0.7f, 0.0f,   0.7f, 0.8f, 0.0f,  -0.8f, 0.8f, 0.0f,
  };
  static const GLushort idx[6] = {0, 1, 2,  0, 2, 3};

  static uint32_t want_first[W * H], want_second[W * H];

  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, quad);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
  memcpy(want_first, fb, sizeof want_first);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx + 3);
  memcpy(want_second, fb, sizeof want_second);
  ASSERT_TRUE(memcmp(want_first, want_second, sizeof want_first) != 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  GLuint ibo = 0;
  glGenBuffers(1, &ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)sizeof idx, idx, GL_STATIC_DRAW);

  /* Offset zero, arriving as NULL and meaning the first index. */
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, NULL);
  ASSERT_EQ(memcmp(fb, want_first, sizeof want_first), 0);

  /* Three indices in: a byte offset, not an index count. */
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT,
                 (const GLvoid *)(uintptr_t)(3 * sizeof(GLushort)));
  ASSERT_EQ(memcmp(fb, want_second, sizeof want_second), 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The two binding points are independent: an array binding is not an element binding. */
  GLint iv[1];
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, iv);
  ASSERT_EQ(iv[0], (GLint)ibo);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, iv);
  ASSERT_EQ(iv[0], 0);

  /* Bound to a buffer with no storage: refused rather than read from an address off NULL. */
  GLuint empty = 0;
  glGenBuffers(1, &empty);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, empty);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, NULL);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* And the client-memory path still works once the binding is dropped. */
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
  ASSERT_EQ(memcmp(fb, want_first, sizeof want_first), 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glDisableClientState(GL_VERTEX_ARRAY);
  glDeleteBuffers(1, &ibo);
  glDeleteBuffers(1, &empty);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **glArrayElement is the bridge between the two ways of feeding geometry**, so the thing to
 * check is that it really reads the arrays rather than the current attributes - and that a
 * *disabled* array still falls back to the current one, which is the half that is easy to lose.
 */
static void test_gl_array_element_pulls_from_the_enabled_arrays(void) {
  enum { W = 64, H = 64 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  uint32_t *fb = oops_display_get_framebuffer(disp);
  (void)glGetError();

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  /* Four corners; the indices pick three of them in an order that is not 0,1,2. */
  static const GLfloat verts[12] = {
    -0.8f, -0.7f, 0.0f,   0.7f, -0.7f, 0.0f,   0.7f, 0.8f, 0.0f,  -0.8f, 0.8f, 0.0f,
  };
  static const GLfloat cols[16] = {
    1.0f, 0.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 1.0f, 1.0f,   1.0f, 1.0f, 0.0f, 1.0f,
  };

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_COLOR_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, verts);
  glColorPointer(4, GL_FLOAT, 0, cols);

  /* The reference: the same three corners through glDrawElements. */
  static const GLushort idx[3] = {0, 2, 3};
  static uint32_t want[W * H];
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
  memcpy(want, fb, sizeof want);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The same picture assembled by hand. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_TRIANGLES);
  glArrayElement(0);
  glArrayElement(2);
  glArrayElement(3);
  ASSERT_EQ(ctx->imm_count, 3);
  glEnd();
  ASSERT_EQ(memcmp(fb, want, sizeof want), 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* It really read the vertex array: element 1 is a different corner, so swapping one index
   * must change the picture. Without this the comparison above passes against an
   * implementation that ignored the index entirely. */
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_TRIANGLES);
  glArrayElement(0);
  glArrayElement(1);
  glArrayElement(3);
  glEnd();
  ASSERT_TRUE(memcmp(fb, want, sizeof want) != 0);

  /* **A disabled array falls back to the current attribute.** With the colour array off, every
   * vertex takes the current colour - so this is a flat triangle, not the interpolated one. */
  glDisableClientState(GL_COLOR_ARRAY);
  glColor3f(0.0f, 1.0f, 1.0f);
  glBegin(GL_TRIANGLES);
  glArrayElement(0);
  glEnd();
  const gl_vertex_t *v = ctx->imm_verts;
  ASSERT_TRUE(v[0].x == -0.8f && v[0].y == -0.7f);
  ASSERT_TRUE(v[0].r == 0.0f && v[0].g == 1.0f && v[0].b == 1.0f);
  glEnableClientState(GL_COLOR_ARRAY);

  /* Outside glBegin/glEnd there is nowhere to put the vertex; doing nothing is the reading
   * that cannot corrupt anything. A negative index is an error. */
  glArrayElement(0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glBegin(GL_TRIANGLES);
  glArrayElement(-1);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
  ASSERT_EQ(ctx->imm_count, 0);
  glEnd();

  /* glDrawRangeElements is glDrawElements with a promise, and the promise is checked only
   * where the specification requires it. */
  glClear(GL_COLOR_BUFFER_BIT);
  glDrawRangeElements(GL_TRIANGLES, 0, 3, 3, GL_UNSIGNED_SHORT, idx);
  ASSERT_EQ(memcmp(fb, want, sizeof want), 0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glDrawRangeElements(GL_TRIANGLES, 5, 2, 3, GL_UNSIGNED_SHORT, idx);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  glDisableClientState(GL_VERTEX_ARRAY);
  glDisableClientState(GL_COLOR_ARRAY);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **An integer colour is not an integer cast.**
 *
 * glLightiv maps a colour component across the whole signed range onto [-1, 1], so INT_MAX
 * means 1.0 - while a position or an attenuation is an ordinary cast. Casting a colour instead
 * would make every integer colour astronomically out of range and light the scene pure white,
 * which looks like a lighting bug anywhere but here. The rule is Mesa's INT_TO_FLOAT.
 */
static void test_gl_integer_lighting_forms_convert_colours_by_range(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  GLfloat fv[4];

  /* A colour: the extremes of the integer range are the extremes of [-1, 1]. */
  const GLint white[4] = {2147483647, 2147483647, 2147483647, 2147483647};
  glLightiv(GL_LIGHT1, GL_DIFFUSE, white);
  glGetLightfv(GL_LIGHT1, GL_DIFFUSE, fv);
  ASSERT_TRUE(fv[0] > 0.9999f && fv[0] <= 1.0f);
  ASSERT_TRUE(fv[3] > 0.9999f && fv[3] <= 1.0f);

  const GLint black[4] = {0, 0, 0, 0};
  glLightiv(GL_LIGHT1, GL_DIFFUSE, black);
  glGetLightfv(GL_LIGHT1, GL_DIFFUSE, fv);
  ASSERT_TRUE(fv[0] > -0.0001f && fv[0] < 0.0001f);

  /* **A position is a plain cast**, so 3 means 3.0 and not something near zero. */
  const GLint pos[4] = {3, 4, 5, 1};
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  glLightiv(GL_LIGHT1, GL_POSITION, pos);
  glGetLightfv(GL_LIGHT1, GL_POSITION, fv);
  ASSERT_TRUE(fv[0] == 3.0f && fv[1] == 4.0f && fv[2] == 5.0f && fv[3] == 1.0f);
  glPopMatrix();

  /* And a scalar. */
  glLighti(GL_LIGHT1, GL_SPOT_EXPONENT, 7);
  glGetLightfv(GL_LIGHT1, GL_SPOT_EXPONENT, fv);
  ASSERT_TRUE(fv[0] == 7.0f);

  /* Materials follow the same split, with GL_SHININESS on the cast side. */
  const GLint emis[4] = {2147483647, 0, 0, 2147483647};
  glMaterialiv(GL_FRONT, GL_EMISSION, emis);
  glGetMaterialfv(GL_FRONT, GL_EMISSION, fv);
  ASSERT_TRUE(fv[0] > 0.9999f && fv[1] < 0.0001f);
  glMateriali(GL_FRONT, GL_SHININESS, 42);
  glGetMaterialfv(GL_FRONT, GL_SHININESS, fv);
  ASSERT_TRUE(fv[0] == 42.0f);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **Transposing is not reversing.** `out[c*4+r] = in[r*4+c]`; walking the input backwards is a
 * different operation that happens to agree on a symmetric matrix, so the fixture here is
 * deliberately asymmetric in both directions. */
static void test_gl_transpose_matrix_forms_transpose(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  glMatrixMode(GL_MODELVIEW);

  GLfloat m[16], t[16], reversed[16];
  for (int i = 0; i < 16; i++) m[i] = (GLfloat)(i + 1);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) t[c * 4 + r] = m[r * 4 + c];
  }
  for (int i = 0; i < 16; i++) reversed[i] = m[15 - i];

  glLoadTransposeMatrixf(m);
  gl_mat4_t got = ctx->modelview_stack[ctx->modelview_depth];
  glLoadMatrixf(t);
  ASSERT_EQ(memcmp(&got, &ctx->modelview_stack[ctx->modelview_depth], sizeof got), 0);

  /* Not the same as reversing, and not the same as not transposing at all. */
  glLoadMatrixf(reversed);
  ASSERT_TRUE(memcmp(&got, &ctx->modelview_stack[ctx->modelview_depth], sizeof got) != 0);
  glLoadMatrixf(m);
  ASSERT_TRUE(memcmp(&got, &ctx->modelview_stack[ctx->modelview_depth], sizeof got) != 0);

  /* The multiplying form, and the double spellings, reach the same place. */
  glLoadIdentity();
  glMultTransposeMatrixf(m);
  gl_mat4_t mult_f = ctx->modelview_stack[ctx->modelview_depth];
  glLoadIdentity();
  glMultMatrixf(t);
  ASSERT_EQ(memcmp(&mult_f, &ctx->modelview_stack[ctx->modelview_depth], sizeof mult_f), 0);

  GLdouble md[16];
  for (int i = 0; i < 16; i++) md[i] = (GLdouble)m[i];
  glLoadTransposeMatrixd(md);
  ASSERT_EQ(memcmp(&got, &ctx->modelview_stack[ctx->modelview_depth], sizeof got), 0);

  glLoadTransposeMatrixf(NULL);
  glMultTransposeMatrixd(NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Residency: everything that exists is resident, and a name that does not exist is an error
 * rather than a "no" - a program asking about a texture it does not own has a bug. */
static void test_gl_texture_residency_is_honest(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  GLuint tex[2] = {0, 0};
  glGenTextures(2, tex);
  glBindTexture(GL_TEXTURE_2D, tex[0]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  GLboolean res[3] = {0, 0, 42};
  ASSERT_EQ(glAreTexturesResident(2, tex, res), GL_TRUE);
  ASSERT_EQ(res[0], GL_TRUE);
  ASSERT_EQ(res[1], GL_TRUE);
  ASSERT_EQ(res[2], 42); /* not written past the count */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  const GLuint bogus[1] = {9999u};
  ASSERT_EQ(glAreTexturesResident(1, bogus, res), GL_FALSE);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* The priority hint is ignored, but the names are still checked. */
  const GLclampf pri[2] = {0.5f, 1.0f};
  glPrioritizeTextures(2, tex, pri);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glPrioritizeTextures(1, bogus, pri);
  ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

  /* The vector parameter forms reach the scalar ones. */
  const GLint wrap = GL_CLAMP_TO_EDGE;
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap);
  GLint back = 0;
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &back);
  ASSERT_EQ(back, (GLint)GL_CLAMP_TO_EDGE);
  const GLfloat filt = (GLfloat)GL_NEAREST;
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filt);
  glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &back);
  ASSERT_EQ(back, (GLint)GL_NEAREST);
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, NULL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glDeleteTextures(2, tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The texture-environment completions, the hint, and the buffer selectors - three small families
 * whose common thread is **refusing what does not exist rather than accepting it quietly**.
 *
 * The bug this starts from is real: glTexEnvfv returned clean having done nothing for a target
 * or pname it did not keep, while its own sibling glTexEnvi refused the same arguments. Two
 * spellings of one call disagreeing about what is an error is worse than either answer.
 */
static void test_gl_tex_env_hint_and_buffer_selection_refuse_what_is_absent(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  (void)glGetError();

  GLfloat fv[5];
  GLint iv[5];
  const GLint GUARD = 0x5eed;

  /* The environment round-trips through every spelling. */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  iv[1] = GUARD;
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_REPLACE);
  ASSERT_EQ(iv[1], GUARD);

  const GLfloat col[4] = {0.125f, 0.25f, 0.5f, 0.75f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, col);
  fv[4] = -1.0f;
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, fv);
  ASSERT_TRUE(fv[0] == 0.125f && fv[3] == 0.75f && fv[4] == -1.0f);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **An integer environment colour converts by range, like a light's.** INT_MAX is 1.0, not
   * 2147483647.0 - a cast would put the colour astronomically out of range. */
  const GLint icol[4] = {2147483647, 0, 2147483647, 0};
  glTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, icol);
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, fv);
  ASSERT_TRUE(fv[0] > 0.9999f && fv[0] <= 1.0f);
  ASSERT_TRUE(fv[1] > -0.0001f && fv[1] < 0.0001f);

  /* The integer scalar form still goes through the scalar path. */
  glTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (const GLint[]){GL_MODULATE});
  glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, iv);
  ASSERT_EQ(iv[0], (GLint)GL_MODULATE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* **The vector form refuses what the scalar form refuses.** This is the regression: it used
   * to return clean. */
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_WRAP_S, col);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glTexEnvfv(0x2301u /* GL_TEXTURE_FILTER_CONTROL */, GL_TEXTURE_ENV_MODE, col);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_WRAP_S, fv);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

  /* The hints: each target is kept and reported. The fog and line-smooth hints were refused
   * here until 2026-09-19, while this drew no fog and no lines; it draws both, and a hint is a
   * preference an implementation may ignore but not reject. */
  glGetIntegerv(GL_PERSPECTIVE_CORRECTION_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_DONT_CARE); /* the specification's default */
  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
  iv[1] = GUARD;
  glGetIntegerv(GL_PERSPECTIVE_CORRECTION_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NICEST);
  ASSERT_EQ(iv[1], GUARD);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glHint(GL_FOG_HINT, GL_NICEST);
  glHint(GL_LINE_SMOOTH_HINT, GL_FASTEST);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetIntegerv(GL_FOG_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NICEST);
  glGetIntegerv(GL_LINE_SMOOTH_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_FASTEST);
  /* A target that is no hint is still refused. */
  glHint(GL_TEXTURE_2D, GL_NICEST);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* A mode that is not a hint mode is refused before the target is even considered. */
  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_TRIANGLES);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_PERSPECTIVE_CORRECTION_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NICEST); /* unchanged by the refusals */

  /* GL_BACK names the back buffer. The right buffers name buffers this mono visual does not
   * have, so they are GL_INVALID_OPERATION, the refusal of a buffer that cannot be used
   * (GL_INVALID_ENUM until 2026-09-19). The front was refused the same way until that
   * evening, when it became a surface of its own - test_gl_front_buffer. */
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK);
  glGetIntegerv(GL_READ_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK);

  glDrawBuffer(GL_FRONT_RIGHT);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glReadBuffer(GL_BACK_RIGHT);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK);
  glDrawBuffer(GL_FRONT);
  glReadBuffer(GL_FRONT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* -------------------------------------------------------------------------
 * The GLSL lexer (GL 2.0, first stage)
 * ------------------------------------------------------------------------- */

/* Lexes `src` and returns how many tokens came out, writing them into `toks`. The EOF token is
 * not counted, and an error token is. */
static int glsl_lex_all(const char *src, glsl_token_t *toks, int max) {
  glsl_lexer_t lx;
  glsl_lexer_init(&lx, src, strlen(src));
  int n = 0;
  glsl_token_t t;
  while (glsl_lex_next(&lx, &t)) {
    if (n < max) toks[n] = t;
    n++;
  }
  /* The call that returned false still wrote a token - EOF, or the error that stopped it. */
  if (t.type == GLSL_TOK_ERROR && n < max) toks[n++] = t;
  return n;
}

/* **Maximal munch, and keyword matching that is whole-word.**
 *
 * These are the two things a hand-written lexer gets wrong. `>=` must be one token, not `>`
 * then `=`; and `floatx` must be an identifier, not the keyword `float` followed by `x`. The
 * second is the nastier of the two because the program still lexes - it just produces a syntax
 * error pointing at something that was never wrong.
 */
static void test_glsl_lexer_munches_maximally_and_matches_whole_words(void) {
  glsl_token_t t[32];

  /* Every operator that is a prefix of a longer one, next to its longer form. */
  int n = glsl_lex_all(">= > <= < == = != ! ++ + -- - += -= *= /= && || ^^", t, 32);
  ASSERT_EQ(n, 19);
  ASSERT_EQ(t[0].type, GLSL_TOK_GE);
  ASSERT_EQ(t[1].type, GLSL_TOK_GT);
  ASSERT_EQ(t[2].type, GLSL_TOK_LE);
  ASSERT_EQ(t[3].type, GLSL_TOK_LT);
  ASSERT_EQ(t[4].type, GLSL_TOK_EQ);
  ASSERT_EQ(t[5].type, GLSL_TOK_ASSIGN);
  ASSERT_EQ(t[6].type, GLSL_TOK_NE);
  ASSERT_EQ(t[7].type, GLSL_TOK_BANG);
  ASSERT_EQ(t[8].type, GLSL_TOK_INC);
  ASSERT_EQ(t[9].type, GLSL_TOK_PLUS);
  ASSERT_EQ(t[10].type, GLSL_TOK_DEC);
  ASSERT_EQ(t[11].type, GLSL_TOK_MINUS);
  ASSERT_EQ(t[12].type, GLSL_TOK_ADD_ASSIGN);
  ASSERT_EQ(t[13].type, GLSL_TOK_SUB_ASSIGN);
  ASSERT_EQ(t[14].type, GLSL_TOK_MUL_ASSIGN);
  ASSERT_EQ(t[15].type, GLSL_TOK_DIV_ASSIGN);
  ASSERT_EQ(t[16].type, GLSL_TOK_AND_AND);
  ASSERT_EQ(t[17].type, GLSL_TOK_OR_OR);
  ASSERT_EQ(t[18].type, GLSL_TOK_XOR_XOR);

  /* `++` with no space is one token; separated it is two. A lexer that did not munch
   * maximally would give the same answer for both. */
  n = glsl_lex_all("++", t, 32);
  ASSERT_EQ(n, 1);
  ASSERT_EQ(t[0].type, GLSL_TOK_INC);
  n = glsl_lex_all("+ +", t, 32);
  ASSERT_EQ(n, 2);
  ASSERT_EQ(t[0].type, GLSL_TOK_PLUS);
  ASSERT_EQ(t[1].type, GLSL_TOK_PLUS);

  /* **Whole-word keywords.** Each of these has a keyword as a strict prefix. */
  n = glsl_lex_all("float floatx xfloat in inout input int2", t, 32);
  ASSERT_EQ(n, 7);
  ASSERT_EQ(t[0].type, GLSL_TOK_KW_FLOAT);
  ASSERT_EQ(t[1].type, GLSL_TOK_IDENTIFIER); /* floatx */
  ASSERT_EQ(t[2].type, GLSL_TOK_IDENTIFIER); /* xfloat */
  ASSERT_EQ(t[3].type, GLSL_TOK_KW_IN);
  ASSERT_EQ(t[4].type, GLSL_TOK_KW_INOUT);   /* not `in` followed by `out` */
  ASSERT_EQ(t[5].type, GLSL_TOK_KW_RESERVED); /* `input`, reserved by 1.10 */
  ASSERT_EQ(t[6].type, GLSL_TOK_IDENTIFIER); /* int2 */
  ASSERT_EQ(t[1].length, 6u);
  ASSERT_TRUE(strncmp(t[1].text, "floatx", 6) == 0);

  /* A reserved word is named rather than passed through as an identifier, so a program using
   * one can be told which word it may not use. */
  n = glsl_lex_all("goto switch double", t, 32);
  ASSERT_EQ(n, 3);
  ASSERT_EQ(t[0].type, GLSL_TOK_KW_RESERVED);
  ASSERT_EQ(t[1].type, GLSL_TOK_KW_RESERVED);
  ASSERT_EQ(t[2].type, GLSL_TOK_KW_RESERVED);
}

/* Numbers, where GLSL's grammar is fussiest: the int-versus-float decision is made by what the
 * scan found, not by looking for a dot. */
static void test_glsl_lexer_reads_the_number_forms(void) {
  glsl_token_t t[16];

  int n = glsl_lex_all("1 017 0x1f 0X1F", t, 16);
  ASSERT_EQ(n, 4);
  ASSERT_EQ(t[0].type, GLSL_TOK_INTCONST);
  ASSERT_TRUE(t[0].value == 1.0);
  ASSERT_EQ(t[1].type, GLSL_TOK_INTCONST);
  ASSERT_EQ(t[2].type, GLSL_TOK_INTCONST);
  ASSERT_TRUE(t[2].value == 31.0);
  /* Case-folded: 0X1F is the same number as 0x1f. */
  ASSERT_TRUE(t[3].value == 31.0);

  /* **Floats without a dot, and floats with nothing after one.** `1e5` has no dot and is a
   * float; `1.` has nothing after the dot and is still a float. A lexer deciding on the
   * presence of a `.` gets both wrong. */
  n = glsl_lex_all("1.0 1. .5 1e5 1.5E-3 2e+2", t, 16);
  ASSERT_EQ(n, 6);
  for (int i = 0; i < 6; i++) ASSERT_EQ(t[i].type, GLSL_TOK_FLOATCONST);
  ASSERT_TRUE(t[0].value == 1.0);
  ASSERT_TRUE(t[1].value == 1.0);
  ASSERT_TRUE(t[2].value > 0.4999 && t[2].value < 0.5001);
  ASSERT_TRUE(t[3].value > 99999.0 && t[3].value < 100001.0);
  ASSERT_TRUE(t[4].value > 0.00149 && t[4].value < 0.00151);
  ASSERT_TRUE(t[5].value > 199.9 && t[5].value < 200.1);

  /* A bare `.` is the selector operator, not a number. */
  n = glsl_lex_all("v.x", t, 16);
  ASSERT_EQ(n, 3);
  ASSERT_EQ(t[0].type, GLSL_TOK_IDENTIFIER);
  ASSERT_EQ(t[1].type, GLSL_TOK_DOT);
  ASSERT_EQ(t[2].type, GLSL_TOK_IDENTIFIER);

  /* An `e` that cannot start an exponent is not consumed as one: `1einvalid` is the number 1
   * followed by an identifier, which is then refused as juxtaposition rather than becoming a
   * confusing exponent error. */
  n = glsl_lex_all("123abc", t, 16);
  ASSERT_TRUE(n >= 1);
  ASSERT_EQ(t[0].type, GLSL_TOK_ERROR);
  ASSERT_TRUE(t[0].error != NULL);

  n = glsl_lex_all("0x", t, 16);
  ASSERT_TRUE(n >= 1);
  ASSERT_EQ(t[0].type, GLSL_TOK_ERROR);
}

/* **Line and column survive comments**, which is the whole reason trivia is skipped through the
 * same advance() that counts newlines. A diagnostic after a twenty-line block comment naming
 * line 1 is worse than no diagnostic. */
static void test_glsl_lexer_keeps_position_through_trivia(void) {
  glsl_token_t t[16];

  int n = glsl_lex_all("a\nb\n\nc", t, 16);
  ASSERT_EQ(n, 3);
  ASSERT_EQ(t[0].line, 1);
  ASSERT_EQ(t[0].column, 1);
  ASSERT_EQ(t[1].line, 2);
  ASSERT_EQ(t[2].line, 4);

  /* A block comment spanning lines moves the line counter with it. */
  n = glsl_lex_all("a /* one\ntwo\nthree */ b", t, 16);
  ASSERT_EQ(n, 2);
  ASSERT_EQ(t[0].line, 1);
  ASSERT_EQ(t[1].line, 3);
  ASSERT_TRUE(t[1].column > 1);

  /* A line comment ends at the newline and no further. */
  n = glsl_lex_all("a // b c d\ne", t, 16);
  ASSERT_EQ(n, 2);
  ASSERT_EQ(t[0].type, GLSL_TOK_IDENTIFIER);
  ASSERT_EQ(t[1].line, 2);
  ASSERT_TRUE(strncmp(t[1].text, "e", 1) == 0);

  /* The token's position is its own, not that of the whitespace in front of it. */
  n = glsl_lex_all("    x", t, 16);
  ASSERT_EQ(n, 1);
  ASSERT_EQ(t[0].column, 5);

  /* **An unterminated block comment is refused**, not treated as running to end of file -
   * otherwise it silently swallows the program and the parser blames the last line. */
  n = glsl_lex_all("a /* never closed", t, 16);
  ASSERT_EQ(n, 2);
  ASSERT_EQ(t[0].type, GLSL_TOK_IDENTIFIER);
  ASSERT_EQ(t[1].type, GLSL_TOK_ERROR);
  ASSERT_TRUE(t[1].error != NULL);
}

/* A whole small shader, to show the pieces work together rather than only in isolation. */
static void test_glsl_lexer_reads_a_small_shader(void) {
  static const char *src =
      "#version 110\n"
      "uniform mat4 mvp;\n"
      "attribute vec3 pos;\n"
      "varying vec2 uv;\n"
      "void main() {\n"
      "  gl_Position = mvp * vec4(pos, 1.0);\n"
      "  uv = pos.xy * 0.5 + 0.5; // to texture space\n"
      "}\n";

  glsl_lexer_t lx;
  glsl_lexer_init(&lx, src, strlen(src));
  glsl_token_t t;
  int count = 0, idents = 0, errors = 0;
  GLboolean saw_hash = GL_FALSE, saw_uniform = GL_FALSE, saw_varying = GL_FALSE;
  GLboolean saw_mat4 = GL_FALSE, saw_vec4 = GL_FALSE;

  while (glsl_lex_next(&lx, &t)) {
    count++;
    if (t.type == GLSL_TOK_ERROR) errors++;
    if (t.type == GLSL_TOK_IDENTIFIER) idents++;
    if (t.type == GLSL_TOK_HASH) saw_hash = GL_TRUE;
    if (t.type == GLSL_TOK_KW_UNIFORM) saw_uniform = GL_TRUE;
    if (t.type == GLSL_TOK_KW_VARYING) saw_varying = GL_TRUE;
    if (t.type == GLSL_TOK_KW_MAT4) saw_mat4 = GL_TRUE;
    if (t.type == GLSL_TOK_KW_VEC4) saw_vec4 = GL_TRUE;
  }

  ASSERT_EQ(errors, 0);
  ASSERT_TRUE(saw_hash && saw_uniform && saw_varying && saw_mat4 && saw_vec4);
  /* `gl_Position` is an identifier here - the built-ins are the semantic stage's business, and
   * a lexer that special-cased them would be deciding something it cannot see. */
  ASSERT_TRUE(idents >= 6);
  ASSERT_TRUE(count > 40);

  /* Empty and null sources terminate rather than running off anything. */
  glsl_lexer_init(&lx, "", 0);
  ASSERT_EQ(glsl_lex_next(&lx, &t), GL_FALSE);
  ASSERT_EQ(t.type, GLSL_TOK_EOF);
  glsl_lexer_init(&lx, NULL, 0);
  ASSERT_EQ(glsl_lex_next(&lx, &t), GL_FALSE);
  ASSERT_EQ(t.type, GLSL_TOK_EOF);
}

/* -------------------------------------------------------------------------
 * The GLSL expression parser
 * ------------------------------------------------------------------------- */

/* Renders a tree **fully parenthesised**, so precedence and associativity are readable in the
 * expected string rather than inferred from node counts. `1+2*3` must print as `(1+(2*3))`, and
 * an implementation that got the precedence backwards prints `((1+2)*3)` - which is the whole
 * assertion, visible at a glance. */
static const char *glsl_op_text(glsl_token_type_t op) {
  switch (op) {
    case GLSL_TOK_PLUS: return "+";        case GLSL_TOK_MINUS: return "-";
    case GLSL_TOK_STAR: return "*";        case GLSL_TOK_SLASH: return "/";
    case GLSL_TOK_PERCENT: return "%";     case GLSL_TOK_BANG: return "!";
    case GLSL_TOK_TILDE: return "~";       case GLSL_TOK_LT: return "<";
    case GLSL_TOK_GT: return ">";          case GLSL_TOK_LE: return "<=";
    case GLSL_TOK_GE: return ">=";         case GLSL_TOK_EQ: return "==";
    case GLSL_TOK_NE: return "!=";         case GLSL_TOK_AND_AND: return "&&";
    case GLSL_TOK_OR_OR: return "||";      case GLSL_TOK_XOR_XOR: return "^^";
    case GLSL_TOK_AMP: return "&";         case GLSL_TOK_PIPE: return "|";
    case GLSL_TOK_CARET: return "^";       case GLSL_TOK_INC: return "++";
    case GLSL_TOK_DEC: return "--";        case GLSL_TOK_ASSIGN: return "=";
    case GLSL_TOK_ADD_ASSIGN: return "+="; case GLSL_TOK_SUB_ASSIGN: return "-=";
    case GLSL_TOK_MUL_ASSIGN: return "*="; case GLSL_TOK_DIV_ASSIGN: return "/=";
    default: return "?";
  }
}

static void glsl_print(const glsl_ast_t *ast, int32_t at, char *buf, size_t cap, size_t *len) {
  if (at == GLSL_NO_NODE || *len + 1 >= cap) return;
  const glsl_node_t *n = &ast->nodes[at];
  #define PUT(s) do { const char *_s = (s); \
      while (*_s && *len + 1 < cap) buf[(*len)++] = *_s++; buf[*len] = '\0'; } while (0)

  switch (n->kind) {
    case GLSL_NODE_IDENTIFIER:
    case GLSL_NODE_INTCONST:
    case GLSL_NODE_FLOATCONST:
      for (size_t i = 0; i < n->length && *len + 1 < cap; i++) buf[(*len)++] = n->text[i];
      buf[*len] = '\0';
      break;
    case GLSL_NODE_BOOLCONST: PUT(n->value != 0.0 ? "true" : "false"); break;
    case GLSL_NODE_UNARY:
      PUT("("); PUT(glsl_op_text(n->op)); glsl_print(ast, n->a, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_POSTFIX:
      PUT("("); glsl_print(ast, n->a, buf, cap, len); PUT(glsl_op_text(n->op)); PUT(")");
      break;
    case GLSL_NODE_BINARY:
    case GLSL_NODE_ASSIGN:
      PUT("("); glsl_print(ast, n->a, buf, cap, len);
      PUT(glsl_op_text(n->op));
      glsl_print(ast, n->b, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_SEQUENCE:
      PUT("("); glsl_print(ast, n->a, buf, cap, len); PUT(",");
      glsl_print(ast, n->b, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_CONDITIONAL:
      PUT("("); glsl_print(ast, n->a, buf, cap, len); PUT("?");
      glsl_print(ast, n->b, buf, cap, len); PUT(":");
      glsl_print(ast, n->c, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_INDEX:
      PUT("("); glsl_print(ast, n->a, buf, cap, len); PUT("[");
      glsl_print(ast, n->b, buf, cap, len); PUT("])");
      break;
    case GLSL_NODE_FIELD:
      PUT("("); glsl_print(ast, n->a, buf, cap, len); PUT(".");
      for (size_t i = 0; i < n->length && *len + 1 < cap; i++) buf[(*len)++] = n->text[i];
      buf[*len] = '\0';
      PUT(")");
      break;
    case GLSL_NODE_CALL: {
      PUT("("); glsl_print(ast, n->a, buf, cap, len); PUT("(");
      for (int32_t arg = n->b; arg != GLSL_NO_NODE; arg = ast->nodes[arg].sibling) {
        if (arg != n->b) PUT(",");
        glsl_print(ast, arg, buf, cap, len);
      }
      PUT("))");
      break;
    }

    /* Statements and declarations. Printed as s-expressions, which keeps the nesting explicit
     * - an `else` attached to the wrong `if` is visible in the string. */
    case GLSL_NODE_COMPOUND:
      PUT("{");
      for (int32_t s = n->a; s != GLSL_NODE_UNIT && s != GLSL_NO_NODE; s = ast->nodes[s].sibling) {
        glsl_print(ast, s, buf, cap, len);
        if (ast->nodes[s].sibling != GLSL_NO_NODE) PUT(" ");
      }
      PUT("}");
      break;
    case GLSL_NODE_IF:
      PUT("(if "); glsl_print(ast, n->a, buf, cap, len);
      PUT(" "); glsl_print(ast, n->b, buf, cap, len);
      if (n->c != GLSL_NO_NODE) { PUT(" else "); glsl_print(ast, n->c, buf, cap, len); }
      PUT(")");
      break;
    case GLSL_NODE_WHILE:
      PUT("(while "); glsl_print(ast, n->a, buf, cap, len);
      PUT(" "); glsl_print(ast, n->b, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_DO_WHILE:
      PUT("(do "); glsl_print(ast, n->a, buf, cap, len);
      PUT(" while "); glsl_print(ast, n->b, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_FOR:
      PUT("(for ");
      if (n->a != GLSL_NO_NODE) glsl_print(ast, n->a, buf, cap, len); else PUT(";");
      PUT(" ");
      if (n->b != GLSL_NO_NODE) glsl_print(ast, n->b, buf, cap, len); else PUT("-");
      PUT(" ");
      if (n->c != GLSL_NO_NODE) glsl_print(ast, n->c, buf, cap, len); else PUT("-");
      PUT(" "); glsl_print(ast, n->d, buf, cap, len); PUT(")");
      break;
    case GLSL_NODE_RETURN:
      PUT("(return");
      if (n->a != GLSL_NO_NODE) { PUT(" "); glsl_print(ast, n->a, buf, cap, len); }
      PUT(")");
      break;
    case GLSL_NODE_BREAK: PUT("(break)"); break;
    case GLSL_NODE_CONTINUE: PUT("(continue)"); break;
    case GLSL_NODE_DISCARD: PUT("(discard)"); break;
    case GLSL_NODE_EXPR_STMT:
      if (n->a == GLSL_NO_NODE) { PUT(";"); }
      else { glsl_print(ast, n->a, buf, cap, len); PUT(";"); }
      break;
    case GLSL_NODE_DECL:
      PUT("(decl ");
      if (n->qualifier != GLSL_TOK_EOF) { PUT("q "); }
      for (size_t i = 0; i < n->length && *len + 1 < cap; i++) buf[(*len)++] = n->text[i];
      buf[*len] = '\0';
      if (n->array_size != GLSL_NO_NODE) {
        PUT("["); glsl_print(ast, n->array_size, buf, cap, len); PUT("]");
      }
      if (n->a != GLSL_NO_NODE) { PUT("="); glsl_print(ast, n->a, buf, cap, len); }
      PUT(")");
      break;
    case GLSL_NODE_PARAM:
      PUT("(param ");
      for (size_t i = 0; i < n->length && *len + 1 < cap; i++) buf[(*len)++] = n->text[i];
      buf[*len] = '\0';
      PUT(")");
      break;
    case GLSL_NODE_FUNCTION:
      PUT("(fn ");
      for (size_t i = 0; i < n->length && *len + 1 < cap; i++) buf[(*len)++] = n->text[i];
      buf[*len] = '\0';
      PUT("(");
      for (int32_t a = n->b; a != GLSL_NO_NODE; a = ast->nodes[a].sibling) {
        if (a != n->b) PUT(",");
        glsl_print(ast, a, buf, cap, len);
      }
      PUT(")");
      /* A prototype has no body at all, which reads differently from an empty one. */
      if (n->c == GLSL_NO_NODE) PUT(" proto");
      else { PUT(" "); glsl_print(ast, n->c, buf, cap, len); }
      PUT(")");
      break;
    case GLSL_NODE_UNIT:
      PUT("(unit ");
      for (int32_t d = n->a; d != GLSL_NO_NODE; d = ast->nodes[d].sibling) {
        if (d != n->a) PUT(" ");
        glsl_print(ast, d, buf, cap, len);
      }
      PUT(")");
      break;
  }
  #undef PUT
}

static glsl_ast_t g_glsl_ast;

/* Parses `src` and renders it. Returns NULL on a parse error, with `*err` set. */
static const char *glsl_parse_to_string(const char *src, const char **err) {
  static char buf[512];
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, src, strlen(src));
  int32_t root = glsl_parse_expression(&p);
  if (p.error || root == GLSL_NO_NODE) {
    if (err) *err = p.error ? p.error : "no expression";
    return NULL;
  }
  if (err) *err = NULL;
  size_t len = 0;
  buf[0] = '\0';
  glsl_print(&g_glsl_ast, root, buf, sizeof buf, &len);
  return buf;
}

/* **Precedence, one level against the next.** Every adjacent pair in the ladder is checked in
 * the direction that would be wrong if the levels were swapped or one were missing - a missing
 * level does not raise an error, it silently reassociates everything around it. */
static void test_glsl_parser_binds_by_precedence(void) {
  const char *err;
  const char *s;

  s = glsl_parse_to_string("1+2*3", &err);
  ASSERT_TRUE(s != NULL);
  ASSERT_TRUE(strcmp(s, "(1+(2*3))") == 0);

  s = glsl_parse_to_string("1*2+3", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((1*2)+3)") == 0);

  /* additive tighter than relational, relational than equality */
  s = glsl_parse_to_string("a+b<c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a+b)<c)") == 0);
  s = glsl_parse_to_string("a<b==c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a<b)==c)") == 0);

  /* equality tighter than &, & than ^, ^ than |, | than &&, && than ^^, ^^ than || */
  s = glsl_parse_to_string("a==b&c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a==b)&c)") == 0);
  s = glsl_parse_to_string("a&b^c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a&b)^c)") == 0);
  s = glsl_parse_to_string("a^b|c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a^b)|c)") == 0);
  s = glsl_parse_to_string("a|b&&c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a|b)&&c)") == 0);
  s = glsl_parse_to_string("a&&b^^c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a&&b)^^c)") == 0);
  s = glsl_parse_to_string("a^^b||c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a^^b)||c)") == 0);

  /* || tighter than ?:, ?: tighter than assignment, assignment than comma */
  s = glsl_parse_to_string("a||b?c:d", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a||b)?c:d)") == 0);
  s = glsl_parse_to_string("a=b?c:d", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(a=(b?c:d))") == 0);
  s = glsl_parse_to_string("a=b,c=d", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a=b),(c=d))") == 0);

  /* unary tighter than multiplicative; postfix tighter than unary */
  s = glsl_parse_to_string("-a*b", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((-a)*b)") == 0);
  s = glsl_parse_to_string("!a==b", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((!a)==b)") == 0);

  /* Parentheses override, and leave no node of their own behind. */
  s = glsl_parse_to_string("(1+2)*3", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((1+2)*3)") == 0);
}

/* **Associativity. Getting this backwards still parses every program** - it just computes a
 * different answer, which is why it is asserted rather than assumed. */
static void test_glsl_parser_associates_correctly(void) {
  const char *err;
  const char *s;

  /* Left-associative: subtraction and division are the ones where it is observable. */
  s = glsl_parse_to_string("a-b-c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a-b)-c)") == 0);
  s = glsl_parse_to_string("a/b/c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a/b)/c)") == 0);
  s = glsl_parse_to_string("a,b,c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a,b),c)") == 0);

  /* Right-associative: assignment and the conditional. */
  s = glsl_parse_to_string("a=b=c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(a=(b=c))") == 0);
  s = glsl_parse_to_string("a+=b+=c", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(a+=(b+=c))") == 0);
  s = glsl_parse_to_string("a?b:c?d:e", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(a?b:(c?d:e))") == 0);

  /* Prefix unaries chain rightwards. */
  s = glsl_parse_to_string("!!a", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(!(!a))") == 0);
}

/* Postfix chains, which are where a real shader spends its syntax: `mvp * vec4(pos, 1.0)` and
 * `v.xy[0]++`. */
static void test_glsl_parser_reads_postfix_chains(void) {
  const char *err;
  const char *s;

  s = glsl_parse_to_string("v.x", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(v.x)") == 0);
  s = glsl_parse_to_string("a[i]", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(a[i])") == 0);
  s = glsl_parse_to_string("f()", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(f())") == 0);
  s = glsl_parse_to_string("f(a)", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(f(a))") == 0);

  /* **A comma inside an argument list separates arguments**; it is not the sequence operator.
   * Parsing the list with the full expression parser would make `f(a,b)` a one-argument call
   * whose argument is `(a,b)` - which type-checks differently and is very hard to see. */
  s = glsl_parse_to_string("f(a,b,c)", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(f(a,b,c))") == 0);
  s = glsl_parse_to_string("f((a,b))", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(f((a,b)))") == 0);

  /* Chaining left to right. */
  s = glsl_parse_to_string("a[i].x", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a[i]).x)") == 0);
  s = glsl_parse_to_string("a.x++", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((a.x)++)") == 0);
  s = glsl_parse_to_string("f(a)[i]", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "((f(a))[i])") == 0);

  /* A type name is a constructor here; telling types from functions is the semantic stage's
   * job and the grammar cannot do it. */
  s = glsl_parse_to_string("mvp*vec4(pos,1.0)", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(mvp*(vec4(pos,1.0)))") == 0);

  /* Prefix and postfix together: `-a++` is `-(a++)`, because postfix binds tighter. */
  s = glsl_parse_to_string("-a++", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(-(a++))") == 0);
}

/* Errors: refused with a reason and a position, rather than a tree that is wrong in a way
 * nothing downstream would notice. */
static void test_glsl_parser_refuses_malformed_input(void) {
  const char *err = NULL;

  ASSERT_TRUE(glsl_parse_to_string("1+", &err) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_parse_to_string("(1+2", &err) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_parse_to_string("f(a,", &err) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_parse_to_string("a[0", &err) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_parse_to_string("a?b", &err) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_parse_to_string("a.", &err) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_parse_to_string("", &err) == NULL);
  ASSERT_TRUE(err != NULL);

  /* A reserved word is refused by name rather than parsed as an identifier. */
  ASSERT_TRUE(glsl_parse_to_string("goto+1", &err) == NULL);
  ASSERT_TRUE(err != NULL);

  /* A lexer error becomes a parse error rather than being swallowed. */
  ASSERT_TRUE(glsl_parse_to_string("1+123abc", &err) == NULL);
  ASSERT_TRUE(err != NULL);

  /* The position travels with the error. */
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, "a +\n  *", 7);
  (void)glsl_parse_expression(&p);
  ASSERT_TRUE(p.error != NULL);
  ASSERT_EQ(p.error_line, 2);

  /* And a good parse afterwards is unaffected: the arena resets. */
  const char *s = glsl_parse_to_string("1+2", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(1+2)") == 0);
}

/* Parses a whole translation unit and renders it. */
static const char *glsl_unit_to_string(const char *src, const char **err) {
  static char buf[1024];
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, src, strlen(src));
  int32_t root = glsl_parse_translation_unit(&p);
  if (p.error || root == GLSL_NO_NODE) {
    if (err) *err = p.error ? p.error : "no unit";
    return NULL;
  }
  if (err) *err = NULL;
  size_t len = 0;
  buf[0] = '\0';
  glsl_print(&g_glsl_ast, root, buf, sizeof buf, &len);
  return buf;
}

/* Parses a single statement and renders it. */
static const char *glsl_stmt_to_string(const char *src, const char **err) {
  static char buf[512];
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, src, strlen(src));
  int32_t root = glsl_parse_statement(&p);
  if (p.error || root == GLSL_NO_NODE) {
    if (err) *err = p.error ? p.error : "no statement";
    return NULL;
  }
  if (err) *err = NULL;
  size_t len = 0;
  buf[0] = '\0';
  glsl_print(&g_glsl_ast, root, buf, sizeof buf, &len);
  return buf;
}

/* **The dangling else, which is the one thing every statement parser is asked about.**
 *
 * `if (a) if (b) x; else y;` - the `else` belongs to the *inner* `if`. Both readings parse the
 * whole program, and the wrong one silently runs `y` under conditions the author did not
 * write. Taking the `else` greedily as soon as the then-branch finishes is what produces the
 * right answer, and the rendered tree shows which happened.
 */
static void test_glsl_parser_binds_else_to_the_nearest_if(void) {
  const char *err;
  const char *s;

  s = glsl_stmt_to_string("if (a) if (b) x; else y;", &err);
  ASSERT_TRUE(s != NULL);
  ASSERT_TRUE(strcmp(s, "(if a (if b x; else y;))") == 0);

  /* Braces move it back out, which is the reason a program would write them. */
  s = glsl_stmt_to_string("if (a) { if (b) x; } else y;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(if a {(if b x;)} else y;)") == 0);

  /* An if with no else records none, rather than an empty statement. */
  s = glsl_stmt_to_string("if (a) x;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(if a x;)") == 0);
}

static void test_glsl_parser_reads_statements(void) {
  const char *err;
  const char *s;

  s = glsl_stmt_to_string("{ a; b; }", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "{a; b;}") == 0);
  s = glsl_stmt_to_string("{}", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "{}") == 0);
  s = glsl_stmt_to_string(";", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, ";") == 0);

  s = glsl_stmt_to_string("while (a<b) ++a;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(while (a<b) (++a);)") == 0);
  s = glsl_stmt_to_string("do x; while (a);", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(do x; while a)") == 0);

  /* The for header: init is a statement, so it can declare; the condition and step are
   * optional and record their absence. */
  s = glsl_stmt_to_string("for (int i = 0; i < n; ++i) x;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(for (decl i=0) (i<n) (++i) x;)") == 0);
  s = glsl_stmt_to_string("for (;;) x;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(for ; - - x;)") == 0);
  s = glsl_stmt_to_string("for (i=0; ; i++) x;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(for (i=0); - (i++) x;)") == 0);

  s = glsl_stmt_to_string("return;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(return)") == 0);
  s = glsl_stmt_to_string("return a+1;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(return (a+1))") == 0);
  s = glsl_stmt_to_string("discard;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(discard)") == 0);
  s = glsl_stmt_to_string("break;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(break)") == 0);
  s = glsl_stmt_to_string("continue;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(continue)") == 0);

  /* Refusals. */
  ASSERT_TRUE(glsl_stmt_to_string("{ a; ", &err) == NULL);
  ASSERT_TRUE(glsl_stmt_to_string("if a) x;", &err) == NULL);
  ASSERT_TRUE(glsl_stmt_to_string("a", &err) == NULL);        /* no semicolon */
  ASSERT_TRUE(glsl_stmt_to_string("do x; while (a)", &err) == NULL); /* no semicolon */
}

/* **Every declarator carries the shared type**, and a comma in a declaration separates
 * declarators rather than acting as the sequence operator. `float a = 1, b = 2;` is two
 * declarations; parsing the initialiser with the full expression parser makes it one, with `b`
 * swallowed into `a`'s initialiser. */
static void test_glsl_parser_reads_declarations(void) {
  const char *err;
  const char *s;

  s = glsl_unit_to_string("float a;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (decl a))") == 0);

  s = glsl_unit_to_string("float a, b, c;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (decl a) (decl b) (decl c))") == 0);

  s = glsl_unit_to_string("float a = 1.0, b = 2.0;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (decl a=1.0) (decl b=2.0))") == 0);

  /* **Three, not two.** The first declarator's initialiser is parsed at a different call site
   * from the rest, so two declarators cannot tell whether the *list* parser uses the comma
   * correctly - a mutation making it use the full expression parser passed the two-declarator
   * case unchanged. With three, `b`'s initialiser would swallow `, c = 3.0`. */
  s = glsl_unit_to_string("float a = 1.0, b = 2.0, c = 3.0;", &err);
  ASSERT_TRUE(s != NULL);
  ASSERT_TRUE(strcmp(s, "(unit (decl a=1.0) (decl b=2.0) (decl c=3.0))") == 0);

  s = glsl_unit_to_string("uniform mat4 mvp;", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (decl q mvp))") == 0);

  s = glsl_unit_to_string("vec4 v[4];", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (decl v[4]))") == 0);

  /* Functions: a prototype has no body at all, which reads differently from an empty one. */
  s = glsl_unit_to_string("void main() {}", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (fn main() {}))") == 0);

  s = glsl_unit_to_string("float f(float x);", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (fn f((param x)) proto))") == 0);

  /* `void` alone is no parameters, not a parameter named nothing. */
  s = glsl_unit_to_string("void main(void) {}", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (fn main() {}))") == 0);

  s = glsl_unit_to_string("float f(float x, int y) { return x; }", &err);
  ASSERT_TRUE(s != NULL && strcmp(s, "(unit (fn f((param x),(param y)) {(return x)}))") == 0);

  /* **A declarator list at the top level must not break the unit's own chain.** Both use
   * `sibling`, so the unit has to walk to the tail rather than assume the node it just got is
   * the last one - otherwise `float a, b;` followed by anything loses the rest of the file. */
  s = glsl_unit_to_string("float a, b; void main() {}", &err);
  ASSERT_TRUE(s != NULL);
  ASSERT_TRUE(strcmp(s, "(unit (decl a) (decl b) (fn main() {}))") == 0);

  /* Refusals. */
  ASSERT_TRUE(glsl_unit_to_string("float;", &err) == NULL);
  ASSERT_TRUE(glsl_unit_to_string("uniform;", &err) == NULL);
  ASSERT_TRUE(glsl_unit_to_string("void main() { ", &err) == NULL);
  /* The preprocessor is refused by name, not skipped - skipping would silently ignore a
   * `#version` and compile something the program did not write. */
  ASSERT_TRUE(glsl_unit_to_string("#version 110\nvoid main(){}", &err) == NULL);
  ASSERT_TRUE(err != NULL);
}

/* A whole small shader through the real entry point. */
static void test_glsl_parser_reads_a_whole_shader(void) {
  static const char *src =
      "uniform mat4 mvp;\n"
      "attribute vec3 pos;\n"
      "varying vec2 uv;\n"
      "void main() {\n"
      "  gl_Position = mvp * vec4(pos, 1.0);\n"
      "  uv = pos.xy * 0.5 + 0.5;\n"
      "}\n";

  const char *err = NULL;
  const char *s = glsl_unit_to_string(src, &err);
  ASSERT_TRUE(s != NULL);
  ASSERT_TRUE(err == NULL);
  ASSERT_TRUE(strstr(s, "(decl q mvp)") != NULL);
  ASSERT_TRUE(strstr(s, "(decl q pos)") != NULL);
  ASSERT_TRUE(strstr(s, "(decl q uv)") != NULL);
  ASSERT_TRUE(strstr(s, "(fn main()") != NULL);
  /* The precedence inside the body survived: `mvp * vec4(...)` and `a*b+c`. */
  ASSERT_TRUE(strstr(s, "(mvp*(vec4(pos,1.0)))") != NULL);
  ASSERT_TRUE(strstr(s, "(((pos.xy)*0.5)+0.5)") != NULL);
}

/* -------------------------------------------------------------------------
 * The GLSL preprocessor
 * ------------------------------------------------------------------------- */

/* Runs the preprocessor and joins the surviving tokens with single spaces, so what a test
 * asserts is exactly what the parser would have seen. */
static const char *glsl_pp_to_string(const char *src, const char **err, int *version) {
  static char buf[512];
  static glsl_pp_t pp;
  glsl_pp_init(&pp, src, strlen(src));
  size_t len = 0;
  buf[0] = '\0';
  glsl_token_t t;
  while (glsl_pp_next(&pp, &t)) {
    if (len && len + 1 < sizeof buf) buf[len++] = ' ';
    for (size_t i = 0; i < t.length && len + 1 < sizeof buf; i++) buf[len++] = t.text[i];
    buf[len] = '\0';
  }
  if (version) *version = pp.version;
  if (pp.error) {
    if (err) *err = pp.error;
    return NULL;
  }
  if (err) *err = NULL;
  return buf;
}

static void test_glsl_preprocessor_expands_and_records(void) {
  const char *err;
  int version = 0;
  const char *s;

  /* `#version` is recorded and does not reach the parser. */
  s = glsl_pp_to_string("#version 110\nfloat a;", &err, &version);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(version, 110);
  ASSERT_TRUE(strcmp(s, "float a ;") == 0);

  /* Object-like macros expand. */
  s = glsl_pp_to_string("#define N 4\nfloat a[N];", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "float a [ 4 ] ;") == 0);

  /* A multi-token body splices in whole. */
  s = glsl_pp_to_string("#define V vec3(1.0)\nV;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "vec3 ( 1.0 ) ;") == 0);

  /* An empty macro expands to nothing rather than to its own name. */
  s = glsl_pp_to_string("#define E\nfloat E a;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "float a ;") == 0);

  /* **`#define A A` must not loop.** A macro is not expanded while its own expansion is being
   * consumed, so this yields the identifier once. */
  s = glsl_pp_to_string("#define A A\nA;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "A ;") == 0);

  /* A macro whose body names another expands through. */
  s = glsl_pp_to_string("#define A B\n#define B 7\nA;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "7 ;") == 0);

  /* `#undef`, including of something never defined, which is legal. */
  s = glsl_pp_to_string("#define N 4\n#undef N\nN;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "N ;") == 0);
  s = glsl_pp_to_string("#undef NOPE\nx;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "x ;") == 0);

  /* Redefinition takes the later body. */
  s = glsl_pp_to_string("#define N 1\n#define N 2\nN;", &err, &version);
  ASSERT_TRUE(s != NULL && strcmp(s, "2 ;") == 0);
}

/* **Skipping still has to parse the directives.** Inside a false branch the tokens go, but the
 * conditionals do not - otherwise nested `#ifdef`s pair with the wrong `#endif` and the error
 * surfaces hundreds of lines later as a brace mismatch. */
static void test_glsl_preprocessor_nests_conditionals(void) {
  const char *err;
  const char *s;

  s = glsl_pp_to_string("#define A\n#ifdef A\nyes;\n#endif\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "yes ;") == 0);

  s = glsl_pp_to_string("#ifdef A\nyes;\n#endif\nafter;", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "after ;") == 0);

  s = glsl_pp_to_string("#ifndef A\nyes;\n#endif\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "yes ;") == 0);

  /* `#else` takes the other branch, and only one of them. */
  s = glsl_pp_to_string("#define A\n#ifdef A\none;\n#else\ntwo;\n#endif\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "one ;") == 0);
  s = glsl_pp_to_string("#ifdef A\none;\n#else\ntwo;\n#endif\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "two ;") == 0);

  /* **The nesting case.** The inner `#endif` must close the inner `#ifdef`, leaving `after`
   * outside both. A skipper that ran to the first `#endif` would emit `after` inside the dark
   * region, or drop it entirely. */
  s = glsl_pp_to_string(
      "#ifdef OFF\n"
      "  #ifdef ALSO_OFF\n"
      "    deep;\n"
      "  #endif\n"
      "  shallow;\n"
      "#endif\n"
      "after;\n", &err, NULL);
  ASSERT_TRUE(s != NULL);
  ASSERT_TRUE(strcmp(s, "after ;") == 0);

  /* A live outer with a dark inner, and back out again. */
  s = glsl_pp_to_string(
      "#define ON\n"
      "#ifdef ON\n"
      "  a;\n"
      "  #ifdef OFF\n"
      "    b;\n"
      "  #endif\n"
      "  c;\n"
      "#endif\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "a ; c ;") == 0);

  /* **A branch inside a dark region stays dark however its own condition reads.** `ON` is
   * defined, so a naive implementation would light `b` up inside a region that is off. */
  s = glsl_pp_to_string(
      "#define ON\n"
      "#ifdef OFF\n"
      "  #ifdef ON\n"
      "    b;\n"
      "  #endif\n"
      "#endif\n"
      "after;\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "after ;") == 0);

  /* An `#else` inside a dark region is also dark - both branches are skipped. */
  s = glsl_pp_to_string(
      "#ifdef OFF\n"
      "  #ifdef ALSO\n"
      "    b;\n"
      "  #else\n"
      "    c;\n"
      "  #endif\n"
      "#endif\n"
      "after;\n", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "after ;") == 0);

  /* Definitions inside a dark branch do not happen. */
  s = glsl_pp_to_string("#ifdef OFF\n#define N 9\n#endif\nN;", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "N ;") == 0);
}

/* Refusals. A directive this does not implement is named rather than skipped: a skipped
 * `#extension` compiles a shader that asked for something it did not get. */
static void test_glsl_preprocessor_refuses_by_name(void) {
  const char *err = NULL;

  ASSERT_TRUE(glsl_pp_to_string("#if 1\nx;\n#endif\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_pp_to_string("#extension GL_ARB_foo : enable\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_pp_to_string("#pragma optimize(on)\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_pp_to_string("#nonsense\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);
  ASSERT_TRUE(glsl_pp_to_string("#error something went wrong\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);

  /* **A function-like macro is refused rather than read as an object-like one** whose body
   * happens to start with a parenthesis - that would expand to something that compiles and is
   * wrong. The `(` must be adjacent to the name; with a space it is an object-like body. */
  ASSERT_TRUE(glsl_pp_to_string("#define F(x) x\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);
  const char *s = glsl_pp_to_string("#define F (x)\nF;", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "( x ) ;") == 0);

  /* Unbalanced conditionals, both directions. */
  ASSERT_TRUE(glsl_pp_to_string("#endif\n", &err, NULL) == NULL);
  ASSERT_TRUE(glsl_pp_to_string("#else\n", &err, NULL) == NULL);
  ASSERT_TRUE(glsl_pp_to_string("#ifdef A\nx;\n", &err, NULL) == NULL);
  ASSERT_TRUE(err != NULL);

  /* A directive inside a dark branch is ignored entirely, including one that would otherwise
   * be refused: a `#pragma` in a branch that is not compiled has not been asked for. */
  s = glsl_pp_to_string("#ifdef OFF\n#pragma whatever\n#endif\nafter;", &err, NULL);
  ASSERT_TRUE(s != NULL && strcmp(s, "after ;") == 0);
}

/* -------------------------------------------------------------------------
 * GLSL types and the semantic stage
 * ------------------------------------------------------------------------- */

static glsl_sema_t g_glsl_sema;

/* Declares a few names, parses `expr`, and returns its type - or GLSL_TYPE_ERROR with `*err`
 * set. The fixture types cover the shapes the operator rules turn on. */
static glsl_type_t glsl_expr_type(const char *expr, const char **err) {
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, expr, strlen(expr));
  int32_t root = glsl_parse_expression(&p);
  if (p.error || root == GLSL_NO_NODE) {
    if (err) *err = p.error ? p.error : "parse failed";
    return GLSL_TYPE_ERROR;
  }
  glsl_sema_init(&g_glsl_sema, &g_glsl_ast);
  glsl_declare(&g_glsl_sema, "f", 1, GLSL_TYPE_FLOAT, GL_FALSE);
  glsl_declare(&g_glsl_sema, "i", 1, GLSL_TYPE_INT, GL_FALSE);
  glsl_declare(&g_glsl_sema, "b", 1, GLSL_TYPE_BOOL, GL_FALSE);
  glsl_declare(&g_glsl_sema, "v2", 2, GLSL_TYPE_VEC2, GL_FALSE);
  glsl_declare(&g_glsl_sema, "v3", 2, GLSL_TYPE_VEC3, GL_FALSE);
  glsl_declare(&g_glsl_sema, "v4", 2, GLSL_TYPE_VEC4, GL_FALSE);
  glsl_declare(&g_glsl_sema, "iv3", 3, GLSL_TYPE_IVEC3, GL_FALSE);
  glsl_declare(&g_glsl_sema, "bv3", 3, GLSL_TYPE_BVEC3, GL_FALSE);
  glsl_declare(&g_glsl_sema, "m3", 2, GLSL_TYPE_MAT3, GL_FALSE);
  glsl_declare(&g_glsl_sema, "m4", 2, GLSL_TYPE_MAT4, GL_FALSE);
  glsl_declare(&g_glsl_sema, "samp", 4, GLSL_TYPE_SAMPLER2D, GL_FALSE);
  glsl_type_t t = glsl_type_of(&g_glsl_sema, root);
  if (err) *err = g_glsl_sema.error;
  return t;
}

/* **GLSL's operators are not C's, and the differences are the point.** */
static void test_glsl_sema_types_the_operators(void) {
  const char *err;

  /* A scalar against a vector is component-wise and keeps the vector type, in either order. */
  ASSERT_EQ(glsl_expr_type("v3*f", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("f*v3", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("v3+v3", &err), GLSL_TYPE_VEC3);

  /* **`mat * vec` is a transform, not a component-wise multiply**: mat4*vec4 is a vec4, and
   * mat4*vec3 has dimensions that do not meet. A component-wise implementation would accept
   * the second and produce a plausible, wrong type. */
  ASSERT_EQ(glsl_expr_type("m4*v4", &err), GLSL_TYPE_VEC4);
  ASSERT_EQ(glsl_expr_type("v4*m4", &err), GLSL_TYPE_VEC4);
  ASSERT_EQ(glsl_expr_type("m3*v3", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("m4*v3", &err), GLSL_TYPE_ERROR);
  ASSERT_TRUE(err != NULL);
  ASSERT_EQ(glsl_expr_type("m4*m3", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("m4*m4", &err), GLSL_TYPE_MAT4);

  /* **No implicit conversion between int and float.** This is the rule that most separates
   * GLSL from C, and accepting it would pick a type the author did not write. */
  ASSERT_EQ(glsl_expr_type("i+f", &err), GLSL_TYPE_ERROR);
  ASSERT_TRUE(err != NULL);
  ASSERT_EQ(glsl_expr_type("v3*i", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("iv3*i", &err), GLSL_TYPE_IVEC3);
  ASSERT_EQ(glsl_expr_type("i+i", &err), GLSL_TYPE_INT);
  ASSERT_EQ(glsl_expr_type("f+f", &err), GLSL_TYPE_FLOAT);

  /* Comparison yields bool; ordering is scalars only, because `<` on vectors has no single
   * answer - GLSL has lessThan() for that. */
  ASSERT_EQ(glsl_expr_type("f<f", &err), GLSL_TYPE_BOOL);
  ASSERT_EQ(glsl_expr_type("v3<v3", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("v3==v3", &err), GLSL_TYPE_BOOL);
  ASSERT_EQ(glsl_expr_type("v3==v4", &err), GLSL_TYPE_ERROR);

  /* Logical operators are bool-only, and arithmetic on bool is refused. */
  ASSERT_EQ(glsl_expr_type("b&&b", &err), GLSL_TYPE_BOOL);
  ASSERT_EQ(glsl_expr_type("f&&f", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("!b", &err), GLSL_TYPE_BOOL);
  ASSERT_EQ(glsl_expr_type("!f", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("b+b", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("bv3+bv3", &err), GLSL_TYPE_ERROR);

  /* A sampler is opaque: it can be passed and sampled, never computed with. */
  ASSERT_EQ(glsl_expr_type("samp*f", &err), GLSL_TYPE_ERROR);
  ASSERT_TRUE(err != NULL);

  /* Conditional and assignment. */
  ASSERT_EQ(glsl_expr_type("b?v3:v3", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("f?v3:v3", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("b?v3:v4", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("v3=v3", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("v3=v4", &err), GLSL_TYPE_ERROR);
  /* `v3 *= f` is `v3 = v3 * f`, which is a vec3 and assigns cleanly. */
  ASSERT_EQ(glsl_expr_type("v3*=f", &err), GLSL_TYPE_VEC3);
  /* `f *= v3` would assign a vec3 into a float. */
  ASSERT_EQ(glsl_expr_type("f*=v3", &err), GLSL_TYPE_ERROR);

  /* Indexing: a matrix gives a column, a vector gives a component. */
  ASSERT_EQ(glsl_expr_type("m4[i]", &err), GLSL_TYPE_VEC4);
  ASSERT_EQ(glsl_expr_type("v3[i]", &err), GLSL_TYPE_FLOAT);
  ASSERT_EQ(glsl_expr_type("v3[f]", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("f[i]", &err), GLSL_TYPE_ERROR);

  /* An undeclared name is refused rather than given a type. */
  ASSERT_EQ(glsl_expr_type("nope+f", &err), GLSL_TYPE_ERROR);
  ASSERT_TRUE(err != NULL);
}

/* **Swizzles: the three vocabularies may not be mixed, and a component past the end is an
 * error.** `v.xg` is the typo that compiles in a careless implementation. */
static void test_glsl_sema_checks_swizzles(void) {
  const char *err;

  ASSERT_EQ(glsl_expr_type("v4.x", &err), GLSL_TYPE_FLOAT);
  ASSERT_EQ(glsl_expr_type("v4.xy", &err), GLSL_TYPE_VEC2);
  ASSERT_EQ(glsl_expr_type("v4.xyz", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("v4.wzyx", &err), GLSL_TYPE_VEC4);

  /* Repetition is legal and widens: `.xxx` on a vec2 is a vec3. */
  ASSERT_EQ(glsl_expr_type("v2.xxx", &err), GLSL_TYPE_VEC3);

  /* The colour and texture vocabularies name the same components. */
  ASSERT_EQ(glsl_expr_type("v4.rgb", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("v4.stpq", &err), GLSL_TYPE_VEC4);

  /* **Mixing them is refused.** */
  ASSERT_EQ(glsl_expr_type("v4.xg", &err), GLSL_TYPE_ERROR);
  ASSERT_TRUE(err != NULL);
  ASSERT_EQ(glsl_expr_type("v4.rs", &err), GLSL_TYPE_ERROR);

  /* **Past the end of the operand.** `.w` on a vec3 is the one that otherwise reads whatever
   * sits after the vector. */
  ASSERT_EQ(glsl_expr_type("v3.w", &err), GLSL_TYPE_ERROR);
  ASSERT_TRUE(err != NULL);
  ASSERT_EQ(glsl_expr_type("v2.z", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("v3.z", &err), GLSL_TYPE_FLOAT);

  /* Not a component name, too many components, and a swizzle on a non-vector. */
  ASSERT_EQ(glsl_expr_type("v4.k", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("v4.xyzwx", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("f.x", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("m4.x", &err), GLSL_TYPE_ERROR);

  /* The base type survives the swizzle: an ivec3 swizzles to an ivec2. */
  ASSERT_EQ(glsl_expr_type("iv3.xy", &err), GLSL_TYPE_IVEC2);
  ASSERT_EQ(glsl_expr_type("bv3.x", &err), GLSL_TYPE_BOOL);
}

/* **Constructors count components, not arguments.** `vec4(v3, 1.0)` is two arguments and four
 * components; `vec4(1.0)` is one of each and fills. Counting arguments rejects the first. */
static void test_glsl_sema_checks_constructors(void) {
  const char *err;

  ASSERT_EQ(glsl_expr_type("vec4(v3,f)", &err), GLSL_TYPE_VEC4);
  ASSERT_EQ(glsl_expr_type("vec4(f)", &err), GLSL_TYPE_VEC4);
  ASSERT_EQ(glsl_expr_type("vec3(f,f,f)", &err), GLSL_TYPE_VEC3);
  ASSERT_EQ(glsl_expr_type("vec2(v4)", &err), GLSL_TYPE_VEC2); /* trailing components dropped */
  ASSERT_EQ(glsl_expr_type("mat4(f)", &err), GLSL_TYPE_MAT4);

  ASSERT_EQ(glsl_expr_type("vec4(v2)", &err), GLSL_TYPE_ERROR); /* two components, needs four */
  ASSERT_TRUE(err != NULL);
  ASSERT_EQ(glsl_expr_type("vec2(f,f,f)", &err), GLSL_TYPE_ERROR);
  ASSERT_EQ(glsl_expr_type("vec4(samp)", &err), GLSL_TYPE_ERROR);

  /* The real idiom, end to end. */
  ASSERT_EQ(glsl_expr_type("m4*vec4(v3,1.0)", &err), GLSL_TYPE_VEC4);
  ASSERT_EQ(glsl_expr_type("v3.xy*0.5+0.5", &err), GLSL_TYPE_VEC2);
}

/* Scopes: an inner declaration shadows an outer one, and dies with its block. */
static void test_glsl_sema_scopes_and_shadowing(void) {
  glsl_sema_init(&g_glsl_sema, &g_glsl_ast);

  ASSERT_EQ(glsl_declare(&g_glsl_sema, "x", 1, GLSL_TYPE_FLOAT, GL_FALSE), GL_TRUE);
  /* **Redeclaration in the same scope is an error.** */
  ASSERT_EQ(glsl_declare(&g_glsl_sema, "x", 1, GLSL_TYPE_INT, GL_FALSE), GL_FALSE);
  ASSERT_TRUE(g_glsl_sema.error != NULL);

  glsl_sema_init(&g_glsl_sema, &g_glsl_ast);
  glsl_declare(&g_glsl_sema, "x", 1, GLSL_TYPE_FLOAT, GL_FALSE);

  /* Shadowing in an inner scope is legal, and the inner one wins while it lasts. */
  glsl_scope_push(&g_glsl_sema);
  ASSERT_EQ(glsl_declare(&g_glsl_sema, "x", 1, GLSL_TYPE_VEC3, GL_FALSE), GL_TRUE);
  ASSERT_TRUE(g_glsl_sema.error == NULL);
  ASSERT_EQ(g_glsl_sema.count, 2);

  /* **Leaving the block drops it**, or the name stays visible after its scope closed. */
  glsl_scope_pop(&g_glsl_sema);
  ASSERT_EQ(g_glsl_sema.count, 1);
  ASSERT_EQ(g_glsl_sema.symbols[0].type, GLSL_TYPE_FLOAT);

  /* And the outer name is declarable again only after the inner one is gone. */
  glsl_scope_push(&g_glsl_sema);
  glsl_declare(&g_glsl_sema, "y", 1, GLSL_TYPE_INT, GL_FALSE);
  glsl_declare(&g_glsl_sema, "z", 1, GLSL_TYPE_INT, GL_FALSE);
  ASSERT_EQ(g_glsl_sema.count, 3);
  glsl_scope_pop(&g_glsl_sema);
  ASSERT_EQ(g_glsl_sema.count, 1);
  ASSERT_TRUE(g_glsl_sema.error == NULL);
}

/* Parses and checks a whole shader. Returns NULL on success, or the first error. */
static const char *glsl_check_shader(const char *src) {
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, src, strlen(src));
  int32_t unit = glsl_parse_translation_unit(&p);
  if (p.error || unit == GLSL_NO_NODE) return p.error ? p.error : "parse failed";
  glsl_sema_init(&g_glsl_sema, &g_glsl_ast);
  if (!glsl_check_unit(&g_glsl_sema, unit)) {
    return g_glsl_sema.error ? g_glsl_sema.error : "check failed";
  }
  return NULL;
}

/* **L-values: what may be written to.** The obvious cases are not the interesting ones. */
static void test_glsl_sema_checks_lvalues(void) {
  /* A plain local, a component, an indexed element: all writable. */
  ASSERT_TRUE(glsl_check_shader("void main(){ float a; a = 1.0; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3 v; v.x = 1.0; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3 v; v.xy = vec2(1.0); }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3 v; int i; v[i] = 1.0; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ float a; a += 1.0; ++a; a++; }") == NULL);

  /* **A swizzle that repeats a component is not assignable** - two values, one place. This is
   * the one an implementation that merely checks "is it a field selection" gets wrong. */
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3 v; v.xx = vec2(1.0); }") != NULL);

  /* A literal and a call result cannot be written to. */
  ASSERT_TRUE(glsl_check_shader("void main(){ 1.0 = 2.0; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3(1.0) = vec3(2.0); }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ float a; (a+1.0) = 2.0; }") != NULL);

  /* **A uniform, an attribute and a const are read-only.** Syntactically an l-value, and not
   * one semantically - which needs the storage qualifier, not the shape of the expression. */
  ASSERT_TRUE(glsl_check_shader("uniform float u; void main(){ u = 1.0; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("attribute vec3 p; void main(){ p = vec3(1.0); }") != NULL);
  ASSERT_TRUE(glsl_check_shader("const float c = 1.0; void main(){ c = 2.0; }") != NULL);
  /* Reading them is fine. */
  ASSERT_TRUE(glsl_check_shader("uniform float u; void main(){ float a; a = u; }") == NULL);
  /* A varying is written by a vertex shader, so it is not read-only. */
  ASSERT_TRUE(glsl_check_shader("varying vec2 uv; void main(){ uv = vec2(1.0); }") == NULL);

  /* `++` on something unwritable is the same error one level along. */
  ASSERT_TRUE(glsl_check_shader("uniform float u; void main(){ ++u; }") != NULL);
}

static void test_glsl_sema_checks_statements(void) {
  /* **A condition must be a bool.** GLSL does not take "non-zero is true" from C. */
  ASSERT_TRUE(glsl_check_shader("void main(){ if (true) ; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ int i; if (i) ; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ float f; while (f) ; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ int i; for (i=0; i; ) ; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ int i; for (i=0; i<3; ++i) ; }") == NULL);

  /* **`break` and `continue` need a loop.** The parser cannot tell; it has no idea where it
   * is. */
  ASSERT_TRUE(glsl_check_shader("void main(){ break; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ continue; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ while (true) break; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ for (;;) { continue; } }") == NULL);
  /* And the depth unwinds: after the loop closes, it is out of scope again. */
  ASSERT_TRUE(glsl_check_shader("void main(){ while (true) { } break; }") != NULL);

  /* Returns are checked against the enclosing function. */
  ASSERT_TRUE(glsl_check_shader("void main(){ return; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ return 1.0; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("float f(){ return 1.0; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("float f(){ return; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("float f(){ return 1; }") != NULL); /* int is not float */

  /* **A `for` init declares into the loop's own scope**, so two loops in a row do not collide
   * and `i` does not leak out. */
  ASSERT_TRUE(glsl_check_shader(
      "void main(){ for (int i=0; i<3; ++i) ; for (int i=0; i<3; ++i) ; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ for (int i=0; i<3; ++i) ; i = 1; }") != NULL);

  /* **A declarator chain inside a block must not be spliced over.** `float a, b;` is two DECL
   * nodes on the same `sibling` field the statement list uses, so a compound that assumes the
   * node it just parsed is the last one overwrites the second declarator and `b` never exists.
   * Only a *use* of `b` afterwards notices. */
  ASSERT_TRUE(glsl_check_shader("void main(){ float a, b; b = 1.0; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ float a, b, c; c = 1.0; a = c; }") == NULL);

  /* A block's declarations die with it. */
  ASSERT_TRUE(glsl_check_shader("void main(){ { float a; } float a; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ { float a; } a = 1.0; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ float a; float a; }") != NULL);

  /* An initialiser must match, and is typed *before* the name exists - so `float x = x;` is an
   * error rather than a variable initialised from itself. */
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3 v = vec3(1.0); }") == NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ vec3 v = 1.0; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ float x = x; }") != NULL);
  ASSERT_TRUE(glsl_check_shader("void main(){ void v; }") != NULL);
}

static void test_glsl_sema_checks_functions(void) {
  /* Arity and argument types against the recorded signature. */
  ASSERT_TRUE(glsl_check_shader(
      "float sq(float x){ return x*x; } void main(){ float a; a = sq(2.0); }") == NULL);
  ASSERT_TRUE(glsl_check_shader(
      "float sq(float x){ return x*x; } void main(){ float a; a = sq(); }") != NULL);
  ASSERT_TRUE(glsl_check_shader(
      "float sq(float x){ return x*x; } void main(){ float a; a = sq(2.0, 3.0); }") != NULL);
  /* No implicit conversion, so an int argument to a float parameter is wrong. */
  ASSERT_TRUE(glsl_check_shader(
      "float sq(float x){ return x*x; } void main(){ float a; a = sq(2); }") != NULL);

  /* **A function may be called before it is defined**, because every top-level name is
   * declared in a first pass. Otherwise a shader would compile or not depending on the order
   * somebody happened to write its functions in. */
  ASSERT_TRUE(glsl_check_shader(
      "void main(){ float a; a = later(1.0); } float later(float x){ return x; }") == NULL);

  /* A prototype is enough to call against, and has no body to check. */
  ASSERT_TRUE(glsl_check_shader(
      "float f(float x); void main(){ float a; a = f(1.0); }") == NULL);

  /* Parameters are in scope in the body, and collide with a top-level local of the same name. */
  ASSERT_TRUE(glsl_check_shader("float f(float x){ return x; }") == NULL);
  ASSERT_TRUE(glsl_check_shader("float f(float x){ float x; return 1.0; }") != NULL);
  /* A parameter is writable; it is a copy. */
  ASSERT_TRUE(glsl_check_shader("float f(float x){ x = 2.0; return x; }") == NULL);

  ASSERT_TRUE(glsl_check_shader("void main(){ float a; a = nosuch(1.0); }") != NULL);
  /* Calling a variable is not calling a function. */
  ASSERT_TRUE(glsl_check_shader("void main(){ float a; a = a(1.0); }") != NULL);
}

/* The shader the parser test reads, now type-checked end to end. */
static void test_glsl_sema_accepts_a_real_shader(void) {
  const char *err = glsl_check_shader(
      "uniform mat4 mvp;\n"
      "attribute vec3 pos;\n"
      "varying vec2 uv;\n"
      "void main() {\n"
      "  vec4 clip = mvp * vec4(pos, 1.0);\n"
      "  uv = pos.xy * 0.5 + 0.5;\n"
      "  float d = clip.z / clip.w;\n"
      "  if (d > 0.5) { uv = uv * 2.0; }\n"
      "  for (int i = 0; i < 4; ++i) { uv = uv + 0.01; }\n"
      "}\n");
  ASSERT_TRUE(err == NULL);

  /* One wrong dimension in the middle of it is caught: mat4 * vec3 does not meet. */
  ASSERT_TRUE(glsl_check_shader(
      "uniform mat4 mvp;\n"
      "attribute vec3 pos;\n"
      "void main() { vec4 clip = mvp * pos; }\n") != NULL);
}

/* -------------------------------------------------------------------------
 * The GLSL back end: RDNA2 instruction encoding
 * ------------------------------------------------------------------------- */

/* **Every expected word here came out of a real assembler**, not out of my head:
 *
 *     clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c tools/shader/gl2-transform.s -o /tmp/t.o
 *     objdump -s -j .text /tmp/t.o
 *
 * That matters more than usual because a wrong instruction encoding **cannot fail loudly**. It
 * assembles into the payload, the hardware decodes it as some other instruction, and the result
 * is a wrong frame rather than a stopped build. Asserting against words the encoder itself
 * produced would prove only that it is consistent with itself.
 *
 * The independent cross-check is `s_endpgm` = 0xbf810000: clang produces it, and it is also the
 * word every hand-written shader already in this repository ends with.
 */
static void test_glsl_emit_matches_the_assembler(void) {
  static uint32_t words[64];
  glsl_code_t c;

  /* v_mul_f32 v4, v8, v0 */
  glsl_code_init(&c, words, 64);
  glsl_emit_mul_f32(&c, 4, 8, 0);
  ASSERT_EQ(c.count, 1u);
  ASSERT_EQ(words[0], 0x10080108u);

  /* The other three from the same block, which differ only in register numbers - so a field
   * shifted by one place would pass the first and fail these. */
  glsl_code_init(&c, words, 64);
  glsl_emit_mul_f32(&c, 5, 9, 0);
  glsl_emit_mul_f32(&c, 6, 10, 0);
  glsl_emit_mul_f32(&c, 7, 11, 0);
  ASSERT_EQ(words[0], 0x100a0109u);
  ASSERT_EQ(words[1], 0x100c010au);
  ASSERT_EQ(words[2], 0x100e010bu);

  /* v_fmac_f32 v4, v8, v1  and  v_fmac_f32 v5, v9, v1 */
  glsl_code_init(&c, words, 64);
  glsl_emit_fmac_f32(&c, 4, 8, 1);
  glsl_emit_fmac_f32(&c, 5, 9, 1);
  ASSERT_EQ(words[0], 0x56080308u);
  ASSERT_EQ(words[1], 0x560a0309u);

  /* v_add_f32 v4, v8, v4 */
  glsl_code_init(&c, words, 64);
  glsl_emit_add_f32(&c, 4, 8, 4);
  ASSERT_EQ(words[0], 0x06080908u);

  /* **The two constant forms, which encode differently.** 1.0 is an inline constant and costs
   * one word; anything else is src0 = 255 with a literal dword after it and costs two. Emitting
   * a literal where an inline constant exists is a silent size regression in a shader whose
   * budget is instruction count. */
  glsl_code_init(&c, words, 64);
  glsl_emit_mov_imm(&c, 12, 0x3f800000u); /* v_mov_b32 v12, 1.0 */
  ASSERT_EQ(c.count, 1u);
  ASSERT_EQ(words[0], 0x7e1802f2u);

  glsl_code_init(&c, words, 64);
  glsl_emit_mov_imm(&c, 13, 0x3e800000u); /* v_mov_b32 v13, 0x3e800000 */
  ASSERT_EQ(c.count, 2u);
  ASSERT_EQ(words[0], 0x7e1a02ffu);
  ASSERT_EQ(words[1], 0x3e800000u);

  glsl_code_init(&c, words, 64);
  glsl_emit_mov_imm(&c, 15, 0u); /* v_mov_b32 v15, 0 */
  ASSERT_EQ(c.count, 1u);
  ASSERT_EQ(words[0], 0x7e1e0280u);

  /* **Subtraction, and its operand order.** VOP2 subtracts vsrc1 from src0, and opcode 5 is
   * `v_subrev_f32` with the order reversed - so both a swapped operand and an off-by-one opcode
   * produce the negation of the right answer, silently. */
  glsl_code_init(&c, words, 64);
  glsl_emit_sub_f32(&c, 4, 8, 9); /* v_sub_f32 v4, v8, v9 */
  ASSERT_EQ(c.count, 1u);
  ASSERT_EQ(words[0], 0x08081308u);

  /* Unary minus: zero minus the operand, the zero inline rather than in a register. */
  glsl_code_init(&c, words, 64);
  glsl_emit_neg_f32(&c, 5, 8); /* v_sub_f32 v5, 0, v8 */
  ASSERT_EQ(c.count, 1u);
  ASSERT_EQ(words[0], 0x080a1080u);

  /* v_mov_b32 v14, v4 */
  glsl_code_init(&c, words, 64);
  glsl_emit_mov(&c, 14, 4);
  ASSERT_EQ(words[0], 0x7e1c0304u);

  /* The cross-check. */
  glsl_code_init(&c, words, 64);
  glsl_emit_endpgm(&c);
  ASSERT_EQ(words[0], 0xbf810000u);

  /* **A VGPR source is biased by 256.** Encoding the bare number names an SGPR instead, which
   * is a shader that runs and computes rubbish rather than one that faults. */
  ASSERT_EQ(glsl_vgpr(0), 256u);
  ASSERT_EQ(glsl_vgpr(8), 264u);
  /* And an SGPR is its own number, at the bottom of the same nine-bit space. */
  ASSERT_EQ(glsl_sgpr(4), 4u);

  /* **Scalar memory, which is how a uniform reaches a compiled shader.** All five widths, three
   * destinations and three offsets: the width *is* the opcode and the five are consecutive, so
   * one example would not have separated that field from anything beside it. Words from
   * `tools/shader/gl2-fragment.s`. */
  glsl_code_init(&c, words, 64);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORD, 4u, 0u, 0x0u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORD, 4u, 0u, 0x10u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORD, 12u, 0u, 0x40u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORDX2, 4u, 0u, 0x0u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORDX4, 4u, 0u, 0x0u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORDX8, 4u, 0u, 0x0u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORDX8, 12u, 0u, 0x20u);
  glsl_emit_s_load(&c, GLSL_SMEM_LOAD_DWORDX16, 16u, 0u, 0x0u);
  glsl_emit_s_waitcnt_lgkm(&c);
  ASSERT_EQ(words[0], 0xf4000100u);  /* s_load_dword s4, s[0:1], 0x0 */
  ASSERT_EQ(words[1], 0xfa000000u);  /* **soffset is SGPR_NULL and not s0**, which would be the
                                      * address's own low half added to itself */
  ASSERT_EQ(words[2], 0xf4000100u);
  ASSERT_EQ(words[3], 0xfa000010u);  /* the offset, and only the offset, in the second dword */
  ASSERT_EQ(words[4], 0xf4000300u);  /* s12 - the destination is bits 12:6 */
  ASSERT_EQ(words[5], 0xfa000040u);
  ASSERT_EQ(words[6], 0xf4040100u);  /* x2 */
  ASSERT_EQ(words[8], 0xf4080100u);  /* x4 */
  ASSERT_EQ(words[10], 0xf40c0100u); /* x8 */
  ASSERT_EQ(words[12], 0xf40c0300u); /* x8 into s[12:19] ... */
  ASSERT_EQ(words[13], 0xfa000020u); /* ... at +0x20 */
  ASSERT_EQ(words[14], 0xf4100400u); /* x16 */
  /* **0xc07f, not zero**: the counters this does not wait on are held at their maximum, and a
   * zero there would wait for every outstanding memory and export operation as well. */
  ASSERT_EQ(words[16], 0xbf8cc07fu); /* s_waitcnt lgkmcnt(0) */

  /* An SGPR as a VOP source costs no register and no move - but only in `src0`, because `vsrc1`
   * is eight bits and always a VGPR. */
  glsl_code_init(&c, words, 64);
  glsl_emit_vop1(&c, GLSL_VOP1_MOV_B32, 4u, glsl_sgpr(4u));
  glsl_emit_vop2(&c, GLSL_VOP2_MUL_F32, 4u, glsl_sgpr(4u), 5u);
  glsl_emit_vop2(&c, GLSL_VOP2_ADD_F32, 4u, glsl_sgpr(12u), 5u);
  ASSERT_EQ(words[0], 0x7e080204u); /* v_mov_b32_e32 v4, s4 */
  ASSERT_EQ(words[1], 0x10080a04u); /* v_mul_f32_e32 v4, s4, v5 */
  ASSERT_EQ(words[2], 0x06080a0cu); /* v_add_f32_e32 v4, s12, v5 */

  /* **Control flow, which on this machine is the exec mask and not a branch.** Two destinations
   * for each form, because the register number sits in a different field in SOP1 than in SOP2
   * and one example would not have told them apart. */
  glsl_code_init(&c, words, 64);
  glsl_emit_exec_save_and_vcc(&c, 4u);
  glsl_emit_exec_save_and_vcc(&c, 15u);
  glsl_emit_exec_else(&c, 4u);
  glsl_emit_exec_else(&c, 15u);
  glsl_emit_exec_drop_live(&c, 4u);
  glsl_emit_exec_drop_live(&c, 15u);
  glsl_emit_exec_restore(&c, 4u);
  glsl_emit_exec_restore(&c, 15u);
  glsl_emit_exec_clear(&c);
  ASSERT_EQ(words[0], 0xbe843c6au); /* s_and_saveexec_b32 s4, vcc_lo */
  ASSERT_EQ(words[1], 0xbe8f3c6au); /* s_and_saveexec_b32 s15, vcc_lo */
  /* `s_andn2_b32 d, a, b` is `a & ~b`, so the **saved** mask is ssrc0 and the one to remove is
   * ssrc1. The other way round computes lanes that were never running. */
  ASSERT_EQ(words[2], 0x8a7e7e04u); /* s_andn2_b32 exec_lo, s4, exec_lo */
  ASSERT_EQ(words[3], 0x8a7e7e0fu); /* s_andn2_b32 exec_lo, s15, exec_lo */
  ASSERT_EQ(words[4], 0x8a047e04u); /* s_andn2_b32 s4, s4, exec_lo - what makes a discard stick */
  ASSERT_EQ(words[5], 0x8a0f7e0fu); /* s_andn2_b32 s15, s15, exec_lo */
  ASSERT_EQ(words[6], 0xbefe0304u); /* s_mov_b32 exec_lo, s4 */
  ASSERT_EQ(words[7], 0xbefe030fu); /* s_mov_b32 exec_lo, s15 */
  ASSERT_EQ(words[8], 0xbefe0380u); /* s_mov_b32 exec_lo, 0 */

  /* **The cube face selection**, which is the only VOP3 this back end emits. Words from
   * `tools/shader/tex-cube.s`, whose four instructions all take the same three sources - so the
   * second dword is identical across them and the opcode is the whole difference, which is
   * exactly the shape an off-by-one in the opcode table would hide in. */
  glsl_code_init(&c, words, 64);
  glsl_emit_vop3(&c, GLSL_VOP3_CUBEID_F32, 19u, GLSL_VOP3_VGPR(16u), GLSL_VOP3_VGPR(17u),
                 GLSL_VOP3_VGPR(18u));
  glsl_emit_vop3(&c, GLSL_VOP3_CUBESC_F32, 20u, GLSL_VOP3_VGPR(16u), GLSL_VOP3_VGPR(17u),
                 GLSL_VOP3_VGPR(18u));
  glsl_emit_vop3(&c, GLSL_VOP3_CUBETC_F32, 21u, GLSL_VOP3_VGPR(16u), GLSL_VOP3_VGPR(17u),
                 GLSL_VOP3_VGPR(18u));
  glsl_emit_vop3(&c, GLSL_VOP3_CUBEMA_F32, 22u, GLSL_VOP3_VGPR(16u), GLSL_VOP3_VGPR(17u),
                 GLSL_VOP3_VGPR(18u));
  ASSERT_EQ(words[0], 0xd5440013u); /* v_cubeid_f32 v19, v16, v17, v18 */
  ASSERT_EQ(words[1], 0x044a2310u); /*   ...the three sources, 256 plus the register number */
  ASSERT_EQ(words[2], 0xd5450014u); /* v_cubesc_f32 v20, ... */
  ASSERT_EQ(words[3], 0x044a2310u);
  ASSERT_EQ(words[4], 0xd5460015u); /* v_cubetc_f32 v21, ... */
  ASSERT_EQ(words[5], 0x044a2310u);
  ASSERT_EQ(words[6], 0xd5470016u); /* v_cubema_f32 v22, ... */
  ASSERT_EQ(words[7], 0x044a2310u);

  /* **Branches, which only a loop needs.** The layout below is `tools/shader/branch.s`
   * instruction for instruction, so every word here is one clang produced rather than one this
   * encoder and this test agree about.
   *
   * The offsets are the point. `simm16` counts from the word *after* the branch, so the jump
   * back over a single instruction is -2 and not -1, and the three forward jumps to the same
   * label are +4, +3 and +2 rather than all the same. An encoder off by one here emits a loop
   * that re-enters itself one instruction in - which does not draw wrongly, it hangs. */
  glsl_code_init(&c, words, 64);
  {
    const uint32_t loop_top = glsl_code_here(&c);
    uint32_t fix_execz, fix_scc1, fix_branch;
    ASSERT_EQ(loop_top, 0u);
    glsl_emit_nop(&c);
    glsl_emit_branch_back(&c, GLSL_SOPP_BRANCH, loop_top);
    fix_execz  = glsl_emit_branch_fwd(&c, GLSL_SOPP_CBRANCH_EXECZ);
    fix_scc1   = glsl_emit_branch_fwd(&c, GLSL_SOPP_CBRANCH_SCC1);
    fix_branch = glsl_emit_branch_fwd(&c, GLSL_SOPP_BRANCH);
    glsl_emit_nop(&c);
    glsl_emit_nop(&c);
    /* All three land on the same word, which is `glsl_code_here` = 7 at this moment. */
    ASSERT_TRUE(glsl_patch_branch_here(&c, fix_execz));
    ASSERT_TRUE(glsl_patch_branch_here(&c, fix_scc1));
    ASSERT_TRUE(glsl_patch_branch_here(&c, fix_branch));
    ASSERT_EQ(glsl_code_here(&c), 7u);
  }
  ASSERT_EQ(words[0], 0xbf800000u); /* s_nop 0 - also the placeholder every patch overwrites */
  ASSERT_EQ(words[1], 0xbf82fffeu); /* s_branch loop_top, back over one instruction */
  ASSERT_EQ(words[2], 0xbf880004u); /* s_cbranch_execz loop_exit */
  ASSERT_EQ(words[3], 0xbf850003u); /* s_cbranch_scc1 loop_exit */
  ASSERT_EQ(words[4], 0xbf820002u); /* s_branch loop_exit */

  /* **The trip guard**, which is what makes a real backward branch safe to emit at all: a
   * counter in an SGPR that ends the loop whatever the lanes are doing. A GLSL condition that
   * never goes false is a bug in the shader; without this it is a wedged GPU. */
  glsl_code_init(&c, words, 64);
  glsl_emit_sop1(&c, GLSL_SOP1_MOV_B32, 20u, 128u); /* 128 is the scalar inline constant 0 */
  glsl_emit_s_inc_u32(&c, 20u);
  glsl_emit_s_cmp_ge_u32_imm(&c, 20u, 0x100u);
  ASSERT_EQ(words[0], 0xbe940380u); /* s_mov_b32 s20, 0 */
  ASSERT_EQ(words[1], 0x80148114u); /* s_add_u32 s20, s20, 1 */
  ASSERT_EQ(words[2], 0xbf09ff14u); /* s_cmp_ge_u32 s20, 0x100 */
  ASSERT_EQ(words[3], 0x00000100u); /*   ...the literal that rides behind it */
  ASSERT_EQ(glsl_code_here(&c), 4u);

  /* Both sides of the scalar inline boundary. 0..64 encode as operands 128..192; 65 does not
   * exist in that table and has to spill to a literal. Taking the inline path for 65 would
   * compare the counter against operand 193, which is the inline constant **-4.0**. */
  glsl_code_init(&c, words, 64);
  glsl_emit_s_cmp_ge_u32_imm(&c, 20u, 64u);
  glsl_emit_s_cmp_ge_u32_imm(&c, 20u, 65u);
  ASSERT_EQ(words[0], 0xbf09c014u); /* s_cmp_ge_u32 s20, 64 - one word */
  ASSERT_EQ(words[1], 0xbf09ff14u); /* s_cmp_ge_u32 s20, 65 - two */
  ASSERT_EQ(words[2], 0x00000041u);
  ASSERT_EQ(glsl_code_here(&c), 3u);

  /* **A patch into an overflowed buffer reports rather than lies.** `count` stopped advancing
   * at the capacity, so the distance from the branch to "here" is not the distance the hardware
   * would see; filling it in anyway would produce a stream that branches into the middle of
   * something. The caller has a failed compile by this point either way - this is what stops it
   * being a failed compile that also emits a plausible jump. */
  glsl_code_init(&c, words, 4);
  {
    const uint32_t fix = glsl_emit_branch_fwd(&c, GLSL_SOPP_BRANCH);
    glsl_emit_nop(&c);
    glsl_emit_nop(&c);
    glsl_emit_nop(&c);
    glsl_emit_nop(&c); /* the one too many */
    ASSERT_TRUE(c.overflow);
    ASSERT_TRUE(!glsl_patch_branch_here(&c, fix));
    ASSERT_EQ(words[0], 0xbf820000u); /* left as emitted, not patched to a wrong offset */
  }

  /* **Sampling a texture.** Two destinations, two coordinate pairs, two descriptor sets and all
   * four dimensions, because each of those is its own field and one example would not have told
   * them apart. Words from `tools/shader/gl2-fragment.s`. */
  glsl_code_init(&c, words, 64);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_2D, 4u, 2u, 4u, 12u);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_2D, 8u, 4u, 4u, 12u);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_2D, 8u, 4u, 16u, 24u);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_2D, 12u, 20u, 4u, 12u);
  ASSERT_EQ(words[0], 0xf0800f08u); /* image_sample v[4:7], v[2:3], s[4:11], s[12:15] */
  ASSERT_EQ(words[1], 0x00610402u);
  ASSERT_EQ(words[3], 0x00610804u); /* v[8:11], v[4:5] */
  /* **The descriptor operands are SGPR numbers over four**, so s16/s24 encode as 4 and 6. A
   * register number written in straight names a descriptor four times further up the file. */
  ASSERT_EQ(words[5], 0x00c40804u); /* s[16:23], s[24:27] */
  ASSERT_EQ(words[7], 0x00610c14u); /* v[12:15], v[20:21] */

  glsl_code_init(&c, words, 64);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_1D, 4u, 2u, 4u, 12u);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_3D, 4u, 2u, 4u, 12u);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_CUBE, 4u, 2u, 4u, 12u);
  glsl_emit_s_waitcnt_vm(&c);
  glsl_emit_wqm(&c);
  glsl_emit_exec_save(&c, 28u);
  ASSERT_EQ(words[0], 0xf0800f00u); /* dim 1D */
  ASSERT_EQ(words[2], 0xf0800f10u); /* dim 3D */
  ASSERT_EQ(words[4], 0xf0800f18u); /* dim CUBE */
  /* **The cross-check for this whole family.** `tex-prolog.s` records the sample the textured
   * pixel shader used to carry as `0xf09c0f08 0x00610402`, and the wait after it as
   * `0xbf8c3f70` - both from a different assembly run, years of this file's history apart. */
  glsl_code_init(&c, words, 64);
  glsl_emit_image_sample(&c, GLSL_MIMG_SAMPLE_LZ, GLSL_IMG_DIM_2D, 4u, 2u, 4u, 12u);
  glsl_emit_s_waitcnt_vm(&c);
  ASSERT_EQ(words[0], 0xf09c0f08u);
  ASSERT_EQ(words[1], 0x00610402u);
  ASSERT_EQ(words[2], 0xbf8c3f70u);

  glsl_code_init(&c, words, 64);
  glsl_emit_wqm(&c);
  glsl_emit_exec_save(&c, 28u);
  glsl_emit_exec_save(&c, 40u);
  ASSERT_EQ(words[0], 0xbefe097eu); /* s_wqm_b32 exec_lo, exec_lo */
  ASSERT_EQ(words[1], 0xbe9c037eu); /* s_mov_b32 s28, exec_lo */
  ASSERT_EQ(words[2], 0xbea8037eu); /* s_mov_b32 s40, exec_lo */

  /* **The cross-check that the SOP2 fields are right rather than merely self-consistent**: this
   * exact word is already in the tree behind glAlphaFunc and the polygon stipple, and it comes
   * out of the same encoder as the four above. */
  glsl_code_init(&c, words, 64);
  glsl_emit_sop2(&c, GLSL_SOP2_AND_B32, GLSL_SREG_EXEC_LO, GLSL_SREG_EXEC_LO, GLSL_SREG_VCC_LO);
  ASSERT_EQ(words[0], 0x877e6a7eu); /* s_and_b32 exec_lo, exec_lo, vcc_lo */

  /* **Overflow is recorded, not wrapped.** A truncated shader is a valid instruction stream
   * that stops in the middle, which the GPU will happily execute. */
  static uint32_t tiny[2];
  glsl_code_init(&c, tiny, 2);
  glsl_emit_mov(&c, 1, 2);
  glsl_emit_mov(&c, 3, 4);
  ASSERT_EQ(c.overflow, GL_FALSE);
  glsl_emit_mov(&c, 5, 6);
  ASSERT_EQ(c.overflow, GL_TRUE);
  ASSERT_EQ(c.count, 2u); /* nothing written past the end */
}

/* `mat4 * vec4` - what a vertex shader is mostly made of, and where column-major matters.
 *
 * GL lays a matrix out as four columns end to end, so the product is
 * `col0*v.x + col1*v.y + col2*v.z + col3*v.w`. Treating the block as rows transposes it, and
 * **a transposed model-view matrix still draws a cube** - just the wrong way round, which is a
 * bug that survives a screenshot. So the test reads the register numbers back out of the
 * encoded words rather than trusting the shape.
 */
static void test_glsl_emit_mat4_is_column_major(void) {
  static uint32_t words[64];
  glsl_code_t c;
  glsl_code_init(&c, words, 64);

  /* m in v8..v23, v in v4..v7, result into v24..v27. */
  glsl_emit_mat4_mul_vec4(&c, 24, 8, 4);
  ASSERT_EQ(c.count, 16u); /* four multiplies and twelve fused multiply-adds */

  /* Decoding a VOP2 word back into its fields, which is also a second opinion on the encoder:
   * if the shifts were wrong these would not round-trip. */
  #define OPC(w)  (((w) >> 25) & 0x3fu)
  #define VDST(w) (((w) >> 17) & 0xffu)
  #define SRC1(w) (((w) >> 9) & 0xffu)
  #define SRC0(w) ((w) & 0x1ffu)

  /* The first four are multiplies against v.x, and their matrix operands must be the *first
   * column* - v8, v9, v10, v11 - not the first row, which would be v8, v12, v16, v20. */
  for (uint32_t row = 0; row < 4u; row++) {
    ASSERT_EQ(OPC(words[row]), GLSL_VOP2_MUL_F32);
    ASSERT_EQ(VDST(words[row]), 24u + row);
    ASSERT_EQ(SRC0(words[row]), 256u + 8u + row); /* column 0, element `row` */
    ASSERT_EQ(SRC1(words[row]), 4u);              /* v.x */
  }

  /* Column 1 accumulates against v.y: v12..v15, and the vector operand moves on by one. */
  for (uint32_t row = 0; row < 4u; row++) {
    const uint32_t w = words[4u + row];
    ASSERT_EQ(OPC(w), GLSL_VOP2_FMAC_F32);
    ASSERT_EQ(VDST(w), 24u + row);
    ASSERT_EQ(SRC0(w), 256u + 12u + row);
    ASSERT_EQ(SRC1(w), 5u); /* v.y */
  }

  /* And the last column against v.w, which is the one an implementation that stopped at three
   * columns would leave out - the translation, so the cube would be at the origin. */
  for (uint32_t row = 0; row < 4u; row++) {
    const uint32_t w = words[12u + row];
    ASSERT_EQ(OPC(w), GLSL_VOP2_FMAC_F32);
    ASSERT_EQ(SRC0(w), 256u + 20u + row); /* column 3 */
    ASSERT_EQ(SRC1(w), 7u);               /* v.w */
  }

  #undef OPC
  #undef VDST
  #undef SRC1
  #undef SRC0
}

/*
 * **A title holding GL in function pointers gets them from here, and a NULL is a jump to zero.**
 *
 * `oops_gl_get_proc_address` answered NULL for everything until 2026-09-22, on the reasoning
 * that a statically linked payload has every entry point already bound. Neverball showed what
 * that misses: its `share/glext.c` fills `glGenBuffers_` from the *string* `"glGenBuffersARB"`,
 * so the linker never saw the name and had nothing to bind. The pointer stayed NULL, the first
 * mesh it loaded called through it, and the console faulted at `rip = 0` inside `sol_load_full`.
 *
 * These are the names Neverball actually asks for, which is why they are named one at a time
 * rather than swept: the resolution of one of them is the difference between a port that runs
 * and a port that faults, and a sweep would not say which.
 */
static void test_gl_proc_address_resolves_entry_points_by_name(void) {
  /*
   * **Every name in the list, checked against the function it names.** The test walks
   * `OOPS_GL_PROC_LIST` rather than a copy of it, so the two cannot disagree: a row added to the
   * table is a row checked here, and a row whose spelling is wrong does not compile in either
   * place. What it cannot catch is an extension added to `gl.h` and never added to the list -
   * that is the one thing the list asks a person to remember.
   *
   * This is worth more than it looks. The resolution of any single one of these is the
   * difference between a port that runs and a port that faults at `rip = 0`, which is how
   * Neverball failed twice: once because this answered NULL for everything by design, and once
   * because the answer came from a symbol table the console does not map.
   */
#define CHECK_CORE(fn)         ASSERT_EQ(oops_gl_get_proc_address(#fn), (void *)fn);
#define CHECK_SUFFIXED(fn, s)  ASSERT_EQ(oops_gl_get_proc_address(#fn #s), (void *)fn##s);
  OOPS_GL_PROC_LIST(CHECK_CORE, CHECK_SUFFIXED)
#undef CHECK_CORE
#undef CHECK_SUFFIXED

  /* The suffixed spelling and the core one are separate definitions, not aliases, so they have
   * different addresses - and a caller asking for one must not be handed the other. */
  ASSERT_NE((void *)glGenBuffersARB, (void *)glGenBuffers);
  ASSERT_EQ(oops_gl_get_proc_address("glGenBuffersARB"), (void *)glGenBuffersARB);
  ASSERT_EQ(oops_gl_get_proc_address("glGenBuffers"), (void *)glGenBuffers);

  /* A GL name this GL does not have is NULL, which is what a program probing for an extension it
   * can do without is asking. Neverball asks for all three of these and takes no for an answer;
   * had they resolved to anything, it would have called them. */
  ASSERT_EQ(oops_gl_get_proc_address("glCreateShaderObjectARB"), NULL);
  ASSERT_EQ(oops_gl_get_proc_address("glStringMarkerGREMEDY"), NULL);
  ASSERT_EQ(oops_gl_get_proc_address("glGenFramebuffers"), NULL);

  /* Core GL 1.1 is linked by symbol and never asked for by name, so it is deliberately not in
   * the table - and a name that is not GL's at all is certainly not. */
  ASSERT_EQ(oops_gl_get_proc_address("glBegin"), NULL);
  ASSERT_EQ(oops_gl_get_proc_address("malloc"), NULL);
  ASSERT_EQ(oops_gl_get_proc_address(""), NULL);
  ASSERT_EQ(oops_gl_get_proc_address(NULL), NULL);
}

/*
 * `CB_BLEND0_CONTROL`, which the hardware path used to write as a constant.
 *
 * The constant was `0x00002504`. Its two colour factors were right - 4 and 5, GL's default
 * `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA` - and **`ENABLE`, bit 30, was clear**, so the colour block
 * was handed the right factors and never told to blend. It also set bit 13, where the register
 * has no field. Field positions and both enums are from
 * `oops-mesa/mesa/src/amd/registers/gfx103.json` and its `gfx10.json` base.
 */
static void test_gl_blend_control_carries_the_gl_state(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  ASSERT_TRUE(disp != NULL);
  void *ctx = glContextCreate(disp);
  ASSERT_TRUE(ctx != NULL);
  glContextMakeCurrent(ctx);
  gl_context_t *c = (gl_context_t *)ctx;

  #define BSRC(v)  ((v) & 0x1fu)
  #define BCOMB(v) (((v) >> 5) & 0x7u)
  #define BDST(v)  (((v) >> 8) & 0x1fu)
  #define BEN(v)   (((v) >> 30) & 0x1u)

  /* Disabled is zero, which is also what it was before. */
  glDisable(GL_BLEND);
  ASSERT_EQ(gl_compute_cb_blend_control(c), 0u);

  /* **Enabled must actually set ENABLE.** This is the whole bug: the old constant did not, so
   * nothing the rest of this test checks would have reached the colour block anyway. */
  glEnable(GL_BLEND);
  uint32_t v = gl_compute_cb_blend_control(c);
  ASSERT_EQ(BEN(v), 1u);
  ASSERT_NE(v, 0x00002504u);
  /* And nothing lands at bit 13, where the register has no field. */
  ASSERT_EQ((v >> 13) & 0x7u, 0u);

  /* The default factors are GL's own, GL_ONE and GL_ZERO (Mesa main/blend.c:1148-1151). This
   * used to assert SRC_ALPHA / ONE_MINUS_SRC_ALPHA, which was oops-gl's default and nobody
   * else's. */
  ASSERT_EQ(BSRC(v), 1u);  /* BLEND_ONE */
  ASSERT_EQ(BDST(v), 0u);  /* BLEND_ZERO */
  ASSERT_EQ(BCOMB(v), 0u); /* COMB_DST_PLUS_SRC, GL_FUNC_ADD */

  /* The pair the old constant carried still decodes to what it had right. */
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  v = gl_compute_cb_blend_control(c);
  ASSERT_EQ(BSRC(v), 4u);  /* BLEND_SRC_ALPHA */
  ASSERT_EQ(BDST(v), 5u);  /* BLEND_ONE_MINUS_SRC_ALPHA */

  /* glBlendFunc reaches the register at all. */
  glBlendFunc(GL_ONE, GL_ZERO);
  v = gl_compute_cb_blend_control(c);
  ASSERT_EQ(BSRC(v), 1u); /* BLEND_ONE */
  ASSERT_EQ(BDST(v), 0u); /* BLEND_ZERO */

  glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_DST_ALPHA);
  v = gl_compute_cb_blend_control(c);
  ASSERT_EQ(BSRC(v), 8u); /* BLEND_DST_COLOR */
  ASSERT_EQ(BDST(v), 7u); /* BLEND_ONE_MINUS_DST_ALPHA */

  /* **The two subtractions are different registers values and not interchangeable.** GL's
   * FUNC_SUBTRACT is src - dst (COMB_SRC_MINUS_DST, 1); REVERSE_SUBTRACT is dst - src
   * (COMB_DST_MINUS_SRC, 4). Swapping them negates every blended pixel. */
  glBlendEquation(GL_FUNC_SUBTRACT);
  ASSERT_EQ(BCOMB(gl_compute_cb_blend_control(c)), 1u);
  glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
  ASSERT_EQ(BCOMB(gl_compute_cb_blend_control(c)), 4u);

  /* GL_MIN and GL_MAX ignore the factors, so the factor fields are held at ONE rather than
   * left carrying whatever glBlendFunc last said. */
  glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_DST_ALPHA);
  glBlendEquation(GL_MIN);
  v = gl_compute_cb_blend_control(c);
  ASSERT_EQ(BCOMB(v), 2u); /* COMB_MIN_DST_SRC */
  ASSERT_EQ(BSRC(v), 1u);
  ASSERT_EQ(BDST(v), 1u);
  glBlendEquation(GL_MAX);
  ASSERT_EQ(BCOMB(gl_compute_cb_blend_control(c)), 3u); /* COMB_MAX_DST_SRC */

  #undef BSRC
  #undef BCOMB
  #undef BDST
  #undef BEN
  glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Instruction selection
 * ------------------------------------------------------------------------- */

#define GOPC(w)  (((w) >> 25) & 0x3fu)
#define GVDST(w) (((w) >> 17) & 0xffu)
#define GSRC1(w) (((w) >> 9) & 0xffu)
#define GSRC0(w) ((w) & 0x1ffu)
#define GV1OP(w) (((w) >> 9) & 0xffu)

static glsl_gen_t g_glsl_gen;
static uint32_t g_gen_words[256];
static glsl_code_t g_gen_code;

/*
 * Parses `expr`, types it, and generates it - with the fixture names given **fixed register
 * homes in a fixed order**, so every expected register number below is arithmetic rather than a
 * guess: f at v0, v3 at v1..v3, v4 at v4..v7, m4 at v8..v23. Temporaries therefore start at v24.
 */
static glsl_value_t glsl_gen_of(const char *expr) {
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, expr, strlen(expr));
  int32_t root = glsl_parse_expression(&p);
  glsl_code_init(&g_gen_code, g_gen_words, 256);
  glsl_value_t none; none.base = 0u; none.count = 0;
  if (p.error || root == GLSL_NO_NODE) return none;

  glsl_sema_init(&g_glsl_sema, &g_glsl_ast);
  glsl_declare(&g_glsl_sema, "f", 1, GLSL_TYPE_FLOAT, GL_FALSE);
  glsl_declare(&g_glsl_sema, "v3", 2, GLSL_TYPE_VEC3, GL_FALSE);
  glsl_declare(&g_glsl_sema, "v4", 2, GLSL_TYPE_VEC4, GL_FALSE);
  glsl_declare(&g_glsl_sema, "m4", 2, GLSL_TYPE_MAT4, GL_FALSE);
  glsl_declare(&g_glsl_sema, "iv3", 3, GLSL_TYPE_IVEC3, GL_FALSE);

  glsl_gen_init(&g_glsl_gen, &g_glsl_ast, &g_glsl_sema, &g_gen_code);
  glsl_gen_declare_input(&g_glsl_gen, "f", 1, GLSL_TYPE_FLOAT);
  glsl_gen_declare_input(&g_glsl_gen, "v3", 2, GLSL_TYPE_VEC3);
  glsl_gen_declare_input(&g_glsl_gen, "v4", 2, GLSL_TYPE_VEC4);
  glsl_gen_declare_input(&g_glsl_gen, "m4", 2, GLSL_TYPE_MAT4);
  return glsl_gen_expression(&g_glsl_gen, root);
}

/* **The shapes an operator turns into, and the ones it must refuse.** */
static void test_glsl_gen_selects_arithmetic(void) {
  /* A scalar against a vector broadcasts: three multiplies, all reading the same scalar
   * register, each writing its own component. A generator that indexed the scalar too would
   * read v0, v1, v2 - which are the scalar and then two components of an unrelated variable. */
  glsl_value_t r = glsl_gen_of("v3*f");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 3);
  ASSERT_EQ(g_gen_code.count, 3u);
  for (uint32_t i = 0; i < 3u; i++) {
    ASSERT_EQ(GOPC(g_gen_words[i]), GLSL_VOP2_MUL_F32);
    ASSERT_EQ(GVDST(g_gen_words[i]), r.base + i);
    ASSERT_EQ(GSRC0(g_gen_words[i]), 256u + 1u + i); /* v3's components */
    ASSERT_EQ(GSRC1(g_gen_words[i]), 0u);            /* f, the same register each time */
  }

  /* The same the other way round, which is the same meaning and **not** the same encoding:
   * VOP2 has one biased source and one bare one, so the operands are not interchangeable. */
  r = glsl_gen_of("f*v3");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 3);
  ASSERT_EQ(g_gen_code.count, 3u);
  for (uint32_t i = 0; i < 3u; i++) {
    ASSERT_EQ(GSRC0(g_gen_words[i]), 256u + 0u);     /* f is src0 now */
    ASSERT_EQ(GSRC1(g_gen_words[i]), 1u + i);        /* v3's components are vsrc1 */
  }

  /* Subtraction keeps its operand order - the reversed form is a different opcode. */
  r = glsl_gen_of("v3-v3");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(g_gen_code.count, 3u);
  ASSERT_EQ(GOPC(g_gen_words[0]), GLSL_VOP2_SUB_F32);

  /* Unary minus is zero minus the operand, with the zero inline. */
  r = glsl_gen_of("-v3");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 3);
  ASSERT_EQ(g_gen_code.count, 3u);
  ASSERT_EQ(GOPC(g_gen_words[0]), GLSL_VOP2_SUB_F32);
  ASSERT_EQ(GSRC0(g_gen_words[0]), 128u); /* the inline zero, not a register */

  /* `mat4 * vec4` is a transform, not sixteen component-wise multiplies - and the widths do not
   * match, so the component-wise reading is not merely unsupported, it is wrong. */
  r = glsl_gen_of("m4*v4");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 4);
  ASSERT_EQ(g_gen_code.count, 16u);
  ASSERT_EQ(GOPC(g_gen_words[0]), GLSL_VOP2_MUL_F32);
  ASSERT_EQ(GSRC0(g_gen_words[0]), 256u + 8u); /* m4's first column */
  ASSERT_EQ(GSRC1(g_gen_words[0]), 4u);        /* v4.x */
  ASSERT_EQ(GOPC(g_gen_words[4]), GLSL_VOP2_FMAC_F32);
}

/* A swizzle is a move per component, and a repeated component is a legal read. */
static void test_glsl_gen_selects_swizzles_and_constructors(void) {
  glsl_value_t r = glsl_gen_of("v4.zyx");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 3);
  ASSERT_EQ(g_gen_code.count, 3u);
  ASSERT_EQ(GSRC0(g_gen_words[0]), 256u + 4u + 2u); /* .z */
  ASSERT_EQ(GSRC0(g_gen_words[1]), 256u + 4u + 1u); /* .y */
  ASSERT_EQ(GSRC0(g_gen_words[2]), 256u + 4u + 0u); /* .x */

  /* The colour vocabulary names the same registers as the positional one. */
  r = glsl_gen_of("v4.rgb");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(GSRC0(g_gen_words[0]), 256u + 4u + 0u);
  ASSERT_EQ(GSRC0(g_gen_words[2]), 256u + 4u + 2u);

  /* A repeated component reads the same register twice. */
  r = glsl_gen_of("v4.xx");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 2);
  ASSERT_EQ(GSRC0(g_gen_words[0]), GSRC0(g_gen_words[1]));

  /* `vec4(v3, 1.0)` is four components from two arguments. */
  r = glsl_gen_of("vec4(v3, 1.0)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 4);

  /* One scalar fills a vector... */
  r = glsl_gen_of("vec4(1.0)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 4);
  ASSERT_EQ(g_gen_code.count, 5u); /* the literal, then four broadcasts */

  /* ...but **fills a matrix's diagonal**, which is a different rule. `mat4(1.0)` is the
   * identity, not a matrix of ones - and a matrix of ones transforms every vertex to the same
   * point, which is a black screen on hardware and nothing at all on the host. */
  r = glsl_gen_of("mat4(1.0)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  ASSERT_EQ(r.count, 16);
  ASSERT_EQ(g_gen_code.count, 17u); /* the literal, then sixteen elements */
  /* Element 0 and element 5 are on the diagonal and take the scalar; element 1 is not.
   * The result block is allocated before the arguments are evaluated - v24..v39 for the matrix -
   * so the literal lands above it at v40. */
  ASSERT_EQ(GV1OP(g_gen_words[1]), GLSL_VOP1_MOV_B32);
  ASSERT_EQ(GSRC0(g_gen_words[1]), 256u + 40u); /* the literal's register */
  ASSERT_EQ(GSRC0(g_gen_words[2]), 128u);       /* inline zero, off the diagonal */
  ASSERT_EQ(GSRC0(g_gen_words[6]), 256u + 40u); /* element 5, back on the diagonal */
}

/*
 * **What it refuses, which is the part that keeps it honest.**
 *
 * Every one of these parses and type-checks. A generator that emitted *something* for them would
 * produce a shader that runs and computes the wrong thing, on hardware, with nothing on the host
 * to show for it. Refusing is not a gap to be filled in later so much as the behaviour: an
 * instruction whose encoding has not been read out of an assembler does not get guessed.
 */
static void test_glsl_gen_refuses_what_it_cannot_encode(void) {
  /* Integer arithmetic through the float instructions would be silently wrong. */
  glsl_gen_of("iv3+iv3");
  ASSERT_TRUE(g_glsl_gen.error != NULL);

  /* An **ordering** comparison of two vectors: GLSL spells that `lessThan`, and the answer is a
   * bvec, which is a per-component bool and not the single one this has a register shape for. */
  glsl_gen_of("v3<v3");
  ASSERT_TRUE(g_glsl_gen.error != NULL);

  /* **A matrix with a vector that is not its width.** `m4 * v4` is a product and `m4 * f` is a
   * broadcast; `m4 * v3` is neither, and treating it as componentwise would compute something
   * that is not a product at all rather than say so. `m4 * m4` is generated now - n of the
   * matrix-vector products, one per column. */
  glsl_gen_of("m4*m4");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  glsl_gen_of("m4*v3");
  ASSERT_TRUE(g_glsl_gen.error != NULL);

  /* **`atan` is generated now, and the reason is whose polynomial it is.** There is still no
   * instruction for it; what changed is that the coefficients are not somebody's choice made
   * here but the ones `oops_atan2f` already ships, which is what the software rasteriser answers
   * every `atan` in this SDK through. The two paths compute one function rather than two that
   * agree - see the lowering in `glsl_gen.c`. */
  glsl_gen_of("atan(f)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  glsl_gen_of("asin(f)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* A texture lookup needs descriptors handed to the shader, which the compiled path has not
   * wired up. Refused on the name before the arguments are looked at, which is why this needs
   * no sampler in the fixture. */
  glsl_gen_of("texture2D(v3, v3)");
  ASSERT_TRUE(g_glsl_gen.error != NULL);

  /* There is still no call mechanism, so a user-defined function is refused rather than
   * quietly inlined - and the message says so rather than naming a built-in. */
  glsl_gen_of("notAFunction(f)");
  ASSERT_TRUE(g_glsl_gen.error != NULL);
}

/*
 * **What it now generates that it used to refuse**, each with the reason the refusal lifted.
 *
 * A refusal is evidence of discipline only while the thing is genuinely unencodable; once the
 * opcode is pinned, leaving the refusal in place is just a shader that will not compile. These
 * are the four that moved, and the test is here so the boundary between the two lists is
 * written down rather than remembered.
 */
static void test_glsl_gen_selects_what_the_opcodes_now_allow(void) {
  /* `/` is `v_rcp_f32` and `v_mul_f32`, both in `tools/shader/gl2-fragment.s`. */
  glsl_gen_of("v3/f");
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* A built-in whose lowering is one instruction a component. */
  glsl_gen_of("sqrt(f)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* A swizzle write is a move into each register the swizzle names - not a masked move, because
   * a value here is one VGPR a component with no packing. */
  glsl_gen_of("v4.xy = v4.zw");
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* And a compound assignment is one instruction a component, the destination being its own
   * first operand. */
  glsl_gen_of("v3 *= f");
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* **A bool is a float that is 0.0 or 1.0**, so a comparison is `v_cmp` into `vcc` and a
   * `v_cndmask` straight back out of it - and `&&`, `||` and `!` are then `min`, `max` and
   * `1 - x` with no comparison at all. */
  glsl_gen_of("f<f");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  glsl_gen_of("v3==v3");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  glsl_gen_of("(f<f)&&(f>f)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  glsl_gen_of("!(f<f)");
  ASSERT_TRUE(g_glsl_gen.error == NULL);
  glsl_gen_of("f<f ? v3 : v3");
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* **`min` is only the same as `&&` while the right side does nothing.** When it assigns, the
   * right side runs under a narrowed `exec` instead - the lanes the left operand has not
   * already decided for - and the difference is visible in the words: a `s_and_saveexec_b32`
   * appears where the pure form has none. */
  {
    const uint32_t SAVEEXEC = 60u; /* SOP1 op: `s_and_saveexec_b32 sN, vcc_lo` */
    int saves = 0;
    glsl_gen_of("(f<f)&&(f>f)");
    ASSERT_TRUE(g_glsl_gen.error == NULL);
    for (uint32_t i = 0; i < g_gen_code.count; i++) {
      if ((g_gen_words[i] >> 23) == 0x17du && ((g_gen_words[i] >> 8) & 0xffu) == SAVEEXEC) {
        saves++;
      }
    }
    ASSERT_EQ(saves, 0); /* both sides are pure: `min` and no mask */

    saves = 0;
    glsl_gen_of("(f<f)&&((v3.x=f)<f)");
    ASSERT_TRUE(g_glsl_gen.error == NULL);
    for (uint32_t i = 0; i < g_gen_code.count; i++) {
      if ((g_gen_words[i] >> 23) == 0x17du && ((g_gen_words[i] >> 8) & 0xffu) == SAVEEXEC) {
        saves++;
      }
    }
    ASSERT_EQ(saves, 1); /* the assigning right side runs masked */
  }
}

/* Declarations get a home, initialisers land in it, and a statement's temporaries are reclaimed
 * afterwards - so a long body does not run the register file out on scratch it has finished
 * with. */
static void test_glsl_gen_allocates_registers_for_statements(void) {
  const char *src = "{ float a = 1.0; vec3 b = v3 * a; vec3 c = b + v3; }";
  glsl_parser_t p;
  glsl_parser_init(&p, &g_glsl_ast, src, strlen(src));
  int32_t root = glsl_parse_statement(&p);
  ASSERT_TRUE(p.error == NULL);
  ASSERT_TRUE(root != GLSL_NO_NODE);

  glsl_sema_init(&g_glsl_sema, &g_glsl_ast);
  glsl_declare(&g_glsl_sema, "v3", 2, GLSL_TYPE_VEC3, GL_FALSE);

  glsl_code_init(&g_gen_code, g_gen_words, 256);
  glsl_gen_init(&g_glsl_gen, &g_glsl_ast, &g_glsl_sema, &g_gen_code);
  glsl_value_t in = glsl_gen_declare_input(&g_glsl_gen, "v3", 2, GLSL_TYPE_VEC3);
  ASSERT_EQ(in.count, 3);
  ASSERT_EQ(in.base, 0u);

  ASSERT_TRUE(glsl_gen_stmt(&g_glsl_gen, root) == GL_TRUE);
  ASSERT_TRUE(g_glsl_gen.error == NULL);

  /* Three locals - a (1), b (3), c (3) - on top of the input's three, so seven registers are
   * permanently spoken for. The high-water mark is above that because each statement's
   * temporaries sit on top, and it is the number the shader's stage would have to reserve. */
  ASSERT_TRUE(g_glsl_gen.high_water >= 10u);
  ASSERT_TRUE(g_glsl_gen.high_water < 32u);

  /* The block's variables go out of scope with it. */
  ASSERT_EQ(g_glsl_gen.var_count, 1); /* only the input survives */

  /* And something was actually emitted for all three statements. */
  ASSERT_TRUE(g_gen_code.count >= 10u);
}

#undef GOPC
#undef GVDST
#undef GSRC1
#undef GSRC0
#undef GV1OP

/* **The eight descriptor words had no test at all.** They are what the sampler is told about a
 * texture - where it starts, how wide it is, how far apart its rows are - and every one of them
 * is arithmetic that runs identically on a host. Until now the only way to find out what a
 * texture's descriptor came out as was to put a build on the console and read a log, which is
 * both slow and only available when the console is.
 *
 * The widths are the ones a real port produces: 97 of Neverball's 292 images have a width that
 * is not a multiple of 64, and its `back/` strips - the ones that draw the corrupted background -
 * are 4 and 16 pixels wide. 64 is here as the control, being the width at which the custom pitch
 * is supposed to go quiet.
 *
 * The field encodings are checked against Mesa's, which is the reference implementation for this
 * silicon: WIDTH_LO is two bits at bit 30 of word 1 and WIDTH_HI fourteen at bit 0 of word 2
 * (`S_00A004_WIDTH_LO`/`S_00A008_WIDTH_HI`, ac_descriptors.c:519-522), and the custom pitch is
 * `DEPTH(pitch - 1) | PITCH_MSB((pitch - 1) >> 13)` (`ac_set_mutable_tex_desc_fields`, :707-712). */
static void test_gl_texture_descriptor_describes_the_image_it_was_given(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  static const GLsizei widths[] = {4, 16, 64, 100};
  for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
    const GLsizei w = widths[i];
    const GLsizei h = 8;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* Each row a different value, so a row read from the wrong place is a wrong number rather
     * than a coincidence. Blue carries the column for the same reason. */
    static GLubyte img[256 * 8 * 4];
    for (GLsizei y = 0; y < h; y++) {
      for (GLsizei x = 0; x < w; x++) {
        GLubyte *t = img + (((size_t)y * (size_t)w) + (size_t)x) * 4u;
        t[0] = (GLubyte)(y + 1); t[1] = 0; t[2] = (GLubyte)(x + 1); t[3] = 255;
      }
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    const gl_texture_object_t *stored = test_find_texture(ctx, tex);
    ASSERT_TRUE(stored != NULL && stored->pixels != NULL);

    /* The pitch addrlib gives a linear surface: 256 bytes, which at four bytes a texel is 64. */
    const uint32_t want_pitch = ((uint32_t)w + 63u) & ~63u;
    ASSERT_EQ(stored->pitch, want_pitch);

    /* The size the sampler will read back out of the words. */
    const uint32_t got_w = ((((stored->img_desc[2] & 0x3fffu) << 2) |
                             ((stored->img_desc[1] >> 30) & 3u))) + 1u;
    const uint32_t got_h = ((stored->img_desc[2] >> 14) & 0x3fffu) + 1u;
    ASSERT_EQ(got_w, (uint32_t)w);
    ASSERT_EQ(got_h, (uint32_t)h);

    /* WORD5 PERF_MOD, which Mesa writes as 4 for every gfx10 texture it builds
     * (`S_00A014_PERF_MOD(4)`, ac_descriptors.c:543) and this wrote as 0 until the comparison
     * was made. */
    ASSERT_EQ((stored->img_desc[5] >> 20) & 7u, 4u);

    /* WORD4. Inert when the rows are exactly as wide as the image, the pitch otherwise. */
    if (want_pitch > (uint32_t)w) {
      const uint32_t p1 = want_pitch - 1u;
      ASSERT_EQ(stored->img_desc[4], (p1 & 0x1fffu) | (((p1 >> 13) & 1u) << 13));
    } else {
      ASSERT_EQ(stored->img_desc[4], 0u);
    }

    /* And the bytes are where the descriptor says they are - row y at `pitch` texels, not `w`.
     * A texture whose rows were written at width stride while the sampler reads them at pitch
     * stride is exactly the skewed, banded surface a wrong pitch draws. */
    const GLubyte *p = (const GLubyte *)stored->pixels;
    for (GLsizei y = 0; y < h; y++) {
      const GLubyte *row = p + ((size_t)y * (size_t)stored->pitch) * 4u;
      ASSERT_EQ(row[0], (GLubyte)(y + 1));
      ASSERT_EQ(row[2], (GLubyte)1);
      /* The padding past the image is the sampler's to read whenever it filters near the right
       * edge, and nothing writes it but the zeroing. */
      if (want_pitch > (uint32_t)w) {
        ASSERT_EQ(row[(size_t)w * 4u + 0u], (GLubyte)0);
      }
    }

    glDeleteTextures(1, &tex);
  }

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **A three-byte source read as four is a colour cast, not a crash.** Neverball's level art
 * arrives as PNGs, many of them 24-bit, while its text arrives from the font rasteriser as RGBA -
 * so a GL_RGB expansion that lost a byte somewhere would corrupt exactly the textures that look
 * wrong on the console and spare exactly the one that looks right. That is a specific enough
 * story to be worth refuting rather than believing, and it refutes on a host. */
static void test_gl_rgb_upload_expands_to_opaque_rgba(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  (void)glGetError();

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  /* Four texels, three bytes each, every channel distinguishable from every other. */
  static const GLubyte rgb[4 * 3] = {
    10, 20, 30,
    40, 50, 60,
    70, 80, 90,
    100, 110, 120,
  };
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  const gl_texture_object_t *stored = test_find_texture(ctx, tex);
  ASSERT_TRUE(stored != NULL && stored->pixels != NULL);
  const GLubyte *p = (const GLubyte *)stored->pixels;
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(p[i * 4 + 0], rgb[i * 3 + 0]);
    ASSERT_EQ(p[i * 4 + 1], rgb[i * 3 + 1]);
    ASSERT_EQ(p[i * 4 + 2], rgb[i * 3 + 2]);
    /* Alpha is 1 for a base format with no alpha of its own (GL 2.1, table 3.15). A zero here
     * is an invisible texture under GL_MODULATE with blending on. */
    ASSERT_EQ(p[i * 4 + 3], 255);
  }

  glDeleteTextures(1, &tex);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

void run_unit_tests_gl(void) {
    TEST_SUITE_BEGIN("Fixed-function 3D instrument (oops-gl)");
    RUN_TEST(test_gl_context_lifecycle);
    RUN_TEST(test_gl_state_enables);
    RUN_TEST(test_gl_matrix_transforms);
    RUN_TEST(test_gl_clear_and_draw);
    RUN_TEST(test_gl_vertex_arrays_cube);
    RUN_TEST(test_gl_texture_lifecycle);
    RUN_TEST(test_gl_texture_rendering);
    RUN_TEST(test_gl_lighting_state);
    RUN_TEST(test_gl_lighting_rendering);
    RUN_TEST(test_gl_blending_modes);
    RUN_TEST(test_gl_texture_env_modes);
    RUN_TEST(test_gl_matrix_stack_limits);
    RUN_TEST(test_gl_unsupported_primitive_mode_is_refused);
    RUN_TEST(test_gl_draw_count_separates_empty_from_invalid);
    RUN_TEST(test_gl_draw_elements_refuses_an_unreadable_index_type);
    RUN_TEST(test_gl_strings_are_honest_and_parseable);
    RUN_TEST(test_gl_extension_entry_points_are_the_core_ones);
    RUN_TEST(test_gl_capture_replays_to_identical_pixels);
    RUN_TEST(test_gl_unsupported_state_enums_are_refused);
    RUN_TEST(test_gl_hardware_badge_does_not_overclaim);
    RUN_TEST(test_gl_matrix_builders_match_their_definitions);
    RUN_TEST(test_gl_glu_scales_projects_and_builds_mipmaps);
    RUN_TEST(test_gl_glu_quadrics_wind_and_texture_as_the_spec_says);
    RUN_TEST(test_gl_platonic_solids_are_the_solids_they_claim);
    RUN_TEST(test_gl_bitmap_font_draws_what_it_is_a_picture_of);
    RUN_TEST(test_gl_the_newest_calls_refuse_rather_than_fault);
    RUN_TEST(test_gl_glut_main_loop_dispatches_and_can_be_left);
    RUN_TEST(test_gl_vector_forms_match_their_scalar_twins);
    RUN_TEST(test_gl_first_error_is_the_one_retained);
    RUN_TEST(test_gl_scalar_twins_agree_with_their_vector_forms);
    RUN_TEST(test_gl_display_list_compiles_then_replays);
    RUN_TEST(test_gl_display_list_compile_and_execute_does_both);
    RUN_TEST(test_gl_display_list_refusals);
    RUN_TEST(test_gl_display_list_records_what_gl_compiles);
    RUN_TEST(test_gl_display_list_recursion_is_bounded);
    RUN_TEST(test_gl_tex_sub_image_updates_only_its_rectangle);
    RUN_TEST(test_gl_texture_descriptor_describes_the_image_it_was_given);
    RUN_TEST(test_gl_rgb_upload_expands_to_opaque_rgba);
    RUN_TEST(test_gl_pixel_store_alignment_moves_the_rows);
    RUN_TEST(test_gl_tex_image_refuses_a_format_it_cannot_convert);
    RUN_TEST(test_gl_read_pixels_flips_to_gl_orientation);
    RUN_TEST(test_gl_read_pixels_formats_and_bounds);
    RUN_TEST(test_gl_alpha_test_patches_both_shaders);
    RUN_TEST(test_gl_copy_tex_sub_image_matches_a_read_then_upload);
    RUN_TEST(test_gl_attrib_stack_saves_and_restores);
    RUN_TEST(test_gl_client_attrib_stack_is_its_own_stack);
    RUN_TEST(test_gl_rect_draws_the_quad_the_spec_names);
    RUN_TEST(test_gl_copy_tex_image_allocates_then_copies);
    RUN_TEST(test_gl_every_spelling_reaches_the_same_state);
    RUN_TEST(test_gl_integer_spellings_normalise_only_what_should_be);
    RUN_TEST(test_gl_multitexture_state_is_per_unit);
    RUN_TEST(test_gl_two_texture_units_draw);
    RUN_TEST(test_gl_mat4_invert_round_trips);
    RUN_TEST(test_gl_clip_planes_cut_geometry);
    RUN_TEST(test_gl_stencil_test_and_operations);
    RUN_TEST(test_gl_raster_position_and_pixel_ops);
    RUN_TEST(test_gl_texture_1d_is_its_own_binding_point);
    RUN_TEST(test_gl_compressed_textures_are_refused_not_absent);
    RUN_TEST(test_gl_points_and_lines_expand_to_triangles);
    RUN_TEST(test_gl_fog_blends_by_eye_distance);
    RUN_TEST(test_gl_logic_op_combines_stored_bits);
    RUN_TEST(test_gl_blend_constant_and_factor_rules);
    RUN_TEST(test_gl_vector_queries_answer_every_component);
    RUN_TEST(test_gl_texgen_keeps_the_vertex_own_coordinates);
    RUN_TEST(test_gl_polygon_mode_draws_boundaries);
    RUN_TEST(test_gl_rgba_context_keeps_index_and_sample_state);
    RUN_TEST(test_gl_mipmaps_completeness_and_filtering);
    RUN_TEST(test_gl_evaluators);
    RUN_TEST(test_gl_selection_and_feedback);
    RUN_TEST(test_gl_texture_matrix_and_raster_vertex);
    RUN_TEST(test_gl_pixel_transfer_and_maps);
    RUN_TEST(test_gl_line_and_polygon_stipple);
    RUN_TEST(test_gl_accumulation_buffer);
    RUN_TEST(test_gl_clear_keeps_to_scissor_and_masks);
    RUN_TEST(test_gl_texture_3d);
    RUN_TEST(test_gl_pixel_types_and_store);
    RUN_TEST(test_gl_internal_formats_and_proxies);
    RUN_TEST(test_gl_texture_parameters_and_wrap_modes);
    RUN_TEST(test_gl_texture_lod_parameters);
    RUN_TEST(test_gl_separate_specular_and_rescale_normal);
    RUN_TEST(test_gl_two_sided_lighting);
    RUN_TEST(test_gl_cube_maps);
    RUN_TEST(test_gl_texture_combine);
    RUN_TEST(test_gl_smooth_points_lines_polygons);
    RUN_TEST(test_gl_color_material_writes_the_material);
    RUN_TEST(test_gl_array_types_and_window_pos);
    RUN_TEST(test_gl_secondary_color_and_color_sum);
    RUN_TEST(test_gl_fog_coordinates);
    RUN_TEST(test_gl_pixel_rectangles_are_fragments);
    RUN_TEST(test_gl_cpu_pixel_ops_drain_what_they_wrote);
    RUN_TEST(test_gl_point_parameters_and_multi_draw);
    RUN_TEST(test_gl_lod_bias_and_generate_mipmap);
    RUN_TEST(test_gl_depth_stencil_pixels_and_depth_textures);
    RUN_TEST(test_gl_colour_index_images);
    RUN_TEST(test_gl_projective_texcoords_divide_per_fragment);
    RUN_TEST(test_gl_drawn_clear_and_enable_bit_cover_every_texture_enable);
    RUN_TEST(test_gl_state_table_audit_findings);
    RUN_TEST(test_gl_front_buffer);
    RUN_TEST(test_gl_buffer_mapping_and_occlusion_queries);
    RUN_TEST(test_gl_texgen_generates_and_eye_planes_are_fixed_at_specification);
    RUN_TEST(test_gl_double_matrix_forms_narrow_element_by_element);
    RUN_TEST(test_gl_queries_report_and_refuse);
    RUN_TEST(test_gl_get_tex_image_reads_back_unflipped);
    RUN_TEST(test_gl_per_object_queries_match_their_setters);
    RUN_TEST(test_gl_interleaved_arrays_sets_the_pointers_the_spec_names);
    RUN_TEST(test_gl_buffer_objects_resolve_at_draw_time);
    RUN_TEST(test_gl_element_array_buffer_draws_from_its_offsets);
    RUN_TEST(test_gl_array_element_pulls_from_the_enabled_arrays);
    RUN_TEST(test_gl_integer_lighting_forms_convert_colours_by_range);
    RUN_TEST(test_gl_transpose_matrix_forms_transpose);
    RUN_TEST(test_gl_texture_residency_is_honest);
    RUN_TEST(test_gl_tex_env_hint_and_buffer_selection_refuse_what_is_absent);
    RUN_TEST(test_glsl_lexer_munches_maximally_and_matches_whole_words);
    RUN_TEST(test_glsl_lexer_reads_the_number_forms);
    RUN_TEST(test_glsl_lexer_keeps_position_through_trivia);
    RUN_TEST(test_glsl_lexer_reads_a_small_shader);
    RUN_TEST(test_glsl_parser_binds_by_precedence);
    RUN_TEST(test_glsl_parser_associates_correctly);
    RUN_TEST(test_glsl_parser_reads_postfix_chains);
    RUN_TEST(test_glsl_parser_refuses_malformed_input);
    RUN_TEST(test_glsl_parser_binds_else_to_the_nearest_if);
    RUN_TEST(test_glsl_parser_reads_statements);
    RUN_TEST(test_glsl_parser_reads_declarations);
    RUN_TEST(test_glsl_parser_reads_a_whole_shader);
    RUN_TEST(test_glsl_preprocessor_expands_and_records);
    RUN_TEST(test_glsl_preprocessor_nests_conditionals);
    RUN_TEST(test_glsl_preprocessor_refuses_by_name);
    RUN_TEST(test_glsl_sema_types_the_operators);
    RUN_TEST(test_glsl_sema_checks_swizzles);
    RUN_TEST(test_glsl_sema_checks_constructors);
    RUN_TEST(test_glsl_sema_scopes_and_shadowing);
    RUN_TEST(test_glsl_sema_checks_lvalues);
    RUN_TEST(test_glsl_sema_checks_statements);
    RUN_TEST(test_glsl_sema_checks_functions);
    RUN_TEST(test_glsl_sema_accepts_a_real_shader);
    RUN_TEST(test_glsl_emit_matches_the_assembler);
    RUN_TEST(test_glsl_emit_mat4_is_column_major);
    RUN_TEST(test_gl_blend_control_carries_the_gl_state);
    RUN_TEST(test_gl_proc_address_resolves_entry_points_by_name);
    RUN_TEST(test_glsl_gen_selects_arithmetic);
    RUN_TEST(test_glsl_gen_selects_swizzles_and_constructors);
    RUN_TEST(test_glsl_gen_refuses_what_it_cannot_encode);
    RUN_TEST(test_glsl_gen_selects_what_the_opcodes_now_allow);
    RUN_TEST(test_glsl_gen_allocates_registers_for_statements);
}
