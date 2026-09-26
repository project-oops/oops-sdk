/*
 * oops-glut: the subset of GLUT that a GL 1.x program actually calls, over this SDK's
 * display, input and timing. See include/GL/glut.h for what is here and what is not.
 *
 * # The main loop
 *
 * GLUT's contract is that `glutMainLoop` never returns and the program lives in its
 * callbacks. On a console that leaves no way to stop, so `glutLeaveMainLoop` -
 * freeglut's spelling, because GLUT has none - ends it, and `glutMainLoop` returns to
 * whatever called it.
 *
 * One pass of the loop: drain the input devices and dispatch their callbacks, fire any
 * timer that is due, then either redisplay or idle. A program that never calls
 * `glutPostRedisplay` and has no idle callback still polls input, so it can always be
 * left.
 *
 * # Input
 *
 * The keyboard arrives as HID usage codes, which this maps to the ASCII
 * `glutKeyboardFunc` expects and the `GLUT_KEY_*` values `glutSpecialFunc` expects.
 * Only the keys those two callbacks can express are mapped; anything else is dropped
 * rather than delivered as a plausible wrong character.
 *
 * The mouse is relative, and GLUT's callbacks are absolute, so a cursor position is
 * kept here and clamped to the window. It starts at the centre, which is where a
 * program that reads the first motion event expects to find it.
 *
 * The pad is not GLUT's idea at all - see `glutOopsPadKeys`.
 */
#include "GL/glut.h"
#include "gl_internal.h"
#include "oops/gfx.h"
#include "oops/display.h"
#include "oops/input.h"
#include "oops/keyboard.h"
#include "oops/mouse.h"
#include "oops/system.h"
#include "oops/time.h"

#include <string.h> /* strstr, for glutExtensionSupported's whole-word match */

#define GLUT_MAX_TIMERS 16

typedef struct {
    void (*func)(int value);
    int value;
    uint64_t due_ns;
    int used;
} glut_timer_t;

static struct {
    oops_gfx_t *gfx;   /* the display and context, brought up together (oops/gfx.h) */
    int width, height; /* what was opened */
    int want_width, want_height; /* what glutInitWindowSize asked for */
    unsigned int mode;
    int created;
    int running;
    int redisplay;
    int modifiers;
    int mouse_x, mouse_y;
    uint8_t mouse_buttons;
    uint32_t pad_buttons;
    int pad_keys;
    uint64_t start_ns;
    void (*display)(void);
    void (*reshape)(int, int);
    void (*idle)(void);
    void (*keyboard)(unsigned char, int, int);
    void (*keyboard_up)(unsigned char, int, int);
    void (*special)(int, int, int);
    void (*special_up)(int, int, int);
    void (*mouse)(int, int, int, int);
    void (*motion)(int, int);
    void (*passive)(int, int);
    void (*visibility)(int);
    glut_timer_t timers[GLUT_MAX_TIMERS];
} g;

/* ---------------------------------------------------------------------------
 * Setting up
 * --------------------------------------------------------------------------- */

void glutInit(int *argcp, char **argv) {
    (void)argcp;
    (void)argv;
    /* GLUT strips its own arguments here; this has none to strip, and a program that
     * passes
     * `-geometry` gets the window it was given either way. */
    g.want_width = g.want_width ? g.want_width : 1280;
    g.want_height = g.want_height ? g.want_height : 720;
    g.pad_keys = 1;
    oops_time_init();
    g.start_ns = oops_time_get_ns();
}

void glutInitDisplayMode(unsigned int mode) {
    g.mode = mode;
}

void glutInitWindowSize(int width, int height) {
    if (width > 0)
        g.want_width = width;
    if (height > 0)
        g.want_height = height;
}

void glutInitWindowPosition(int x, int y) {
    (void)x;
    (void)y; /* one window, and it is where the display is */
}

int glutCreateWindow(const char *title) {
    (void)title; /* nothing shows it */
    if (g.created)
        return 1;
    if (!g.want_width)
        glutInit((int *)0, (char **)0);
    /* Display and context in one call, made current for us. GLUT wanted a specific
     * size, so it is passed through; a depth buffer and vsync are the GLUT defaults a
     * program expects. */
    g.gfx = oops_gfx_create(&(oops_gfx_desc_t){.width = (uint32_t)g.want_width,
                                               .height = (uint32_t)g.want_height,
                                               .depth = true,
                                               .vsync = true});
    if (!g.gfx)
        return 0;
    {
        uint32_t w = 0, h = 0;
        oops_gfx_extent(g.gfx, &w, &h);
        g.width = (int)w;
        g.height = (int)h;
    }
    g.mouse_x = g.width / 2;
    g.mouse_y = g.height / 2;
    g.created = 1;
    /* The devices a GLUT program expects to exist. A console with none attached simply
     * delivers no events; the pad below is usually what is there. */
    oops_keyboard_init();
    oops_mouse_init();
    oops_input_init();
    oops_system_install_close_handler(); /* so glutMainLoop can end on a dashboard Close
                                          */
    return 1;
}

void glutDestroyWindow(int window) {
    (void)window;
    if (!g.created)
        return;
    oops_gfx_destroy(g.gfx);
    g.gfx = (oops_gfx_t *)0;
    g.created = 0;
    g.running = 0;
}

/* ---------------------------------------------------------------------------
 * The callbacks
 * --------------------------------------------------------------------------- */

void glutDisplayFunc(void (*func)(void)) {
    g.display = func;
}
void glutReshapeFunc(void (*func)(int, int)) {
    g.reshape = func;
}
void glutIdleFunc(void (*func)(void)) {
    g.idle = func;
}
/*
 * Registering a keyboard callback says the program reads characters, so the keyboard
 * stops doubling as a pad for it. Without this, `a` arrives as both the letter and
 * `GLUT_KEY_LEFT`
 * (`oops_input_set_keyboard_as_pad`). A program that registers no keyboard callback
 * keeps the fallback, which is what makes a keyboard usable in a pad-only demo.
 */
