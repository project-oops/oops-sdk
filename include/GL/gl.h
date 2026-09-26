/*
 * oops-gl: freestanding fixed-function 3D instrument for the OOPS SDK.
 *
 * Clean-room, hardware-accelerated 3D on Prospero-generation RDNA2, lowering GL-shaped
 * calls straight to PM4 with no vendor driver underneath.
 *
 * This is NOT an OpenGL version and NOT the GL to write an application against. The
 * surface is OpenGL 1.1-class fixed function, plus individual later calls added when an
 * oracle program needed one. It exists so the command stream stays readable as a
 * hardware record. For applications use oops-mesa, which provides OpenGL 3.3 Core and
 * GLSL 3.30. See oops-sdk D007.
 */

#ifndef __GL_H__
#define __GL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard GL Types */
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
/* Pointer-sized, because a buffer object's size and a byte offset into it both have to
 * be able to exceed 2GB on a 64-bit target - and because the buffer entry points below
 * take an *offset* in a parameter typed as a pointer, which only works if the two are
 * the same width. */
typedef long GLintptr;
typedef long GLsizeiptr;

/* Boolean values */
#define GL_FALSE 0
#define GL_TRUE 1

/* Clear buffer bits */
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

/* Primitives */
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009

/* Matrix Modes */
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702

/* Comparison / Depth Functions */
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

/* Blending */
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_BLEND_DST 0x0BE0
#define GL_BLEND_SRC 0x0BE1
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_ALPHA 0x80CA
#define GL_BLEND_SRC_ALPHA 0x80CB
/* Constant-colour factors and the colour they read (GL 1.4; the imaging subset before
 * it). */
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_BLEND_COLOR 0x8005

/* Logic operations. GL_LOGIC_OP is the 1.0 name for GL_INDEX_LOGIC_OP - the
 * colour-index one, which this library has no colour-index mode to apply to - so only
 * GL_COLOR_LOGIC_OP is a capability here. */
#define GL_LOGIC_OP 0x0BF1
#define GL_INDEX_LOGIC_OP 0x0BF1
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_LOGIC_OP_MODE 0x0BF0
#define GL_CLEAR 0x1500
#define GL_AND 0x1501
#define GL_AND_REVERSE 0x1502
#define GL_COPY 0x1503
#define GL_AND_INVERTED 0x1504
#define GL_NOOP 0x1505
#define GL_XOR 0x1506
#define GL_OR 0x1507
#define GL_NOR 0x1508
#define GL_EQUIV 0x1509
/* GL_INVERT (0x150A) is shared with the stencil operations and defined there. */
#define GL_OR_REVERSE 0x150B
#define GL_COPY_INVERTED 0x150C
#define GL_OR_INVERTED 0x150D
#define GL_NAND 0x150E
#define GL_SET 0x150F

/* Culling & Front Face */
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
/* The rest of GL 1.0's colour buffer names, for glDrawBuffer and glReadBuffer (Mesa
 * include/GL/gl.h). The visual is double-buffered and mono, with no auxiliary buffers.
 */
#define GL_FRONT_LEFT 0x0400
#define GL_FRONT_RIGHT 0x0401
#define GL_BACK_LEFT 0x0402
#define GL_BACK_RIGHT 0x0403
#define GL_LEFT 0x0406
#define GL_RIGHT 0x0407
#define GL_AUX0 0x0409
#define GL_AUX1 0x040A
#define GL_AUX2 0x040B
#define GL_AUX3 0x040C

/* Capabilities */
#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_COLOR_MATERIAL_FACE 0x0B55
#define GL_COLOR_MATERIAL_PARAMETER 0x0B56
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_COLOR_MATERIAL 0x0B57
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_NORMALIZE 0x0BA1
/* GL 1.2: normals rescaled by the modelview's uniform scale rather than normalised, and
 * the specular term kept apart from the rest of the lit colour and added after
 * texturing. */
#define GL_RESCALE_NORMAL 0x803A
#define GL_LIGHT_MODEL_COLOR_CONTROL 0x81F8
#define GL_SINGLE_COLOR 0x81F9
#define GL_SEPARATE_SPECULAR_COLOR 0x81FA
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_TEXTURE_2D 0x0DE1

/* Lighting Parameters */
#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_LIGHT2 0x4002
#define GL_LIGHT3 0x4003
#define GL_LIGHT4 0x4004
#define GL_LIGHT5 0x4005
#define GL_LIGHT6 0x4006
#define GL_LIGHT7 0x4007

#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209

/* Materials */
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_COLOR_INDEXES 0x1603

/* Client State (Vertex Arrays) */
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* Data types */
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DOUBLE 0x140A
/* glCallLists' byte-string name types (GL 1.0) - big-endian 2, 3 and 4 bytes a name. */
#define GL_2_BYTES 0x1407
#define GL_3_BYTES 0x1408
#define GL_4_BYTES 0x1409

/* Shading Model */
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01

/* String queries */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03

/* Error codes */
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_OUT_OF_MEMORY 0x0505

/* State Queries */
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_SCISSOR_BOX 0x0C10
#define GL_TEXTURE_BINDING_2D 0x8069

/* Texture Mapping */
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP 0x2900
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CLAMP_TO_BORDER 0x812D /* GL 1.3 */
#define GL_MIRRORED_REPEAT 0x8370 /* GL 1.4 */
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_PRIORITY 0x8066
#define GL_TEXTURE_RESIDENT 0x8067
/* GL 1.2's level-of-detail parameters */
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
/* GL 1.4's level-of-detail bias: per texture (glTexParameter) and per unit (glTexEnv
 * with the GL_TEXTURE_FILTER_CONTROL target), summed and clamped to
 * GL_MAX_TEXTURE_LOD_BIAS - 14 here, as Mesa's MAX_TEXTURE_LOD_BIAS. */
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_FILTER_CONTROL 0x8500
#define GL_MAX_TEXTURE_LOD_BIAS 0x84FD
#define OOPS_GL_MAX_TEXTURE_LOD_BIAS 14.0f
/* GL 1.4's automatic mipmaps: with GL_GENERATE_MIPMAP on, a change to the base level
 * rebuilds the levels above it by averaging; GL_GENERATE_MIPMAP_HINT is recorded. */
#define GL_GENERATE_MIPMAP 0x8191
#define GL_GENERATE_MIPMAP_HINT 0x8192
/* GL 1.4's depth textures (1D and 2D, uploaded as GL_DEPTH_COMPONENT) and the shadow
 * comparison: under GL_COMPARE_R_TO_TEXTURE each texel is 1 where r
 * GL_TEXTURE_COMPARE_FUNC the texel holds, 0 otherwise, filtered after comparing;
 * GL_DEPTH_TEXTURE_MODE says whether the result reads as luminance, intensity or alpha.
 * Stored as 32-bit floats. **Both paths sample them** since 2026-09-20: on a console
 * the image format is 32_FLOAT, the sampler's own DEPTH_COMPARE_FUNC does the
 * comparison, and the pixel shader hands it the clamped reference.
 *
 * The `_ARB` spellings below are GL_ARB_depth_texture's and GL_ARB_shadow's; both
 * extensions add enums only, and both are in the extension string. */
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32 0x81A7
#define GL_TEXTURE_DEPTH_SIZE 0x884A
#define GL_DEPTH_TEXTURE_MODE 0x884B
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_R_TO_TEXTURE 0x884E
/* **Spelled as their values, not as the core names.** A hosted title includes this
 * header and Mesa's `GL/glext.h`, which defines the same extension enums; a macro
 * redefined with a *different* token sequence is a diagnostic under `-Werror` even when
 * the value is the same, while an identical one is legal and silent. So every alias
 * below is the literal, which is what the `GL_TEXTUREn_ARB` block further down has
 * always done. Defining them in terms of the core names broke both oops-mesa probes on
 * 2026-09-20. */
#define GL_DEPTH_COMPONENT16_ARB 0x81A5
#define GL_DEPTH_COMPONENT24_ARB 0x81A6
#define GL_DEPTH_COMPONENT32_ARB 0x81A7
#define GL_TEXTURE_DEPTH_SIZE_ARB 0x884A
#define GL_DEPTH_TEXTURE_MODE_ARB 0x884B
#define GL_TEXTURE_COMPARE_MODE_ARB 0x884C
#define GL_TEXTURE_COMPARE_FUNC_ARB 0x884D
#define GL_COMPARE_R_TO_TEXTURE_ARB 0x884E
#define GL_NONE 0
#define GL_RGB 0x1907
#define GL_RGBA 0x1908

/* Internal formats (GL 1.1). Each names a base format - the components a texture keeps
 * and how the texture environment combines them - and a resolution the implementation
 * may round: every one here is stored at eight bits a component, and the size queries
 * say so. */
#define GL_ALPHA4 0x803B
#define GL_ALPHA8 0x803C
#define GL_ALPHA12 0x803D
#define GL_ALPHA16 0x803E
#define GL_LUMINANCE4 0x803F
#define GL_LUMINANCE8 0x8040
#define GL_LUMINANCE12 0x8041
#define GL_LUMINANCE16 0x8042
#define GL_LUMINANCE4_ALPHA4 0x8043
#define GL_LUMINANCE6_ALPHA2 0x8044
#define GL_LUMINANCE8_ALPHA8 0x8045
#define GL_LUMINANCE12_ALPHA4 0x8046
#define GL_LUMINANCE12_ALPHA12 0x8047
#define GL_LUMINANCE16_ALPHA16 0x8048
#define GL_INTENSITY 0x8049
#define GL_INTENSITY4 0x804A
#define GL_INTENSITY8 0x804B
#define GL_INTENSITY12 0x804C
#define GL_INTENSITY16 0x804D
#define GL_R3_G3_B2 0x2A10
#define GL_RGB4 0x804F
#define GL_RGB5 0x8050
#define GL_RGB8 0x8051
#define GL_RGB10 0x8052
#define GL_RGB12 0x8053
#define GL_RGB16 0x8054
#define GL_RGBA2 0x8055
#define GL_RGBA4 0x8056
#define GL_RGB5_A1 0x8057
#define GL_RGBA8 0x8058
#define GL_RGB10_A2 0x8059
#define GL_RGBA12 0x805A
#define GL_RGBA16 0x805B
#define GL_TEXTURE_RED_SIZE 0x805C
#define GL_TEXTURE_GREEN_SIZE 0x805D
#define GL_TEXTURE_BLUE_SIZE 0x805E
#define GL_TEXTURE_ALPHA_SIZE 0x805F
#define GL_TEXTURE_LUMINANCE_SIZE 0x8060
#define GL_TEXTURE_INTENSITY_SIZE 0x8061

/* Proxy targets (GL 1.1, 1.2): glTexImage against one allocates nothing and reports
 * through glGetTexLevelParameter whether the image would have fitted - a zero width if
 * not, and no error. */
#define GL_PROXY_TEXTURE_1D 0x8063
#define GL_PROXY_TEXTURE_2D 0x8064
#define GL_PROXY_TEXTURE_3D 0x8070

/* Texture Environment */
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_REPLACE 0x1E01

/* The combiner (GL 1.3): GL_TEXTURE_ENV_MODE GL_COMBINE, whose colour and alpha are
 * each a function of up to three arguments - each a source's colour or alpha, or one
 * minus it - scaled by 1, 2 or 4. GL_DOT3_RGB and GL_DOT3_RGBA are the colour functions
 * for bump mapping.
 * **Combined on both paths**: the console's pixel shader has a sixty-four-word slot the
 * driver writes the combiner into as instructions, one per channel for the simple forms
 * and a generated program for the rest (`tools/shader/combine.s`). */
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SUBTRACT 0x84E7
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE2_RGB 0x8582
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_SOURCE2_ALPHA 0x858A
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_OPERAND2_ALPHA 0x859A
/* GL 1.5's names for the same six (Mesa include/GL/glext.h, GL_VERSION_1_5). */
#define GL_SRC0_RGB 0x8580
#define GL_SRC1_RGB 0x8581
#define GL_SRC2_RGB 0x8582
#define GL_SRC0_ALPHA 0x8588
#define GL_SRC1_ALPHA 0x8589
#define GL_SRC2_ALPHA 0x858A
#define GL_DOT3_RGB 0x86AE
#define GL_DOT3_RGBA 0x86AF

/* Blend Equations */
#define GL_BLEND_EQUATION 0x8009
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_MIN 0x8007
#define GL_MAX 0x8008

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
void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha,
                         GLenum dfactorAlpha);
void glBlendEquation(GLenum mode);
void glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glLogicOp(GLenum opcode);
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

/* The per-object queries: state that belongs to a texture, a light or a material rather
 * than to the context. Same discipline as the state queries - an unanswerable name is
 * refused. */
void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params);
void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname,
                              GLfloat *params);
void glGetLightfv(GLenum light, GLenum pname, GLfloat *params);
void glGetLightiv(GLenum light, GLenum pname, GLint *params);
void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params);
void glGetMaterialiv(GLenum face, GLenum pname, GLint *params);
void glGetPointerv(GLenum pname, GLvoid **params);
#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_COMPONENTS 0x1003 /* GL 1.0's name for it */
#define GL_TEXTURE_BORDER 0x1005
#define GL_VERTEX_ARRAY_POINTER 0x808E
#define GL_NORMAL_ARRAY_POINTER 0x808F
#define GL_COLOR_ARRAY_POINTER 0x8090
#define GL_TEXTURE_COORD_ARRAY_POINTER 0x8092
/* The rest of each array's description, for glGetIntegerv. */
#define GL_VERTEX_ARRAY_SIZE 0x807A
#define GL_VERTEX_ARRAY_TYPE 0x807B
#define GL_VERTEX_ARRAY_STRIDE 0x807C
#define GL_NORMAL_ARRAY_TYPE 0x807E
#define GL_NORMAL_ARRAY_STRIDE 0x807F
#define GL_COLOR_ARRAY_SIZE 0x8081
#define GL_COLOR_ARRAY_TYPE 0x8082
#define GL_COLOR_ARRAY_STRIDE 0x8083
#define GL_TEXTURE_COORD_ARRAY_SIZE 0x8088
#define GL_TEXTURE_COORD_ARRAY_TYPE 0x8089
#define GL_TEXTURE_COORD_ARRAY_STRIDE 0x808A
#define GL_VERTEX_ARRAY_BUFFER_BINDING 0x8896
#define GL_NORMAL_ARRAY_BUFFER_BINDING 0x8897
#define GL_COLOR_ARRAY_BUFFER_BINDING 0x8898
#define GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING 0x889A
#define GL_EDGE_FLAG_ARRAY_BUFFER_BINDING 0x889B

/* GL 1.4's secondary colour: a second per-vertex colour, added to the primary after
 * texturing when GL_COLOR_SUM is enabled (or lighting keeps the specular term apart),
 * and its array. */
