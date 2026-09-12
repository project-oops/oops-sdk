/*
 * oops-gl: OpenGL 1.3 / GLES 1.1 Freestanding Profile for OOPS SDK
 * Clean-room hardware accelerated 3D graphics for Sony PlayStation 5 (RDNA2).
 */

#ifndef __GL_H__
#define __GL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard GL Types */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;

/* Boolean values */
#define GL_FALSE                                0
#define GL_TRUE                                 1

/* Clear buffer bits */
#define GL_DEPTH_BUFFER_BIT                     0x00000100
#define GL_STENCIL_BUFFER_BIT                   0x00000400
#define GL_COLOR_BUFFER_BIT                     0x00004000

/* Primitives */
#define GL_POINTS                               0x0000
#define GL_LINES                                0x0001
#define GL_LINE_LOOP                            0x0002
#define GL_LINE_STRIP                           0x0003
#define GL_TRIANGLES                            0x0004
#define GL_TRIANGLE_STRIP                       0x0005
#define GL_TRIANGLE_FAN                         0x0006
#define GL_QUADS                                0x0007
#define GL_QUAD_STRIP                           0x0008
#define GL_POLYGON                              0x0009

/* Matrix Modes */
#define GL_MODELVIEW                            0x1700
#define GL_PROJECTION                           0x1701
#define GL_TEXTURE                              0x1702

/* Comparison / Depth Functions */
#define GL_NEVER                                0x0200
#define GL_LESS                                 0x0201
#define GL_EQUAL                                0x0202
#define GL_LEQUAL                               0x0203
#define GL_GREATER                              0x0204
#define GL_NOTEQUAL                             0x0205
#define GL_GEQUAL                               0x0206
#define GL_ALWAYS                               0x0207

/* Blending */
#define GL_ZERO                                 0
#define GL_ONE                                  1
#define GL_SRC_COLOR                            0x0300
#define GL_ONE_MINUS_SRC_COLOR                  0x0301
#define GL_SRC_ALPHA                            0x0302
#define GL_ONE_MINUS_SRC_ALPHA                  0x0303
#define GL_DST_ALPHA                            0x0304
#define GL_ONE_MINUS_DST_ALPHA                  0x0305
#define GL_DST_COLOR                            0x0306
#define GL_ONE_MINUS_DST_COLOR                  0x0307
#define GL_SRC_ALPHA_SATURATE                   0x0308
#define GL_BLEND_DST                            0x0BE0
#define GL_BLEND_SRC                            0x0BE1
#define GL_BLEND_DST_RGB                        0x80C8
#define GL_BLEND_SRC_RGB                        0x80C9
#define GL_BLEND_DST_ALPHA                      0x80CA
#define GL_BLEND_SRC_ALPHA                      0x80CB

/* Culling & Front Face */
#define GL_CW                                   0x0900
#define GL_CCW                                  0x0901
#define GL_FRONT                                0x0404
#define GL_BACK                                 0x0405
#define GL_FRONT_AND_BACK                       0x0408

/* Capabilities */
#define GL_CULL_FACE                            0x0B44
#define GL_LIGHTING                             0x0B50
#define GL_LIGHT_MODEL_LOCAL_VIEWER             0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE                 0x0B52
#define GL_LIGHT_MODEL_AMBIENT                  0x0B53
#define GL_COLOR_MATERIAL                       0x0B57
#define GL_DEPTH_TEST                           0x0B71
#define GL_STENCIL_TEST                         0x0B90
#define GL_NORMALIZE                            0x0BA1
#define GL_VIEWPORT                             0x0BA2
#define GL_BLEND                                0x0BE2
#define GL_SCISSOR_TEST                         0x0C11
#define GL_TEXTURE_2D                           0x0DE1

/* Lighting Parameters */
#define GL_LIGHT0                               0x4000
#define GL_LIGHT1                               0x4001
#define GL_LIGHT2                               0x4002
#define GL_LIGHT3                               0x4003
#define GL_LIGHT4                               0x4004
#define GL_LIGHT5                               0x4005
#define GL_LIGHT6                               0x4006
#define GL_LIGHT7                               0x4007

#define GL_AMBIENT                              0x1200
#define GL_DIFFUSE                              0x1201
#define GL_SPECULAR                             0x1202
#define GL_POSITION                             0x1203
#define GL_SPOT_DIRECTION                       0x1204
#define GL_SPOT_EXPONENT                        0x1205
#define GL_SPOT_CUTOFF                          0x1206
#define GL_CONSTANT_ATTENUATION                 0x1207
#define GL_LINEAR_ATTENUATION                   0x1208
#define GL_QUADRATIC_ATTENUATION                0x1209

/* Materials */
#define GL_EMISSION                             0x1600
#define GL_SHININESS                            0x1601
#define GL_AMBIENT_AND_DIFFUSE                  0x1602
#define GL_COLOR_INDEXES                        0x1603

/* Client State (Vertex Arrays) */
#define GL_VERTEX_ARRAY                         0x8074
#define GL_NORMAL_ARRAY                         0x8075
#define GL_COLOR_ARRAY                          0x8076
#define GL_TEXTURE_COORD_ARRAY                  0x8078

