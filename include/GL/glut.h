/*
 * oops-glut: enough of GLUT that a program written against it builds and runs.
 *
 * Most GL 1.x code in the world - the tutorials, the demos, the small games, the
 * homebrew - does not open a window or read input itself. It calls `glutCreateWindow`,
 * registers callbacks and hands control to `glutMainLoop`. Without that, every port
 * begins by rewriting the one part of the program that has nothing to do with what it
 * draws.
 *
 * This is **not** GLUT. It is the subset those programs use, over this SDK's own
 * display, input and timing:
 *
 * - one window, always the display's size. `glutInitWindowPosition` is accepted and
 * ignored, `glutInitWindowSize` is a request, and `glutGet(GLUT_WINDOW_WIDTH)` answers
 * what was opened.
 * - `glutInitDisplayMode` is recorded and mostly implied: the context is always RGBA
 * and double buffered, and depth and stencil always exist. A mode this cannot honour is
 * not refused - it is answered by `glutGet(GLUT_DISPLAY_MODE_POSSIBLE)`, which is what
 * a program that cares asks.
 * - the callbacks below, dispatched from `glutMainLoop`, which returns when
 *   `glutLeaveMainLoop()` is called. A console program that never leaves its loop has
 * no way to stop; freeglut's spelling is used because GLUT has none.
 *
 * **Not here:** subwindows, menus, overlays, the proportional fonts and the stroke
 * font, the teapot, `glutInit`'s display-string form, game mode, and the calls that
 * would move or resize a window that is the display
 * (`glutReshapeWindow`, `glutPositionWindow`, `glutWarpPointer`). A program calling one
 * of those fails to link, which says so where a stub that draws nothing would not.
 *
 * **The line between the two is whether the answer is exact.** `glutFullScreen` is here
 * because the window is permanently full screen, so the call gets what it asked for;
 * `glutReshapeWindow` is not, because it cannot. That is the same test `glutGet` is
 * answered by.
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
 * glutGet. **Every one of these has an exact answer here** - the window is the display,
 * its position is the origin, the channels are eight bits each and the accumulation
 * buffer's are sixteen - which is why they are answered rather than left out. A query
 * this cannot answer exactly is not in the list and reads as 0, GLUT's own answer for
 * one it does not know.
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

/* glutSetCursor. **There is no cursor on a console**, so the state is always
 * GLUT_CURSOR_NONE and glutGet(GLUT_WINDOW_CURSOR) says so whatever was asked for - a
 * program that cares can see what it got rather than being told its request was
 * honoured. */
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

/* glutVisibilityFunc's states. The window is always visible here, so the callback is
 * called once with GLUT_VISIBLE when the main loop starts and never again. */
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

void glutInit(int *argcp, char **argv);
void glutInitDisplayMode(unsigned int mode);
void glutInitWindowSize(int width, int height);
void glutInitWindowPosition(int x, int y);
int glutCreateWindow(const char *title);
void glutDestroyWindow(int window);

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

void glutMainLoop(void);
void glutLeaveMainLoop(void);
void glutPostRedisplay(void);
void glutSwapBuffers(void);
int glutGet(GLenum state);
int glutGetModifiers(void);

/*
 * **The one window, named and asked about.** These are here because on this SDK each
 * one is already true rather than approximated: there is exactly one window, it is the
 * display, and it fills it. `glutFullScreen` asks for a state the window is permanently
 * in; `glutSetWindow` accepts the only window there is; the titles have nowhere to go
 * and are accepted and ignored, as `glutInitWindowPosition` already is.
 *
 * **Not here, because they cannot be honoured:** `glutReshapeWindow` and
 * `glutPositionWindow`, which would have to lie about a window that is the display's
 * size at the display's origin; subwindows and menus; `glutWarpPointer`, there being no
 * pointer to warp. Each fails to link.
 */
int glutGetWindow(void);
void glutSetWindow(int window);
void glutSetWindowTitle(const char *title);
void glutSetIconTitle(const char *title);
void glutFullScreen(void);
void glutSetCursor(int cursor);