#define GL_COLOR_SUM 0x8458
#define GL_CURRENT_SECONDARY_COLOR 0x8459
#define GL_SECONDARY_COLOR_ARRAY_SIZE 0x845A
#define GL_SECONDARY_COLOR_ARRAY_TYPE 0x845B
#define GL_SECONDARY_COLOR_ARRAY_STRIDE 0x845C
#define GL_SECONDARY_COLOR_ARRAY_POINTER 0x845D
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#define GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING 0x889C

/* `glInterleavedArrays` sets the four client arrays from one buffer holding them
 * interleaved, which is how a lot of 1.x code feeds geometry. It is the pointer calls
 * it would otherwise have made, so there is nothing here the draw path does not already
 * read. */
#define GL_V2F 0x2A20
#define GL_V3F 0x2A21
#define GL_C4UB_V2F 0x2A22
#define GL_C4UB_V3F 0x2A23
#define GL_C3F_V3F 0x2A24
#define GL_N3F_V3F 0x2A25
#define GL_C4F_N3F_V3F 0x2A26
#define GL_T2F_V3F 0x2A27
#define GL_T4F_V4F 0x2A28
#define GL_T2F_C4UB_V3F 0x2A29
#define GL_T2F_C3F_V3F 0x2A2A
#define GL_T2F_N3F_V3F 0x2A2B
#define GL_T2F_C4F_N3F_V3F 0x2A2C
#define GL_T4F_C4F_N3F_V4F 0x2A2D
void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer);

/* Buffer objects (GL 1.5). A named block of memory the arrays are read out of, instead
 * of a pointer into the caller's own storage.
 *
 * **The `pointer` argument of glVertexPointer and friends becomes a byte offset** while
 * a buffer is bound to GL_ARRAY_BUFFER - the same parameter, read a different way,
 * which is the one thing about this API that surprises people. The same is true of
 * glDrawElements' `indices` while a buffer is bound to GL_ELEMENT_ARRAY_BUFFER, and
 * there an offset of 0 is both legal and indistinguishable from a null pointer.
 *
 * The array remembers the buffer *object*, not an address, because glBufferData may
 * reallocate underneath it. Resolving the address at glVertexPointer time would leave a
 * stale pointer the first time a program respecified a buffer, which is an ordinary
 * thing to do every frame.
 *
 * Enumerant values taken from oops-mesa/mesa/include/GL/glext.h. */
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_BUFFER_SIZE 0x8764
#define GL_BUFFER_USAGE 0x8765
#define GL_STREAM_DRAW 0x88E0
#define GL_STREAM_READ 0x88E1
#define GL_STREAM_COPY 0x88E2
#define GL_STATIC_DRAW 0x88E4
#define GL_STATIC_READ 0x88E5
#define GL_STATIC_COPY 0x88E6
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_DYNAMIC_READ 0x88E9
#define GL_DYNAMIC_COPY 0x88EA
/* Mapping: a buffer's store handed to the program to read or write in place until it
 * unmaps. The store is process memory here, so the pointer is the store itself. While
 * mapped the buffer may not be updated, read back or drawn from - GL_INVALID_OPERATION.
 */
#define GL_READ_ONLY 0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_READ_WRITE 0x88BA
#define GL_BUFFER_ACCESS 0x88BB
#define GL_BUFFER_MAPPED 0x88BC
#define GL_BUFFER_MAP_POINTER 0x88BD
void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                     const GLvoid *data);
void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, GLvoid *data);
GLvoid *glMapBuffer(GLenum target, GLenum access);
GLboolean glUnmapBuffer(GLenum target);
GLboolean glIsBuffer(GLuint buffer);
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);
void glGetBufferPointerv(GLenum target, GLenum pname, GLvoid **params);

/* GL 1.5's occlusion queries: the samples that pass the depth test between glBeginQuery
 * and glEndQuery of GL_SAMPLES_PASSED. Counted exactly by the software rasteriser, and
 * **by the GPU on the console** since 2026-09-20 - two ZPASS_DONE events bracket the
 * query and the answer is the sum over the render backends. GL_QUERY_COUNTER_BITS is 32
 * on both paths; it was 0 on the console, GL 1.5's way of saying the count carries no
 * information. A query whose draws never test depth is the one case the console still
 * does not count, and it says so in the log: the counters need a bound depth surface.
 * Results are always available - the count is known when glEndQuery returns, because
 * that is where the frame is submitted.
 *
 * The `_ARB` spellings are GL_ARB_occlusion_query's, which is in the extension string.
 * Its GL_SAMPLES_PASSED_ARB is the same token as the core one. */
#define GL_SAMPLES_PASSED 0x8914
#define GL_QUERY_COUNTER_BITS 0x8864
#define GL_CURRENT_QUERY 0x8865
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
/* The literals, not the core names - see GL_DEPTH_COMPONENT16_ARB for why. */
#define GL_SAMPLES_PASSED_ARB 0x8914
#define GL_QUERY_COUNTER_BITS_ARB 0x8864
#define GL_CURRENT_QUERY_ARB 0x8865
#define GL_QUERY_RESULT_ARB 0x8866
#define GL_QUERY_RESULT_AVAILABLE_ARB 0x8867
void glGenQueries(GLsizei n, GLuint *ids);
void glDeleteQueries(GLsizei n, const GLuint *ids);
GLboolean glIsQuery(GLuint id);
void glBeginQuery(GLenum target, GLuint id);
void glEndQuery(GLenum target);
void glGetQueryiv(GLenum target, GLenum pname, GLint *params);
void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params);
void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params);
void glGenQueriesARB(GLsizei n, GLuint *ids);
void glDeleteQueriesARB(GLsizei n, const GLuint *ids);
GLboolean glIsQueryARB(GLuint id);
void glBeginQueryARB(GLenum target, GLuint id);
void glEndQueryARB(GLenum target);
void glGetQueryivARB(GLenum target, GLenum pname, GLint *params);
void glGetQueryObjectivARB(GLuint id, GLenum pname, GLint *params);
void glGetQueryObjectuivARB(GLuint id, GLenum pname, GLuint *params);

/* The bound texture, read back. Unlike glReadPixels this does **not** flip: a texture
 * has no window origin, so row 0 of the result is row 0 of what was uploaded. */
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type,
                   GLvoid *pixels);

/* What the queries above can be asked for. Every one of these is a number already held
 * in the context; the family is a dispatch table rather than a feature.
 *
 * A pname not listed here is **refused with GL_INVALID_ENUM**, not ignored. Ignoring it
 * leaves the caller's buffer holding whatever it held before - usually stack garbage -
 * with no error raised, which is the worst of the three possible behaviours: the
 * program reads a number that looks real.
 *
 * Enumerant values taken from oops-mesa/mesa/include/GL/gl.h, not from memory. */
#define GL_CURRENT_COLOR 0x0B00
#define GL_CURRENT_NORMAL 0x0B02
#define GL_CURRENT_TEXTURE_COORDS 0x0B03
#define GL_LIST_BASE 0x0B32
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_SHADE_MODEL 0x0B54
#define GL_CULL_FACE_MODE 0x0B45
#define GL_FRONT_FACE 0x0B46
#define GL_DEPTH_RANGE 0x0B70
#define GL_DEPTH_WRITEMASK 0x0B72
#define GL_DEPTH_CLEAR_VALUE 0x0B73
#define GL_DEPTH_FUNC 0x0B74
#define GL_ALPHA_TEST_FUNC 0x0BC1
#define GL_ALPHA_TEST_REF 0x0BC2
#define GL_ATTRIB_STACK_DEPTH 0x0BB0
#define GL_CLIENT_ATTRIB_STACK_DEPTH 0x0BB1
#define GL_MODELVIEW_STACK_DEPTH 0x0BA3
#define GL_PROJECTION_STACK_DEPTH 0x0BA4
#define GL_TEXTURE_STACK_DEPTH 0x0BA5
/* State queries the specification's tables list that were not declared until 2026-09-19
 * - found by querying every table name (values from Mesa include/GL/gl.h:250-491,
 * :1443-1444). */
#define GL_LIST_MODE 0x0B30
#define GL_MAX_LIST_NESTING 0x0B31
#define GL_LIST_INDEX 0x0B33
#define GL_CURRENT_RASTER_INDEX 0x0B05
#define GL_AUX_BUFFERS 0x0C00
#define GL_DOUBLEBUFFER 0x0C32
#define GL_STEREO 0x0C33
#define GL_SUBPIXEL_BITS 0x0D50
#define GL_MAX_ELEMENTS_VERTICES 0x80E8
#define GL_MAX_ELEMENTS_INDICES 0x80E9
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_MAX_LIGHTS 0x0D31
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_ATTRIB_STACK_DEPTH 0x0D35
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_TEXTURE_STACK_DEPTH 0x0D39
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_MAX_CLIENT_ATTRIB_STACK_DEPTH 0x0D3B
#define GL_POLYGON_OFFSET_FACTOR 0x8038
#define GL_POLYGON_OFFSET_UNITS 0x2A00

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
/* The double spellings. The matrices are held as float either way, so these narrow and
 * forward rather than carrying a second precision through the stacks - which is what a
 * GL 1.x implementation does in practice, and what makes
 * glLoadMatrixd(glGetDoublev(...)) round-trip to the same picture rather than a
 * slightly different one. */
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z);
void glScaled(GLdouble x, GLdouble y, GLdouble z);
void glLoadMatrixd(const GLdouble *m);
void glMultMatrixd(const GLdouble *m);
/* The transposed spellings: the same sixteen numbers written row-major, which is how a
 * C programmer naturally writes a matrix literal. */
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

/* GL 1.4: the current secondary colour, every type normalised as glColor's are, and its
 * array. */
void glSecondaryColor3f(GLfloat red, GLfloat green, GLfloat blue);
void glSecondaryColor3d(GLdouble red, GLdouble green, GLdouble blue);
void glSecondaryColor3b(GLbyte red, GLbyte green, GLbyte blue);
void glSecondaryColor3s(GLshort red, GLshort green, GLshort blue);
void glSecondaryColor3i(GLint red, GLint green, GLint blue);
void glSecondaryColor3ub(GLubyte red, GLubyte green, GLubyte blue);
void glSecondaryColor3us(GLushort red, GLushort green, GLushort blue);
void glSecondaryColor3ui(GLuint red, GLuint green, GLuint blue);
void glSecondaryColor3fv(const GLfloat *v);
void glSecondaryColor3dv(const GLdouble *v);
void glSecondaryColor3bv(const GLbyte *v);
void glSecondaryColor3sv(const GLshort *v);
void glSecondaryColor3iv(const GLint *v);
void glSecondaryColor3ubv(const GLubyte *v);
void glSecondaryColor3usv(const GLushort *v);
void glSecondaryColor3uiv(const GLuint *v);
void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride,
                             const GLvoid *pointer);

/* Draw Operations */
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
/* One vertex pulled out of the enabled arrays, inside glBegin/glEnd - the bridge
 * between the two ways of feeding geometry, for a program that keeps its data in arrays
 * but wants to assemble the primitives by hand. */
void glArrayElement(GLint i);
/* glDrawElements plus a promise about the index range. The promise is a hint an
 * implementation may use or ignore; this one ignores it, which the specification
 * allows. */
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                         GLenum type, const GLvoid *indices);
/* GL 1.4: several glDrawArrays or glDrawElements of one mode in a call - every count
 * checked before any is drawn, and an empty one skipped. */
void glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count,
                       GLsizei drawcount);
void glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
                         const GLvoid *const *indices, GLsizei drawcount);

/* Immediate Mode Emulation */
/* The attribute stack. Every GL 1.x attribute group exists; a bit outside them is
 * refused. GL_ALL_ATTRIB_BITS is narrowed to those groups. GL_POINT_BIT, GL_LINE_BIT,
 * GL_HINT_BIT, GL_MULTISAMPLE_BIT, GL_EVAL_BIT, GL_PIXEL_MODE_BIT,
 * GL_POLYGON_STIPPLE_BIT and GL_ACCUM_BUFFER_BIT are declared with the state they save.
 */
#define GL_CURRENT_BIT 0x00000001
#define GL_POLYGON_BIT 0x00000008
#define GL_LIGHTING_BIT 0x00000040
#define GL_FOG_BIT 0x00000080
#define GL_DEPTH_BUFFER_BIT_ATTRIB 0x00000100
#define GL_VIEWPORT_BIT 0x00000800
#define GL_TRANSFORM_BIT 0x00001000
#define GL_ENABLE_BIT 0x00002000
#define GL_LIST_BIT 0x00020000
#define GL_TEXTURE_BIT 0x00040000
#define GL_SCISSOR_BIT 0x00080000
/* The Khronos value (Mesa include/GL/gl.h:683). This header had GL 1.0's 0x000FFFFF,
 * which leaves out GL_MULTISAMPLE_BIT; glPushAttrib accepts either as "all". */
#define GL_ALL_ATTRIB_BITS 0xFFFFFFFF
void glPushAttrib(GLbitfield mask);
void glPopAttrib(void);

/* The client half of the attribute stack: state that lives in this process rather than
 * on the GPU - the array pointers and the pixel-store modes. It is a separate stack in
 * the specification and a separate stack here, because a library that brackets its
 * array setup with glPushClientAttrib must not disturb the server state its caller
 * pushed. */
#define GL_CLIENT_PIXEL_STORE_BIT 0x00000001
#define GL_CLIENT_VERTEX_ARRAY_BIT 0x00000002
#define GL_CLIENT_ALL_ATTRIB_BITS 0xFFFFFFFF
#define GL_ALL_CLIENT_ATTRIB_BITS 0xFFFFFFFF /* the spelling Mesa's gl.h also has */
void glPushClientAttrib(GLbitfield mask);
void glPopClientAttrib(void);

/* `glRect*` draws a screen-aligned rectangle in the z=0 plane with the current colour,
 * normal and texture coordinate. The specification writes it as a four-vertex
 * GL_POLYGON; a rectangle is convex, so GL_QUADS draws the identical two triangles and
 * is a primitive this supports. */
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2);
void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2);
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2);
void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2);
void glRectfv(const GLfloat *v1, const GLfloat *v2);
void glRectdv(const GLdouble *v1, const GLdouble *v2);
void glRectiv(const GLint *v1, const GLint *v2);
void glRectsv(const GLshort *v1, const GLshort *v2);

/* Discards a fragment whose alpha fails the comparison. RDNA2 has no fixed-function
 * alpha test, so this is a discard written into the pixel shader (D008). */
#define GL_ALPHA_TEST 0x0BC0
void glAlphaFunc(GLenum func, GLclampf ref);

/* Nudges a polygon's depth, so coplanar geometry can be drawn over it. Each enable
 * covers the polygons glPolygonMode draws that way: GL_POLYGON_OFFSET_FILL the filled
 * ones, GL_POLYGON_OFFSET_LINE the outlines, GL_POLYGON_OFFSET_POINT the corner points.
 * None of them touches a GL_LINES or GL_POINTS primitive. */
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_POLYGON_OFFSET_POINT 0x2A01
#define GL_POLYGON_OFFSET_LINE 0x2A02
void glPolygonOffset(GLfloat factor, GLfloat units);

