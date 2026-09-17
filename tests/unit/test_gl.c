/*
 * Unit tests for oops-gl core state machine, matrix stacks, and rasterization
 */

#include "GL/gl.h"
#include "GL/glu.h"
#include "oops/display.h"
/* The texture tests read a texture's own storage rather than a sampled result: a sampled check
 * would also pass if an update landed in the wrong place and the sampler happened to fetch the
 * right colour, so the bytes are what say which texels moved. `test_pm4.c` reaches into
 * `agc_internal.h` for the same reason. */
#include "src/gl/gl_internal.h"
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

    /* Lines are declared in the header and not implemented. */
    glBegin(GL_LINES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-0.9f, 0.0f, 0.0f);
    glVertex3f( 0.9f, 0.0f, 0.0f);
    glEnd();
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_EQ(fb[240 / 2 * 320 + 320 / 2], 0xff000000u);

    /* An enum that is not a primitive mode at all takes the same path. */
    glBegin((GLenum)0xdeadbeefu);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* The array entry points refuse it too - three switches used to disagree about this. */
    static const GLfloat verts[9] = {
        -0.5f, -0.5f, 0.0f,  0.5f, -0.5f, 0.0f,  0.0f, 0.5f, 0.0f
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glDrawArrays(GL_LINE_STRIP, 0, 3);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_EQ(fb[240 / 2 * 320 + 320 / 2], 0xff000000u);

    static const GLubyte idx[3] = {0, 1, 2};
    glDrawElements(GL_POLYGON, 3, GL_UNSIGNED_BYTE, idx);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* Positive control: a mode this subset does draw still draws, and raises nothing. */
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

    /* The brand that used to be in GL_RENDERER is not in any of them (conventions 2). */
    const GLubyte *renderer = glGetString(GL_RENDERER);
    ASSERT_TRUE(renderer != NULL);
    ASSERT_TRUE(strstr((const char *)renderer, "PlayStation") == NULL);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* **The extension list names no extension, because none of them is supported.**
     *
     * An extension string is a promise about that extension's own entry points. This used to
     * say `GL_EXT_vertex_array` while `glVertexPointerEXT`, `glDrawArraysEXT` and
     * `glArrayElementEXT` did not exist - only the core spellings did. The same reasoning kept
     * `GL_ARB_vertex_buffer_object` out when buffer objects landed.
     *
     * Asserted by the rule rather than by the exact text, so a genuinely supported extension
     * can be added later: every name in the list must end in a suffix this library actually
     * provides entry points for, and today there are none. */
    const GLubyte *ext = glGetString(GL_EXTENSIONS);
    ASSERT_TRUE(ext != NULL); /* empty, not NULL: "no extensions" is a real answer */
    ASSERT_EQ(ext[0], '\0');
    ASSERT_TRUE(strstr((const char *)ext, "GL_EXT_vertex_array") == NULL);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* An unrecognised name is NULL plus an error, not an empty string that reads as a real
     * answer of "nothing". The contrast with GL_EXTENSIONS above is the point: empty is the
     * true answer to "which extensions", and no answer at all is the true answer to a name
     * this does not know. */
    ASSERT_TRUE(glGetString((GLenum)0x1234u) == NULL);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    glContextDestroy(ctx);
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
static void test_gl_unsupported_state_enums_are_refused(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 320, 240);
    void *ctx = glContextCreate(disp);
    (void)glGetError();

    /* Real GL capabilities this subset does not implement. Only GL_STENCIL_TEST is declared
     * in <GL/gl.h>; the other two are written as their specification values rather than added
     * to the header, because a #define there would read as a claim to support them. */
    glEnable(GL_STENCIL_TEST);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glEnable((GLenum)0x0B60u); /* GL_FOG */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    /* GL_ALPHA_TEST used to be the example here and is implemented now, which is this test
     * doing its job: a refusal that stops being a refusal shows up as a failure rather than as
     * silence. GL_DITHER took its place - a real capability this still does not have. */
    glDisable((GLenum)0x0BD0u); /* GL_DITHER */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    /* And the one that replaced it is accepted, with a function and a reference. */
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDisable(GL_ALPHA_TEST);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The ones it does have, including a light, still work and raise nothing. */
    glEnable(GL_DEPTH_TEST);
    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_TRUE);
    glEnable(GL_LIGHT0);
    glDisable(GL_DEPTH_TEST);
    ASSERT_EQ(glIsEnabled(GL_DEPTH_TEST), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Client arrays. */
    glEnableClientState((GLenum)0x8077u); /* GL_INDEX_ARRAY */
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
     * caller would hash and record as a frame. */
    ASSERT_TRUE(glGetFrameReadback() == NULL);

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

    /* An enum error, then a value error. The enum one came first and must be what is read. */
    glEnable(GL_STENCIL_TEST);          /* GL_INVALID_ENUM */
    glDrawArrays(GL_TRIANGLES, 0, -4);  /* GL_INVALID_VALUE */
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* Reading it cleared it, so the next error is visible - and this time the value error
     * happens first, proving the order is what decides rather than the error code. */
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDrawArrays(GL_TRIANGLES, 0, -4);  /* GL_INVALID_VALUE */
    glEnable(GL_STENCIL_TEST);          /* GL_INVALID_ENUM */
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
    /* The target was validated and the parameter was not: GL_TEXTURE_WRAP_R is a real GL
     * parameter this subset does not keep, and it used to be dropped in silence. */
    glTexParameteri(GL_TEXTURE_2D, (GLenum)0x8072u, GL_REPEAT); /* GL_TEXTURE_WRAP_R */
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
    glLightModelf(GL_LIGHT_MODEL_TWO_SIDE, 0.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

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

  /* A real GL format this does not convert. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, (GLenum)0x1908u /* GL_RGBA */, GL_FLOAT, data);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM); /* the type, not the format */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, (GLenum)0x1234u, GL_UNSIGNED_BYTE, data);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

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

  /* **The vertex-array draws cannot be compiled**, and say so rather than being dropped. */
  static const GLfloat verts[9] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f};
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(3, GL_FLOAT, 0, verts);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
  glEndList();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* glEndList with nothing open is an error. */
  glEndList();
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* Outside a list the same draw is fine - the refusal is about compiling, not the call. */
  glDrawArrays(GL_TRIANGLES, 0, 3);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glDisableClientState(GL_VERTEX_ARRAY);

  /* **Calling a list that was never defined is ignored, not an error.** The specification is
   * explicit, and it is what lets a list reference one compiled later. */
  glCallList(run + 2u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glCallList(999999u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* An index type glCallLists cannot read is refused before it reads one. */
  static const GLubyte names[2] = {1, 2};
  glCallLists(2, GL_FLOAT, names);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glCallLists(2, GL_UNSIGNED_BYTE, names);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx);
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

  /* A type this cannot pack is refused rather than written as bytes. */
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, rgba);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
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

  const uint32_t *untex = (const uint32_t *)((const char *)payload + 0x300);
  const uint32_t *tex = (const uint32_t *)((const char *)payload + 0x200);

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

  /* A mask naming state this does not have is refused, because a push that saved nothing is
   * the leak this function exists to prevent. */
  glPushAttrib(0x00000400u); /* GL_STENCIL_BUFFER_BIT */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glPushAttrib(0x00000080u); /* GL_FOG_BIT */
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

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
  const uint32_t *untex = (const uint32_t *)((const char *)payload + 0x300);
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
  ASSERT_EQ(ctx->array_texcoord.enabled, GL_FALSE);
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
  ASSERT_TRUE(ctx->cur_texcoord[0] == 0.25f && ctx->cur_texcoord[1] == 0.75f);
  glTexCoord2i(3, 7);
  ASSERT_TRUE(ctx->cur_texcoord[0] == 3.0f && ctx->cur_texcoord[1] == 7.0f);
  /* The one-coordinate form leaves t at zero, so it has to clear a t that was set. */
  glTexCoord1f(0.5f);
  ASSERT_TRUE(ctx->cur_texcoord[0] == 0.5f && ctx->cur_texcoord[1] == 0.0f);
  const GLfloat t2f[2] = {0.125f, 0.875f};
  glTexCoord2fv(t2f);
  ASSERT_TRUE(ctx->cur_texcoord[0] == 0.125f && ctx->cur_texcoord[1] == 0.875f);
  const GLdouble t2d[2] = {0.875, 0.125};
  glTexCoord2dv(t2d);
  ASSERT_TRUE(ctx->cur_texcoord[0] == 0.875f && ctx->cur_texcoord[1] == 0.125f);
  const GLint t2i[2] = {5, 9};
  glTexCoord2iv(t2i);
  ASSERT_TRUE(ctx->cur_texcoord[0] == 5.0f && ctx->cur_texcoord[1] == 9.0f);

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
  glGetTexImage(0x0DE0u /* GL_TEXTURE_1D */, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, back);
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
  /* **What is stored, not what was asked for.** GL_RGB went in; every upload is converted to
   * RGBA8, so reporting GL_RGB back would have a program size its readback for three bytes a
   * texel against an image that holds four. */
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_RGBA);
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
  ASSERT_EQ(ctx->array_texcoord.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_color.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_normal.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_vertex.enabled, GL_TRUE);
  ASSERT_EQ(ctx->array_texcoord.size, 2);
  ASSERT_EQ(ctx->array_color.size, 4);
  ASSERT_EQ(ctx->array_vertex.size, 3);
  ASSERT_TRUE(ctx->array_texcoord.pointer == (const void *)base);
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
  ASSERT_EQ(ctx->array_texcoord.enabled, GL_FALSE);
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
  ASSERT_EQ(ctx->array_texcoord.size, 4);
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

  /* The hint: the one that names something real is kept and reported, and the ones that name
   * fog, points and lines are refused because this draws none of those. */
  glGetIntegerv(GL_PERSPECTIVE_CORRECTION_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_DONT_CARE); /* the specification's default */
  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
  iv[1] = GUARD;
  glGetIntegerv(GL_PERSPECTIVE_CORRECTION_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NICEST);
  ASSERT_EQ(iv[1], GUARD);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glHint(0x0C54u /* GL_FOG_HINT */, GL_NICEST);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glHint(0x0C52u /* GL_LINE_SMOOTH_HINT */, GL_FASTEST);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  /* A mode that is not a hint mode is refused before the target is even considered. */
  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_TRIANGLES);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glGetIntegerv(GL_PERSPECTIVE_CORRECTION_HINT, iv);
  ASSERT_EQ(iv[0], (GLint)GL_NICEST); /* unchanged by the refusals */

  /* One surface. GL_BACK names it; GL_NONE would have a program believe it had switched
   * drawing off, which is the refusal that matters most here. */
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glGetIntegerv(GL_DRAW_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK);
  glGetIntegerv(GL_READ_BUFFER, iv);
  ASSERT_EQ(iv[0], (GLint)GL_BACK);

  glDrawBuffer(GL_FRONT);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glDrawBuffer(0u /* GL_NONE, which this deliberately does not define */);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
  glReadBuffer(GL_FRONT);
  ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

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
    RUN_TEST(test_gl_unsupported_state_enums_are_refused);
    RUN_TEST(test_gl_hardware_badge_does_not_overclaim);
    RUN_TEST(test_gl_matrix_builders_match_their_definitions);
    RUN_TEST(test_gl_vector_forms_match_their_scalar_twins);
    RUN_TEST(test_gl_first_error_is_the_one_retained);
    RUN_TEST(test_gl_scalar_twins_agree_with_their_vector_forms);
    RUN_TEST(test_gl_display_list_compiles_then_replays);
    RUN_TEST(test_gl_display_list_compile_and_execute_does_both);
    RUN_TEST(test_gl_display_list_refusals);
    RUN_TEST(test_gl_display_list_recursion_is_bounded);
    RUN_TEST(test_gl_tex_sub_image_updates_only_its_rectangle);
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
}
