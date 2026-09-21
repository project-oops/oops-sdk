/*
 * The Utah teapot, as Bezier patch data.
 *
 * # Where this came from, and why it is copied rather than derived
 *
 * Martin Newell modelled it at the University of Utah in 1975 as a test object for rendering
 * research - curved throughout, with a saddle at the handle, a concave spout, a hole that makes
 * it genus-1, and enough asymmetry that an orientation bug shows immediately. A sphere hides all
 * of those. It became the field's standard test model, and `glutSolidTeapot` is the reason every
 * GL programmer has drawn one.
 *
 * **The numbers are transcribed, not computed, so they carry their source.** This is
 * `patchdata_teapot` and `cpdata_teapot` from freeglut's `src/fg_teapot_data.h`, taken at
 * 2026-09-21 from https://github.com/freeglut/freeglut. freeglut is MIT licensed and its notice
 * is reproduced below; this file keeps only the teapot, dropping the teacup, teaspoon and
 * doughnut that share that header. freeglut's own provenance note says the data derives from an
 * archive uploaded by Juhana Kouhia (`ftp://ftp.funet.fi/pub/sci/graphics/packages/objects/`),
 * scaled to match freeglut's output, with duplicate control points removed and a bottom added -
 * so this is the teapot `glutSolidTeapot` draws everywhere else, which is the point of having
 * it, rather than Newell's raw 1975 table (which has no bottom).
 *
 * It is vendored once rather than fetched: 129 control points that have not changed since 1975
 * are not a dependency to track, and `common/upstream-fetch.sh` exists for programs that move.
 *
 * # The shape of the data
 *
 * Ten patches over 129 control points, and the rest of the teapot comes from symmetry - the
 * comment freeglut carries for it is kept below because it is the instruction for using it:
 * rim, body, lid and bottom are rotated through all four quadrants, while handle and spout are
 * mirrored across the x-y plane only. Each patch is sixteen indices into the control points, a
 * bicubic Bezier patch, which is exactly what `glMap2f` and `glEvalMesh2` take.
 *
 * ---- freeglut's licence, reproduced in full as its terms require ----
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * -------------------------------------------------------------------
 */
#ifndef OOPS_GLUT_TEAPOT_DATA_H
#define OOPS_GLUT_TEAPOT_DATA_H

/*
 * Rim, body, lid, and bottom data must be rotated along all four quadrants;
 * handle and spout data is flipped across the x-y plane (negate z values) only.
 */
#define GLUT_TEAPOT_N_INPUT_PATCHES 10
static int patchdata_teapot[GLUT_TEAPOT_N_INPUT_PATCHES][16] =
{
    {  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15, }, /* rim    */
    { 12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27, }, /* body   */
    { 24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39, },
    { 40,  41,  42,  40,  43,  44,  45,  46,  47,  47,  47,  47,  48,  49,  50,  51, }, /* lid    */
    { 48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63, },
    { 64,  64,  64,  64,  65,  66,  67,  68,  69,  70,  71,  72,  39,  38,  37,  36, }, /* bottom */
    { 73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88, }, /* handle */
    { 85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99, 100, },
    {101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, }, /* spout  */
    {113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128  }
};