/* How a polygon is drawn, per face: filled, as its boundary edges, or as its boundary
 * vertices. An outline is the polygon's own edges - never the diagonals a quad or
 * polygon is triangulated with - and edge flags mark which of those count. */
#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
#define GL_POLYGON_MODE 0x0B40
#define GL_EDGE_FLAG 0x0B43
#define GL_EDGE_FLAG_ARRAY 0x8079
#define GL_EDGE_FLAG_ARRAY_STRIDE 0x808C
#define GL_EDGE_FLAG_ARRAY_POINTER 0x8093
void glPolygonMode(GLenum face, GLenum mode);
void glEdgeFlag(GLboolean flag);
void glEdgeFlagv(const GLboolean *flag);
void glEdgeFlagPointer(GLsizei stride, const GLvoid *pointer);

/* Colour-index state. This is an RGBA context - GL_INDEX_MODE answers GL_FALSE and
 * GL_RGBA_MODE GL_TRUE - and in an RGBA context the specification keeps the current
 * index, the index write mask, the clear index and the index array exactly as state and
 * uses none of them to draw. So storing and reporting them is the complete
 * implementation, not a stand-in for one. The index is a plain number, not a normalised
 * colour: every spelling converts by cast. */
#define GL_CURRENT_INDEX 0x0B01
#define GL_INDEX_CLEAR_VALUE 0x0C20
#define GL_INDEX_WRITEMASK 0x0C21
#define GL_INDEX_MODE 0x0C30
#define GL_RGBA_MODE 0x0C31
#define GL_INDEX_BITS 0x0D51
#define GL_INDEX_ARRAY 0x8077
#define GL_INDEX_ARRAY_TYPE 0x8085
#define GL_INDEX_ARRAY_STRIDE 0x8086
#define GL_INDEX_ARRAY_POINTER 0x8091
#define GL_INDEX_ARRAY_BUFFER_BINDING 0x8899
void glIndexf(GLfloat c);
void glIndexd(GLdouble c);
void glIndexi(GLint c);
void glIndexs(GLshort c);
void glIndexub(GLubyte c);
void glIndexfv(const GLfloat *c);
void glIndexdv(const GLdouble *c);
void glIndexiv(const GLint *c);
void glIndexsv(const GLshort *c);
void glIndexubv(const GLubyte *c);
void glClearIndex(GLfloat c);
void glIndexMask(GLuint mask);
void glIndexPointer(GLenum type, GLsizei stride, const GLvoid *pointer);

/* Multisampling (GL 1.3). There is no multisample buffer - GL_SAMPLE_BUFFERS and
 * GL_SAMPLES are 0 - and with none, the specification says these enables and the
 * coverage value have no effect on rendering. They are still state: GL_MULTISAMPLE
 * starts enabled, and all of it is queried and saved with GL_MULTISAMPLE_BIT. */
#define GL_MULTISAMPLE 0x809D
#define GL_SAMPLE_ALPHA_TO_COVERAGE 0x809E
#define GL_SAMPLE_ALPHA_TO_ONE 0x809F
#define GL_SAMPLE_COVERAGE 0x80A0
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_SAMPLES 0x80A9
#define GL_SAMPLE_COVERAGE_VALUE 0x80AA
#define GL_SAMPLE_COVERAGE_INVERT 0x80AB
#define GL_MULTISAMPLE_BIT 0x20000000
void glSampleCoverage(GLclampf value, GLboolean invert);

/* Dithering. On by default, and **a flag with no effect here** - which is what Mesa's
 * hardware drivers make of it too: the colour block rounds, and the specification
 * leaves the dithering algorithm to the implementation. Accepted and reported so the
 * glDisable(GL_DITHER) most 1.x programs start with is not an error. */
#define GL_DITHER 0x0BD0

/* The attribute groups for state that exists and had no bit to save it with. */
#define GL_POINT_BIT 0x00000002
#define GL_LINE_BIT 0x00000004
#define GL_HINT_BIT 0x00008000

/* Evaluators: Bezier curves and surfaces, evaluated on the CPU into ordinary glVertex,
 * glNormal, glColor and glTexCoord calls. A map is a target's control points over a
 * domain; glEvalCoord evaluates every enabled map of its dimension at one point and
 * issues the vertex, and glMapGrid with glEvalMesh walks a grid of such points as
 * points, lines or quad strips. Evaluating never changes the current colour, normal or
 * texture coordinate. A colour-index map is kept and reported and, this being an RGBA
 * context, evaluates to nothing. */
#define GL_AUTO_NORMAL 0x0D80
#define GL_MAP1_COLOR_4 0x0D90
#define GL_MAP1_INDEX 0x0D91
#define GL_MAP1_NORMAL 0x0D92
#define GL_MAP1_TEXTURE_COORD_1 0x0D93
#define GL_MAP1_TEXTURE_COORD_2 0x0D94
#define GL_MAP1_TEXTURE_COORD_3 0x0D95
#define GL_MAP1_TEXTURE_COORD_4 0x0D96
#define GL_MAP1_VERTEX_3 0x0D97
#define GL_MAP1_VERTEX_4 0x0D98
#define GL_MAP2_COLOR_4 0x0DB0
#define GL_MAP2_INDEX 0x0DB1
#define GL_MAP2_NORMAL 0x0DB2
#define GL_MAP2_TEXTURE_COORD_1 0x0DB3
#define GL_MAP2_TEXTURE_COORD_2 0x0DB4
#define GL_MAP2_TEXTURE_COORD_3 0x0DB5
#define GL_MAP2_TEXTURE_COORD_4 0x0DB6
#define GL_MAP2_VERTEX_3 0x0DB7
#define GL_MAP2_VERTEX_4 0x0DB8
#define GL_MAP1_GRID_DOMAIN 0x0DD0
#define GL_MAP1_GRID_SEGMENTS 0x0DD1
#define GL_MAP2_GRID_DOMAIN 0x0DD2
#define GL_MAP2_GRID_SEGMENTS 0x0DD3
#define GL_COEFF 0x0A00
#define GL_ORDER 0x0A01
#define GL_DOMAIN 0x0A02
#define GL_MAX_EVAL_ORDER 0x0D30
#define GL_EVAL_BIT 0x00010000
void glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order,
             const GLfloat *points);
void glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order,
             const GLdouble *points);
void glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder,
             GLfloat v1, GLfloat v2, GLint vstride, GLint vorder,
             const GLfloat *points);
void glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder,
             GLdouble v1, GLdouble v2, GLint vstride, GLint vorder,
             const GLdouble *points);
void glGetMapfv(GLenum target, GLenum query, GLfloat *v);
void glGetMapdv(GLenum target, GLenum query, GLdouble *v);
void glGetMapiv(GLenum target, GLenum query, GLint *v);
void glEvalCoord1f(GLfloat u);
void glEvalCoord1d(GLdouble u);
void glEvalCoord1fv(const GLfloat *u);
void glEvalCoord1dv(const GLdouble *u);
void glEvalCoord2f(GLfloat u, GLfloat v);
void glEvalCoord2d(GLdouble u, GLdouble v);
void glEvalCoord2fv(const GLfloat *u);
void glEvalCoord2dv(const GLdouble *u);
void glMapGrid1f(GLint un, GLfloat u1, GLfloat u2);
void glMapGrid1d(GLint un, GLdouble u1, GLdouble u2);
void glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2);
void glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1,
                 GLdouble v2);
void glEvalPoint1(GLint i);
void glEvalPoint2(GLint i, GLint j);
void glEvalMesh1(GLenum mode, GLint i1, GLint i2);
void glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2);

/* Selection and feedback: the other two render modes, in which primitives are
 * transformed, lit and clipped and then reported instead of drawn. GL_SELECT records a
 * hit - the name stack and the depth range - for every primitive that survives clipping
 * and culling, which is how a GL 1.x program picks with the mouse (a pick matrix around
 * the cursor, then the scene drawn again). GL_FEEDBACK writes each clipped primitive's
 * window coordinates, colours and texture coordinates into the caller's buffer. Nothing
 * reaches the framebuffer in either, glClear included. glRenderMode answers how much
 * was written, or -1 if the buffer overflowed. */
#define GL_RENDER 0x1C00
#define GL_FEEDBACK 0x1C01
#define GL_SELECT 0x1C02
#define GL_RENDER_MODE 0x0C40
#define GL_2D 0x0600
#define GL_3D 0x0601
#define GL_3D_COLOR 0x0602
#define GL_3D_COLOR_TEXTURE 0x0603
#define GL_4D_COLOR_TEXTURE 0x0604
#define GL_PASS_THROUGH_TOKEN 0x0700
#define GL_POINT_TOKEN 0x0701
#define GL_LINE_TOKEN 0x0702
#define GL_POLYGON_TOKEN 0x0703
#define GL_BITMAP_TOKEN 0x0704
#define GL_DRAW_PIXEL_TOKEN 0x0705
#define GL_COPY_PIXEL_TOKEN 0x0706
#define GL_LINE_RESET_TOKEN 0x0707
#define GL_FEEDBACK_BUFFER_POINTER 0x0DF0
#define GL_FEEDBACK_BUFFER_SIZE 0x0DF1
#define GL_FEEDBACK_BUFFER_TYPE 0x0DF2
#define GL_SELECTION_BUFFER_POINTER 0x0DF3
#define GL_SELECTION_BUFFER_SIZE 0x0DF4
#define GL_MAX_NAME_STACK_DEPTH 0x0D37
#define GL_NAME_STACK_DEPTH 0x0D70
/* Pixel transfer: what happens to every pixel rectangle's colours on the way in -
 * glDrawPixels, glCopyPixels, the texture uploads and copies - and on the way out of
 * glReadPixels. Each component is scaled and biased, then with GL_MAP_COLOR looked up
 * in its GL_PIXEL_MAP_x_TO_x table, then clamped. The colour-index and stencil maps,
 * the index shift and offset and the depth scale and bias are kept and reported, and
 * act on no pixels here: the index, stencil and depth pixel formats they apply to are
 * refused. glGetTexImage is not transferred, as in Mesa. */
#define GL_MAP_COLOR 0x0D10
#define GL_MAP_STENCIL 0x0D11
#define GL_INDEX_SHIFT 0x0D12
#define GL_INDEX_OFFSET 0x0D13
#define GL_RED_SCALE 0x0D14
#define GL_RED_BIAS 0x0D15
#define GL_GREEN_SCALE 0x0D18
#define GL_GREEN_BIAS 0x0D19
#define GL_BLUE_SCALE 0x0D1A
#define GL_BLUE_BIAS 0x0D1B
#define GL_ALPHA_SCALE 0x0D1C
#define GL_ALPHA_BIAS 0x0D1D
#define GL_DEPTH_SCALE 0x0D1E
#define GL_DEPTH_BIAS 0x0D1F
#define GL_PIXEL_MAP_I_TO_I 0x0C70
#define GL_PIXEL_MAP_S_TO_S 0x0C71
#define GL_PIXEL_MAP_I_TO_R 0x0C72
#define GL_PIXEL_MAP_I_TO_G 0x0C73
#define GL_PIXEL_MAP_I_TO_B 0x0C74
#define GL_PIXEL_MAP_I_TO_A 0x0C75
#define GL_PIXEL_MAP_R_TO_R 0x0C76
#define GL_PIXEL_MAP_G_TO_G 0x0C77
#define GL_PIXEL_MAP_B_TO_B 0x0C78
#define GL_PIXEL_MAP_A_TO_A 0x0C79
#define GL_PIXEL_MAP_I_TO_I_SIZE 0x0CB0
#define GL_PIXEL_MAP_S_TO_S_SIZE 0x0CB1
#define GL_PIXEL_MAP_I_TO_R_SIZE 0x0CB2
#define GL_PIXEL_MAP_I_TO_G_SIZE 0x0CB3
#define GL_PIXEL_MAP_I_TO_B_SIZE 0x0CB4
#define GL_PIXEL_MAP_I_TO_A_SIZE 0x0CB5
#define GL_PIXEL_MAP_R_TO_R_SIZE 0x0CB6
#define GL_PIXEL_MAP_G_TO_G_SIZE 0x0CB7
#define GL_PIXEL_MAP_B_TO_B_SIZE 0x0CB8
#define GL_PIXEL_MAP_A_TO_A_SIZE 0x0CB9
#define GL_MAX_PIXEL_MAP_TABLE 0x0D34
#define GL_PIXEL_MODE_BIT 0x00000020
void glPixelTransferf(GLenum pname, GLfloat param);
void glPixelTransferi(GLenum pname, GLint param);
void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat *values);
void glPixelMapuiv(GLenum map, GLsizei mapsize, const GLuint *values);
void glPixelMapusv(GLenum map, GLsizei mapsize, const GLushort *values);
void glGetPixelMapfv(GLenum map, GLfloat *values);
void glGetPixelMapuiv(GLenum map, GLuint *values);
void glGetPixelMapusv(GLenum map, GLushort *values);

/* Stipple. A stippled line is cut into its dashes before it is widened - a 16-bit
 * pattern, each bit covering `factor` pixels along the line's major axis, counting on
 * across a strip and starting again for each separate line and each outlined polygon -
 * so the dashes are drawn like any line, on both paths. A stippled polygon keeps the
 * fragments whose window position finds a 1 in a 32x32 mask, bottom row first - **on
 * both paths** since 2026-09-20, the console getting the fragment's position in the
 * pixel shader and killing the lanes whose bit is clear
 * (`tools/shader/polygon-stipple.s`). */
#define GL_LINE_STIPPLE 0x0B24
#define GL_LINE_STIPPLE_PATTERN 0x0B25
#define GL_LINE_STIPPLE_REPEAT 0x0B26
#define GL_POLYGON_STIPPLE 0x0B42
#define GL_POLYGON_STIPPLE_BIT 0x00000010
void glLineStipple(GLint factor, GLushort pattern);
void glPolygonStipple(const GLubyte *mask);
void glGetPolygonStipple(GLubyte *mask);

/* 3D textures (GL 1.2): their own binding, enable and default texture, a third
 * coordinate r with its own wrap mode, mip levels that halve depth as well, and images
 * addressed slice by slice with GL_UNPACK_IMAGE_HEIGHT. GL_TEXTURE_3D beats 2D and 1D
 * when several are enabled. **Both paths sample them** since 2026-09-20: on a console
 * the descriptor carries TYPE 0xa and the last slice, the vertex carries r in its third
 * parameter, and the pixel shader samples with three coordinates. A volume with no
 * image yet is drawn untextured and says so once in the log. The mip chain is still
 * level 0 only - a volume's levels halve depth as well, which the chain builder here
 * does not lay out. */
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_BINDING_3D 0x806A
/* Cube maps (GL 1.3): six square faces, each uploaded through glTexImage2D against its
 * own target, looked up by a direction - (s, t, r), usually generated by
 * GL_REFLECTION_MAP or GL_NORMAL_MAP. **Sampled on both paths** since 2026-09-20: on a
 * console the six faces upload as one array and the pixel shader finds the face from
 * the direction. An incomplete cube map is drawn untextured on both, which is what GL
 * does with one. */
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_PROXY_TEXTURE_CUBE_MAP 0x851B
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE 0x851C
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_DEPTH 0x8071
#define GL_MAX_3D_TEXTURE_SIZE 0x8073
#define GL_PACK_IMAGE_HEIGHT 0x806C
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLsizei depth, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels);
void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                     GLenum format, GLenum type, const GLvoid *pixels);
void glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint zoffset, GLint x, GLint y, GLsizei width,
                         GLsizei height);

