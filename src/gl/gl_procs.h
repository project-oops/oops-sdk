/*
 * The entry points a title resolves by name, in one list.
 *
 * `oops_gl_get_proc_address` answers `SDL_GL_GetProcAddress`, which is
 * `glXGetProcAddress` on a desktop: a program written against post-1.1 GL holds
 * function pointers and fills them from strings, so the name has to be looked up.
 *
 * A list, not the symbol table: `make title` converts the payload to the console's
 * module format, whose dynamic symbol table sits in a `PT_SCE_DYNLIBDATA` segment
 * (0x61000000) with `p_memsz` zero. It is never mapped, so there is no symbol table to
 * search at run time. The `.elf` from `make elf` has an ordinary `.dynsym`, but the
 * console never sees it.
 *
 * This file holds the extension entry points under every spelling oops-gl publishes,
 * derived from `GL/gl.h`: every ARB/EXT-suffixed entry point and the core spelling of
 * each, matching what `glGetString(GL_EXTENSIONS)` advertises. `glGenBuffersARB` and
 * `glGenBuffers` are separate definitions in `gl_state.c`, so both are listed.
 *
 * Each name appears as an identifier as well as a string, so a name that does not exist
 * is a compile error. `test_gl_proc_address_resolves_entry_points_by_name` checks each
 * resolves to the function it names. Adding an extension to `gl.h` means adding it
 * here.
 */
#ifndef OOPS_GL_PROCS_H
#define OOPS_GL_PROCS_H

/*
 * The core entry points are part of the same table: an engine such as ioquake3 fills
 * every pointer from a string, core names included. They live in `gl_procs_core.h` only
 * because that list is long and derived.
 */
#include "gl_procs_core.h"