static GLfloat cpdata_teapot[][3] =
{
    { 1.40000f,  0.00000f,  2.40000f}, { 1.40000f, -0.78400f,  2.40000f},
    { 0.78400f, -1.40000f,  2.40000f}, { 0.00000f, -1.40000f,  2.40000f},
    { 1.33750f,  0.00000f,  2.53125f}, { 1.33750f, -0.74900f,  2.53125f},
    { 0.74900f, -1.33750f,  2.53125f}, { 0.00000f, -1.33750f,  2.53125f},
    { 1.43750f,  0.00000f,  2.53125f}, { 1.43750f, -0.80500f,  2.53125f},
    { 0.80500f, -1.43750f,  2.53125f}, { 0.00000f, -1.43750f,  2.53125f},
    { 1.50000f,  0.00000f,  2.40000f}, { 1.50000f, -0.84000f,  2.40000f},
    { 0.84000f, -1.50000f,  2.40000f}, { 0.00000f, -1.50000f,  2.40000f},
    { 1.75000f,  0.00000f,  1.87500f}, { 1.75000f, -0.98000f,  1.87500f},
    { 0.98000f, -1.75000f,  1.87500f}, { 0.00000f, -1.75000f,  1.87500f},
    { 2.00000f,  0.00000f,  1.35000f}, { 2.00000f, -1.12000f,  1.35000f},
    { 1.12000f, -2.00000f,  1.35000f}, { 0.00000f, -2.00000f,  1.35000f},
    { 2.00000f,  0.00000f,  0.90000f}, { 2.00000f, -1.12000f,  0.90000f},
    { 1.12000f, -2.00000f,  0.90000f}, { 0.00000f, -2.00000f,  0.90000f},
    { 2.00000f,  0.00000f,  0.45000f}, { 2.00000f, -1.12000f,  0.45000f},
    { 1.12000f, -2.00000f,  0.45000f}, { 0.00000f, -2.00000f,  0.45000f},
    { 1.50000f,  0.00000f,  0.22500f}, { 1.50000f, -0.84000f,  0.22500f},
    { 0.84000f, -1.50000f,  0.22500f}, { 0.00000f, -1.50000f,  0.22500f},
    { 1.50000f,  0.00000f,  0.15000f}, { 1.50000f, -0.84000f,  0.15000f},
    { 0.84000f, -1.50000f,  0.15000f}, { 0.00000f, -1.50000f,  0.15000f},
    { 0.00000f,  0.00000f,  3.15000f}, { 0.00000f, -0.00200f,  3.15000f},
    { 0.00200f,  0.00000f,  3.15000f}, { 0.80000f,  0.00000f,  3.15000f},
    { 0.80000f, -0.45000f,  3.15000f}, { 0.45000f, -0.80000f,  3.15000f},
    { 0.00000f, -0.80000f,  3.15000f}, { 0.00000f,  0.00000f,  2.85000f},
    { 0.20000f,  0.00000f,  2.70000f}, { 0.20000f, -0.11200f,  2.70000f},
    { 0.11200f, -0.20000f,  2.70000f}, { 0.00000f, -0.20000f,  2.70000f},
    { 0.40000f,  0.00000f,  2.55000f}, { 0.40000f, -0.22400f,  2.55000f},
    { 0.22400f, -0.40000f,  2.55000f}, { 0.00000f, -0.40000f,  2.55000f},
    { 1.30000f,  0.00000f,  2.55000f}, { 1.30000f, -0.72800f,  2.55000f},
    { 0.72800f, -1.30000f,  2.55000f}, { 0.00000f, -1.30000f,  2.55000f},
    { 1.30000f,  0.00000f,  2.40000f}, { 1.30000f, -0.72800f,  2.40000f},
    { 0.72800f, -1.30000f,  2.40000f}, { 0.00000f, -1.30000f,  2.40000f},
    { 0.00000f,  0.00000f,  0.00000f}, { 0.00000f, -1.42500f,  0.00000f},
    { 0.79800f, -1.42500f,  0.00000f}, { 1.42500f, -0.79800f,  0.00000f},
    { 1.42500f,  0.00000f,  0.00000f}, { 0.00000f, -1.50000f,  0.07500f},
    { 0.84000f, -1.50000f,  0.07500f}, { 1.50000f, -0.84000f,  0.07500f},
    { 1.50000f,  0.00000f,  0.07500f}, {-1.60000f,  0.00000f,  2.02500f},
    {-1.60000f, -0.30000f,  2.02500f}, {-1.50000f, -0.30000f,  2.25000f},
    {-1.50000f,  0.00000f,  2.25000f}, {-2.30000f,  0.00000f,  2.02500f},
    {-2.30000f, -0.30000f,  2.02500f}, {-2.50000f, -0.30000f,  2.25000f},
    {-2.50000f,  0.00000f,  2.25000f}, {-2.70000f,  0.00000f,  2.02500f},
    {-2.70000f, -0.30000f,  2.02500f}, {-3.00000f, -0.30000f,  2.25000f},
    {-3.00000f,  0.00000f,  2.25000f}, {-2.70000f,  0.00000f,  1.80000f},
    {-2.70000f, -0.30000f,  1.80000f}, {-3.00000f, -0.30000f,  1.80000f},
    {-3.00000f,  0.00000f,  1.80000f}, {-2.70000f,  0.00000f,  1.57500f},
    {-2.70000f, -0.30000f,  1.57500f}, {-3.00000f, -0.30000f,  1.35000f},
    {-3.00000f,  0.00000f,  1.35000f}, {-2.50000f,  0.00000f,  1.12500f},
    {-2.50000f, -0.30000f,  1.12500f}, {-2.65000f, -0.30000f,  0.93750f},
    {-2.65000f,  0.00000f,  0.93750f}, {-2.00000f,  0.00000f,  0.90000f},
    {-2.00000f, -0.30000f,  0.90000f}, {-1.90000f, -0.30000f,  0.60000f},
    {-1.90000f,  0.00000f,  0.60000f}, { 1.70000f,  0.00000f,  1.42500f},
    { 1.70000f, -0.66000f,  1.42500f}, { 1.70000f, -0.66000f,  0.60000f},
    { 1.70000f,  0.00000f,  0.60000f}, { 2.60000f,  0.00000f,  1.42500f},
    { 2.60000f, -0.66000f,  1.42500f}, { 3.10000f, -0.66000f,  0.82500f},
    { 3.10000f,  0.00000f,  0.82500f}, { 2.30000f,  0.00000f,  2.10000f},
    { 2.30000f, -0.25000f,  2.10000f}, { 2.40000f, -0.25000f,  2.02500f},
    { 2.40000f,  0.00000f,  2.02500f}, { 2.70000f,  0.00000f,  2.40000f},
    { 2.70000f, -0.25000f,  2.40000f}, { 3.30000f, -0.25000f,  2.40000f},
    { 3.30000f,  0.00000f,  2.40000f}, { 2.80000f,  0.00000f,  2.47500f},
    { 2.80000f, -0.25000f,  2.47500f}, { 3.52500f, -0.25000f,  2.49375f},
    { 3.52500f,  0.00000f,  2.49375f}, { 2.90000f,  0.00000f,  2.47500f},
    { 2.90000f, -0.15000f,  2.47500f}, { 3.45000f, -0.15000f,  2.51250f},
    { 3.45000f,  0.00000f,  2.51250f}, { 2.80000f,  0.00000f,  2.40000f},
    { 2.80000f, -0.15000f,  2.40000f}, { 3.20000f, -0.15000f,  2.40000f},
    { 3.20000f,  0.00000f,  2.40000f}
};

#endif /* OOPS_GLUT_TEAPOT_DATA_H */