/* The accumulation buffer: a signed 16-bit colour per pixel, kept on the CPU and
 * allocated the first time it is used. glAccum adds the colour buffer into it
 * (GL_ACCUM), loads it from the colour buffer (GL_LOAD), scales or offsets it (GL_MULT,
 * GL_ADD), or writes it back scaled (GL_RETURN) through the colour mask. Every
 * operation, and glClear's GL_ACCUM_BUFFER_BIT, keeps to the scissor box when the
 * scissor test is on. Motion blur and antialiasing by jitter are what it is for. The
 * colour-buffer and depth-buffer sizes are reported alongside it. */
#define GL_ACCUM 0x0100
#define GL_LOAD 0x0101
#define GL_RETURN 0x0102
#define GL_MULT 0x0103
#define GL_ACCUM_CLEAR_VALUE 0x0B80
#define GL_ACCUM_RED_BITS 0x0D58
#define GL_ACCUM_GREEN_BITS 0x0D59
#define GL_ACCUM_BLUE_BITS 0x0D5A
#define GL_ACCUM_ALPHA_BITS 0x0D5B
#define GL_ACCUM_BUFFER_BIT 0x00000200
#define GL_RED_BITS 0x0D52
#define GL_GREEN_BITS 0x0D53
#define GL_BLUE_BITS 0x0D54
#define GL_ALPHA_BITS 0x0D55
#define GL_DEPTH_BITS 0x0D56
void glAccum(GLenum op, GLfloat value);
void glClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);

GLint glRenderMode(GLenum mode);
void glSelectBuffer(GLsizei size, GLuint *buffer);
void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer);
void glInitNames(void);
void glLoadName(GLuint name);
void glPushName(GLuint name);
void glPopName(void);
void glPassThrough(GLfloat token);

/* Where NDC z lands in the depth buffer. `near` above `far` reverses the buffer, which
 * is a technique rather than a mistake, so it is accepted. */
void glDepthRange(GLclampd near_val, GLclampd far_val);

/* Pixel rectangles in the program's memory, for every call that reads or writes one.
 * Formats: GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_RGB, GL_BGR, GL_RGBA, GL_BGRA,
 * GL_LUMINANCE and GL_LUMINANCE_ALPHA. Types: GL_UNSIGNED_BYTE, GL_BYTE,
 * GL_UNSIGNED_SHORT, GL_SHORT, GL_UNSIGNED_INT, GL_INT, GL_FLOAT, and GL 1.2's packed
 * types below (a packed type with a format it does not fit is GL_INVALID_OPERATION).
 * Every glPixelStorei parameter of GL 1.2 is kept, on both the unpack and pack sides.
 * Colour-index, stencil and depth rectangles are refused. */
#define GL_BGR 0x80E0
#define GL_BGRA 0x80E1
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_ALPHA 0x1906
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_BLUE 0x1905
#define GL_UNSIGNED_BYTE_3_3_2 0x8032
#define GL_UNSIGNED_BYTE_2_3_3_REV 0x8362
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_5_6_5_REV 0x8364
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_INT_8_8_8_8 0x8035
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_UNSIGNED_INT_10_10_10_2 0x8036
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#define GL_UNPACK_SWAP_BYTES 0x0CF0
#define GL_UNPACK_LSB_FIRST 0x0CF1
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_SKIP_IMAGES 0x806D
#define GL_PACK_SWAP_BYTES 0x0D00
#define GL_PACK_LSB_FIRST 0x0D01
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_SKIP_IMAGES 0x806B
void glPixelStorei(GLenum pname, GLint param);
void glPixelStoref(GLenum pname, GLfloat param);
/* The framebuffer, read back. GL's origin is the bottom-left corner, so row 0 of the
 * result is the *bottom* row of the window - the rows are flipped relative to memory.
 */
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                  GLenum type, GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *pixels);
/* The framebuffer into a bound texture: render-to-texture, the 1.x way. Reads through
 * glReadPixels, so the result is the same way up as a glTexSubImage2D of the same
 * pixels. */
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height);
/* The allocating half of the pair: sizes the texture from the window rectangle first,
 * then copies into it through the same path, so the two cannot disagree about
 * orientation. */
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x,
                      GLint y, GLsizei width, GLsizei height, GLint border);

/* Display lists (D008). A list records the calls made between glNewList and glEndList
 * and replays them on glCallList; what is recorded is the call, not its effect, so a
 * list compiled before a texture is bound and executed after it draws with the later
 * texture.
 *
 * The vertex-array draws cannot be compiled into a list here - their semantics need the
 * client pointers dereferenced at compile time - and are refused with
 * GL_INVALID_OPERATION rather than dropped. */
#define GL_COMPILE 0x1300
#define GL_COMPILE_AND_EXECUTE 0x1301
#define GL_LIST_BASE 0x0B32
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

/* The other spellings of the same four calls. GL 1.x names each attribute once per C
 * type and once per arity, and real 1.x code reaches for all of them - `glVertex3d` and
 * `glColor3ub` as readily as the float forms. Every one below converts and forwards to
 * the `f` sibling above, so there is one implementation of each attribute and the
 * spellings cannot drift apart.
 *
 * The unsigned-byte colours divide by 255, which is the specification's mapping: 255
 * must reach exactly 1.0, so it is not a shift. */
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

/* Fog.
 *
 * A per-fragment blend towards the fog colour by distance, which is much of what this
 * generation of 3D looked like. The three modes are the specification's: linear between
 * a start and an end, and two exponentials by density. Fog does **not** touch alpha, so
 * the alpha test sees the same value with fog on or off.
 *
 * `GL_FOG_INDEX` belongs to colour-index mode: kept and reported, never drawn with,
 * like the rest of colour-index state in an RGBA context. */
#define GL_FOG 0x0B60
#define GL_FOG_INDEX 0x0B61
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801

/* GL 1.4's fog coordinate: with GL_FOG_COORD_SRC set to GL_FOG_COORD, fog reads a
 * per-vertex value the program supplies instead of the eye distance. GL 1.5 renamed the
 * enums; both spellings are the same values. */
#define GL_FOG_COORDINATE_SOURCE 0x8450
#define GL_FOG_COORDINATE 0x8451
#define GL_FRAGMENT_DEPTH 0x8452
#define GL_CURRENT_FOG_COORDINATE 0x8453
#define GL_FOG_COORDINATE_ARRAY_TYPE 0x8454
#define GL_FOG_COORDINATE_ARRAY_STRIDE 0x8455
#define GL_FOG_COORDINATE_ARRAY_POINTER 0x8456
#define GL_FOG_COORDINATE_ARRAY 0x8457
#define GL_FOG_COORDINATE_ARRAY_BUFFER_BINDING 0x889D
#define GL_FOG_COORD_SRC 0x8450
#define GL_FOG_COORD 0x8451
#define GL_CURRENT_FOG_COORD 0x8453
#define GL_FOG_COORD_ARRAY_TYPE 0x8454
#define GL_FOG_COORD_ARRAY_STRIDE 0x8455
#define GL_FOG_COORD_ARRAY_POINTER 0x8456
#define GL_FOG_COORD_ARRAY 0x8457
#define GL_FOG_COORD_ARRAY_BUFFER_BINDING 0x889D

void glFogf(GLenum pname, GLfloat param);
void glFogi(GLenum pname, GLint param);
void glFogfv(GLenum pname, const GLfloat *params);
void glFogiv(GLenum pname, const GLint *params);
void glFogCoordf(GLfloat coord);
void glFogCoordd(GLdouble coord);
void glFogCoordfv(const GLfloat *coord);
void glFogCoorddv(const GLdouble *coord);
void glFogCoordPointer(GLenum type, GLsizei stride, const GLvoid *pointer);

/* Points and lines.
 *
 * **Drawn as triangles, because the geometry engine will not take one or two
 * vertices.** That is measured, not assumed: obSCEne submitted a one-vertex point and a
 * two-vertex line on retail hardware across five sweeps - including one on oops-gl's
 * own `VGT_SHADER_STAGES_EN` - and every run recorded `fence-hit 0`. The pipe stops
 * rather than the primitive drawing wrongly.
 *
 * What that measurement closed is the *native* primitive, not the feature. A line is a
 * screen width quad and a point is a square, both of which are triangles, and this
 * already expands `GL_QUADS` the same way. So the geometry engine never sees fewer than
 * three vertices and no stage this does not build is needed. */
#define GL_POINT_SIZE 0x0B11
#define GL_POINT_SIZE_RANGE 0x0B12
#define GL_POINT_SIZE_GRANULARITY 0x0B13
#define GL_LINE_WIDTH 0x0B21
#define GL_LINE_WIDTH_RANGE 0x0B22
#define GL_LINE_WIDTH_GRANULARITY 0x0B23
#define GL_ALIASED_POINT_SIZE_RANGE 0x846D
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
/* Sizes and widths are drawn as the specification draws aliased ones: rounded to the
 * nearest integer, at least 1, and at most OOPS_GL_MAX_POINT_LINE_SIZE - the range
 * every
 * *_RANGE query reports. **Smoothed** (GL_POINT_SMOOTH, GL_LINE_SMOOTH) they are not
 * rounded but taken in steps of OOPS_GL_SMOOTH_GRANULARITY, which the *_GRANULARITY
 * queries - GL 1.2's GL_SMOOTH_* names for the same enums - report. */
#define OOPS_GL_MAX_POINT_LINE_SIZE 256
#define OOPS_GL_SMOOTH_GRANULARITY 0.125f
#define GL_SMOOTH_POINT_SIZE_RANGE 0x0B12
#define GL_SMOOTH_POINT_SIZE_GRANULARITY 0x0B13
#define GL_SMOOTH_LINE_WIDTH_RANGE 0x0B22
#define GL_SMOOTH_LINE_WIDTH_GRANULARITY 0x0B23
/* Antialiasing: each fragment's alpha multiplied by the fraction of its pixel the
 * point, line or polygon covers. The software rasteriser does all three. **The console
 * does points and lines** since 2026-09-20 - the CPU widens the primitive into a quad
 * and writes each corner's offset from the centre where the pixel shader can
 * interpolate it. It still draws a *textured* smooth primitive aliased, the parameter
 * that offset rides in being the texture coordinate, and GL_POLYGON_SMOOTH always: a
 * polygon's coverage is three edge fades rather than one distance, and it needs the
 * pixels a triangle only partly covers, which the rasteriser does not raise. Both say
 * so in the log the first time they matter. */
#define GL_POINT_SMOOTH 0x0B10
#define GL_LINE_SMOOTH 0x0B20
#define GL_POLYGON_SMOOTH 0x0B41

void glPointSize(GLfloat size);
void glLineWidth(GLfloat width);

/* GL 1.4's point parameters: every point's size clamped to [GL_POINT_SIZE_MIN,
 * GL_POINT_SIZE_MAX], and with GL_POINT_DISTANCE_ATTENUATION (a, b, c) divided first by
 * sqrt(a + b d + c d^2), d the point's eye distance - points that shrink into the
 * distance. GL_POINT_FADE_THRESHOLD_SIZE is kept and reported: its fade applies to
 * multisampled points, and there is no multisample buffer here. */
#define GL_POINT_SIZE_MIN 0x8126
#define GL_POINT_SIZE_MAX 0x8127
#define GL_POINT_FADE_THRESHOLD_SIZE 0x8128
#define GL_POINT_DISTANCE_ATTENUATION 0x8129
void glPointParameterf(GLenum pname, GLfloat param);
void glPointParameterfv(GLenum pname, const GLfloat *params);
void glPointParameteri(GLenum pname, GLint param);
void glPointParameteriv(GLenum pname, const GLint *params);

/* Compressed textures.
 *
 * **This implementation supports no compressed formats, and says so.**
 * `GL_NUM_COMPRESSED_TEXTURE_FORMATS` is 0 and every `glCompressedTexImage*` call is
 * refused with `GL_INVALID_ENUM`, which is what the specification says a driver with no
 * compressed formats does - the set is allowed to be empty, and a program is expected
 * to query it. That makes these entry points *conformant*, not stubs: a program that
 * asks first takes its uncompressed path, and one that does not gets an error it can
 * read instead of a call to address zero.
 *
 * `GL_TEXTURE_COMPRESSED` reports false for every texture, which is true of all of
 * them.
 *
 * The generic compressed formats are accepted as the internal format of an
 * *uncompressed* upload, which the specification allows: with no specific compressed
 * format available, each is replaced by its base format, and
 * `GL_TEXTURE_INTERNAL_FORMAT` answers that base format. */
#define GL_COMPRESSED_ALPHA 0x84E9
#define GL_COMPRESSED_LUMINANCE 0x84EA
#define GL_COMPRESSED_LUMINANCE_ALPHA 0x84EB
#define GL_COMPRESSED_INTENSITY 0x84EC
#define GL_COMPRESSED_RGB 0x84ED
#define GL_COMPRESSED_RGBA 0x84EE
#define GL_TEXTURE_COMPRESSION_HINT 0x84EF
#define GL_TEXTURE_COMPRESSED_IMAGE_SIZE 0x86A0
#define GL_TEXTURE_COMPRESSED 0x86A1
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS 0x86A3

void glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei width, GLint border, GLsizei imageSize,
                            const GLvoid *data);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLsizei imageSize, const GLvoid *data);
void glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei width, GLsizei height, GLsizei depth, GLint border,
                            GLsizei imageSize, const GLvoid *data);
void glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                               GLenum format, GLsizei imageSize, const GLvoid *data);
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLsizei width, GLsizei height, GLenum format,
                               GLsizei imageSize, const GLvoid *data);
void glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLint zoffset, GLsizei width, GLsizei height,
                               GLsizei depth, GLenum format, GLsizei imageSize,
                               const GLvoid *data);
void glGetCompressedTexImage(GLenum target, GLint level, GLvoid *img);

/* One-dimensional textures.
 *
 * **GL_TEXTURE_1D is its own binding point, not a height-1 GL_TEXTURE_2D.** Both can be
 * bound at once and each has its own enable, and when both are enabled the higher
 * dimensionality wins - so a 1D texture can be set up and left alone while 2D drawing
 * continues over it. Treating them as one binding would give a program its 2D texture
 * back where it asked for its 1D one. */
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_BINDING_1D 0x8068
#define GL_TEXTURE_BINDING_2D 0x8069

void glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                     GLenum format, GLenum type, const GLvoid *pixels);
void glCopyTexImage1D(GLenum target, GLint level, GLenum internalFormat, GLint x,
                      GLint y, GLsizei width, GLint border);
void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y,
                         GLsizei width);

/* Raster position, and the pixel operations that draw at it.
 *
 * The raster position is a point put through the whole vertex transform - modelview,
 * projection, clip, viewport - and then remembered. `glDrawPixels` and `glBitmap` draw
 * there. A position that clipped is **invalid**, and an invalid position draws nothing
 * at all rather than drawing at the edge, which is the specification's rule and the
 * reason the validity flag is queryable. */
#define GL_CURRENT_RASTER_COLOR 0x0B04
#define GL_CURRENT_RASTER_TEXTURE_COORDS 0x0B06
#define GL_CURRENT_RASTER_POSITION 0x0B07
#define GL_CURRENT_RASTER_POSITION_VALID 0x0B08
#define GL_CURRENT_RASTER_DISTANCE 0x0B09
#define GL_ZOOM_X 0x0D16
#define GL_ZOOM_Y 0x0D17
#define GL_BITMAP 0x1A00
/* glCopyPixels' buffers, and the depth and stencil pixel formats glReadPixels,
 * glDrawPixels and a depth texture's uploads take. Depth goes through GL_DEPTH_SCALE
 * and GL_DEPTH_BIAS; a stencil index through GL_INDEX_SHIFT/OFFSET and GL_MAP_STENCIL.
 * On the hardware path the depth and stencil buffers are the GPU's tiled surfaces,
 * which these read and write through their tiling - see GL_ROADMAP.md. */
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_DEPTH_COMPONENT 0x1902
/* Colour indices, which an RGBA context takes as images - glDrawPixels and the texture
 * uploads, through GL_INDEX_SHIFT/OFFSET and the GL_PIXEL_MAP_I_TO_R/G/B/A maps - and
 * never reads back. */
#define GL_COLOR_INDEX 0x1900

void glRasterPos2f(GLfloat x, GLfloat y);
void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z);
void glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glRasterPos2d(GLdouble x, GLdouble y);
void glRasterPos3d(GLdouble x, GLdouble y, GLdouble z);
void glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w);
void glRasterPos2i(GLint x, GLint y);
void glRasterPos3i(GLint x, GLint y, GLint z);
void glRasterPos4i(GLint x, GLint y, GLint z, GLint w);
void glRasterPos2s(GLshort x, GLshort y);
void glRasterPos3s(GLshort x, GLshort y, GLshort z);
void glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w);
void glRasterPos2fv(const GLfloat *v);
void glRasterPos3fv(const GLfloat *v);
void glRasterPos4fv(const GLfloat *v);
void glRasterPos2dv(const GLdouble *v);
void glRasterPos3dv(const GLdouble *v);
void glRasterPos4dv(const GLdouble *v);
void glRasterPos2iv(const GLint *v);
void glRasterPos3iv(const GLint *v);
void glRasterPos4iv(const GLint *v);
void glRasterPos2sv(const GLshort *v);
void glRasterPos3sv(const GLshort *v);
void glRasterPos4sv(const GLshort *v);

/* GL 1.4: the raster position set directly in window coordinates - no transform, no
 * clip test, z mapped through the depth range, the colour and texture coordinate the
 * current ones unlit. */
void glWindowPos2f(GLfloat x, GLfloat y);
void glWindowPos2d(GLdouble x, GLdouble y);
void glWindowPos2i(GLint x, GLint y);
void glWindowPos2s(GLshort x, GLshort y);
void glWindowPos3f(GLfloat x, GLfloat y, GLfloat z);
void glWindowPos3d(GLdouble x, GLdouble y, GLdouble z);
void glWindowPos3i(GLint x, GLint y, GLint z);
void glWindowPos3s(GLshort x, GLshort y, GLshort z);
void glWindowPos2fv(const GLfloat *v);
void glWindowPos2dv(const GLdouble *v);
void glWindowPos2iv(const GLint *v);
void glWindowPos2sv(const GLshort *v);
void glWindowPos3fv(const GLfloat *v);
void glWindowPos3dv(const GLdouble *v);
void glWindowPos3iv(const GLint *v);
void glWindowPos3sv(const GLshort *v);

void glPixelZoom(GLfloat xfactor, GLfloat yfactor);
void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type,
                  const GLvoid *pixels);
void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type);
void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig,
              GLfloat xmove, GLfloat ymove, const GLubyte *bitmap);

/* Stencil.
 *
 * `GL_REPLACE` is shared with the texture environment and `GL_INVERT` with the logic
 * ops; both are single values in GL's one enum space, so they are defined once here. */
#define GL_STENCIL_CLEAR_VALUE 0x0B91
#define GL_STENCIL_FUNC 0x0B92
#define GL_STENCIL_VALUE_MASK 0x0B93
#define GL_STENCIL_FAIL 0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL 0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS 0x0B96
#define GL_STENCIL_REF 0x0B97
#define GL_STENCIL_WRITEMASK 0x0B98
#define GL_STENCIL_BITS 0x0D57
#define GL_STENCIL_INDEX 0x1901
#define GL_KEEP 0x1E00
#define GL_INCR 0x1E02
#define GL_DECR 0x1E03
#define GL_INVERT 0x150A
/* GL 1.4: increment and decrement that wrap round the stencil buffer's range rather
 * than saturating at its ends. */
#define GL_INCR_WRAP 0x8507
#define GL_DECR_WRAP 0x8508

void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass);
void glStencilMask(GLuint mask);
void glClearStencil(GLint s);

/* User clip planes.
 *
 * Six of them, which is the minimum the specification requires and what this hardware's
 * fixed-function clipper has. The plane is given in object coordinates and stored in
 * eye coordinates, so it stays where it was put while the modelview moves afterwards.
 */
#define GL_CLIP_PLANE0 0x3000
#define GL_CLIP_PLANE1 0x3001
#define GL_CLIP_PLANE2 0x3002
#define GL_CLIP_PLANE3 0x3003
#define GL_CLIP_PLANE4 0x3004
#define GL_CLIP_PLANE5 0x3005
#define GL_MAX_CLIP_PLANES 0x0D32

void glClipPlane(GLenum plane, const GLdouble *equation);
void glGetClipPlane(GLenum plane, GLdouble *equation);

/* Texture coordinate generation.
 *
 * The coordinate is computed from the vertex instead of being taken from glTexCoord,
 * per coordinate and per mode. GL_SPHERE_MAP is the one most old code wants - it is how
 * a reflective surface is faked without a cube map. */
#define GL_S 0x2000
#define GL_T 0x2001
#define GL_R 0x2002
#define GL_Q 0x2003
#define GL_TEXTURE_GEN_S 0x0C60
#define GL_TEXTURE_GEN_T 0x0C61
#define GL_TEXTURE_GEN_R 0x0C62
#define GL_TEXTURE_GEN_Q 0x0C63
#define GL_TEXTURE_GEN_MODE 0x2500
#define GL_OBJECT_PLANE 0x2501
#define GL_EYE_PLANE 0x2502
#define GL_EYE_LINEAR 0x2400
#define GL_OBJECT_LINEAR 0x2401
#define GL_SPHERE_MAP 0x2402
#define GL_NORMAL_MAP 0x8511
#define GL_REFLECTION_MAP 0x8512

void glTexGeni(GLenum coord, GLenum pname, GLint param);
void glTexGenf(GLenum coord, GLenum pname, GLfloat param);
void glTexGend(GLenum coord, GLenum pname, GLdouble param);
void glTexGeniv(GLenum coord, GLenum pname, const GLint *params);
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params);
void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params);
void glGetTexGeniv(GLenum coord, GLenum pname, GLint *params);
void glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params);
void glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params);

/* Multitexture (GL 1.3, and ARB_multitexture before it).
 *
 * **Two texture units** since 2026-09-19, GL 1.3's minimum (section 2.6) - one before,
 * which was short of it. GL_MAX_TEXTURE_UNITS reports 2, and a unit past it is refused
 * with GL_INVALID_ENUM rather than quietly written somewhere else. The console applies
 * unit 0 alone until its pixel shader takes a second coordinate, and logs once when a
 * draw uses unit 1.
 *
 * A second unit needs a third parameter export from the vertex shader, which is a
 * hardware measurement this does not have (see the roadmap, and obSCEne
 * REQ-20260917T1652Z-7c40). When it arrives, the refusal above is the only thing that
 * has to change. */
#define GL_TEXTURE0 0x84C0
/* The rest of GL 1.3's unit names (Mesa include/GL/gl.h:1710-1740), which were left out
 * while there was one unit to name. Two units exist; a name past GL_MAX_TEXTURE_UNITS
 * is refused. */
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_TEXTURE4 0x84C4
#define GL_TEXTURE5 0x84C5
#define GL_TEXTURE6 0x84C6
#define GL_TEXTURE7 0x84C7
#define GL_TEXTURE8 0x84C8
#define GL_TEXTURE9 0x84C9
#define GL_TEXTURE10 0x84CA
#define GL_TEXTURE11 0x84CB
#define GL_TEXTURE12 0x84CC
#define GL_TEXTURE13 0x84CD
#define GL_TEXTURE14 0x84CE
#define GL_TEXTURE15 0x84CF
#define GL_TEXTURE16 0x84D0
#define GL_TEXTURE17 0x84D1
#define GL_TEXTURE18 0x84D2
#define GL_TEXTURE19 0x84D3
#define GL_TEXTURE20 0x84D4
#define GL_TEXTURE21 0x84D5
#define GL_TEXTURE22 0x84D6
#define GL_TEXTURE23 0x84D7
#define GL_TEXTURE24 0x84D8
#define GL_TEXTURE25 0x84D9
#define GL_TEXTURE26 0x84DA
#define GL_TEXTURE27 0x84DB
#define GL_TEXTURE28 0x84DC
#define GL_TEXTURE29 0x84DD
#define GL_TEXTURE30 0x84DE
#define GL_TEXTURE31 0x84DF
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_CLIENT_ACTIVE_TEXTURE 0x84E1
#define GL_MAX_TEXTURE_UNITS 0x84E2
/* GL 1.3's transposed-matrix queries, the glGet side of glLoadTransposeMatrix (Mesa
 * include/GL/gl.h, GL_VERSION_1_3). */
#define GL_TRANSPOSE_MODELVIEW_MATRIX 0x84E3
#define GL_TRANSPOSE_PROJECTION_MATRIX 0x84E4
#define GL_TRANSPOSE_TEXTURE_MATRIX 0x84E5
/* GL_ARB_multitexture's spellings of the same enums, beside its *ARB entry points
 * declared below. */
#define GL_TEXTURE0_ARB 0x84C0
#define GL_TEXTURE1_ARB 0x84C1
#define GL_TEXTURE2_ARB 0x84C2
#define GL_TEXTURE3_ARB 0x84C3
#define GL_TEXTURE4_ARB 0x84C4
#define GL_TEXTURE5_ARB 0x84C5
#define GL_TEXTURE6_ARB 0x84C6
#define GL_TEXTURE7_ARB 0x84C7
#define GL_TEXTURE8_ARB 0x84C8
#define GL_TEXTURE9_ARB 0x84C9
#define GL_TEXTURE10_ARB 0x84CA
#define GL_TEXTURE11_ARB 0x84CB
#define GL_TEXTURE12_ARB 0x84CC
#define GL_TEXTURE13_ARB 0x84CD
#define GL_TEXTURE14_ARB 0x84CE
#define GL_TEXTURE15_ARB 0x84CF
#define GL_TEXTURE16_ARB 0x84D0
#define GL_TEXTURE17_ARB 0x84D1
#define GL_TEXTURE18_ARB 0x84D2
#define GL_TEXTURE19_ARB 0x84D3
#define GL_TEXTURE20_ARB 0x84D4
#define GL_TEXTURE21_ARB 0x84D5
#define GL_TEXTURE22_ARB 0x84D6
#define GL_TEXTURE23_ARB 0x84D7
#define GL_TEXTURE24_ARB 0x84D8
#define GL_TEXTURE25_ARB 0x84D9
#define GL_TEXTURE26_ARB 0x84DA
#define GL_TEXTURE27_ARB 0x84DB
#define GL_TEXTURE28_ARB 0x84DC
#define GL_TEXTURE29_ARB 0x84DD
#define GL_TEXTURE30_ARB 0x84DE
#define GL_TEXTURE31_ARB 0x84DF
#define GL_ACTIVE_TEXTURE_ARB 0x84E0
#define GL_CLIENT_ACTIVE_TEXTURE_ARB 0x84E1
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2

void glActiveTexture(GLenum texture);
void glClientActiveTexture(GLenum texture);
void glMultiTexCoord1f(GLenum target, GLfloat s);
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);
void glMultiTexCoord3f(GLenum target, GLfloat s, GLfloat t, GLfloat r);
void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glMultiTexCoord1d(GLenum target, GLdouble s);
void glMultiTexCoord2d(GLenum target, GLdouble s, GLdouble t);
void glMultiTexCoord3d(GLenum target, GLdouble s, GLdouble t, GLdouble r);
void glMultiTexCoord4d(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q);
void glMultiTexCoord1i(GLenum target, GLint s);
void glMultiTexCoord2i(GLenum target, GLint s, GLint t);
void glMultiTexCoord3i(GLenum target, GLint s, GLint t, GLint r);
void glMultiTexCoord4i(GLenum target, GLint s, GLint t, GLint r, GLint q);
void glMultiTexCoord1s(GLenum target, GLshort s);
void glMultiTexCoord2s(GLenum target, GLshort s, GLshort t);
void glMultiTexCoord3s(GLenum target, GLshort s, GLshort t, GLshort r);
void glMultiTexCoord4s(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q);
void glMultiTexCoord1fv(GLenum target, const GLfloat *v);
void glMultiTexCoord2fv(GLenum target, const GLfloat *v);
void glMultiTexCoord3fv(GLenum target, const GLfloat *v);
void glMultiTexCoord4fv(GLenum target, const GLfloat *v);
void glMultiTexCoord1dv(GLenum target, const GLdouble *v);
void glMultiTexCoord2dv(GLenum target, const GLdouble *v);
void glMultiTexCoord3dv(GLenum target, const GLdouble *v);
void glMultiTexCoord4dv(GLenum target, const GLdouble *v);
void glMultiTexCoord1iv(GLenum target, const GLint *v);
void glMultiTexCoord2iv(GLenum target, const GLint *v);
void glMultiTexCoord3iv(GLenum target, const GLint *v);
void glMultiTexCoord4iv(GLenum target, const GLint *v);
void glMultiTexCoord1sv(GLenum target, const GLshort *v);
void glMultiTexCoord2sv(GLenum target, const GLshort *v);
void glMultiTexCoord3sv(GLenum target, const GLshort *v);
void glMultiTexCoord4sv(GLenum target, const GLshort *v);

