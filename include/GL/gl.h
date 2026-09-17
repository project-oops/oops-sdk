/*
 * oops-gl: freestanding fixed-function 3D instrument for the OOPS SDK.
 *
 * Clean-room, hardware-accelerated 3D on Prospero-generation RDNA2, lowering GL-shaped calls
 * straight to PM4 with no vendor driver underneath.
 *
 * This is NOT an OpenGL version and NOT the GL to write an application against. The surface is
 * OpenGL 1.1-class fixed function, plus individual later calls added when an oracle program
 * needed one. It exists so the command stream stays readable as a hardware record. For
 * applications use oops-mesa, which provides OpenGL 3.3 Core and GLSL 3.30. See oops-sdk D007.
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
/* Pointer-sized, because a buffer object's size and a byte offset into it both have to be able
 * to exceed 2GB on a 64-bit target - and because the buffer entry points below take an *offset*
 * in a parameter typed as a pointer, which only works if the two are the same width. */
typedef long           GLintptr;
typedef long           GLsizeiptr;

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
void glGetDoublev(GLenum pname, GLdouble *params);

/* The per-object queries: state that belongs to a texture, a light or a material rather than to
 * the context. Same discipline as the state queries - an unanswerable name is refused. */
void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params);
void glGetLightfv(GLenum light, GLenum pname, GLfloat *params);
void glGetLightiv(GLenum light, GLenum pname, GLint *params);
void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params);
void glGetMaterialiv(GLenum face, GLenum pname, GLint *params);
void glGetPointerv(GLenum pname, GLvoid **params);
#define GL_TEXTURE_WIDTH                        0x1000
#define GL_TEXTURE_HEIGHT                       0x1001
#define GL_TEXTURE_INTERNAL_FORMAT              0x1003
#define GL_TEXTURE_BORDER                       0x1005
#define GL_VERTEX_ARRAY_POINTER                 0x808E
#define GL_NORMAL_ARRAY_POINTER                 0x808F
#define GL_COLOR_ARRAY_POINTER                  0x8090
#define GL_TEXTURE_COORD_ARRAY_POINTER          0x8092

/* `glInterleavedArrays` sets the four client arrays from one buffer holding them interleaved,
 * which is how a lot of 1.x code feeds geometry. It is the pointer calls it would otherwise
 * have made, so there is nothing here the draw path does not already read. */
#define GL_V2F                                  0x2A20
#define GL_V3F                                  0x2A21
#define GL_C4UB_V2F                             0x2A22
#define GL_C4UB_V3F                             0x2A23
#define GL_C3F_V3F                              0x2A24
#define GL_N3F_V3F                              0x2A25
#define GL_C4F_N3F_V3F                          0x2A26
#define GL_T2F_V3F                              0x2A27
#define GL_T4F_V4F                              0x2A28
#define GL_T2F_C4UB_V3F                         0x2A29
#define GL_T2F_C3F_V3F                          0x2A2A
#define GL_T2F_N3F_V3F                          0x2A2B
#define GL_T2F_C4F_N3F_V3F                      0x2A2C
#define GL_T4F_C4F_N3F_V4F                      0x2A2D
void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer);

/* Buffer objects (GL 1.5). A named block of memory the arrays are read out of, instead of a
 * pointer into the caller's own storage.
 *
 * **The `pointer` argument of glVertexPointer and friends becomes a byte offset** while a
 * buffer is bound to GL_ARRAY_BUFFER - the same parameter, read a different way, which is the
 * one thing about this API that surprises people. The same is true of glDrawElements' `indices`
 * while a buffer is bound to GL_ELEMENT_ARRAY_BUFFER, and there an offset of 0 is both legal
 * and indistinguishable from a null pointer.
 *
 * The array remembers the buffer *object*, not an address, because glBufferData may reallocate
 * underneath it. Resolving the address at glVertexPointer time would leave a stale pointer the
 * first time a program respecified a buffer, which is an ordinary thing to do every frame.
 *
 * Enumerant values taken from oops-mesa/mesa/include/GL/glext.h. */
