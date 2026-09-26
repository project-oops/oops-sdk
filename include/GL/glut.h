/*
 * oops-glut: the subset of GLUT that GL 1.x programs use, over this SDK's display,
 * input and timing.
 *
 * There is one window, always the display's size. `glutInitDisplayMode` is recorded:
 * the context is always RGBA and double buffered with depth and stencil, and
 * `glutGet(GLUT_DISPLAY_MODE_POSSIBLE)` answers whether a mode can be honoured.
 * `glutMainLoop` returns when freeglut's `glutLeaveMainLoop()` is called. A call is
 * here only when its answer is exact; subwindows, menus, overlays, the proportional and
 * stroke fonts, game mode and the calls that move or resize the window are absent, so a
 * program needing them fails to link.
 */
#ifndef __GLUT_H__
#define __GLUT_H__

#include "gl.h"
#include "glu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* glutInitDisplayMode's bits, GLUT's own values. */
#define GLUT_RGB 0x0000
#define GLUT_RGBA 0x0000
#define GLUT_INDEX 0x0001
#define GLUT_SINGLE 0x0000
#define GLUT_DOUBLE 0x0002
#define GLUT_ACCUM 0x0004
#define GLUT_ALPHA 0x0008
#define GLUT_DEPTH 0x0010
#define GLUT_STENCIL 0x0020
#define GLUT_MULTISAMPLE 0x0080

/*
 * glutGet's queries, each with an exact answer: the window is the display at the
 * origin, colour channels are eight bits and accumulation channels sixteen. Any other
 * query reads as 0, GLUT's answer for one it does not know.
 */
#define GLUT_WINDOW_X 100
#define GLUT_WINDOW_Y 101
#define GLUT_WINDOW_WIDTH 102
#define GLUT_WINDOW_HEIGHT 103
#define GLUT_WINDOW_BUFFER_SIZE 104
#define GLUT_WINDOW_STENCIL_SIZE 105
#define GLUT_WINDOW_DEPTH_SIZE 106
#define GLUT_WINDOW_RED_SIZE 107
#define GLUT_WINDOW_GREEN_SIZE 108
#define GLUT_WINDOW_BLUE_SIZE 109
#define GLUT_WINDOW_ALPHA_SIZE 110
#define GLUT_WINDOW_ACCUM_RED_SIZE 111
#define GLUT_WINDOW_ACCUM_GREEN_SIZE 112
#define GLUT_WINDOW_ACCUM_BLUE_SIZE 113
#define GLUT_WINDOW_ACCUM_ALPHA_SIZE 114
#define GLUT_WINDOW_DOUBLEBUFFER 115
#define GLUT_WINDOW_RGBA 116
#define GLUT_WINDOW_PARENT 117
#define GLUT_WINDOW_NUM_CHILDREN 118
#define GLUT_WINDOW_COLORMAP_SIZE 119
#define GLUT_WINDOW_NUM_SAMPLES 120
#define GLUT_WINDOW_STEREO 121
#define GLUT_WINDOW_CURSOR 122
#define GLUT_SCREEN_WIDTH 200
#define GLUT_SCREEN_HEIGHT 201
#define GLUT_DISPLAY_MODE_POSSIBLE 400
#define GLUT_INIT_WINDOW_X 500
#define GLUT_INIT_WINDOW_Y 501
#define GLUT_INIT_WINDOW_WIDTH 502
#define GLUT_INIT_WINDOW_HEIGHT 503
#define GLUT_INIT_DISPLAY_MODE 504
#define GLUT_ELAPSED_TIME 700

/* glutSetCursor's shapes. There is no cursor on a console, so
 * glutGet(GLUT_WINDOW_CURSOR) answers GLUT_CURSOR_NONE whatever was asked for. */
