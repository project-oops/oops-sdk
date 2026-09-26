/*
 * oops-glut's bitmap font: `glutBitmapCharacter` and the calls that measure it.
 *
 * # Why there is a font here at all
 *
 * Most GLUT code that draws anything also draws text - a frame counter, a key legend, a
 * score - and it draws it with `glutBitmapCharacter`. A port that cannot call it has to
 * have its text rewritten, which is the one thing this library exists to avoid.
 *
 * # Why it is this font and not X11's
 *
 * GLUT's `GLUT_BITMAP_8_BY_13` and `GLUT_BITMAP_9_BY_15` are the X11 `fixed` fonts, and
 * every GLUT ships their glyph data as a table. **Copying one would be taking someone
 * else's font**, so the glyphs below are drawn here, in a 5x7 box with two rows of
 * descender under it. They are not X11's shapes and do not pretend to be: what they
 * keep is the part a program depends on - the advance (8 and 9 pixels), the cell height
 * (13 and 15), the baseline, and ASCII 32 to 126 all being present and legible.
 *
 * A program that lays text out by `glutBitmapWidth` gets the same layout it would
 * anywhere, because both fonts are fixed-width and the widths are GLUT's own.
 *
 * # Why the glyphs are pictures in the source
 *
 * A font is the kind of data that is wrong in one glyph and looks right everywhere: a
 * hex table cannot be proofread, only tested one character at a time. So each glyph
 * below is nine rows of
 * `#` and `.` in source order, top row first, which is a picture of the letter - and
 * `glut_font_bits` turns it into the bytes `glBitmap` wants. A reader checks the font
 * by reading it.
 *
 * # What is not here
 *
 * `GLUT_BITMAP_HELVETICA_*` and `GLUT_BITMAP_TIMES_ROMAN_*` are **proportional** fonts,
 * and a fixed-width font offered under those names would return the wrong width from
 * `glutBitmapWidth` and break the layout of every program that measures before it
 * draws. They are absent, so such a program fails to link and knows.
 * `glutStrokeCharacter` is absent for the same reason: its glyphs are line segments,
 * which these are not.
 */
#include "GL/glut.h"
#include "gl_internal.h"

/* The two handles, which are addresses rather than values - GLUT's are, and a program
 * stores one in a variable of type `void *`. */
static const int glut_font_8x13 = 13;
static const int glut_font_9x15 = 15;
void *const glutBitmapFixed8x13 = (void *)&glut_font_8x13;
void *const glutBitmapFixed9x15 = (void *)&glut_font_9x15;

#define GLUT_FONT_FIRST 32
#define GLUT_FONT_LAST 126
#define GLUT_FONT_COUNT (GLUT_FONT_LAST - GLUT_FONT_FIRST + 1)
#define GLUT_FONT_ROWS 9 /* seven above the baseline, two below */

/*
 * The glyphs, ASCII 32 to 126, nine rows each and the top row first. Five columns are
 * drawn and the rest of the cell is the space between letters.
 *
 * The last two rows of each glyph are below the baseline: blank for everything except
 * the descenders (`g j p q y`), the comma and the semicolon.
 */