#define GL_ARRAY_BUFFER                         0x8892
#define GL_ELEMENT_ARRAY_BUFFER                 0x8893
#define GL_ARRAY_BUFFER_BINDING                 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING         0x8895
#define GL_BUFFER_SIZE                          0x8764
#define GL_BUFFER_USAGE                         0x8765
#define GL_STREAM_DRAW                          0x88E0
#define GL_STATIC_DRAW                          0x88E4
#define GL_DYNAMIC_DRAW                         0x88E8
void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data);
GLboolean glIsBuffer(GLuint buffer);
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);

/* The bound texture, read back. Unlike glReadPixels this does **not** flip: a texture has no
 * window origin, so row 0 of the result is row 0 of what was uploaded. */
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels);

/* What the queries above can be asked for. Every one of these is a number already held in the
 * context; the family is a dispatch table rather than a feature.
 *
 * A pname not listed here is **refused with GL_INVALID_ENUM**, not ignored. Ignoring it leaves
 * the caller's buffer holding whatever it held before - usually stack garbage - with no error
 * raised, which is the worst of the three possible behaviours: the program reads a number that
 * looks real.
 *
 * Enumerant values taken from oops-mesa/mesa/include/GL/gl.h, not from memory. */
#define GL_CURRENT_COLOR                        0x0B00
#define GL_CURRENT_NORMAL                       0x0B02
#define GL_CURRENT_TEXTURE_COORDS               0x0B03
#define GL_LIST_BASE                            0x0B32
#define GL_LIGHT_MODEL_AMBIENT                  0x0B53
#define GL_SHADE_MODEL                          0x0B54
#define GL_CULL_FACE_MODE                       0x0B45
#define GL_FRONT_FACE                           0x0B46
#define GL_DEPTH_RANGE                          0x0B70
#define GL_DEPTH_WRITEMASK                      0x0B72
#define GL_DEPTH_CLEAR_VALUE                    0x0B73
#define GL_DEPTH_FUNC                           0x0B74
#define GL_ALPHA_TEST_FUNC                      0x0BC1
#define GL_ALPHA_TEST_REF                       0x0BC2
#define GL_ATTRIB_STACK_DEPTH                   0x0BB0
#define GL_CLIENT_ATTRIB_STACK_DEPTH            0x0BB1
#define GL_MODELVIEW_STACK_DEPTH                0x0BA3
#define GL_PROJECTION_STACK_DEPTH               0x0BA4
#define GL_TEXTURE_MATRIX                       0x0BA8
#define GL_COLOR_CLEAR_VALUE                    0x0C22
#define GL_COLOR_WRITEMASK                      0x0C23
#define GL_MAX_LIGHTS                           0x0D31
#define GL_MAX_TEXTURE_SIZE                     0x0D33
#define GL_MAX_ATTRIB_STACK_DEPTH               0x0D35
#define GL_MAX_MODELVIEW_STACK_DEPTH            0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH           0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH              0x0D39
#define GL_MAX_VIEWPORT_DIMS                    0x0D3A
#define GL_MAX_CLIENT_ATTRIB_STACK_DEPTH        0x0D3B
#define GL_POLYGON_OFFSET_FACTOR                0x8038
#define GL_POLYGON_OFFSET_UNITS                 0x2A00

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
/* The double spellings. The matrices are held as float either way, so these narrow and forward
 * rather than carrying a second precision through the stacks - which is what a GL 1.x
 * implementation does in practice, and what makes glLoadMatrixd(glGetDoublev(...)) round-trip
 * to the same picture rather than a slightly different one. */
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
void glScaled(GLdouble x, GLdouble y, GLdouble z);
void glLoadMatrixd(const GLdouble *m);
void glMultMatrixd(const GLdouble *m);
/* The transposed spellings: the same sixteen numbers written row-major, which is how a C
 * programmer naturally writes a matrix literal. */
