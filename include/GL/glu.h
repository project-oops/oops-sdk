/*
 * oops-gl: the OpenGL Utility Library (GLU 1.3) - projection helpers, mipmap builders,
 * project/unproject and quadrics, built on public GL calls only.
 */

#ifndef __GLU_H__
#define __GLU_H__

#include "gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Multiplies a perspective projection onto the current matrix; `fovy` is in degrees. */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
/* Multiplies a viewing transform looking from the eye towards the centre, `up` upward.
 */
void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ, GLdouble centerX,
               GLdouble centerY, GLdouble centerZ, GLdouble upX, GLdouble upY,
               GLdouble upZ);
/* glOrtho with near -1 and far 1. */
void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top);
/* Restricts drawing to a dx by dy window rectangle centred on (x, y) - multiplied onto
 * the projection before it, for picking with GL_SELECT. */
void gluPickMatrix(GLdouble x, GLdouble y, GLdouble dx, GLdouble dy, GLint *viewport);
/* A readable name for a GL or GLU error code. */
const GLubyte *gluErrorString(GLenum error);

/* GLU's own return codes. A GLU function answers 0 or one of these and records nothing
 * in glGetError; gluErrorString names them. */
#define GLU_INVALID_ENUM 100900
#define GLU_INVALID_VALUE 100901
#define GLU_OUT_OF_MEMORY 100902
#define GLU_INCOMPATIBLE_GL_VERSION 100903
#define GLU_INVALID_OPERATION 100904
#define GLU_VERSION 100800
#define GLU_EXTENSIONS 100801

/* GLU_VERSION gives "1.3". The tessellator and the NURBS interfaces are absent, so a
 * program needing them fails to link rather than drawing nothing. */
const GLubyte *gluGetString(GLenum name);

/*
 * Scaling and mipmap chains.
 *
 * `gluScaleImage` reads its source through the GL_UNPACK_* state and writes its
 * destination through GL_PACK_*, as the specification says. The filter is a box: an
 * output pixel is the average of the input pixels its footprint covers, and
 * magnification replicates.
 *
 * The mipmap builders scale the image to powers of two if it is not already, upload
 * level zero, then halve repeatedly to 1x1 - each level built from the one above it, so
 * a level is the average of the whole image above rather than a sample of the original.
 * They set the unpack state they need for their own levels and put the caller's back.
 */
GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn, GLenum typeIn,
                    const void *dataIn, GLsizei wOut, GLsizei hOut, GLenum typeOut,
                    void *dataOut);
GLint gluBuild2DMipmaps(GLenum target, GLint internalFormat, GLsizei width,
                        GLsizei height, GLenum format, GLenum type, const void *data);
GLint gluBuild1DMipmaps(GLenum target, GLint internalFormat, GLsizei width,
                        GLenum format, GLenum type, const void *data);

/*
 * Object space to the window and back: `gluProject` puts an object-space point where it
 * lands on screen, `gluUnProject` takes a window position (with a depth read back, or 0
 * and 1 for the ends of a pick ray) to object space. Both take the matrices as
 * `glGetDoublev(GL_MODELVIEW_MATRIX)` and `GL_PROJECTION_MATRIX` give them and the
 * viewport as `glGetIntegerv(GL_VIEWPORT)` does. They answer GL_TRUE, or GL_FALSE when
 * the point is on the eye plane or the matrix is singular.
 *
 * `winZ` runs 0 to 1 whatever glDepthRange is set to - GLU takes no account of it.
 */
GLint gluProject(GLdouble objX, GLdouble objY, GLdouble objZ, const GLdouble *model,
                 const GLdouble *proj, const GLint *view, GLdouble *winX,
                 GLdouble *winY, GLdouble *winZ);
GLint gluUnProject(GLdouble winX, GLdouble winY, GLdouble winZ, const GLdouble *model,
                   const GLdouble *proj, const GLint *view, GLdouble *objX,
                   GLdouble *objY, GLdouble *objZ);

/*
 * Quadrics: a ball, a tube and a ring, drawn as ordinary glVertex calls - so they
 * light, texture, compile into a display list and reach the hardware path like
 * typed-out geometry.
 *
 * The conventions are the specification's. A sphere and a cylinder are divided around
 * the z axis into `slices` and along it into `stacks`; a disk lies in z = 0, divided
 * into `slices` and `loops`. `s` is 0 at the +y axis, 0.25 at +x, 0.5 at -y, 0.75 at
 * -x; a sphere's `t` is 0 at z = -radius and 1 at +radius, a cylinder's 0 at z = 0 and
 * 1 at z = height. A cylinder's ends are open - `gluDisk` caps them. GLU_OUTSIDE faces
 * the normals away from the axis and winds the surface counter-clockwise seen from
 * outside, so it survives back-face culling; GLU_INSIDE does both the other way.
 * Normals are unit length, and a cone's lean with its slope.
 *
 * `gluQuadricCallback` takes GLU_ERROR only, and a quadric with no callback ignores a
 * bad setting rather than recording an error GLU has nowhere to put.
 */
typedef struct GLUquadric GLUquadric;
typedef struct GLUquadric GLUquadricObj; /* the older spelling, which ports use */

#define GLU_SMOOTH 100000
#define GLU_FLAT 100001
#define GLU_NONE 100002
#define GLU_POINT 100010
#define GLU_LINE 100011
#define GLU_FILL 100012
#define GLU_SILHOUETTE 100013
#define GLU_OUTSIDE 100020
#define GLU_INSIDE 100021
#define GLU_ERROR 100103

GLUquadric *gluNewQuadric(void);
void gluDeleteQuadric(GLUquadric *q);
void gluQuadricDrawStyle(GLUquadric *q, GLenum draw);
void gluQuadricNormals(GLUquadric *q, GLenum normal);
void gluQuadricOrientation(GLUquadric *q, GLenum orientation);
void gluQuadricTexture(GLUquadric *q, GLboolean texture);
void gluQuadricCallback(GLUquadric *q, GLenum which, void (*fn)(void));
void gluSphere(GLUquadric *q, GLdouble radius, GLint slices, GLint stacks);
void gluCylinder(GLUquadric *q, GLdouble base, GLdouble top, GLdouble height,
                 GLint slices, GLint stacks);
void gluDisk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices, GLint loops);
void gluPartialDisk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices,
                    GLint loops, GLdouble start, GLdouble sweep);

#ifdef __cplusplus
}
#endif

#endif /* __GLU_H__ */