static const char *const glut_font_art[GLUT_FONT_COUNT][GLUT_FONT_ROWS] = {
    /* ' ' */ {".....", ".....", ".....", ".....", ".....", ".....", ".....", ".....",
               "....."},
    /* '!' */
    {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#..", ".....", "....."},
    /* '"' */
    {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", ".....", ".....", "....."},
    /* '#' */
    {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#.", ".....", "....."},
    /* '$' */
    {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", ".....", "....."},
    /* '%' */
    {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##", ".....", "....."},
    /* '&' */
    {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#", ".....", "....."},
    /* '\'' */
    {"..#..", "..#..", ".....", ".....", ".....", ".....", ".....", ".....", "....."},
    /* '(' */
    {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.", ".....", "....."},
    /* ')' */
    {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...", ".....", "....."},
    /* '*' */
    {".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", ".....", ".....", "....."},
    /* '+' */
    {".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....", ".....", "....."},
    /* ',' */
    {".....", ".....", ".....", ".....", ".....", ".##..", ".##..", "..#..", ".#..."},
    /* '-' */
    {".....", ".....", ".....", "#####", ".....", ".....", ".....", ".....", "....."},
    /* '.' */
    {".....", ".....", ".....", ".....", ".....", ".##..", ".##..", ".....", "....."},
    /* '/' */
    {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#....", ".....", "....."},
    /* '0' */
    {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", ".....", "....."},
    /* '1' */
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", ".....", "....."},
    /* '2' */
    {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", ".....", "....."},
    /* '3' */
    {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###.", ".....", "....."},
    /* '4' */
    {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", ".....", "....."},
    /* '5' */
    {"#####", "#....", "####.", "....#", "....#", "#...#", ".###.", ".....", "....."},
    /* '6' */
    {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", ".....", "....."},
    /* '7' */
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", ".....", "....."},
    /* '8' */
    {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", ".....", "....."},
    /* '9' */
    {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", ".....", "....."},
    /* ':' */
    {".....", ".##..", ".##..", ".....", ".##..", ".##..", ".....", ".....", "....."},
    /* ';' */
    {".....", ".##..", ".##..", ".....", ".##..", ".##..", "..#..", ".#...", "....."},
    /* '<' */
    {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#.", ".....", "....."},
    /* '=' */
    {".....", ".....", "#####", ".....", "#####", ".....", ".....", ".....", "....."},
    /* '>' */
    {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#...", ".....", "....."},
    /* '?' */
    {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..", ".....", "....."},
    /* '@' */
    {".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####", ".....", "....."},
    /* 'A' */
    {"..#..", ".#.#.", "#...#", "#...#", "#####", "#...#", "#...#", ".....", "....."},
    /* 'B' */
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", ".....", "....."},
    /* 'C' */
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", ".....", "....."},
    /* 'D' */
    {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###..", ".....", "....."},
    /* 'E' */
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####", ".....", "....."},
    /* 'F' */
    {"#####", "#....", "#....", "####.", "#....", "#....", "#....", ".....", "....."},
    /* 'G' */
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####", ".....", "....."},
    /* 'H' */
    {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", ".....", "....."},
    /* 'I' */
    {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", ".....", "....."},
    /* 'J' */
    {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..", ".....", "....."},
    /* 'K' */
    {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", ".....", "....."},
    /* 'L' */
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####", ".....", "....."},
    /* 'M' */
    {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", ".....", "....."},
    /* 'N' */
    {"#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#", ".....", "....."},
    /* 'O' */
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", ".....", "....."},
    /* 'P' */
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#....", ".....", "....."},
    /* 'Q' */
    {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", ".....", "....."},
    /* 'R' */
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", ".....", "....."},
    /* 'S' */
    {".####", "#....", "#....", ".###.", "....#", "....#", "####.", ".....", "....."},
    /* 'T' */
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "....."},
    /* 'U' */
    {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", ".....", "....."},
    /* 'V' */
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", ".....", "....."},
    /* 'W' */
    {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#", ".....", "....."},
    /* 'X' */
    {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#", ".....", "....."},
    /* 'Y' */
    {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", ".....", "....."},
    /* 'Z' */
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", ".....", "....."},
    /* '[' */
    {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.", ".....", "....."},
    /* '\\' */
    {"#....", "#....", ".#...", "..#..", "...#.", "....#", "....#", ".....", "....."},
    /* ']' */
    {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###.", ".....", "....."},
    /* '^' */
    {"..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....", ".....", "....."},
    /* '_' */
    {".....", ".....", ".....", ".....", ".....", ".....", "#####", ".....", "....."},
    /* '`' */
    {".#...", "..#..", ".....", ".....", ".....", ".....", ".....", ".....", "....."},
    /* 'a' */
    {".....", ".....", ".###.", "....#", ".####", "#...#", ".####", ".....", "....."},
    /* 'b' */
    {"#....", "#....", "####.", "#...#", "#...#", "#...#", "####.", ".....", "....."},
    /* 'c' */
    {".....", ".....", ".###.", "#....", "#....", "#...#", ".###.", ".....", "....."},
    /* 'd' */
    {"....#", "....#", ".####", "#...#", "#...#", "#...#", ".####", ".....", "....."},
    /* 'e' */
    {".....", ".....", ".###.", "#...#", "#####", "#....", ".###.", ".....", "....."},
    /* 'f' */
    {"..##.", ".#..#", ".#...", "####.", ".#...", ".#...", ".#...", ".....", "....."},
    /* 'g' */
    {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "#...#", ".###."},
    /* 'h' */
    {"#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#", ".....", "....."},
    /* 'i' */
    {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###.", ".....", "....."},
    /* 'j' */
    {"...#.", ".....", "..##.", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."},
    /* 'k' */
    {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", ".....", "....."},
    /* 'l' */
    {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", ".....", "....."},
    /* 'm' */
    {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#...#", ".....", "....."},
    /* 'n' */
    {".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#", ".....", "....."},
    /* 'o' */
    {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###.", ".....", "....."},
    /* 'p' */
    {".....", ".....", "####.", "#...#", "#...#", "####.", "#....", "#....", "....."},
    /* 'q' */
    {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#", "....."},
    /* 'r' */
    {".....", ".....", "#.##.", "##..#", "#....", "#....", "#....", ".....", "....."},
    /* 's' */
    {".....", ".....", ".####", "#....", ".###.", "....#", "####.", ".....", "....."},
    /* 't' */
    {".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##.", ".....", "....."},
    /* 'u' */
    {".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####", ".....", "....."},
    /* 'v' */
    {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..", ".....", "....."},
    /* 'w' */
    {".....", ".....", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#.", ".....", "....."},
    /* 'x' */
    {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", ".....", "....."},
    /* 'y' */
    {".....", ".....", "#...#", "#...#", "#...#", ".####", "....#", "...#.", ".##.."},
    /* 'z' */
    {".....", ".....", "#####", "...#.", "..#..", ".#...", "#####", ".....", "....."},
    /* '{' */
    {"..##.", ".#...", ".#...", "##...", ".#...", ".#...", "..##.", ".....", "....."},
    /* '|' */
    {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", ".....", "....."},
    /* '}' */
    {".##..", "...#.", "...#.", "...##", "...#.", "...#.", ".##..", ".....", "....."},
    /* '~' */
    {".....", ".#..#", "#.#.#", "#..#.", ".....", ".....", ".....", ".....", "....."},
};

/*
 * One glyph as `glBitmap` wants it: nine rows of eight bits, **bottom row first**,
 * because a bitmap's first row is its bottom one. The art above is written top row
 * first, which is how a letter is read, so this reverses it.
 *
 * Built once into a static table rather than on every character, since a frame counter
 * draws a dozen glyphs sixty times a second.
 */
static GLubyte glut_font_bits[GLUT_FONT_COUNT][GLUT_FONT_ROWS];
static int glut_font_ready = 0;

static void glut_font_build(void) {
    if (glut_font_ready)
        return;
    for (int c = 0; c < GLUT_FONT_COUNT; c++) {
        for (int r = 0; r < GLUT_FONT_ROWS; r++) {
            const char *art = glut_font_art[c][GLUT_FONT_ROWS - 1 - r];
            GLubyte bits = 0u;
            for (int x = 0; x < 5; x++) {
                if (art[x] == '#')
                    bits |= (GLubyte)(0x80u >> x);
            }
            glut_font_bits[c][r] = bits;
        }
    }
    glut_font_ready = 1;
}

/* 8 for the 8x13 font, 9 for the 9x15 one; anything else is not a font this has. */
static int glut_font_advance(void *font) {
    if (font == glutBitmapFixed9x15)
        return 9;
    if (font == glutBitmapFixed8x13)
        return 8;
    return 0;
}

void glutBitmapCharacter(void *font, int character) {
    const int adv = glut_font_advance(font);
    if (adv == 0)
        return;
    glut_font_build();
    if (character < GLUT_FONT_FIRST || character > GLUT_FONT_LAST) {
        /* Outside the range this font has - the position still moves, so a string with
         * a tab or a stray byte in it stays aligned rather than collapsing. */
        glBitmap(0, 0, 0.0f, 0.0f, (GLfloat)adv, 0.0f, (const GLubyte *)0);
        return;
    }
    /* **The unpack state is the caller's**, and one row here is one byte, so an
     * alignment of 4 - the default - would have glBitmap read four bytes a row. Set to
     * 1 around the call and put back, which is what a font in a library has to do. */
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (align != 1)
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* The origin is two rows up from the bottom of the bitmap: that is the baseline,
     * and the two rows below it are where the descenders go. */
    glBitmap(8, GLUT_FONT_ROWS, 0.0f, 2.0f, (GLfloat)adv, 0.0f,
             glut_font_bits[character - GLUT_FONT_FIRST]);
    if (align != 1)
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
}

/* freeglut's, and the reason it is here: a program that draws a whole string in one
 * call is common enough that leaving it out means editing the port. */
void glutBitmapString(void *font, const unsigned char *string) {
    if (!string)
        return;
    for (const unsigned char *p = string; *p; p++)
        glutBitmapCharacter(font, (int)*p);
}

int glutBitmapWidth(void *font, int character) {
    (void)character; /* both fonts are fixed-width, which is what makes them these two
                        fonts */
    return glut_font_advance(font);
}

int glutBitmapLength(void *font, const unsigned char *string) {
    const int adv = glut_font_advance(font);
    int n = 0;
    if (!string || adv == 0)
        return 0;
    for (const unsigned char *p = string; *p; p++)
        n++;
    return n * adv;
}

/* The cell height GLUT's names carry - 13 and 15 - not the nine rows drawn. A program
 * spacing lines uses this, and spacing them by the ink would run them together. */
int glutBitmapHeight(void *font) {
    if (font == glutBitmapFixed9x15)
        return 15;
    if (font == glutBitmapFixed8x13)
        return 13;
    return 0;
}