void glLoadTransposeMatrixf(const GLfloat *m);
void glMultTransposeMatrixf(const GLfloat *m);
void glLoadTransposeMatrixd(const GLdouble *m);
void glMultTransposeMatrixd(const GLdouble *m);
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
/* One vertex pulled out of the enabled arrays, inside glBegin/glEnd - the bridge between the
 * two ways of feeding geometry, for a program that keeps its data in arrays but wants to
 * assemble the primitives by hand. */
void glArrayElement(GLint i);
/* glDrawElements plus a promise about the index range. The promise is a hint an implementation
 * may use or ignore; this one ignores it, which the specification allows. */
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                         GLenum type, const GLvoid *indices);

/* Immediate Mode Emulation */
/* The attribute stack. A mask naming state this subset does not have - accumulation buffer,
 * stencil, fog, evaluators, stipple, hints, pixel mode, points, lines - is refused, because a
 * push that silently saved nothing is the leak this exists to prevent. GL_ALL_ATTRIB_BITS is
 * narrowed to what exists rather than refused. */
#define GL_CURRENT_BIT                          0x00000001
#define GL_POLYGON_BIT                          0x00000008
#define GL_LIGHTING_BIT                         0x00000040
#define GL_DEPTH_BUFFER_BIT_ATTRIB              0x00000100
#define GL_VIEWPORT_BIT                         0x00000800
#define GL_TRANSFORM_BIT                        0x00001000
#define GL_ENABLE_BIT                           0x00002000
#define GL_LIST_BIT                             0x00020000
#define GL_TEXTURE_BIT                          0x00040000
#define GL_SCISSOR_BIT                          0x00080000
#define GL_ALL_ATTRIB_BITS                      0x000FFFFF
void glPushAttrib(GLbitfield mask);
void glPopAttrib(void);

/* The client half of the attribute stack: state that lives in this process rather than on the
 * GPU - the array pointers and the pixel-store modes. It is a separate stack in the
 * specification and a separate stack here, because a library that brackets its array setup with
 * glPushClientAttrib must not disturb the server state its caller pushed. */
#define GL_CLIENT_PIXEL_STORE_BIT               0x00000001
#define GL_CLIENT_VERTEX_ARRAY_BIT              0x00000002
#define GL_CLIENT_ALL_ATTRIB_BITS               0xFFFFFFFF
void glPushClientAttrib(GLbitfield mask);
void glPopClientAttrib(void);

/* `glRect*` draws a screen-aligned rectangle in the z=0 plane with the current colour, normal
 * and texture coordinate. The specification writes it as a four-vertex GL_POLYGON; a rectangle
 * is convex, so GL_QUADS draws the identical two triangles and is a primitive this supports. */
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2);
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2);
void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2);
void glRectfv(const GLfloat *v1, const GLfloat *v2);
void glRectdv(const GLdouble *v1, const GLdouble *v2);
void glRectiv(const GLint *v1, const GLint *v2);
void glRectsv(const GLshort *v1, const GLshort *v2);

/* Discards a fragment whose alpha fails the comparison. RDNA2 has no fixed-function alpha test,
 * so this is a discard written into the pixel shader (D008). */
#define GL_ALPHA_TEST                           0x0BC0
void glAlphaFunc(GLenum func, GLclampf ref);

/* Nudges a filled polygon's depth, so coplanar geometry can be drawn over it. Enabled with
 * GL_POLYGON_OFFSET_FILL; the line and point variants need glPolygonMode. */
#define GL_POLYGON_OFFSET_FILL                  0x8037
void glPolygonOffset(GLfloat factor, GLfloat units);

/* Where NDC z lands in the depth buffer. `near` above `far` reverses the buffer, which is a
 * technique rather than a mistake, so it is accepted. */
void glDepthRange(GLclampd near_val, GLclampd far_val);

/* Pixel formats the texture upload converts, and the unpack state that says how a client's
 * rows are laid out. A format not listed here is refused rather than converted wrongly. */