/* The ARB spellings are the same functions. They are separate symbols rather than
 * macros because a program may take their address, and because
 * `-Wl,--unresolved-symbols=ignore-all` turns an absent symbol into a jump to zero
 * rather than a link error. */
void glActiveTextureARB(GLenum texture);
void glClientActiveTextureARB(GLenum texture);
void glMultiTexCoord1fARB(GLenum target, GLfloat s);
void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t);
void glMultiTexCoord3fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r);
void glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glMultiTexCoord1dARB(GLenum target, GLdouble s);
void glMultiTexCoord2dARB(GLenum target, GLdouble s, GLdouble t);
void glMultiTexCoord3dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r);
void glMultiTexCoord4dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r,
                          GLdouble q);
void glMultiTexCoord1iARB(GLenum target, GLint s);
void glMultiTexCoord2iARB(GLenum target, GLint s, GLint t);
void glMultiTexCoord3iARB(GLenum target, GLint s, GLint t, GLint r);
void glMultiTexCoord4iARB(GLenum target, GLint s, GLint t, GLint r, GLint q);
void glMultiTexCoord1sARB(GLenum target, GLshort s);
void glMultiTexCoord2sARB(GLenum target, GLshort s, GLshort t);
void glMultiTexCoord3sARB(GLenum target, GLshort s, GLshort t, GLshort r);
void glMultiTexCoord4sARB(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q);
void glMultiTexCoord1fvARB(GLenum target, const GLfloat *v);
void glMultiTexCoord2fvARB(GLenum target, const GLfloat *v);
void glMultiTexCoord3fvARB(GLenum target, const GLfloat *v);
void glMultiTexCoord4fvARB(GLenum target, const GLfloat *v);
void glMultiTexCoord1dvARB(GLenum target, const GLdouble *v);
void glMultiTexCoord2dvARB(GLenum target, const GLdouble *v);
void glMultiTexCoord3dvARB(GLenum target, const GLdouble *v);
void glMultiTexCoord4dvARB(GLenum target, const GLdouble *v);
void glMultiTexCoord1ivARB(GLenum target, const GLint *v);
void glMultiTexCoord2ivARB(GLenum target, const GLint *v);
void glMultiTexCoord3ivARB(GLenum target, const GLint *v);
void glMultiTexCoord4ivARB(GLenum target, const GLint *v);
void glMultiTexCoord1svARB(GLenum target, const GLshort *v);
void glMultiTexCoord2svARB(GLenum target, const GLshort *v);
void glMultiTexCoord3svARB(GLenum target, const GLshort *v);
void glMultiTexCoord4svARB(GLenum target, const GLshort *v);

/* The rest of the type-and-arity grid.
 *
 * Declared because the payload link ignores unresolved symbols: a spelling that is
 * missing here is not a compile error in the calling program, it is a call to address
 * zero at run time. */
void glVertex2s(GLshort x, GLshort y);
void glVertex3s(GLshort x, GLshort y, GLshort z);
void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w);
void glVertex4i(GLint x, GLint y, GLint z, GLint w);
void glVertex2sv(const GLshort *v);
void glVertex3sv(const GLshort *v);
void glVertex4sv(const GLshort *v);
void glVertex4iv(const GLint *v);
void glVertex4dv(const GLdouble *v);

void glColor3b(GLbyte red, GLbyte green, GLbyte blue);
void glColor3s(GLshort red, GLshort green, GLshort blue);
void glColor3i(GLint red, GLint green, GLint blue);
void glColor3us(GLushort red, GLushort green, GLushort blue);
void glColor3ui(GLuint red, GLuint green, GLuint blue);
void glColor4b(GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha);
void glColor4s(GLshort red, GLshort green, GLshort blue, GLshort alpha);
void glColor4i(GLint red, GLint green, GLint blue, GLint alpha);
void glColor4us(GLushort red, GLushort green, GLushort blue, GLushort alpha);
void glColor4ui(GLuint red, GLuint green, GLuint blue, GLuint alpha);
void glColor3bv(const GLbyte *v);
void glColor3sv(const GLshort *v);
void glColor3iv(const GLint *v);
void glColor3usv(const GLushort *v);
void glColor3uiv(const GLuint *v);
void glColor4bv(const GLbyte *v);
void glColor4sv(const GLshort *v);
void glColor4iv(const GLint *v);
void glColor4usv(const GLushort *v);
void glColor4uiv(const GLuint *v);

void glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz);
void glNormal3s(GLshort nx, GLshort ny, GLshort nz);
void glNormal3i(GLint nx, GLint ny, GLint nz);
void glNormal3bv(const GLbyte *v);
void glNormal3sv(const GLshort *v);
void glNormal3iv(const GLint *v);

void glTexCoord1d(GLdouble s);
void glTexCoord1i(GLint s);
void glTexCoord1s(GLshort s);
void glTexCoord2s(GLshort s, GLshort t);
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void glTexCoord3d(GLdouble s, GLdouble t, GLdouble r);
void glTexCoord3i(GLint s, GLint t, GLint r);
void glTexCoord3s(GLshort s, GLshort t, GLshort r);
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q);
void glTexCoord4i(GLint s, GLint t, GLint r, GLint q);
void glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q);
void glTexCoord1fv(const GLfloat *v);
void glTexCoord1dv(const GLdouble *v);
void glTexCoord1iv(const GLint *v);
void glTexCoord1sv(const GLshort *v);
void glTexCoord2sv(const GLshort *v);
void glTexCoord3fv(const GLfloat *v);
void glTexCoord3dv(const GLdouble *v);
void glTexCoord3iv(const GLint *v);
void glTexCoord3sv(const GLshort *v);
void glTexCoord4fv(const GLfloat *v);
void glTexCoord4dv(const GLdouble *v);
void glTexCoord4iv(const GLint *v);
void glTexCoord4sv(const GLshort *v);

/* Fixed-Function Lighting & Materials */
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightModelf(GLenum pname, GLfloat param);
/* The integer spellings. **A colour converts differently from a scalar**: an integer
 * colour component is mapped across the whole signed range onto [-1, 1], so GL_AMBIENT
 * with INT_MAX means 1.0, while a position or an attenuation is an ordinary cast. */
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
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLint border, GLenum format, GLenum type,
                  const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
/* Residency and the priority hint. Nothing here is ever evicted, so every texture that
 * exists is resident; a name that was never generated is an error rather than a "no".
 */
GLboolean glAreTexturesResident(GLsizei n, const GLuint *textures,
                                GLboolean *residences);
void glPrioritizeTextures(GLsizei n, const GLuint *textures,
                          const GLclampf *priorities);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);
void glTexEnviv(GLenum target, GLenum pname, const GLint *params);
void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params);
void glGetTexEnviv(GLenum target, GLenum pname, GLint *params);

/* `glHint` is advisory: every target is recorded and reported, and none changes a pixel
 * - which is all a hint is entitled to. (The fog, point and line hints were refused
 * while this drew no fog, points or lines; it draws all three now.)
 * GL_TEXTURE_COMPRESSION_HINT is declared with the compressed-texture calls below. */
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_POINT_SMOOTH_HINT 0x0C51
#define GL_LINE_SMOOTH_HINT 0x0C52
#define GL_POLYGON_SMOOTH_HINT 0x0C53
#define GL_FOG_HINT 0x0C54
#define GL_DONT_CARE 0x1100
#define GL_FASTEST 0x1101
#define GL_NICEST 0x1102
void glHint(GLenum target, GLenum mode);

/* **Two colour buffers.** GL_BACK names the one glSwapBuffers presents. GL_FRONT names
 * the picture on screen, a surface of oops-gl's own that glFlush and glFinish present
 * while it is drawn into (since 2026-09-19). GL_FRONT_AND_BACK and GL_LEFT name both.
 * GL_NONE switches colour writes off. The right and auxiliary buffers do not exist and
 * are GL_INVALID_OPERATION. A draw into both reaches both on either path (on the
 * console through a second colour target and a second export, since 2026-09-20). */
#define GL_DRAW_BUFFER 0x0C01
#define GL_READ_BUFFER 0x0C02
void glDrawBuffer(GLenum buf);
void glReadBuffer(GLenum src);
GLboolean glIsTexture(GLuint texture);

/* -------------------------------------------------------------------------
 * OpenGL 2.0: the programmable pipeline
 *
 * GL 2.0 replaces the fixed-function transform-and-light stage and the texture combiner
 * with two programs written in GLSL. Everything from here to the end of this section is
 * that - the objects a program is built from, the uniforms and generic attributes it
 * reads - plus the three parts of GL 2.0 that are not about shaders at all: separate
 * stencil state for the two faces, a blend equation per channel group, and the point
 * sprite.
 *
 * Enumerant values are Mesa's `include/GL/glext.h`, `GL_VERSION_2_0`, rather than
 * written from memory. A wrong enumerant here is not a compile error anywhere: it is a
 * call that silently means something else.
 *
 * # Shader objects and program objects share one name space
 *
 * `glCreateShader` and `glCreateProgram` hand out names from the same counter, which
 * the specification requires (GL 2.0, 2.15.1). So a shader name can never equal a
 * program name, and `glGetShaderiv` on a program name is GL_INVALID_OPERATION rather
 * than a read of the wrong object. Two independent counters would serve almost every
 * program correctly and then fail the one that deletes a shader and creates a program
 * expecting a distinct name.
 *
 * # Deletion is deferred, not immediate
 *
 * `glDeleteShader` on a shader still attached to a program, and `glDeleteProgram` on
 * the program in use, both only **flag** the object: it keeps working and disappears
 * when the last reference goes. `glIsShader` answers false from the moment it is
 * flagged, while `glGetShaderiv` with GL_DELETE_STATUS still answers on it - which is
 * the pair of behaviours that makes the deferral observable, and the reason both are
 * implemented rather than one.
 *
 * # GLSL 1.10 is the language
 *
 * That is what GL 2.0 defines, and it is the whole of what the front end accepts.
 * `#version 120` and later are refused by name rather than compiled as 1.10, because
 * a 1.20 shader whose `varying` array or implicit int-to-float conversion quietly did
 * something else is a wrong picture with no diagnostic attached to it.
 * ------------------------------------------------------------------------- */

/* The character type the shader-object calls take. `char`, as the specification says -
 * not `GLbyte`, which is signed char and a different type to a C compiler even where it
 * is the same width. Declaring it wrongly conflicts the moment this header meets a real
 * `GL/glext.h`. */
typedef char GLchar;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_SHADER_TYPE 0x8B4F
#define GL_DELETE_STATUS 0x8B80
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VALIDATE_STATUS 0x8B83
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ATTACHED_SHADERS 0x8B85
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define GL_CURRENT_PROGRAM 0x8B8D

/* **The shading language's own version string**, which `glGetString` answers separately
 * from GL_VERSION. A program that reads GL_VERSION and finds 2.0 still asks this before
 * deciding which dialect to hand over. */
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

/* The types `glGetActiveUniform` and `glGetActiveAttrib` report. GL_BOOL is a type
 * enumerant here and not a boolean value; the two never meet, since nothing takes both.
 */
#define GL_FLOAT_VEC2 0x8B50
#define GL_FLOAT_VEC3 0x8B51
#define GL_FLOAT_VEC4 0x8B52
#define GL_INT_VEC2 0x8B53
#define GL_INT_VEC3 0x8B54
#define GL_INT_VEC4 0x8B55
#define GL_BOOL 0x8B56
#define GL_BOOL_VEC2 0x8B57
#define GL_BOOL_VEC3 0x8B58
#define GL_BOOL_VEC4 0x8B59
#define GL_FLOAT_MAT2 0x8B5A
#define GL_FLOAT_MAT3 0x8B5B
#define GL_FLOAT_MAT4 0x8B5C
#define GL_SAMPLER_1D 0x8B5D
#define GL_SAMPLER_2D 0x8B5E
#define GL_SAMPLER_3D 0x8B5F
#define GL_SAMPLER_CUBE 0x8B60
#define GL_SAMPLER_1D_SHADOW 0x8B61
#define GL_SAMPLER_2D_SHADOW 0x8B62
/* The non-square matrices, which GLSL 1.20 added and GL 2.1 named. A `#version 120`
 * shader may declare one, so the uniform machinery has to be able to report and set it
 * - the alternative is a uniform that links with a type of zero and cannot be written.
 */
#define GL_FLOAT_MAT2x3 0x8B65
#define GL_FLOAT_MAT2x4 0x8B66
#define GL_FLOAT_MAT3x2 0x8B67
#define GL_FLOAT_MAT3x4 0x8B68
#define GL_FLOAT_MAT4x2 0x8B69
#define GL_FLOAT_MAT4x3 0x8B6A

/* The programmable pipeline's limits, every one of them a glGet. Their values are this
 * implementation's and are stated where they are answered, in gl_state.c. */
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS 0x8B4A
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS 0x8B49
#define GL_MAX_VARYING_FLOATS 0x8B4B
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_TEXTURE_COORDS 0x8871

/* Shader objects: created, given source, compiled, and queried for whether that worked.
 */
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length);
void glCompileShader(GLuint shader);
void glDeleteShader(GLuint shader);
GLboolean glIsShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                        GLchar *infoLog);
void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);

/* Program objects: shaders attached, linked, and made current. */
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glDetachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glUseProgram(GLuint program);
void glValidateProgram(GLuint program);
void glDeleteProgram(GLuint program);
GLboolean glIsProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length,
                         GLchar *infoLog);
void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count,
                          GLuint *shaders);

/* Uniforms.
 *
 * **A location is a property of the linked program, not of the name**, so it is fetched
 * after `glLinkProgram` and again after every relink. A location of -1 means the name
 * is not an active uniform - including a uniform the linker removed because nothing
 * read it - and `glUniform` on -1 is defined to do nothing rather than to fail. A
 * program that treats -1 as an error would refuse to run against an implementation that
 * optimised better than it expected.
 *
 * Every `glUniform` acts on the program in use, which is why none of them names one. */
GLint glGetUniformLocation(GLuint program, const GLchar *name);
void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                        GLint *size, GLenum *type, GLchar *name);
void glGetUniformfv(GLuint program, GLint location, GLfloat *params);
void glGetUniformiv(GLuint program, GLint location, GLint *params);
void glUniform1f(GLint location, GLfloat v0);
void glUniform2f(GLint location, GLfloat v0, GLfloat v1);
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
void glUniform1i(GLint location, GLint v0);
void glUniform2i(GLint location, GLint v0, GLint v1);
void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2);
void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
void glUniform1fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform3fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
void glUniform1iv(GLint location, GLsizei count, const GLint *value);
void glUniform2iv(GLint location, GLsizei count, const GLint *value);
void glUniform3iv(GLint location, GLsizei count, const GLint *value);
void glUniform4iv(GLint location, GLsizei count, const GLint *value);
/* **`transpose` is GL_FALSE for a column-major matrix**, which is how GL has always
 * stored one and how `glLoadMatrixf` takes it. A caller passing a row-major matrix and
 * GL_FALSE gets the transpose of what it meant, which still draws - just wrongly, and a
 * screenshot of a symmetric scene will not show it. */