/* Data types */
#define GL_BYTE                                 0x1400
#define GL_UNSIGNED_BYTE                        0x1401
#define GL_SHORT                                0x1402
#define GL_UNSIGNED_SHORT                       0x1403
#define GL_INT                                  0x1404
#define GL_UNSIGNED_INT                         0x1405
#define GL_FLOAT                                0x1406
#define GL_DOUBLE                               0x140A

/* Shading Model */
#define GL_FLAT                                 0x1D00
#define GL_SMOOTH                               0x1D01

/* String queries */
#define GL_VENDOR                               0x1F00
#define GL_RENDERER                             0x1F01
#define GL_VERSION                              0x1F02
#define GL_EXTENSIONS                           0x1F03

/* Error codes */
#define GL_NO_ERROR                             0
#define GL_INVALID_ENUM                         0x0500
#define GL_INVALID_VALUE                        0x0501
#define GL_INVALID_OPERATION                    0x0502
#define GL_STACK_OVERFLOW                       0x0503
#define GL_STACK_UNDERFLOW                      0x0504
#define GL_OUT_OF_MEMORY                        0x0505

/* State Queries */
#define GL_MATRIX_MODE                          0x0BA0
#define GL_MODELVIEW_MATRIX                     0x0BA6
#define GL_PROJECTION_MATRIX                    0x0BA7
#define GL_SCISSOR_BOX                          0x0C10
#define GL_TEXTURE_BINDING_2D                   0x8069

/* Texture Mapping */
#define GL_TEXTURE_MAG_FILTER                   0x2800
#define GL_TEXTURE_MIN_FILTER                   0x2801
#define GL_TEXTURE_WRAP_S                       0x2802
#define GL_TEXTURE_WRAP_T                       0x2803
#define GL_NEAREST                              0x2600
#define GL_LINEAR                               0x2601
#define GL_NEAREST_MIPMAP_NEAREST               0x2700
#define GL_LINEAR_MIPMAP_NEAREST                0x2701
#define GL_NEAREST_MIPMAP_LINEAR                0x2702
#define GL_LINEAR_MIPMAP_LINEAR                 0x2703
#define GL_CLAMP                                0x2900
#define GL_REPEAT                               0x2901
#define GL_CLAMP_TO_EDGE                        0x812F
#define GL_RGB                                  0x1907
#define GL_RGBA                                 0x1908
#define GL_RGB8                                 0x8051
#define GL_RGBA8                                0x8058

/* Texture Environment */
#define GL_TEXTURE_ENV                          0x2300
#define GL_TEXTURE_ENV_MODE                     0x2200
#define GL_TEXTURE_ENV_COLOR                    0x2201
#define GL_MODULATE                             0x2100
#define GL_DECAL                                0x2101
#define GL_ADD                                  0x0104
#define GL_REPLACE                              0x1E01

/* Blend Equations */
#define GL_BLEND_EQUATION                       0x8009
#define GL_FUNC_ADD                             0x8006
#define GL_FUNC_SUBTRACT                        0x800A
#define GL_FUNC_REVERSE_SUBTRACT                0x800B
#define GL_MIN                                  0x8007
#define GL_MAX                                  0x8008

/* -------------------------------------------------------------------------
 * OpenGL Function Prototypes
 * ------------------------------------------------------------------------- */

/* State & Configuration */
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClear(GLbitfield mask);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
void glBlendEquation(GLenum mode);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);

/* Information & Error Queries */
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetBooleanv(GLenum pname, GLboolean *params);

/* Matrix Stack Operations */
void glMatrixMode(GLenum mode);
void glPushMatrix(void);
void glPopMatrix(void);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val);

/* Client-Side Vertex Arrays */
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer);

/* Draw Operations */
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

/* Immediate Mode Emulation */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glColor3f(GLfloat red, GLfloat green, GLfloat blue);
void glColor3fv(const GLfloat *v);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor4fv(const GLfloat *v);
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void glTexCoord2f(GLfloat s, GLfloat t);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void glNormal3fv(const GLfloat *v);

/* Fixed-Function Lighting & Materials */
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightModelf(GLenum pname, GLfloat param);
void glColorMaterial(GLenum face, GLenum mode);

/* Texture Object Management */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                 GLsizei width, GLsizei height, GLint border,
                 GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
GLboolean glIsTexture(GLuint texture);

/* Pipeline Synchronization & Presentation */
void glFlush(void);
void glFinish(void);
void glGetCanary(GLuint *vs_canary, GLuint *ps_canary);
void glGetCanaryEx(GLuint *vs_canary, GLuint *ps_canary, GLuint *vs_s0, GLuint *ps_s0);

/* OOPS-GL Context Lifecycle (EGL/GLX equivalent) */
struct oops_display;
void *glContextCreate(struct oops_display *disp);
void  glContextDestroy(void *ctx);
void  glContextMakeCurrent(void *ctx);
void *glGetCurrentContext(void);
void  glSwapBuffers(void);

#ifdef __cplusplus
}
#endif

#endif /* __GL_H__ */