#define GL_BGR                                  0x80E0
#define GL_BGRA                                 0x80E1
#define GL_LUMINANCE                            0x1909
#define GL_LUMINANCE_ALPHA                      0x190A
#define GL_ALPHA                                0x1906
#define GL_UNPACK_ALIGNMENT                     0x0CF5
#define GL_UNPACK_ROW_LENGTH                    0x0CF2
#define GL_PACK_ALIGNMENT                       0x0D05
void glPixelStorei(GLenum pname, GLint param);
void glPixelStoref(GLenum pname, GLfloat param);
/* The framebuffer, read back. GL's origin is the bottom-left corner, so row 0 of the result is
 * the *bottom* row of the window - the rows are flipped relative to memory. */
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *pixels);
/* The framebuffer into a bound texture: render-to-texture, the 1.x way. Reads through
 * glReadPixels, so the result is the same way up as a glTexSubImage2D of the same pixels. */
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height);
/* The allocating half of the pair: sizes the texture from the window rectangle first, then
 * copies into it through the same path, so the two cannot disagree about orientation. */
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei width, GLsizei height, GLint border);

/* Display lists (D008). A list records the calls made between glNewList and glEndList and
 * replays them on glCallList; what is recorded is the call, not its effect, so a list compiled
 * before a texture is bound and executed after it draws with the later texture.
 *
 * The vertex-array draws cannot be compiled into a list here - their semantics need the client
 * pointers dereferenced at compile time - and are refused with GL_INVALID_OPERATION rather than
 * dropped. */
#define GL_COMPILE                              0x1300
#define GL_COMPILE_AND_EXECUTE                  0x1301
#define GL_LIST_BASE                            0x0B32
GLuint glGenLists(GLsizei range);
GLboolean glIsList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glCallLists(GLsizei n, GLenum type, const GLvoid *lists);
void glListBase(GLuint base);

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

/* The other spellings of the same four calls. GL 1.x names each attribute once per C type and
 * once per arity, and real 1.x code reaches for all of them - `glVertex3d` and `glColor3ub` as
 * readily as the float forms. Every one below converts and forwards to the `f` sibling above,
 * so there is one implementation of each attribute and the spellings cannot drift apart.
 *
 * The unsigned-byte colours divide by 255, which is the specification's mapping: 255 must reach
 * exactly 1.0, so it is not a shift. */
void glVertex2d(GLdouble x, GLdouble y);
void glVertex3d(GLdouble x, GLdouble y, GLdouble z);
void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w);
void glVertex2i(GLint x, GLint y);
void glVertex3i(GLint x, GLint y, GLint z);
void glVertex2fv(const GLfloat *v);
void glVertex4fv(const GLfloat *v);
void glVertex2dv(const GLdouble *v);
void glVertex3dv(const GLdouble *v);
void glVertex2iv(const GLint *v);
void glVertex3iv(const GLint *v);
void glColor3d(GLdouble red, GLdouble green, GLdouble blue);
void glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha);
void glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
void glColor3dv(const GLdouble *v);
void glColor4dv(const GLdouble *v);
void glColor3ubv(const GLubyte *v);
void glColor4ubv(const GLubyte *v);
void glTexCoord1f(GLfloat s);
void glTexCoord2d(GLdouble s, GLdouble t);
void glTexCoord2i(GLint s, GLint t);
void glTexCoord2fv(const GLfloat *v);
void glTexCoord2dv(const GLdouble *v);
void glTexCoord2iv(const GLint *v);
void glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz);
void glNormal3dv(const GLdouble *v);

/* Fixed-Function Lighting & Materials */
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightModelf(GLenum pname, GLfloat param);
/* The integer spellings. **A colour converts differently from a scalar**: an integer colour
 * component is mapped across the whole signed range onto [-1, 1], so GL_AMBIENT with INT_MAX
 * means 1.0, while a position or an attenuation is an ordinary cast. */