void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value);
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value);
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value);
/* **`CxR` is columns then rows, so `2x3` sends six floats as two columns of three.**
 * The name reads the other way round from the dimensions most people say aloud, and
 * taking it as rows first transposes every uniform a port sets. */
void glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);

/* Generic vertex attributes.
 *
 * The programmable pipeline's replacement for glVertexPointer and its relatives:
 * numbered slots rather than named ones, bound to a shader's `attribute` variables by
 * the linker or by `glBindAttribLocation` before it.
 *
 * `glBindAttribLocation` takes effect at the **next** link, not immediately - so a
 * program that binds after linking and then draws is using the locations the previous
 * link chose. The specification says so (GL 2.0, 2.15.3) and it is the mistake this API
 * most invites. */
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_VERTEX_ATTRIB_ARRAY_SIZE 0x8623
#define GL_VERTEX_ATTRIB_ARRAY_STRIDE 0x8624
#define GL_VERTEX_ATTRIB_ARRAY_TYPE 0x8625
#define GL_CURRENT_VERTEX_ATTRIB 0x8626
#define GL_VERTEX_ATTRIB_ARRAY_POINTER 0x8645
#define GL_VERTEX_ATTRIB_ARRAY_NORMALIZED 0x886A
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const GLvoid *pointer);
void glEnableVertexAttribArray(GLuint index);
void glDisableVertexAttribArray(GLuint index);
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
GLint glGetAttribLocation(GLuint program, const GLchar *name);
void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                       GLint *size, GLenum *type, GLchar *name);
void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params);
void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params);
void glGetVertexAttribdv(GLuint index, GLenum pname, GLdouble *params);
void glGetVertexAttribPointerv(GLuint index, GLenum pname, GLvoid **pointer);

/* The current value of an attribute whose array is disabled - the generic pipeline's
 * glColor. Every form reaches `glVertexAttrib4f`; the `N` forms normalise an integer to
 * [0, 1] or
 * [-1, 1] first, and the plain integer forms convert without scaling, which is the
 * whole difference between `glVertexAttrib4Nubv` and `glVertexAttrib4ubv`. */
void glVertexAttrib1f(GLuint index, GLfloat x);
void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertexAttrib1fv(GLuint index, const GLfloat *v);
void glVertexAttrib2fv(GLuint index, const GLfloat *v);
void glVertexAttrib3fv(GLuint index, const GLfloat *v);
void glVertexAttrib4fv(GLuint index, const GLfloat *v);
void glVertexAttrib1d(GLuint index, GLdouble x);
void glVertexAttrib2d(GLuint index, GLdouble x, GLdouble y);
void glVertexAttrib3d(GLuint index, GLdouble x, GLdouble y, GLdouble z);
void glVertexAttrib4d(GLuint index, GLdouble x, GLdouble y, GLdouble z, GLdouble w);
void glVertexAttrib1dv(GLuint index, const GLdouble *v);
void glVertexAttrib2dv(GLuint index, const GLdouble *v);
void glVertexAttrib3dv(GLuint index, const GLdouble *v);
void glVertexAttrib4dv(GLuint index, const GLdouble *v);
void glVertexAttrib1s(GLuint index, GLshort x);
void glVertexAttrib2s(GLuint index, GLshort x, GLshort y);
void glVertexAttrib3s(GLuint index, GLshort x, GLshort y, GLshort z);
void glVertexAttrib4s(GLuint index, GLshort x, GLshort y, GLshort z, GLshort w);
void glVertexAttrib1sv(GLuint index, const GLshort *v);
void glVertexAttrib2sv(GLuint index, const GLshort *v);
void glVertexAttrib3sv(GLuint index, const GLshort *v);
void glVertexAttrib4sv(GLuint index, const GLshort *v);
void glVertexAttrib4bv(GLuint index, const GLbyte *v);
void glVertexAttrib4iv(GLuint index, const GLint *v);
void glVertexAttrib4ubv(GLuint index, const GLubyte *v);
void glVertexAttrib4uiv(GLuint index, const GLuint *v);
void glVertexAttrib4usv(GLuint index, const GLushort *v);
void glVertexAttrib4Nub(GLuint index, GLubyte x, GLubyte y, GLubyte z, GLubyte w);
void glVertexAttrib4Nbv(GLuint index, const GLbyte *v);
void glVertexAttrib4Nsv(GLuint index, const GLshort *v);
void glVertexAttrib4Niv(GLuint index, const GLint *v);
void glVertexAttrib4Nubv(GLuint index, const GLubyte *v);
void glVertexAttrib4Nusv(GLuint index, const GLushort *v);
void glVertexAttrib4Nuiv(GLuint index, const GLuint *v);

/* -------------------------------------------------------------------------
 * The rest of GL 2.0, which is not about shaders
 * ------------------------------------------------------------------------- */

/* **Separate stencil state for the two faces.** GL 1.x has one stencil function and one
 * set of operations whichever way a polygon faces; 2.0 splits them, which is what a
 * single-pass stencil shadow volume needs. `glStencilFunc` and `glStencilOp` keep
 * working and set both faces, which is how the specification defines them from 2.0
 * onwards. */
#define GL_STENCIL_BACK_FUNC 0x8800
#define GL_STENCIL_BACK_FAIL 0x8801
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL 0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS 0x8803
#define GL_STENCIL_BACK_REF 0x8CA3
#define GL_STENCIL_BACK_VALUE_MASK 0x8CA4
#define GL_STENCIL_BACK_WRITEMASK 0x8CA5
void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass);
void glStencilMaskSeparate(GLenum face, GLuint mask);

/* A blend equation per channel group - GL_FUNC_ADD for the colour and GL_FUNC_SUBTRACT
 * for the alpha, say. `glBlendEquation` sets both. */
#define GL_BLEND_EQUATION_RGB 0x8009 /* the same value as GL_BLEND_EQUATION */
#define GL_BLEND_EQUATION_ALPHA 0x883D
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);

/* `glDrawBuffers`: several colour buffers named at once, each receiving the **same**
 * fragment colour, which is what the call means for a window-system framebuffer. True
 * multiple render targets - a different colour per buffer - belong to framebuffer
 * objects and their GL_COLOR_ATTACHMENT names, which are GL 3.0 and are not here. */
#define GL_MAX_DRAW_BUFFERS 0x8824
#define GL_DRAW_BUFFER0 0x8825
#define GL_DRAW_BUFFER1 0x8826
void glDrawBuffers(GLsizei n, const GLenum *bufs);

/* The point sprite: a point rasterised with texture coordinates generated across it
 * rather than interpolated from its one vertex, and `gl_PointSize` written by the
 * vertex shader. GL_POINT_SPRITE_COORD_ORIGIN says which corner s, t start from -
 * GL_UPPER_LEFT by default, which is the opposite of everything else in GL and is the
 * specification's own choice. */
#define GL_POINT_SPRITE 0x8861
#define GL_COORD_REPLACE 0x8862
#define GL_POINT_SPRITE_COORD_ORIGIN 0x8CA0
#define GL_LOWER_LEFT 0x8CA1
#define GL_UPPER_LEFT 0x8CA2
#define GL_VERTEX_PROGRAM_POINT_SIZE 0x8642
#define GL_VERTEX_PROGRAM_TWO_SIDE 0x8643

/* GL 2.0's hint for the precision of `dFdx`, `dFdy` and `fwidth`. Advisory, like every
 * other hint here: recorded, reported, and changing no pixel. */
#define GL_FRAGMENT_SHADER_DERIVATIVE_HINT 0x8B8B

/* Pipeline Synchronization & Presentation */
void glFlush(void);
void glFinish(void);
void glGetCanary(GLuint *vs_canary, GLuint *ps_canary);
void glGetCanaryEx(GLuint *vs_canary, GLuint *ps_canary, GLuint *vs_s0, GLuint *ps_s0);
/* oops-gl extension: the hardware proof. GL_TRUE only when the GPU-only clear at
 * context creation survived readback and the last submission's end-of-pipe fence and
 * GPU clock both came back. Nothing here reports which code path was chosen. */
GLboolean glIsHardwareAccelerated(void);

typedef struct gl_hw_status {
    GLboolean verified; /* glIsHardwareAccelerated() */
    GLboolean failed; /* a submit or fence failure stopped drawing; nothing is drawn by
                         the CPU */
    const char *failure; /* the reason when failed, else NULL */
    GLuint fence; /* the end-of-pipe fence word read back: 0xbeefcafe when it fired */
    GLuint timestamp_lo; /* the 64-bit GPU clock counter written at end of pipe */
    GLuint timestamp_hi;
    GLuint frames_confirmed; /* submissions whose fence and clock both arrived */
    GLuint clear_colour;     /* the GPU-only clear test: colour, pixels read back in it,
                                pixels in the target */
    GLuint clear_matched;
    GLuint clear_expected;
} gl_hw_status_t;
void glGetHardwareStatus(gl_hw_status_t *out);

/*
 * **What the console back end made of a program's fragment stage** - oops-gl's own
 * queries, not OpenGL's, passed to `glGetProgramiv`.
 *
 * A program that links is not necessarily one that draws here. `glsl_ps.c` compiles the
 * fragment shader to gfx1030 instructions or **refuses it by name**, and a refused one
 * fails the *draw* with GL_INVALID_OPERATION rather than quietly running the
 * fixed-function pixel shader in its place. Nothing in OpenGL asks about that, because
 * on a desktop it cannot happen.
 *
 * `GL_PROGRAM_HW_PS_WORDS` is zero for a program with no console code - either refused,
 * or a program with no fragment stage at all, for which the fixed-function shader runs
 * and nothing was refused. `glGetProgramHardwareLog` tells those two apart: it names
 * the reason for the first and is empty for the second.
 */
#define GL_PROGRAM_HW_PS_WORDS 0x9E00
#define GL_PROGRAM_HW_PS_VGPRS 0x9E01
#define GL_PROGRAM_HW_PARAMS 0x9E02
void glGetProgramHardwareLog(GLuint program, GLsizei bufSize, GLsizei *length,
                             GLchar *infoLog);

/* The next submission logs its command stream, shader words, descriptors, fence and GPU
 * clock to klog as the oracle record. */
void glRequestHardwareDump(void);

/* Words the next and every later frame's command stream opens with, before oops-gl's
 * own state: an experiment hook, so a caller can put another driver's preamble in front
 * of this one and measure what the hardware and the compositor make of it (oops-mesa
 * roadmap unit 2). The words are used in place, not copied, and must stay valid until
 * replaced; NULL or zero removes them. No validation: the caller owns what the command
 * processor is handed. */
void glSetHardwarePrelude(const GLuint *words, GLuint count);

/* The command processor's copy of the last submitted frame's render target, taken after
 * the end-of-pipe cache flush and before the timestamp, in CPU-cached memory: hash
 * this, not the uncached target. NULL on the host and before the first submission.
 *
 * The buffer is CPU-cached, so its lines have to be invalidated before the copy the GPU
 * just wrote is visible - and on a 1920x1080 target that is 129,600 `clflush`, which is
 * not free and is wasted on a caller that reads one word in sixty-four. `line_stride`
 * says how many cache lines to skip between invalidations: 1 for every line, 4 for a
 * caller stepping 64 words, and so on. **A caller that reads a line this did not
 * invalidate gets whatever the CPU had cached**, which for a still frame is the
 * previous frame's pixels. `glGetFrameReadback()` is the every-line form and is what a
 * caller that reads the whole frame wants. */
const GLuint *glGetFrameReadback(void);
const GLuint *glGetFrameReadbackSampled(GLuint line_stride);

/* OOPS-GL Context Lifecycle (EGL/GLX equivalent) */
struct oops_display;
void *glContextCreate(struct oops_display *disp);
void glContextDestroy(void *ctx);
void glContextMakeCurrent(void *ctx);
void *glGetCurrentContext(void);
void glSwapBuffers(void);

/*
 * **What version this context is** (2026-09-20; it began gating the API on 2026-09-21).
 *
 * A context has **the entry points its version defines and no others**.
 * `glContextSetVersion(1, 1)` gives a GL 1.1 context, and `glBindBuffer` on it is
 * GL_INVALID_OPERATION and does nothing - because buffer objects are GL 1.5's and a
 * GL 1.1 context does not have them. `glContextSetVersion(2, 0)` gives the programmable
 * pipeline.
 *
 * On a desktop driver that discipline comes from the linker: an entry point a context
 * does not have is not exported, and a program calling it fails to load. Everything
 * here is compiled into one archive, so the equivalent is a runtime check - and the
 * answer is the specification's for a call that is not in the context:
 * GL_INVALID_OPERATION, and the call does nothing. A function returning a value returns
 * its failure value: 0 for a name, -1 for a location, GL_FALSE for a predicate. An
 * enumerant a later version added is GL_INVALID_ENUM, which is the different thing it
 * is: "I have never heard of this" rather than "not from here".
 *
 * **Why this is worth the friction.** It used to change the reported string and nothing
 * else, so a GL 1.1 program could call `glCreateShader` and a GL 2.0 defect could reach
 * a program that had never asked for the programmable pipeline. It also means a port
 * developed here meets the same refusals it will meet on a driver that really is the
 * version it claims, instead of finding out later.
 *
 * **The default is 1.5** - the highest version that is complete here, so a program that
 * states nothing gets a complete fixed-function context and no GL 1.x port needs to say
 * anything.
 * **2.0 is never the default**: its pipeline is a different thing rather than more of
 * the same one, and the opt-in is what keeps a GL 1.x program out of it.
 *
 * `major`.`minor` must be one this library implements - 1.0 through 1.5, 2.0 or 2.1.
 * Anything else is GL_INVALID_VALUE and the version is left as it was, rather than
 * half-set. A line goes to the log each time, naming what was asked for.
 *
 * **The extension spellings are not gated.** `glActiveTextureARB` is
 * `GL_ARB_multitexture`'s entry point, and an extension is a separate promise from the
 * core version - which is the whole reason those names exist.
 * `glGetString(GL_EXTENSIONS)` is what promises them.
 *
 * The default is the build's: `OOPS_GL_DEFAULT_VERSION_MAJOR` and `_MINOR`.
 */
GLboolean glContextSetVersion(GLuint major, GLuint minor);
void glContextGetVersion(GLuint *major, GLuint *minor);

/*
 * **The extension spellings of entry points this library already has** (2026-09-19).
 *
 * A program written against the OpenGL of that era - which is what a homebrew port
 * usually is - reads `glGetString(GL_EXTENSIONS)`, finds an extension, and then calls
 * *that extension's* names: `glGenBuffersARB`, `glSecondaryColor3fEXT`,
 * `glWindowPos2iARB`. The core spellings it would otherwise need arrived in later
 * versions it does not assume.
 *
 * `glGetString` here advertises an extension only where the extension's own entry
 * points exist (gl_state.c). These are those entry points: each is the core function
 * under its published name, so the promise the string makes is one this library keeps.
 */