void glutKeyboardFunc(void (*func)(unsigned char, int, int)) {
    g.keyboard = func;
    if (func != NULL)
        oops_input_set_keyboard_as_pad(0);
}
void glutKeyboardUpFunc(void (*func)(unsigned char, int, int)) {
    g.keyboard_up = func;
}
void glutSpecialFunc(void (*func)(int, int, int)) {
    g.special = func;
}
void glutSpecialUpFunc(void (*func)(int, int, int)) {
    g.special_up = func;
}
void glutMouseFunc(void (*func)(int, int, int, int)) {
    g.mouse = func;
}
void glutMotionFunc(void (*func)(int, int)) {
    g.motion = func;
}
void glutPassiveMotionFunc(void (*func)(int, int)) {
    g.passive = func;
}
/* The window is visible for as long as the program runs, so this is called once with
 * GLUT_VISIBLE when the main loop starts and never again - which is the whole of what a
 * permanently visible window has to report. */
void glutVisibilityFunc(void (*func)(int)) {
    g.visibility = func;
}
void glutOopsPadKeys(int on) {
    g.pad_keys = on ? 1 : 0;
}

void glutTimerFunc(unsigned int millis, void (*func)(int value), int value) {
    if (!func)
        return;
    for (int i = 0; i < GLUT_MAX_TIMERS; i++) {
        if (g.timers[i].used)
            continue;
        g.timers[i].used = 1;
        g.timers[i].func = func;
        g.timers[i].value = value;
        g.timers[i].due_ns = oops_time_get_ns() + (uint64_t)millis * 1000000ull;
        return;
    }
    /* Out of slots: GLUT has no error for this, and dropping the timer silently would
     * stop a program that reschedules from its own callback. The oldest due one is
     * replaced. */
    int oldest = 0;
    for (int i = 1; i < GLUT_MAX_TIMERS; i++) {
        if (g.timers[i].due_ns < g.timers[oldest].due_ns)
            oldest = i;
    }
    g.timers[oldest].func = func;
    g.timers[oldest].value = value;
    g.timers[oldest].due_ns = oops_time_get_ns() + (uint64_t)millis * 1000000ull;
}

void glutPostRedisplay(void) {
    g.redisplay = 1;
}

void glutSwapBuffers(void) {
    (void)oops_gfx_present(g.gfx);
}

int glutGetModifiers(void) {
    return g.modifiers;
}