#define GLUT_CURSOR_RIGHT_ARROW 0
#define GLUT_CURSOR_LEFT_ARROW 1
#define GLUT_CURSOR_INFO 2
#define GLUT_CURSOR_DESTROY 3
#define GLUT_CURSOR_HELP 4
#define GLUT_CURSOR_CYCLE 5
#define GLUT_CURSOR_SPRAY 6
#define GLUT_CURSOR_WAIT 7
#define GLUT_CURSOR_TEXT 8
#define GLUT_CURSOR_CROSSHAIR 9
#define GLUT_CURSOR_INHERIT 100
#define GLUT_CURSOR_NONE 101
#define GLUT_CURSOR_FULL_CROSSHAIR 102

/* glutVisibilityFunc's states. The window is always visible, so the callback is called
 * once with GLUT_VISIBLE when the main loop starts. */
#define GLUT_NOT_VISIBLE 0
#define GLUT_VISIBLE 1

/* glutMouseFunc's buttons and states. */
#define GLUT_LEFT_BUTTON 0
#define GLUT_MIDDLE_BUTTON 1
#define GLUT_RIGHT_BUTTON 2
#define GLUT_DOWN 0
#define GLUT_UP 1

/* glutSpecialFunc's keys. */
#define GLUT_KEY_F1 1
#define GLUT_KEY_F2 2
#define GLUT_KEY_F3 3
#define GLUT_KEY_F4 4
#define GLUT_KEY_F5 5
#define GLUT_KEY_F6 6
#define GLUT_KEY_F7 7
#define GLUT_KEY_F8 8
#define GLUT_KEY_F9 9
#define GLUT_KEY_F10 10
#define GLUT_KEY_F11 11
#define GLUT_KEY_F12 12
#define GLUT_KEY_LEFT 100
#define GLUT_KEY_UP 101
#define GLUT_KEY_RIGHT 102
#define GLUT_KEY_DOWN 103
#define GLUT_KEY_PAGE_UP 104
#define GLUT_KEY_PAGE_DOWN 105
#define GLUT_KEY_HOME 106
#define GLUT_KEY_END 107
#define GLUT_KEY_INSERT 108

/* glutGetModifiers. */
#define GLUT_ACTIVE_SHIFT 1
#define GLUT_ACTIVE_CTRL 2
#define GLUT_ACTIVE_ALT 4

/* Setup. `glutInitWindowPosition` is accepted and ignored and `glutInitWindowSize` is a
 * request; `glutCreateWindow` opens the display and its GL context. */
void glutInit(int *argcp, char **argv);
void glutInitDisplayMode(unsigned int mode);
void glutInitWindowSize(int width, int height);
void glutInitWindowPosition(int x, int y);
int glutCreateWindow(const char *title);
void glutDestroyWindow(int window);

/* The callbacks, dispatched from `glutMainLoop`. */
void glutDisplayFunc(void (*func)(void));
void glutReshapeFunc(void (*func)(int width, int height));
void glutIdleFunc(void (*func)(void));
void glutKeyboardFunc(void (*func)(unsigned char key, int x, int y));
void glutKeyboardUpFunc(void (*func)(unsigned char key, int x, int y));
void glutSpecialFunc(void (*func)(int key, int x, int y));
void glutSpecialUpFunc(void (*func)(int key, int x, int y));
void glutMouseFunc(void (*func)(int button, int state, int x, int y));
void glutMotionFunc(void (*func)(int x, int y));
void glutPassiveMotionFunc(void (*func)(int x, int y));
void glutTimerFunc(unsigned int millis, void (*func)(int value), int value);

void glutVisibilityFunc(void (*func)(int state));

/* The main loop and its control: `glutLeaveMainLoop` (freeglut's) makes `glutMainLoop`
 * return. */
void glutMainLoop(void);
void glutLeaveMainLoop(void);
void glutPostRedisplay(void);
void glutSwapBuffers(void);
int glutGet(GLenum state);
int glutGetModifiers(void);

/*
 * The one window, named and asked about. Each call is already true here: there is one
 * window, it is the display and it fills it. `glutFullScreen` asks for the state the
 * window is always in; `glutSetWindow` accepts the only window; titles are accepted and
 * ignored. `glutReshapeWindow`, `glutPositionWindow` and `glutWarpPointer` are absent.
 */
