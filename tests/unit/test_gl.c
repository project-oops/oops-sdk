/*
 * Unit tests for oops-gl core state machine, matrix stacks, and rasterization
 */

#include "GL/gl.h"
#include "GL/glu.h"
#include "oops/display.h"
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

    /* 16th push should exceed depth (GL_MAX_MODELVIEW_STACK_DEPTH = 16) */
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

void run_unit_tests_gl(void) {
    TEST_SUITE_BEGIN("OpenGL 1.3 Translation Subsystem (oops-gl)");
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
}