int glutGet(GLenum state) {
    switch (state) {
    /* The window is the display, at its origin. */
    case GLUT_WINDOW_X:
        return 0;
    case GLUT_WINDOW_Y:
        return 0;
    case GLUT_WINDOW_WIDTH:
        return g.width;
    case GLUT_WINDOW_HEIGHT:
        return g.height;
    case GLUT_SCREEN_WIDTH:
        return g.width;
    case GLUT_SCREEN_HEIGHT:
        return g.height;
    case GLUT_WINDOW_RGBA:
        return 1;
    case GLUT_WINDOW_DOUBLEBUFFER:
        return 1;
    case GLUT_WINDOW_DEPTH_SIZE:
        return 32;
    case GLUT_WINDOW_STENCIL_SIZE:
        return 8;
    /* Eight bits a channel, thirty-two to a pixel - what the colour buffer is. */
    case GLUT_WINDOW_RED_SIZE:
    case GLUT_WINDOW_GREEN_SIZE:
    case GLUT_WINDOW_BLUE_SIZE:
    case GLUT_WINDOW_ALPHA_SIZE:
        return 8;
    case GLUT_WINDOW_BUFFER_SIZE:
        return 32;
    /* The accumulation buffer's channels are signed 16-bit (gl_raster.c). */
    case GLUT_WINDOW_ACCUM_RED_SIZE:
    case GLUT_WINDOW_ACCUM_GREEN_SIZE:
    case GLUT_WINDOW_ACCUM_BLUE_SIZE:
    case GLUT_WINDOW_ACCUM_ALPHA_SIZE:
        return 16;
    /* One top-level window, no children, no colour map, no multisampling, no stereo -
     * each of these is the state rather than a value this cannot produce. */
    case GLUT_WINDOW_PARENT:
        return 0;
    case GLUT_WINDOW_NUM_CHILDREN:
        return 0;
    case GLUT_WINDOW_COLORMAP_SIZE:
        return 0;
    case GLUT_WINDOW_NUM_SAMPLES:
        return 0;
    case GLUT_WINDOW_STEREO:
        return 0;
    case GLUT_WINDOW_CURSOR:
        return GLUT_CURSOR_NONE;
    /* What glutInitWindowSize and glutInitDisplayMode were given, which is what GLUT
     * reports here - the request, not what was opened. */
    case GLUT_INIT_WINDOW_X:
        return 0;
    case GLUT_INIT_WINDOW_Y:
        return 0;
    case GLUT_INIT_WINDOW_WIDTH:
        return g.want_width;
    case GLUT_INIT_WINDOW_HEIGHT:
        return g.want_height;
    case GLUT_INIT_DISPLAY_MODE:
        return (int)g.mode;
    /* Every mode but colour-index is available; that one this library does not have, so
     * a program that asks is told before it draws nothing. */
    case GLUT_DISPLAY_MODE_POSSIBLE:
        return (g.mode & GLUT_INDEX) ? 0 : 1;
    case GLUT_ELAPSED_TIME:
        return (int)((oops_time_get_ns() - g.start_ns) / 1000000ull);
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * The one window
 *
 * Each of these is exact here rather than approximated - see glut.h for which of GLUT's
 * window calls are deliberately absent instead.
 * --------------------------------------------------------------------------- */

int glutGetWindow(void) {
    return g.created ? 1 : 0;
}

/* GLUT sets the current window; there is one, so naming it is a no-op and naming
 * anything else is the error GLUT makes it - ignored, with the current window
 * unchanged. */
void glutSetWindow(int window) {
    (void)window;
}

/* A title has nowhere to go on this display. Accepted and ignored, as
 * glutInitWindowPosition is: a program that sets one is not asking for anything it can
 * see fail. */
void glutSetWindowTitle(const char *title) {
    (void)title;
}
void glutSetIconTitle(const char *title) {
    (void)title;
}

/* The window already is the display, full screen and at its origin, so this asks for
 * the state it is permanently in. */
void glutFullScreen(void) {}

/* There is no cursor to set. glutGet(GLUT_WINDOW_CURSOR) answers GLUT_CURSOR_NONE
 * whatever is asked for, so a program that checks sees what it got. */
void glutSetCursor(int cursor) {
    (void)cursor;
}

/* ---------------------------------------------------------------------------
 * Input
 * --------------------------------------------------------------------------- */

/* A HID usage code as the character GLUT's keyboard callback takes, or 0 for a key that
 * callback cannot express. Shift is applied to the letters and digits only - the
 * punctuation rows differ by layout, and a wrong character is worse than none. */
static unsigned char glut_usage_ascii(uint16_t usage, uint8_t mods) {
    const int shift = (mods & OOPS_KMOD_SHIFT) != 0;
    if (usage >= 0x04u && usage <= 0x1du) { /* a..z */
        const unsigned char base = (unsigned char)('a' + (usage - 0x04u));
        return shift ? (unsigned char)(base - 32) : base;
    }
    if (usage >= 0x1eu && usage <= 0x26u) { /* 1..9, unshifted only */
        return shift ? 0u : (unsigned char)('1' + (usage - 0x1eu));
    }
    switch (usage) {
    case 0x27u:
        return shift ? 0u : (unsigned char)'0';
    case 0x28u:
        return (unsigned char)'\r'; /* Return */
    case 0x29u:
        return (unsigned char)'\033'; /* Escape */
    case 0x2au:
        return (unsigned char)'\b'; /* Backspace */
    case 0x2bu:
        return (unsigned char)'\t';
    case 0x2cu:
        return (unsigned char)' ';
    case 0x2du:
        return shift ? 0u : (unsigned char)'-';
    case 0x2eu:
        return shift ? 0u : (unsigned char)'=';
    case 0x36u:
        return shift ? 0u : (unsigned char)',';
    case 0x37u:
        return shift ? 0u : (unsigned char)'.';
    case 0x38u:
        return shift ? 0u : (unsigned char)'/';
    default:
        return 0u;
    }
}

/* The same code as one of glutSpecialFunc's keys, or 0. */
static int glut_usage_special(uint16_t usage) {
    if (usage >= 0x3au && usage <= 0x45u)
        return GLUT_KEY_F1 + (int)(usage - 0x3au); /* F1..F12 */
    switch (usage) {
    case 0x4fu:
        return GLUT_KEY_RIGHT;
    case 0x50u:
        return GLUT_KEY_LEFT;
    case 0x51u:
        return GLUT_KEY_DOWN;
    case 0x52u:
        return GLUT_KEY_UP;
    case 0x4bu:
        return GLUT_KEY_PAGE_UP;
    case 0x4eu:
        return GLUT_KEY_PAGE_DOWN;
    case 0x4au:
        return GLUT_KEY_HOME;
    case 0x4du:
        return GLUT_KEY_END;
    case 0x49u:
        return GLUT_KEY_INSERT;
    default:
        return 0;
    }
}

static void glut_pump_keyboard(void) {
    oops_key_event_t ev[OOPS_MAX_KEY_EVENTS];
    const int n = oops_keyboard_read(ev, OOPS_MAX_KEY_EVENTS);
    for (int i = 0; i < n; i++) {
        g.modifiers = 0;
        if (ev[i].modifiers & OOPS_KMOD_SHIFT)
            g.modifiers |= GLUT_ACTIVE_SHIFT;
        if (ev[i].modifiers & OOPS_KMOD_CTRL)
            g.modifiers |= GLUT_ACTIVE_CTRL;
        if (ev[i].modifiers & OOPS_KMOD_ALT)
            g.modifiers |= GLUT_ACTIVE_ALT;
        const int down = ev[i].transition == OOPS_KEY_DOWN;
        const int special = glut_usage_special(ev[i].usage);
        if (special) {
            if (down && g.special)
                g.special(special, g.mouse_x, g.mouse_y);
            if (!down && g.special_up)
                g.special_up(special, g.mouse_x, g.mouse_y);
            continue;
        }
        const unsigned char ch = glut_usage_ascii(ev[i].usage, ev[i].modifiers);
        if (!ch)
            continue;
        if (down && g.keyboard)
            g.keyboard(ch, g.mouse_x, g.mouse_y);
        if (!down && g.keyboard_up)
            g.keyboard_up(ch, g.mouse_x, g.mouse_y);
    }
}

static void glut_pump_mouse(void) {
    oops_mouse_state_t s[OOPS_MAX_MOUSE_SAMPLES];
    const int n = oops_mouse_read(s, OOPS_MAX_MOUSE_SAMPLES);
    for (int i = 0; i < n; i++) {
        if (s[i].dx || s[i].dy) {
            g.mouse_x += s[i].dx;
            g.mouse_y += s[i].dy;
            if (g.mouse_x < 0)
                g.mouse_x = 0;
            if (g.mouse_y < 0)
                g.mouse_y = 0;
            if (g.mouse_x >= g.width)
                g.mouse_x = g.width - 1;
            if (g.mouse_y >= g.height)
                g.mouse_y = g.height - 1;
            /* GLUT's rule: motion while a button is down, passive motion otherwise. */
            if (g.mouse_buttons && g.motion)
                g.motion(g.mouse_x, g.mouse_y);
            else if (!g.mouse_buttons && g.passive)
                g.passive(g.mouse_x, g.mouse_y);
        }
        const uint8_t changed = (uint8_t)(s[i].buttons ^ g.mouse_buttons);
        if (changed && g.mouse) {
            static const struct {
                uint8_t bit;
                int button;
            } map[3] = {
                {OOPS_MOUSE_LEFT, GLUT_LEFT_BUTTON},
                {OOPS_MOUSE_MIDDLE, GLUT_MIDDLE_BUTTON},
                {OOPS_MOUSE_RIGHT, GLUT_RIGHT_BUTTON},
            };
            for (int b = 0; b < 3; b++) {
                if (!(changed & map[b].bit))
                    continue;
                g.mouse(map[b].button,
                        (s[i].buttons & map[b].bit) ? GLUT_DOWN : GLUT_UP, g.mouse_x,
                        g.mouse_y);
            }
        }
        g.mouse_buttons = s[i].buttons;
    }
}

/*
 * The pad as keys. A GLUT program has no notion of a controller, and a console often
 * has no keyboard, so without this many ports run and cannot be controlled. Edges only
 * - a held button is one event, as a key press is - and the option button ends the
 * loop, which is the way out of a program whose own is a window close.
 */
static void glut_pump_pad(void) {
    if (!g.pad_keys)
        return;
    oops_pad_state_t pad;
    if (oops_input_poll(0u, &pad) != 0)
        return;
    const uint32_t now = pad.buttons, was = g.pad_buttons;
    const uint32_t pressed = now & ~was, released = was & ~now;
    g.pad_buttons = now;
    static const struct {
        uint32_t bit;
        int special;
        unsigned char ch;
    } map[] = {
        {OOPS_BUTTON_LEFT, GLUT_KEY_LEFT, 0u},
        {OOPS_BUTTON_UP, GLUT_KEY_UP, 0u},
        {OOPS_BUTTON_RIGHT, GLUT_KEY_RIGHT, 0u},
        {OOPS_BUTTON_DOWN, GLUT_KEY_DOWN, 0u},
        {OOPS_BUTTON_CROSS, 0, (unsigned char)'\r'},
        {OOPS_BUTTON_CIRCLE, 0, (unsigned char)'\033'},
        {OOPS_BUTTON_SQUARE, 0, (unsigned char)' '},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (pressed & map[i].bit) {
            if (map[i].special && g.special)
                g.special(map[i].special, g.mouse_x, g.mouse_y);
            else if (map[i].ch && g.keyboard)
                g.keyboard(map[i].ch, g.mouse_x, g.mouse_y);
        }
        if (released & map[i].bit) {
            if (map[i].special && g.special_up)
                g.special_up(map[i].special, g.mouse_x, g.mouse_y);
            else if (map[i].ch && g.keyboard_up)
                g.keyboard_up(map[i].ch, g.mouse_x, g.mouse_y);
        }
    }
    if (pressed & OOPS_BUTTON_OPTIONS)
        glutLeaveMainLoop();
}

static void glut_fire_timers(void) {
    const uint64_t now = oops_time_get_ns();
    for (int i = 0; i < GLUT_MAX_TIMERS; i++) {
        if (!g.timers[i].used || g.timers[i].due_ns > now)
            continue;
        void (*f)(int) = g.timers[i].func;
        const int v = g.timers[i].value;
        /* Cleared before the call, so a callback that reschedules itself gets this
         * slot. */
        g.timers[i].used = 0;
        g.timers[i].func = (void (*)(int))0;
        if (f)
            f(v);
    }
}

/* ---------------------------------------------------------------------------
 * The loop
 * --------------------------------------------------------------------------- */

void glutLeaveMainLoop(void) {
    g.running = 0;
}

void glutMainLoop(void) {
    if (!g.created && !glutCreateWindow(""))
        return;
    g.running = 1;
    /* GLUT delivers one reshape before the first display, which is where a program sets
     * its projection, and one visibility event for a window that has just come up. */
    if (g.reshape)
        g.reshape(g.width, g.height);
    if (g.visibility)
        g.visibility(GLUT_VISIBLE);
    g.redisplay = 1;
    while (g.running) {
        /* Cooperate with the dashboard's Close (oops/system.h): end the loop so
         * glutMainLoop returns and the program's own teardown runs, rather than the
         * program being killed mid-frame. Every GLUT program gets this without a line
         * of its own. */
        if (oops_system_close_requested())
            glutLeaveMainLoop();
        /* **And be suspendable, which the Close above does not cover.** The kernel
         * suspends a big-app asynchronously - after a Close, and for rest mode with no
         * signal at all - and kills it at `0xa0d0c00f` if it has not reached a suspend
         * point within a hundred seconds. Servicing the system's queue every frame is
         * half of that; the drain on the way out is the other half, below. The same
         * pair the SDL backend does, so a GLUT title is no more likely to be killed for
         * resting than an SDL one. */
        (void)oops_system_pump_events();
        glut_pump_keyboard();
        glut_pump_mouse();
        glut_pump_pad();
        glut_fire_timers();
        if (!g.running)
            break;
        if (g.redisplay && g.display) {
            g.redisplay = 0;
            g.display();
        } else if (g.idle) {
            g.idle();
        }
    }
    /* Out of the loop for any reason - a Close, or the program's own
       `glutLeaveMainLoop`. Empty the queue once more and drain the renderer, so
       whatever the caller does next happens on a quiesced process and the kernel has a
       suspend point to find. */
    oops_system_prepare_for_suspend();
}

/* ---------------------------------------------------------------------------
 * The solids
 * --------------------------------------------------------------------------- */

static void glut_quadric(GLenum style, void (*body)(GLUquadric *)) {
    GLUquadric *q = gluNewQuadric();
    if (!q)
        return;
    gluQuadricDrawStyle(q, style);
    gluQuadricNormals(q, GLU_SMOOTH);
    body(q);
    gluDeleteQuadric(q);
}

static GLdouble s_radius, s_height, s_base;
static GLint s_slices, s_stacks;
static void glut_sphere_body(GLUquadric *q) {
    gluSphere(q, s_radius, s_slices, s_stacks);
}
static void glut_cone_body(GLUquadric *q) {
    gluCylinder(q, s_base, 0.0, s_height, s_slices, s_stacks);
}

void glutSolidSphere(GLdouble radius, GLint slices, GLint stacks) {
    s_radius = radius;
    s_slices = slices;
    s_stacks = stacks;
    glut_quadric(GLU_FILL, glut_sphere_body);
}

void glutWireSphere(GLdouble radius, GLint slices, GLint stacks) {
    s_radius = radius;
    s_slices = slices;
    s_stacks = stacks;
    glut_quadric(GLU_LINE, glut_sphere_body);
}

void glutSolidCone(GLdouble base, GLdouble height, GLint slices, GLint stacks) {
    s_base = base;
    s_height = height;
    s_slices = slices;
    s_stacks = stacks;
    glut_quadric(GLU_FILL, glut_cone_body);
}

void glutWireCone(GLdouble base, GLdouble height, GLint slices, GLint stacks) {
    s_base = base;
    s_height = height;
    s_slices = slices;
    s_stacks = stacks;
    glut_quadric(GLU_LINE, glut_cone_body);
}

/* The cube, centred on the origin with sides of `size`, one quad a face with its
 * outward normal. GLUT's is the same six faces; nothing here is sampled from it. */
static void glut_cube(GLdouble size, GLenum mode) {
    const GLfloat h = (GLfloat)(size * 0.5);
    static const GLfloat n[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                    {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
    static const int f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6},
                                {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    const GLfloat v[8][3] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                             {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    for (int i = 0; i < 6; i++) {
        glBegin(mode == GLU_FILL ? GL_QUADS : GL_LINE_LOOP);
        glNormal3f(n[i][0], n[i][1], n[i][2]);
        for (int k = 0; k < 4; k++)
            glVertex3f(v[f[i][k]][0], v[f[i][k]][1], v[f[i][k]][2]);
        glEnd();
    }
}

void glutSolidCube(GLdouble size) {
    glut_cube(size, GLU_FILL);
}
void glutWireCube(GLdouble size) {
    glut_cube(size, GLU_LINE);
}

/*
 * **The Platonic solids, derived rather than transcribed.**
 *
 * Every GLUT ships these as literal tables of vertices and face indices. Copying one
 * would be taking another implementation's data, and writing a table out by hand is the
 * kind of work that is wrong in one entry and looks right everywhere - a dodecahedron
 * with one face wound backwards draws almost correctly under the default cull. So the
 * vertices come out of the definitions and the faces are found from the vertices.
 *
 * The vertices:
 *   - tetrahedron: four alternate corners of the cube (+-1, +-1, +-1) - those with an
 * even number of minus signs. Radius sqrt(3).
 *   - octahedron: the six unit axes. Radius 1.
 *   - icosahedron: the twelve corners of three mutually perpendicular golden
 * rectangles, (0, +-1, +-phi) and its two cyclic rotations, scaled to radius 1.
 *   - dodecahedron: the cube's eight corners plus the twelve points (0, +-1/phi, +-phi)
 * and its rotations, all of which are at radius sqrt(3) as the cube's corners are.
 *
 * The faces: for the three triangular solids, a face is any three vertices that are
 * pairwise the shortest distance apart in the solid - which is what "edge" means. For
 * the dodecahedron, whose faces are pentagons, the twelve face normals are the
 * icosahedron's twelve vertices, because the two solids are duals; each face is the
 * five vertices furthest along its normal, put in order around it.
 *
 * The radii are GLUT's documented ones - 1 for the octahedron and the icosahedron,
 * sqrt(3) for the other two - so a program that scales expecting GLUT's sizes gets
 * them.
 */
#define GLUT_POLY_MAX_V 20
#define GLUT_POLY_MAX_F 20
#define GLUT_POLY_MAX_FV 5

typedef struct {
    float v[GLUT_POLY_MAX_V][3];
    int f[GLUT_POLY_MAX_F][GLUT_POLY_MAX_FV];
    int nv, nf, fv;
} glut_poly_t;

static float glut_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void glut_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float glut_dist2(const float a[3], const float b[3]) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return glut_dot(d, d);
}

static void glut_normalize(float v[3]) {
    const float n = gl_sqrt(glut_dot(v, v));
    if (n > 0.0f) {
        v[0] /= n;
        v[1] /= n;
        v[2] /= n;
    }
}

/* Every three vertices that are pairwise an edge apart, wound counter-clockwise seen
 * from outside - which for a solid centred on the origin means the face's own outward
 * direction. */
static void glut_poly_triangles(glut_poly_t *p) {
    float emin = 0.0f;
    for (int i = 0; i < p->nv; i++) {
        for (int j = i + 1; j < p->nv; j++) {
            const float d = glut_dist2(p->v[i], p->v[j]);
            if (emin == 0.0f || d < emin)
                emin = d;
        }
    }
    const float tol = emin * 1e-3f;
    p->fv = 3;
    p->nf = 0;
    for (int i = 0; i < p->nv; i++) {
        for (int j = i + 1; j < p->nv; j++) {
            if (glut_dist2(p->v[i], p->v[j]) > emin + tol)
                continue;
            for (int k = j + 1; k < p->nv; k++) {
                if (glut_dist2(p->v[i], p->v[k]) > emin + tol)
                    continue;
                if (glut_dist2(p->v[j], p->v[k]) > emin + tol)
                    continue;
                if (p->nf >= GLUT_POLY_MAX_F)
                    return;
                float e1[3], e2[3], n[3];
                for (int c = 0; c < 3; c++) {
                    e1[c] = p->v[j][c] - p->v[i][c];
                    e2[c] = p->v[k][c] - p->v[i][c];
                }
                glut_cross(e1, e2, n);
                const int flip = glut_dot(n, p->v[i]) < 0.0f;
                p->f[p->nf][0] = i;
                p->f[p->nf][1] = flip ? k : j;
                p->f[p->nf][2] = flip ? j : k;
                p->nf++;
            }
        }
    }
}

/* The dodecahedron's twelve pentagons, one per direction in `normals`: the five
 * vertices furthest along it, ordered around it.
 *
 * **Ordered without an arc tangent.** The key below runs from 0 to 4 as the angle runs
 * from 0 to 2pi - the first half from the dot product, the second from its reflection -
 * which is monotone in the angle and enough to sort by. */
static void glut_poly_pentagons(glut_poly_t *p, const float (*normals)[3], int count) {
    p->fv = 5;
    p->nf = 0;
    for (int fi = 0; fi < count && p->nf < GLUT_POLY_MAX_F; fi++) {
        const float *n = normals[fi];
        int idx[GLUT_POLY_MAX_FV];
        int used[GLUT_POLY_MAX_V];
        for (int i = 0; i < p->nv; i++)
            used[i] = 0;
        for (int s = 0; s < 5; s++) {
            int best = -1;
            float bestd = 0.0f;
            for (int i = 0; i < p->nv; i++) {
                if (used[i])
                    continue;
                const float d = glut_dot(p->v[i], n);
                if (best < 0 || d > bestd) {
                    best = i;
                    bestd = d;
                }
            }
            used[best] = 1;
            idx[s] = best;
        }
        /* An in-plane frame: u towards the first vertex, w perpendicular to it around
         * n. */
        float u[3], w[3];
        const float c0 = glut_dot(p->v[idx[0]], n);
        for (int c = 0; c < 3; c++)
            u[c] = p->v[idx[0]][c] - c0 * n[c];
        glut_normalize(u);
        glut_cross(n, u, w);
        float key[GLUT_POLY_MAX_FV];
        for (int s = 0; s < 5; s++) {
            float r[3];
            const float cs = glut_dot(p->v[idx[s]], n);
            for (int c = 0; c < 3; c++)
                r[c] = p->v[idx[s]][c] - cs * n[c];
            glut_normalize(r);
            const float t = glut_dot(r, u), sgn = glut_dot(r, w);
            key[s] = (sgn >= 0.0f) ? (1.0f - t) : (3.0f + t);
        }
        for (int a = 1; a < 5;
             a++) { /* five values: an insertion sort is the whole of it */
            const float kv = key[a];
            const int iv = idx[a];
            int b = a - 1;
            while (b >= 0 && key[b] > kv) {
                key[b + 1] = key[b];
                idx[b + 1] = idx[b];
                b--;
            }
            key[b + 1] = kv;
            idx[b + 1] = iv;
        }
        for (int s = 0; s < 5; s++)
            p->f[p->nf][s] = idx[s];
        p->nf++;
    }
}

/*
 * Twelve directions at radius 1, the corners of three perpendicular golden rectangles -
 * an icosahedron.
 *
 * **There are two of them and they are not the same one.** Putting the long side of
 * each rectangle after the short gives (0, +-1, +-phi) and its rotations; putting it
 * first gives (0, +-phi, +-1). Both are regular icosahedra, mirror images of each
 * other, and only the second is the dual of the dodecahedron as its vertices are
 * written below - so only the second is the set of that dodecahedron's face normals.
 * Using the first put five vertices from three different faces into each pentagon, and
 * `test_gl_platonic_solids_are_the_solids_they_claim` caught it on the planarity check:
 * a set of five furthest along a direction that does not lie in one plane is not a
 * face.
 */
static void glut_icosahedron_points(float out[12][3], int long_first) {
    const float phi = 1.61803398874989484820f; /* (1 + sqrt(5)) / 2 */
    int n = 0;
    for (int axis = 0; axis < 3; axis++) {
        for (int sa = -1; sa <= 1; sa += 2) {
            for (int sb = -1; sb <= 1; sb += 2) {
                float v[3] = {0.0f, 0.0f, 0.0f};
                v[(axis + 1) % 3] = long_first ? (float)sa * phi : (float)sa;
                v[(axis + 2) % 3] = long_first ? (float)sb : (float)sb * phi;
                glut_normalize(v);
                out[n][0] = v[0];
                out[n][1] = v[1];
                out[n][2] = v[2];
                n++;
            }
        }
    }
}

static void glut_build_poly(glut_poly_t *p, int which) {
    const float phi = 1.61803398874989484820f;
    p->nv = 0;
    if (which ==
        0) { /* tetrahedron: the cube corners with an even number of minus signs */
        for (int i = 0; i < 8; i++) {
            const float x = (i & 1) ? 1.0f : -1.0f;
            const float y = (i & 2) ? 1.0f : -1.0f;
            const float z = (i & 4) ? 1.0f : -1.0f;
            if (x * y * z < 0.0f)
                continue;
            p->v[p->nv][0] = x;
            p->v[p->nv][1] = y;
            p->v[p->nv][2] = z;
            p->nv++;
        }
        glut_poly_triangles(p);
    } else if (which == 1) { /* octahedron: the six unit axes */
        for (int axis = 0; axis < 3; axis++) {
            for (int s = -1; s <= 1; s += 2) {
                p->v[p->nv][0] = p->v[p->nv][1] = p->v[p->nv][2] = 0.0f;
                p->v[p->nv][axis] = (float)s;
                p->nv++;
            }
        }
        glut_poly_triangles(p);
    } else if (which == 2) { /* icosahedron */
        float pts[12][3];
        glut_icosahedron_points(pts, 0);
        for (int i = 0; i < 12; i++) {
            p->v[i][0] = pts[i][0];
            p->v[i][1] = pts[i][1];
            p->v[i][2] = pts[i][2];
        }
        p->nv = 12;
        glut_poly_triangles(p);
    } else { /* dodecahedron: the cube's corners and three more rectangles, radius
                sqrt(3) */
        for (int i = 0; i < 8; i++) {
            p->v[p->nv][0] = (i & 1) ? 1.0f : -1.0f;
            p->v[p->nv][1] = (i & 2) ? 1.0f : -1.0f;
            p->v[p->nv][2] = (i & 4) ? 1.0f : -1.0f;
            p->nv++;
        }
        for (int axis = 0; axis < 3; axis++) {
            for (int sa = -1; sa <= 1; sa += 2) {
                for (int sb = -1; sb <= 1; sb += 2) {
                    p->v[p->nv][0] = p->v[p->nv][1] = p->v[p->nv][2] = 0.0f;
                    p->v[p->nv][(axis + 1) % 3] = (float)sa / phi;
                    p->v[p->nv][(axis + 2) % 3] = (float)sb * phi;
                    p->nv++;
                }
            }
        }
        /* The dual's directions, the long side first - see glut_icosahedron_points. */
        float normals[12][3];
        glut_icosahedron_points(normals, 1);
        glut_poly_pentagons(p, normals, 12);
    }
}

/* The normal is the face's own centre direction, which for a solid centred on the
 * origin is its outward normal exactly - the same value for every vertex of the face,
 * as a flat face wants. */
static void glut_draw_poly(int which, GLenum mode) {
    glut_poly_t p;
    glut_build_poly(&p, which);
    for (int i = 0; i < p.nf; i++) {
        float n[3] = {0.0f, 0.0f, 0.0f};
        for (int s = 0; s < p.fv; s++) {
            for (int c = 0; c < 3; c++)
                n[c] += p.v[p.f[i][s]][c];
        }
        glut_normalize(n);
        glBegin(mode == GLU_FILL ? (p.fv == 3 ? GL_TRIANGLES : GL_POLYGON)
                                 : GL_LINE_LOOP);
        glNormal3f(n[0], n[1], n[2]);
        for (int s = 0; s < p.fv; s++) {
            const float *v = p.v[p.f[i][s]];
            glVertex3f(v[0], v[1], v[2]);
        }
        glEnd();
    }
}

void glutSolidTetrahedron(void) {
    glut_draw_poly(0, GLU_FILL);
}
void glutWireTetrahedron(void) {
    glut_draw_poly(0, GLU_LINE);
}
void glutSolidOctahedron(void) {
    glut_draw_poly(1, GLU_FILL);
}
void glutWireOctahedron(void) {
    glut_draw_poly(1, GLU_LINE);
}
void glutSolidIcosahedron(void) {
    glut_draw_poly(2, GLU_FILL);
}
void glutWireIcosahedron(void) {
    glut_draw_poly(2, GLU_LINE);
}
void glutSolidDodecahedron(void) {
    glut_draw_poly(3, GLU_FILL);
}
void glutWireDodecahedron(void) {
    glut_draw_poly(3, GLU_LINE);
}

/* The torus: `inner` is the tube's radius and `outer` the distance from the origin to
 * the tube's centre, which is GLUT's meaning of the two and not the other reading. */
static void glut_torus(GLdouble inner, GLdouble outer, GLint sides, GLint rings,
                       GLenum mode) {
    if (sides < 3)
        sides = 3;
    if (rings < 3)
        rings = 3;
    const float two_pi = 6.28318530717958647692f;
    const float dphi = two_pi / (float)rings, dtheta = two_pi / (float)sides;
    for (GLint i = 0; i < rings; i++) {
        glBegin(mode == GLU_FILL ? GL_QUAD_STRIP : GL_LINE_STRIP);
        for (GLint j = 0; j <= sides; j++) {
            const float theta = (float)(j % sides) * dtheta;
            const float ct = gl_cos(theta), st = gl_sin(theta);
            for (int e = 0; e < 2; e++) {
                const float phi = (float)(i + (1 - e)) * dphi;
                const float cp = gl_cos(phi), sp = gl_sin(phi);
                const float nx = ct * cp, ny = ct * sp, nz = st;
                const float r = (float)outer + (float)inner * ct;
                glNormal3f(nx, ny, nz);
                glVertex3f(r * cp, r * sp, (float)inner * st);
            }
        }
        glEnd();
    }
}

void glutSolidTorus(GLdouble inner, GLdouble outer, GLint sides, GLint rings) {
    glut_torus(inner, outer, sides, rings, GLU_FILL);
}

void glutWireTorus(GLdouble inner, GLdouble outer, GLint sides, GLint rings) {
    glut_torus(inner, outer, sides, rings, GLU_LINE);
}

/*
 * The teapot, and it is the only solid here that is not generated.
 *
 * Every other shape in this file comes out of arithmetic - a quadric, or the vertices
 * of a platonic solid. The teapot is 129 measured control points that Martin Newell
 * digitised off his own teapot in 1975, so it is transcribed rather than derived, and
 * it carries its source with it in `glut_teapot_data.h`.
 *
 * # Ten patches become thirty-two by symmetry
 *
 * The data holds ten bicubic Bezier patches and the rest of the pot is their mirrors,
 * which is why 129 points describe a shape that renders as thirty-two patches. The
 * data's z is up and the body is a surface of revolution about it, so:
 *
 *   - the rim, body, lid and bottom (patches 0-5) are drawn four times: as given,
 * mirrored in x, mirrored in y, and mirrored in both, which walks them round all four
 * quadrants;
 *   - the handle and spout (patches 6-9) lie in the x-z plane and are drawn twice,
 * mirrored in y only - a teapot has one of each, not four.
 *
 * That split is the reason for the `i < 6` below, and it is freeglut's own comment on
 * the data turned into code.
 *
 * # Why the transform looks arbitrary
 *
 * `glutSolidTeapot(size)` is expected to put a teapot of roughly `size` units at the
 * origin, y-up. The data is z-up, off-centre, and about two units tall, so the
 * rotate-scale-translate here is the fixed correction every GLUT has applied since
 * 1994. Matching it matters more than tidying it: a program ported to this platform
 * draws the teapot it expects, at the size and orientation it expects, or the port is
 * not a port.
 *
 * `GL_AUTO_NORMAL` is what makes this shade. The evaluator differentiates the surface
 * and emits a normal per vertex, so nothing here computes one - which is just as well,
 * because mirroring a patch reverses its winding and hand-computed normals would point
 * inwards on half the pot.
 */
#include "glut_teapot_data.h"

/* Patches a side, per patch. GLUT's own default, and enough that the silhouette is
 * smooth at the sizes a title draws this. */
#define GLUT_TEAPOT_GRID 14

static void glut_teapot(GLdouble size, GLenum mode) {
    /* Zeroed because `r` and `s` are only filled for the first six patches, and a
     * compiler that cannot see the `i < 6` pairing would call them uninitialised. */
    GLfloat p[4][4][3] = {{{0}}}, q[4][4][3] = {{{0}}};
    GLfloat r[4][4][3] = {{{0}}}, s[4][4][3] = {{{0}}};
    static const GLfloat tex[2][2][2] = {{{0.0f, 0.0f}, {1.0f, 0.0f}},
                                         {{0.0f, 1.0f}, {1.0f, 1.0f}}};
    const GLfloat half = (GLfloat)(size * 0.5);
    int i, j, k, l;

    glPushAttrib(GL_ENABLE_BIT | GL_EVAL_BIT);
    glEnable(GL_AUTO_NORMAL);
    glEnable(GL_NORMALIZE);
    glEnable(GL_MAP2_VERTEX_3);
    glEnable(GL_MAP2_TEXTURE_COORD_2);

    glPushMatrix();
    glRotatef(270.0f, 1.0f, 0.0f, 0.0f); /* the data is z-up; GLUT hands back y-up */
    glScalef(half, half, half);
    glTranslatef(0.0f, 0.0f, -1.5f); /* and sits off-centre until this moves it */

    for (i = 0; i < GLUT_TEAPOT_N_INPUT_PATCHES; i++) {
        for (j = 0; j < 4; j++) {
            for (k = 0; k < 4; k++) {
                for (l = 0; l < 3; l++) {
                    /* `3 - k` reverses the patch as it is mirrored, so the mirrored
                     * copy keeps the same parameter direction and its normals face
                     * outwards. */
                    p[j][k][l] = cpdata_teapot[patchdata_teapot[i][j * 4 + k]][l];
                    q[j][k][l] = cpdata_teapot[patchdata_teapot[i][j * 4 + (3 - k)]][l];
                    if (l == 1) {
                        q[j][k][l] = -q[j][k][l];
                    }

                    if (i < 6) {
                        r[j][k][l] =
                            cpdata_teapot[patchdata_teapot[i][j * 4 + (3 - k)]][l];
                        if (l == 0) {
                            r[j][k][l] = -r[j][k][l];
                        }
                        s[j][k][l] = cpdata_teapot[patchdata_teapot[i][j * 4 + k]][l];
                        if (l == 0 || l == 1) {
                            s[j][k][l] = -s[j][k][l];
                        }
                    }
                }
            }
        }

        glMap2f(GL_MAP2_TEXTURE_COORD_2, 0.0f, 1.0f, 2, 2, 0.0f, 1.0f, 4, 2,
                &tex[0][0][0]);
        glMapGrid2f(GLUT_TEAPOT_GRID, 0.0f, 1.0f, GLUT_TEAPOT_GRID, 0.0f, 1.0f);

        glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 3, 4, 0.0f, 1.0f, 12, 4, &p[0][0][0]);
        glEvalMesh2(mode, 0, GLUT_TEAPOT_GRID, 0, GLUT_TEAPOT_GRID);
        glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 3, 4, 0.0f, 1.0f, 12, 4, &q[0][0][0]);
        glEvalMesh2(mode, 0, GLUT_TEAPOT_GRID, 0, GLUT_TEAPOT_GRID);

        if (i < 6) {
            glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 3, 4, 0.0f, 1.0f, 12, 4, &r[0][0][0]);
            glEvalMesh2(mode, 0, GLUT_TEAPOT_GRID, 0, GLUT_TEAPOT_GRID);
            glMap2f(GL_MAP2_VERTEX_3, 0.0f, 1.0f, 3, 4, 0.0f, 1.0f, 12, 4, &s[0][0][0]);
            glEvalMesh2(mode, 0, GLUT_TEAPOT_GRID, 0, GLUT_TEAPOT_GRID);
        }
    }

    glPopMatrix();
    glPopAttrib();
}

void glutSolidTeapot(GLdouble size) {
    glut_teapot(size, GL_FILL);
}

void glutWireTeapot(GLdouble size) {
    glut_teapot(size, GL_LINE);
}

/* ---------------------------------------------------------------------------
 * Asking the driver what it supports
 * --------------------------------------------------------------------------- */

/*
 * Whole-word search of a space-separated list.
 *
 * `strstr` alone is the trap: `GL_EXT_texture` occurs inside `GL_EXT_texture3D`, so a
 * driver offering only the latter would be reported as offering both. The boundary
 * checks are what make this an answer rather than a guess, and the failure they prevent
 * is silent - a port takes an extension path that is not there and draws nothing.
 */
static int glut_has_word(const char *list, const char *word) {
    if (!list || !word || !*word)
        return 0;

    size_t n = 0;
    while (word[n])
        n++;

    for (const char *p = list; (p = strstr(p, word)) != 0; p += n) {
        const int left_ok = (p == list) || (p[-1] == ' ');
        const int right_ok = (p[n] == '\0') || (p[n] == ' ');
        if (left_ok && right_ok)
            return 1;
    }
    return 0;
}

int glutExtensionSupported(const char *name) {
    /* The flat string first: it is what a compatibility profile answers, which is what
     * this SDK's own contexts are. */
    const GLubyte *all = glGetString(GL_EXTENSIONS);
    if (all && glut_has_word((const char *)all, name))
        return 1;

    /*
     * And the indexed form, for a core profile where the above returns NULL.
     *
     * **Guarded, because this file compiles against two different sets of GL headers.**
     * Built as part of this SDK it sees oops-gl's, which are GL 1.x/2.x and declare
     * neither `GL_NUM_EXTENSIONS` nor `glGetStringi` - and do not need to, because a
     * context that old has only the flat string. Built into a hosted title it sees
     * upstream Mesa's, which declare both. The `#ifdef` is what lets one implementation
     * be correct in both, rather than two copies drifting apart.
     */
#ifdef GL_NUM_EXTENSIONS
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);

    /* A context that does not support the query leaves an error rather than a count; it
     * is cleared so a port does not later find somebody else's. */
    if (count <= 0) {
        (void)glGetError();
        return 0;
    }

    for (GLint i = 0; i < count; i++) {
        const GLubyte *one = glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (one && glut_has_word((const char *)one, name))
            return 1;
    }
#endif
    return 0;
}