typedef GLintptr GLintptrARB;
typedef GLsizeiptr GLsizeiptrARB;

/* GL_ARB_vertex_buffer_object */
void glBindBufferARB(GLenum target, GLuint buffer);
void glDeleteBuffersARB(GLsizei n, const GLuint *buffers);
void glGenBuffersARB(GLsizei n, GLuint *buffers);
GLboolean glIsBufferARB(GLuint buffer);
void glBufferDataARB(GLenum target, GLsizeiptrARB size, const GLvoid *data,
                     GLenum usage);
void glBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size,
                        const GLvoid *data);
void glGetBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size,
                           GLvoid *data);
void *glMapBufferARB(GLenum target, GLenum access);
GLboolean glUnmapBufferARB(GLenum target);
void glGetBufferParameterivARB(GLenum target, GLenum pname, GLint *params);
void glGetBufferPointervARB(GLenum target, GLenum pname, GLvoid **params);

/* GL_EXT_secondary_color */
void glSecondaryColor3bEXT(GLbyte r, GLbyte g, GLbyte b);
void glSecondaryColor3bvEXT(const GLbyte *v);
void glSecondaryColor3dEXT(GLdouble r, GLdouble g, GLdouble b);
void glSecondaryColor3dvEXT(const GLdouble *v);
void glSecondaryColor3fEXT(GLfloat r, GLfloat g, GLfloat b);
void glSecondaryColor3fvEXT(const GLfloat *v);
void glSecondaryColor3iEXT(GLint r, GLint g, GLint b);
void glSecondaryColor3ivEXT(const GLint *v);
void glSecondaryColor3sEXT(GLshort r, GLshort g, GLshort b);
void glSecondaryColor3svEXT(const GLshort *v);
void glSecondaryColor3ubEXT(GLubyte r, GLubyte g, GLubyte b);
void glSecondaryColor3ubvEXT(const GLubyte *v);
void glSecondaryColor3uiEXT(GLuint r, GLuint g, GLuint b);
void glSecondaryColor3uivEXT(const GLuint *v);
void glSecondaryColor3usEXT(GLushort r, GLushort g, GLushort b);
void glSecondaryColor3usvEXT(const GLushort *v);
void glSecondaryColorPointerEXT(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *pointer);

/* GL_EXT_fog_coord */
void glFogCoordfEXT(GLfloat coord);
void glFogCoordfvEXT(const GLfloat *coord);
void glFogCoorddEXT(GLdouble coord);
void glFogCoorddvEXT(const GLdouble *coord);
void glFogCoordPointerEXT(GLenum type, GLsizei stride, const GLvoid *pointer);

/* GL_EXT_draw_range_elements, GL_EXT_multi_draw_arrays */
void glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count,
                            GLenum type, const GLvoid *indices);
void glMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count,
                          GLsizei drawcount);
void glMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type,
                            const GLvoid *const *indices, GLsizei drawcount);

/* GL_ARB_window_pos */
void glWindowPos2dARB(GLdouble x, GLdouble y);
void glWindowPos2dvARB(const GLdouble *p);
void glWindowPos2fARB(GLfloat x, GLfloat y);
void glWindowPos2fvARB(const GLfloat *p);
void glWindowPos2iARB(GLint x, GLint y);
void glWindowPos2ivARB(const GLint *p);
void glWindowPos2sARB(GLshort x, GLshort y);
void glWindowPos2svARB(const GLshort *p);
void glWindowPos3dARB(GLdouble x, GLdouble y, GLdouble z);
void glWindowPos3dvARB(const GLdouble *p);
void glWindowPos3fARB(GLfloat x, GLfloat y, GLfloat z);
void glWindowPos3fvARB(const GLfloat *p);
void glWindowPos3iARB(GLint x, GLint y, GLint z);
void glWindowPos3ivARB(const GLint *p);
void glWindowPos3sARB(GLshort x, GLshort y, GLshort z);
void glWindowPos3svARB(const GLshort *p);

/* GL_EXT_blend_color, GL_EXT_blend_minmax */
void glBlendColorEXT(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glBlendEquationEXT(GLenum mode);

/* GL_ARB_transpose_matrix */
void glLoadTransposeMatrixfARB(const GLfloat *m);
void glLoadTransposeMatrixdARB(const GLdouble *m);
void glMultTransposeMatrixfARB(const GLfloat *m);
void glMultTransposeMatrixdARB(const GLdouble *m);

/* GL_ARB_point_parameters, GL_EXT_point_parameters */
void glPointParameterfARB(GLenum pname, GLfloat param);
void glPointParameterfvARB(GLenum pname, const GLfloat *params);
void glPointParameterfEXT(GLenum pname, GLfloat param);
void glPointParameterfvEXT(GLenum pname, const GLfloat *params);

/* GL_EXT_texture3D (2026-09-20). Two entry points and the enums under their extension
 * spellings - `glCopyTexSubImage3D` belongs to GL 1.2 and to GL_EXT_copy_texture, not
 * here. */
/* The literals, not the core names - see GL_DEPTH_COMPONENT16_ARB above for why. */
#define GL_PACK_SKIP_IMAGES_EXT 0x806B
#define GL_PACK_IMAGE_HEIGHT_EXT 0x806C
#define GL_UNPACK_SKIP_IMAGES_EXT 0x806D
#define GL_UNPACK_IMAGE_HEIGHT_EXT 0x806E
#define GL_TEXTURE_3D_EXT 0x806F
#define GL_PROXY_TEXTURE_3D_EXT 0x8070
#define GL_TEXTURE_DEPTH_EXT 0x8071
#define GL_TEXTURE_WRAP_R_EXT 0x8072
#define GL_MAX_3D_TEXTURE_SIZE_EXT 0x8073
#define GL_TEXTURE_BINDING_3D_EXT 0x806A
/* **`internalformat` is a `GLenum` here and a `GLint` in the core call.** The EXT
 * extension predates GL 1.2 and declares it that way, so a program written against the
 * extension passes one - and declaring it otherwise is a conflicting declaration the
 * moment this header meets a real `GL/glext.h`, which is how it was found. */
void glTexImage3DEXT(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                     GLsizei height, GLsizei depth, GLint border, GLenum format,
                     GLenum type, const GLvoid *pixels);
void glTexSubImage3DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                        GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                        GLenum format, GLenum type, const GLvoid *pixels);

/*
 * **Framebuffer objects: rendering into a texture or a renderbuffer instead of the
 * window.**
 *
 * Core in OpenGL ES 2.0 and in desktop GL 3.0, and an extension
 * (GL_EXT_framebuffer_object) before that. The names here are the **core, unsuffixed**
 * ones, because that is what asks for them: the Khronos CTS builds its function table
 * from `glwInitES20.inl`, which spells every one of these without a suffix, and a
 * loader that cannot find a name reports the whole test `NotSupported` rather than
 * failing it.
 *
 * Measured on 2026-09-25: oops-gl answered 122 of the 142 entry points that table asks
 * for, and
 * **14 of the 20 it did not were this one feature** - which is also the whole of
 * SuperTux's `KNOWN_GAPS`. Two unrelated consumers wanting the same thing is why it is
 * here.
 *
 * Enums are their literal values for the reason the `GL_DEPTH_COMPONENT16_ARB` block
 * above gives: a hosted title sees this header and Mesa's `GL/glext.h` together, and a
 * macro redefined with a different token sequence is a diagnostic even when the value
 * agrees.
 */
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
/* **The read/draw split** (GL 3.0, and EXT_framebuffer_blit before it).
 * `GL_FRAMEBUFFER` binds both, which is why every program written against the older
 * single-binding model keeps working. They exist here for `glBlitFramebuffer`, which is
 * the only operation that reads one framebuffer while writing another and so is the
 * only one that needs to tell them apart. */
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
/* How many samples a multisampled renderbuffer may ask for. Answered honestly - see
 * `glRenderbufferStorageMultisample`, which refuses more rather than quietly giving
 * one. */
#define GL_MAX_SAMPLES 0x8D57
#define GL_RENDERBUFFER_SAMPLES 0x8CAB
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#define GL_RENDERBUFFER_RED_SIZE 0x8D50
#define GL_RENDERBUFFER_GREEN_SIZE 0x8D51
#define GL_RENDERBUFFER_BLUE_SIZE 0x8D52
#define GL_RENDERBUFFER_ALPHA_SIZE 0x8D53
#define GL_RENDERBUFFER_DEPTH_SIZE 0x8D54
#define GL_RENDERBUFFER_STENCIL_SIZE 0x8D55
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE 0x8CD3
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS 0x8CD9
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506
#define GL_RGB565 0x8D62
#define GL_STENCIL_INDEX8 0x8D48

void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
GLboolean glIsFramebuffer(GLuint framebuffer);
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
GLboolean glIsRenderbuffer(GLuint renderbuffer);
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width,
                           GLsizei height);
/* Refuses `samples` above `GL_MAX_SAMPLES`, which is 1 here, rather than quietly giving
 * one sample for four. A caller that wants multisampling asks and falls back. */
void glRenderbufferStorageMultisample(GLenum target, GLsizei samples,
                                      GLenum internalformat, GLsizei width,
                                      GLsizei height);
/* Colour only: the depth and stencil bits are accepted and reported, not copied. See
 * the definition for why that is stated rather than hidden. */
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                       GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                       GLenum filter);
void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer);
void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                           GLenum pname, GLint *params);
GLenum glCheckFramebufferStatus(GLenum target);
void glGenerateMipmap(GLenum target);

/*
 * **The ES spellings of four calls this GL already had, and two that only ES has.**
 *
 * `glClearDepthf` and `glDepthRangef` take floats where the desktop calls take doubles;
 * ES has no double, and `glwInitES20.inl` asks for these names. They are the same
 * state.
 *
 * `glGetShaderPrecisionFormat`, `glReleaseShaderCompiler` and `glShaderBinary` exist
 * because ES allows an implementation with no online compiler. This one has a compiler
 * and no binary shader format, which the specification provides for:
 * `GL_NUM_SHADER_BINARY_FORMATS` is zero, so `glShaderBinary` is required to report
 * `GL_INVALID_ENUM` for any format offered, and `glReleaseShaderCompiler` is a hint
 * that may be ignored. Those are the correct answers here, not stubs standing in for
 * something missing.
 */
#define GL_LOW_FLOAT 0x8DF0
#define GL_MEDIUM_FLOAT 0x8DF1
#define GL_HIGH_FLOAT 0x8DF2
#define GL_LOW_INT 0x8DF3
#define GL_MEDIUM_INT 0x8DF4
#define GL_HIGH_INT 0x8DF5
#define GL_SHADER_COMPILER 0x8DFA
#define GL_SHADER_BINARY_FORMATS 0x8DF8
#define GL_NUM_SHADER_BINARY_FORMATS 0x8DF9

void glClearDepthf(GLclampf depth);
void glDepthRangef(GLclampf zNear, GLclampf zFar);
void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype, GLint *range,
                                GLint *precision);
void glReleaseShaderCompiler(void);
void glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryformat,
                    const void *binary, GLsizei length);

/* The address of a GL entry point by name, which is what `glXGetProcAddress` is on a
 * desktop and what `SDL_GL_GetProcAddress` calls through to here. A title holding
 * post-1.1 GL in function pointers fills them this way and never names the symbols, so
 * being linked in is not enough. The list of what can be asked for is
 * `src/gl/gl_procs.h`, which says why it is a list. NULL for a name this GL does not
 * have, which is what a program probing for an extension expects. */
void *oops_gl_get_proc_address(const char *name);

/* **Recording a frame's GL calls, to replay them somewhere else.**
 *
 * A display list records a call to replay it later in the same process; a capture
 * records it to replay it on a different implementation. The one that matters is the
 * host software rasteriser, which is this library's reference: a capture taken on the
 * console and replayed there renders the same program twice and the difference is the
 * bug.
 *
 * This exists because a conformance suite covers what somebody thought to write down,
 * and a program renders wrong in the combination nobody wrote down. Ninety-three checks
 * passing while a port's colours are wrong is not a failure of the method, it is the
 * method reaching its edge. A capture goes past it by testing the calls actually made.
 *
 *   oops_gl_capture_begin();
 *   ... one frame ...
 *   oops_gl_capture_end();
 *   size_t n; unsigned calls;
 *   const void *p = oops_gl_capture_data(&n, &calls);   // NULL if it overflowed
 *
 * The buffer belongs to the library and lives until the next `oops_gl_capture_begin`.
 * Writing it to a file is the caller's job, so this layer needs no filesystem.
 *
 * Replay executes the stream against the current context through the same executor
 * display lists use, so a capture cannot drift from a list. It returns the number of
 * commands run, which is the count in the header unless the stream was truncated. */
/*
 * **How much oops-gl writes to the kernel log**, on the SDK's own scale.
 *
 * The level is an `oops_log_level_t` from `oops/system.h` - `OOPS_LOG_NONE`, `_ERROR`,
 * `_WARN`,
 * `_INFO`, `_DEBUG`, `_TRACE` - the same vocabulary every other part of this SDK uses,
 * and it is taken as an `int` here only so that a GL header need not include a system
 * one. A second set of names for the same idea was written and withdrawn on 2026-09-24;
 * there is one scale.
 *
 * `OOPS_LOG_INFO` is the default: bring-up, the hardware self-test, GL errors with the
 * call that raised them - the lines worth reading when something is wrong.
 *
 * `OOPS_LOG_DEBUG` adds the per-frame and per-submit counters: draw calls, flushes, the
 * timings, the fence and timestamp, the canaries. They are how the blending fault and
 * its sixteen-byte transaction were measured, so they stay - but a title submitting
 * thirty times a frame writes thousands of lines a second through them and buries
 * everything the title and the rest of the SDK have to say. A diagnostic asks for them;
 * `gl1-probe` does. A title being played should not.
 */
void oops_gl_set_log_level(int level);
int oops_gl_get_log_level(void);

/* **Draw into a linear colour target instead of the scanout buffers in place.**
 *
 * The scanout path draws a frame where it will be shown, in the display's 64KB_R_X
 * swizzle; the linear path draws a plain buffer and has it tiled into place at the
 * swap. The first is faster and is the default. Call this before creating a context to
 * take the second; afterwards the target is already chosen and this does nothing.
 */
void oops_gl_set_linear_target(GLboolean on);

void oops_gl_capture_begin(void);
void oops_gl_capture_end(void);
const void *oops_gl_capture_data(size_t *out_bytes, unsigned *out_calls);
unsigned oops_gl_capture_replay(const void *data, size_t bytes);

/* **Capture a frame without touching the program.** Arms the buffer swap: the frame
 * drawn after `frame` completes is recorded and written to `path`. A port needs no
 * change of its own, which also means what lands in the file is the program's real
 * behaviour and not the behaviour of a program with capture code in it. `path` is not
 * copied, so it must outlive the frame - a string literal is the intended thing. */
void oops_gl_capture_frame(unsigned frame, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* __GL_H__ */