/* X(core name)  -  XS(core name, suffix) for each published suffixed spelling. */
// clang-format off
#define OOPS_GL_PROC_LIST(X, XS) \
    OOPS_GL_PROC_LIST_CORE(X) \
    X(glActiveTexture) XS(glActiveTexture, ARB) \
    X(glBeginQuery) XS(glBeginQuery, ARB) \
    X(glBindBuffer) XS(glBindBuffer, ARB) \
    X(glBlendColor) XS(glBlendColor, EXT) \
    X(glBlendEquation) XS(glBlendEquation, EXT) \
    X(glBufferData) XS(glBufferData, ARB) \
    X(glBufferSubData) XS(glBufferSubData, ARB) \
    X(glClientActiveTexture) XS(glClientActiveTexture, ARB) \
    X(glDeleteBuffers) XS(glDeleteBuffers, ARB) \
    X(glDeleteQueries) XS(glDeleteQueries, ARB) \
    X(glDrawRangeElements) XS(glDrawRangeElements, EXT) \
    X(glEndQuery) XS(glEndQuery, ARB) \
    X(glFogCoordPointer) XS(glFogCoordPointer, EXT) \
    X(glFogCoordd) XS(glFogCoordd, EXT) \
    X(glFogCoorddv) XS(glFogCoorddv, EXT) \
    X(glFogCoordf) XS(glFogCoordf, EXT) \
    X(glFogCoordfv) XS(glFogCoordfv, EXT) \
    X(glGenBuffers) XS(glGenBuffers, ARB) \
    X(glGenQueries) XS(glGenQueries, ARB) \
    X(glGetBufferParameteriv) XS(glGetBufferParameteriv, ARB) \
    X(glGetBufferPointerv) XS(glGetBufferPointerv, ARB) \
    X(glGetBufferSubData) XS(glGetBufferSubData, ARB) \
    X(glGetQueryObjectiv) XS(glGetQueryObjectiv, ARB) \
    X(glGetQueryObjectuiv) XS(glGetQueryObjectuiv, ARB) \
    X(glGetQueryiv) XS(glGetQueryiv, ARB) \
    X(glIsBuffer) XS(glIsBuffer, ARB) \
    X(glIsQuery) XS(glIsQuery, ARB) \
    X(glLoadTransposeMatrixd) XS(glLoadTransposeMatrixd, ARB) \
    X(glLoadTransposeMatrixf) XS(glLoadTransposeMatrixf, ARB) \
    X(glMapBuffer) XS(glMapBuffer, ARB) \
    X(glMultTransposeMatrixd) XS(glMultTransposeMatrixd, ARB) \
    X(glMultTransposeMatrixf) XS(glMultTransposeMatrixf, ARB) \
    X(glMultiDrawArrays) XS(glMultiDrawArrays, EXT) \
    X(glMultiDrawElements) XS(glMultiDrawElements, EXT) \
    X(glMultiTexCoord1d) XS(glMultiTexCoord1d, ARB) \
    X(glMultiTexCoord1dv) XS(glMultiTexCoord1dv, ARB) \
    X(glMultiTexCoord1f) XS(glMultiTexCoord1f, ARB) \
    X(glMultiTexCoord1fv) XS(glMultiTexCoord1fv, ARB) \
    X(glMultiTexCoord1i) XS(glMultiTexCoord1i, ARB) \
    X(glMultiTexCoord1iv) XS(glMultiTexCoord1iv, ARB) \
    X(glMultiTexCoord1s) XS(glMultiTexCoord1s, ARB) \
    X(glMultiTexCoord1sv) XS(glMultiTexCoord1sv, ARB) \
    X(glMultiTexCoord2d) XS(glMultiTexCoord2d, ARB) \
    X(glMultiTexCoord2dv) XS(glMultiTexCoord2dv, ARB) \
    X(glMultiTexCoord2f) XS(glMultiTexCoord2f, ARB) \
    X(glMultiTexCoord2fv) XS(glMultiTexCoord2fv, ARB) \
    X(glMultiTexCoord2i) XS(glMultiTexCoord2i, ARB) \
    X(glMultiTexCoord2iv) XS(glMultiTexCoord2iv, ARB) \
    X(glMultiTexCoord2s) XS(glMultiTexCoord2s, ARB) \
    X(glMultiTexCoord2sv) XS(glMultiTexCoord2sv, ARB) \
    X(glMultiTexCoord3d) XS(glMultiTexCoord3d, ARB) \
    X(glMultiTexCoord3dv) XS(glMultiTexCoord3dv, ARB) \
    X(glMultiTexCoord3f) XS(glMultiTexCoord3f, ARB) \
    X(glMultiTexCoord3fv) XS(glMultiTexCoord3fv, ARB) \
    X(glMultiTexCoord3i) XS(glMultiTexCoord3i, ARB) \
    X(glMultiTexCoord3iv) XS(glMultiTexCoord3iv, ARB) \
    X(glMultiTexCoord3s) XS(glMultiTexCoord3s, ARB) \
    X(glMultiTexCoord3sv) XS(glMultiTexCoord3sv, ARB) \
    X(glMultiTexCoord4d) XS(glMultiTexCoord4d, ARB) \
    X(glMultiTexCoord4dv) XS(glMultiTexCoord4dv, ARB) \
    X(glMultiTexCoord4f) XS(glMultiTexCoord4f, ARB) \
    X(glMultiTexCoord4fv) XS(glMultiTexCoord4fv, ARB) \
    X(glMultiTexCoord4i) XS(glMultiTexCoord4i, ARB) \
    X(glMultiTexCoord4iv) XS(glMultiTexCoord4iv, ARB) \
    X(glMultiTexCoord4s) XS(glMultiTexCoord4s, ARB) \
    X(glMultiTexCoord4sv) XS(glMultiTexCoord4sv, ARB) \
    X(glPointParameterf) XS(glPointParameterf, ARB) XS(glPointParameterf, EXT) \
    X(glPointParameterfv) XS(glPointParameterfv, ARB) XS(glPointParameterfv, EXT) \
    X(glSecondaryColor3b) XS(glSecondaryColor3b, EXT) \
    X(glSecondaryColor3bv) XS(glSecondaryColor3bv, EXT) \
    X(glSecondaryColor3d) XS(glSecondaryColor3d, EXT) \
    X(glSecondaryColor3dv) XS(glSecondaryColor3dv, EXT) \
    X(glSecondaryColor3f) XS(glSecondaryColor3f, EXT) \
    X(glSecondaryColor3fv) XS(glSecondaryColor3fv, EXT) \
    X(glSecondaryColor3i) XS(glSecondaryColor3i, EXT) \
    X(glSecondaryColor3iv) XS(glSecondaryColor3iv, EXT) \
    X(glSecondaryColor3s) XS(glSecondaryColor3s, EXT) \
    X(glSecondaryColor3sv) XS(glSecondaryColor3sv, EXT) \
    X(glSecondaryColor3ub) XS(glSecondaryColor3ub, EXT) \
    X(glSecondaryColor3ubv) XS(glSecondaryColor3ubv, EXT) \
    X(glSecondaryColor3ui) XS(glSecondaryColor3ui, EXT) \
    X(glSecondaryColor3uiv) XS(glSecondaryColor3uiv, EXT) \
    X(glSecondaryColor3us) XS(glSecondaryColor3us, EXT) \
    X(glSecondaryColor3usv) XS(glSecondaryColor3usv, EXT) \
    X(glSecondaryColorPointer) XS(glSecondaryColorPointer, EXT) \
    X(glTexImage3D) XS(glTexImage3D, EXT) \
    X(glTexSubImage3D) XS(glTexSubImage3D, EXT) \
    X(glUnmapBuffer) XS(glUnmapBuffer, ARB) \
    X(glWindowPos2d) XS(glWindowPos2d, ARB) \
    X(glWindowPos2dv) XS(glWindowPos2dv, ARB) \
    X(glWindowPos2f) XS(glWindowPos2f, ARB) \
    X(glWindowPos2fv) XS(glWindowPos2fv, ARB) \
    X(glWindowPos2i) XS(glWindowPos2i, ARB) \
    X(glWindowPos2iv) XS(glWindowPos2iv, ARB) \
    X(glWindowPos2s) XS(glWindowPos2s, ARB) \
    X(glWindowPos2sv) XS(glWindowPos2sv, ARB) \
    X(glWindowPos3d) XS(glWindowPos3d, ARB) \
    X(glWindowPos3dv) XS(glWindowPos3dv, ARB) \
    X(glWindowPos3f) XS(glWindowPos3f, ARB) \
    X(glWindowPos3fv) XS(glWindowPos3fv, ARB) \
    X(glWindowPos3i) XS(glWindowPos3i, ARB) \
    X(glWindowPos3iv) XS(glWindowPos3iv, ARB) \
    X(glWindowPos3s) XS(glWindowPos3s, ARB) \
    X(glWindowPos3sv) XS(glWindowPos3sv, ARB)
// clang-format on

#endif /* OOPS_GL_PROCS_H */