void glLighti(GLenum light, GLenum pname, GLint param);
void glLightiv(GLenum light, GLenum pname, const GLint *params);
void glMateriali(GLenum face, GLenum pname, GLint param);
void glMaterialiv(GLenum face, GLenum pname, const GLint *params);
void glLightModeli(GLenum pname, GLint param);
void glLightModeliv(GLenum pname, const GLint *params);
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
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
/* Residency and the priority hint. Nothing here is ever evicted, so every texture that exists
 * is resident; a name that was never generated is an error rather than a "no". */
GLboolean glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences);
void glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void glTexEnviv(GLenum target, GLenum pname, const GLint *params);
void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params);
void glGetTexEnviv(GLenum target, GLenum pname, GLint *params);

/* `glHint` is advisory, so ignoring one is allowed - but naming a hint for a feature that does
 * not exist is not. Only GL_PERSPECTIVE_CORRECTION_HINT names something real here; the fog,
 * point and line hints are refused, because this draws none of those. */
#define GL_PERSPECTIVE_CORRECTION_HINT          0x0C50
#define GL_DONT_CARE                            0x1100
#define GL_FASTEST                              0x1101
#define GL_NICEST                               0x1102
void glHint(GLenum target, GLenum mode);

/* **There is one surface**, the one the display flips, and GL_BACK names it. GL_FRONT, GL_NONE
 * and the attachments are refused rather than accepted-and-ignored: GL_NONE in particular would
 * have a program believe it had switched drawing off. */
#define GL_DRAW_BUFFER                          0x0C01
#define GL_READ_BUFFER                          0x0C02
void glDrawBuffer(GLenum buf);
void glReadBuffer(GLenum src);
GLboolean glIsTexture(GLuint texture);

/* Pipeline Synchronization & Presentation */
void glFlush(void);
void glFinish(void);
void glGetCanary(GLuint *vs_canary, GLuint *ps_canary);
void glGetCanaryEx(GLuint *vs_canary, GLuint *ps_canary, GLuint *vs_s0, GLuint *ps_s0);
/* oops-gl extension: the hardware proof. GL_TRUE only when the GPU-only clear at context creation
 * survived readback and the last submission's end-of-pipe fence and GPU clock both came back.
 * Nothing here reports which code path was chosen. */
GLboolean glIsHardwareAccelerated(void);

typedef struct gl_hw_status {
    GLboolean verified;        /* glIsHardwareAccelerated() */
    GLboolean failed;          /* a submit or fence failure stopped drawing; nothing is drawn by the CPU */
    const char *failure;       /* the reason when failed, else NULL */
    GLuint fence;              /* the end-of-pipe fence word read back: 0xbeefcafe when it fired */
    GLuint timestamp_lo;       /* the 64-bit GPU clock counter written at end of pipe */
    GLuint timestamp_hi;
    GLuint frames_confirmed;   /* submissions whose fence and clock both arrived */
    GLuint clear_colour;       /* the GPU-only clear test: colour, pixels read back in it, pixels in the target */
    GLuint clear_matched;
    GLuint clear_expected;
} gl_hw_status_t;
void glGetHardwareStatus(gl_hw_status_t *out);

/* The next submission logs its command stream, shader words, descriptors, fence and GPU clock to
 * klog as the oracle record. */
void glRequestHardwareDump(void);

/* Words the next and every later frame's command stream opens with, before oops-gl's own state:
 * an experiment hook, so a caller can put another driver's preamble in front of this one and
 * measure what the hardware and the compositor make of it (oops-mesa roadmap unit 2). The words
 * are used in place, not copied, and must stay valid until replaced; NULL or zero removes them.
 * No validation: the caller owns what the command processor is handed. */
void glSetHardwarePrelude(const GLuint *words, GLuint count);

/* The command processor's copy of the last submitted frame's render target, taken after the
 * end-of-pipe cache flush and before the timestamp, in CPU-cached memory: hash this, not the
 * uncached target. NULL on the host and before the first submission. */
const GLuint *glGetFrameReadback(void);

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