int glutGetWindow(void);
void glutSetWindow(int window);
void glutSetWindowTitle(const char *title);
void glutSetIconTitle(const char *title);
void glutFullScreen(void);
void glutSetCursor(int cursor);

/*
 * The pad as a keyboard (this library's own, not GLUT's), on by default: the d-pad
 * arrives as GLUT_KEY_LEFT/UP/RIGHT/DOWN through the special callback, cross and circle
 * as `\r` and `\033` through the keyboard callback, and the option button ends the main
 * loop. `glutOopsPadKeys(0)` turns it off for a program that reads the pad itself.
 */
void glutOopsPadKeys(int on);

/*
 * The solids: the sphere, cube, cone and torus over the GLU quadrics, and the teapot,
 * ten Bezier patches mirrored into thirty-two and evaluated through
 * `glMap2f`/`glEvalMesh2`. The teapot data is transcribed from freeglut (MIT), with its
 * provenance in `src/gl/glut_teapot_data.h`.
 */
void glutSolidSphere(GLdouble radius, GLint slices, GLint stacks);
void glutWireSphere(GLdouble radius, GLint slices, GLint stacks);
void glutSolidCube(GLdouble size);
void glutWireCube(GLdouble size);
void glutSolidCone(GLdouble base, GLdouble height, GLint slices, GLint stacks);
void glutWireCone(GLdouble base, GLdouble height, GLint slices, GLint stacks);
void glutSolidTorus(GLdouble inner, GLdouble outer, GLint sides, GLint rings);
void glutWireTorus(GLdouble inner, GLdouble outer, GLint sides, GLint rings);
void glutSolidTeapot(GLdouble size);
void glutWireTeapot(GLdouble size);

/*
 * The Platonic solids, at GLUT's documented radii: 1 for the octahedron and the
 * icosahedron, sqrt(3) for the tetrahedron and the dodecahedron. They take no
 * arguments; scale them with the modelview matrix. The faces are derived from the
 * definitions, not transcribed (`test_gl_platonic_solids_are_the_solids_they_claim`).
 */
void glutSolidTetrahedron(void);
void glutWireTetrahedron(void);
void glutSolidOctahedron(void);
void glutWireOctahedron(void);
void glutSolidDodecahedron(void);
void glutWireDodecahedron(void);
void glutSolidIcosahedron(void);
void glutWireIcosahedron(void);

/*
 * The bitmap font: the two fixed-width fonts, with original glyphs in
 * `src/gl/glut_font.c` that keep GLUT's advance (8 and 9), cell height (13 and 15),
 * baseline and ASCII 32 to 126. The proportional fonts and `glutStrokeCharacter` are
 * absent, since a fixed-width stand-in would report wrong widths. `glutBitmapString`
 * is freeglut's.
 */
extern void *const glutBitmapFixed8x13;
extern void *const glutBitmapFixed9x15;
#define GLUT_BITMAP_8_BY_13 glutBitmapFixed8x13
#define GLUT_BITMAP_9_BY_15 glutBitmapFixed9x15
void glutBitmapCharacter(void *font, int character);
void glutBitmapString(void *font, const unsigned char *string);
int glutBitmapWidth(void *font, int character);
int glutBitmapLength(void *font, const unsigned char *string);
int glutBitmapHeight(void *font);

/*
 * Non-zero when the current context offers extension `name`. Matches whole words only
 * (a plain `strstr` finds `GL_EXT_texture` inside `GL_EXT_texture3D`), and asks both
 * the flat string and the indexed form, so it answers on a core profile too. Returns 0
 * with no current context.
 */
int glutExtensionSupported(const char *name);

/*
 * The GLUT API version, for programs that test `GLUT_API_VERSION` before calling
 * something added later. 4 is the last GLUT API version, and the subset here is a
 * GLUT 3/4 one.
 */
#define GLUT_API_VERSION 4

#ifdef __cplusplus
}
#endif

#endif /* __GLUT_H__ */