/*
 * **The pad as a keyboard** (this library's own, not GLUT's).
 *
 * A GLUT program has no idea what a controller is, and a console often has no keyboard
 * attached, so without this most ports run and cannot be controlled. On by default: the
 * d-pad arrives as GLUT_KEY_LEFT/UP/RIGHT/DOWN through the special callback, the cross
 * and circle buttons as
 * `\r` and `\033` through the keyboard callback, and the option button ends the main
 * loop. `glutOopsPadKeys(0)` turns it off for a program that reads the pad itself.
 */
void glutOopsPadKeys(int on);

/*
 * The solids, over the GLU quadrics: `glutSolidSphere` and its wire twin, the cube, the
 * cone and the torus.
 *
 * **And the teapot**, which is none of those. It is 129 control points in ten Bezier
 * patches, mirrored into thirty-two, evaluated through `glMap2f`/`glEvalMesh2` - the
 * data is transcribed from freeglut (MIT) and carries its provenance in
 * `src/gl/glut_teapot_data.h`. It is here because it is part of the GLUT API rather
 * than a nicety: every GLUT ships one, and a program ported to this platform that calls
 * `glutSolidTeapot` should link.
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
 * **The Platonic solids**, at the radii GLUT's own manual states: 1 for the octahedron
 * and the icosahedron, sqrt(3) for the tetrahedron and the dodecahedron. Each takes no
 * arguments, as GLUT's do - scale them with the modelview matrix.
 *
 * **Their faces are derived, not transcribed.** The vertices come out of the
 * definitions - the tetrahedron is four alternate corners of a cube, the octahedron the
 * six unit axes, the icosahedron three golden rectangles, the dodecahedron a cube with
 * three more - and the faces are found from them by adjacency, so nothing here was
 * copied from another implementation's tables and nothing can be wrong in a way that
 * reads as right. `test_gl_platonic_solids_are_the _solids_they_claim` checks the
 * counts, Euler's formula, the radii and the winding.
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
 * **The bitmap font.** Most GLUT code that draws anything draws text too - a frame
 * counter, a key legend - and draws it with `glutBitmapCharacter`, so a port that
 * cannot call it has to have its text rewritten.
 *
 * The glyphs are drawn in `src/gl/glut_font.c` rather than taken from X11's `fixed`
 * fonts, which is what every other GLUT ships. They are not those shapes and do not
 * pretend to be; what they keep is what a program depends on - the advance (8 and 9),
 * the cell height (13 and 15), the baseline, and every character from space to `~`.
 *
 * **Only the two fixed-width fonts are here.** `GLUT_BITMAP_HELVETICA_*` and
 * `GLUT_BITMAP_TIMES_ROMAN_*` are proportional: offering a fixed-width font under those
 * names would return the wrong width from `glutBitmapWidth` and break the layout of
 * every program that measures before it draws. They are absent, and so is
 * `glutStrokeCharacter`, whose glyphs are line segments rather than pixels.
 *
 * `glutBitmapString` is freeglut's and is here because drawing a whole string in one
 * call is common enough that leaving it out means editing the port.
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
 * Asking the driver what it supports.
 *
 * `glutExtensionSupported` is GLUT's own convenience over `glGetString(GL_EXTENSIONS)`,
 * and ports reach for it constantly - it is how a program decides whether to take an
 * extension path at all. It is here rather than left to each port because getting it
 * *right* is fiddly in a way that is quiet when got wrong: a plain `strstr` matches
 * `GL_EXT_texture` inside `GL_EXT_texture3D`, so a driver offering only the latter gets
 * reported as offering both, the port takes a path that is not there, and the screen
 * goes black with nothing in the log. This implementation matches whole words only, and
 * asks both the flat string and the indexed form, so it keeps answering on a core
 * profile where `glGetString(GL_EXTENSIONS)` returns NULL.
 *
 * Returns non-zero when present. With no current context it returns 0, which is the
 * honest answer and lets a port take its "not supported" path rather than fault.
 */
int glutExtensionSupported(const char *name);

/*
 * The GLUT version a port compiles against. Programs test it with `#ifdef
 * GLUT_API_VERSION` or compare it before calling something added later; mesa-demos'
 * `glinfo` is one. 4 is the last GLUT API version, and the subset here is a GLUT 3/4
 * subset, so 4 is what it reports - the alternative is a port silently taking a GLUT-2
 * path for a function that is present.
 */
#define GLUT_API_VERSION 4

#ifdef __cplusplus
}
#endif

#endif /* __GLUT_H__ */
