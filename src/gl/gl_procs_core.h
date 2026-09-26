/*
 * The **core** entry points a title can resolve by name, as opposed to the extensions
 * in `gl_procs.h`.
 *
 * # Why core GL is in a lookup table at all
 *
 * `gl_procs.h` says the list holds extensions because "core GL 1.1 is linked directly,
 * by symbol, and never goes through here". That is true of Neverball, which calls
 * `glBegin` and looks up only what it might not get. **It is not true of an engine that
 * resolves everything by string**, and ioquake3 is one: `sdl_glimp.c:269` fills every
 * `qgl*` pointer through `SDL_GL_GetProcAddress`, core names included, and a single
 * NULL makes `GLimp_GetProcAddresses` return false and the renderer refuse to start:
 *
 *     #define GLE( ret, name, ... ) qgl##name = (name##proc *)
 * SDL_GL_GetProcAddress("gl" #name); \ if ( qgl##name == NULL ) { ...ERROR: Missing
 * OpenGL function...; success = qfalse; }
 *
 * Measured on q3rally before this file existed: it links with nothing undefined - every
 * one of those functions *is* in the payload - and asks for 66 of them by name, of
 * which the table answered **none**. A title in exactly that state boots, opens the
 * display, prints 66 "Missing OpenGL function" lines and stops.
 * `oops-apps/src/oops-titles/q3rally/scripts/gl-surface.sh` is the measurement, and it
 * is worth running for any port that binds GL through a pointer table.
 *
 * # What is in here
 *
 * **Every entry point `GL/gl.h` declares that the always-linked GL objects define** -
 * the set `common/app.mk` calls `OOPS_GL_SRCS`, so not `glut.c` or `gl_glu.c`, whose
 * names would be undefined for a title that does not take those features and would fail
 * its link. 485 of them, derived rather than chosen; `gl_procs.h` keeps the extensions
 * and their suffixed spellings.
 *
 * oops-gl's own entry points are here too - `glContextSetVersion`,
 * `glGetFrameReadback`, `glGetHardwareStatus` and the rest. They are declared in the
 * same header and they resolve, so a program that asks for one by name should get it.
 *
 * **A name here that does not exist is a compile error**, because each appears as an
 * identifier as well as a string. That is what keeps the list from rotting, and it is
 * the same property `gl_procs.h` relies on.
 */
#ifndef OOPS_GL_PROCS_CORE_H
#define OOPS_GL_PROCS_CORE_H

/* X(name) for each. No suffixed spellings: those are extensions and live in
 * `gl_procs.h`. */
// clang-format off
#define OOPS_GL_PROC_LIST_CORE(X) \
X(glAccum) X(glAlphaFunc) X(glAreTexturesResident) X(glArrayElement) X(glAttachShader) \
    X(glBegin) X(glBindAttribLocation) X(glBindFramebuffer) X(glBindRenderbuffer) X(glBindTexture) \
    X(glBitmap) X(glBlendEquationSeparate) X(glBlendFunc) X(glBlendFuncSeparate) X(glBlitFramebuffer) \
    X(glCallList) X(glCallLists) X(glCheckFramebufferStatus) X(glClear) X(glClearAccum) \
    X(glClearColor) X(glClearDepth) X(glClearDepthf) X(glClearIndex) X(glClearStencil) \
    X(glClipPlane) X(glColor3b) X(glColor3bv) X(glColor3d) X(glColor3dv) \
    X(glColor3f) X(glColor3fv) X(glColor3i) X(glColor3iv) X(glColor3s) \
    X(glColor3sv) X(glColor3ub) X(glColor3ubv) X(glColor3ui) X(glColor3uiv) \
    X(glColor3us) X(glColor3usv) X(glColor4b) X(glColor4bv) X(glColor4d) \
    X(glColor4dv) X(glColor4f) X(glColor4fv) X(glColor4i) X(glColor4iv) \
    X(glColor4s) X(glColor4sv) X(glColor4ub) X(glColor4ubv) X(glColor4ui) \
    X(glColor4uiv) X(glColor4us) X(glColor4usv) X(glColorMask) X(glColorMaterial) \
    X(glColorPointer) X(glCompileShader) X(glCompressedTexImage1D) X(glCompressedTexImage2D) X(glCompressedTexImage3D) \
    X(glCompressedTexSubImage1D) X(glCompressedTexSubImage2D) X(glCompressedTexSubImage3D) X(glContextCreate) X(glContextDestroy) \
    X(glContextGetVersion) X(glContextMakeCurrent) X(glContextSetVersion) X(glCopyPixels) X(glCopyTexImage1D) \
    X(glCopyTexImage2D) X(glCopyTexSubImage1D) X(glCopyTexSubImage2D) X(glCopyTexSubImage3D) X(glCreateProgram) \
    X(glCreateShader) X(glCullFace) X(glDeleteFramebuffers) X(glDeleteLists) X(glDeleteProgram) \
    X(glDeleteRenderbuffers) X(glDeleteShader) X(glDeleteTextures) X(glDepthFunc) X(glDepthMask) \
    X(glDepthRange) X(glDepthRangef) X(glDetachShader) X(glDisable) X(glDisableClientState) \
    X(glDisableVertexAttribArray) X(glDrawArrays) X(glDrawBuffer) X(glDrawBuffers) X(glDrawElements) \
    X(glDrawPixels) X(glEdgeFlag) X(glEdgeFlagPointer) X(glEdgeFlagv) X(glEnable) \
    X(glEnableClientState) X(glEnableVertexAttribArray) X(glEnd) X(glEndList) X(glEvalCoord1d) \
    X(glEvalCoord1dv) X(glEvalCoord1f) X(glEvalCoord1fv) X(glEvalCoord2d) X(glEvalCoord2dv) \
    X(glEvalCoord2f) X(glEvalCoord2fv) X(glEvalMesh1) X(glEvalMesh2) X(glEvalPoint1) \
    X(glEvalPoint2) X(glFeedbackBuffer) X(glFinish) X(glFlush) X(glFogf) \
    X(glFogfv) X(glFogi) X(glFogiv) X(glFramebufferRenderbuffer) X(glFramebufferTexture2D) \
    X(glFrontFace) X(glFrustum) X(glGenFramebuffers) X(glGenLists) X(glGenRenderbuffers) \
    X(glGenTextures) X(glGenerateMipmap) X(glGetActiveAttrib) X(glGetActiveUniform) X(glGetAttachedShaders) \
    X(glGetAttribLocation) X(glGetBooleanv) X(glGetCanary) X(glGetCanaryEx) X(glGetClipPlane) \
    X(glGetCompressedTexImage) X(glGetCurrentContext) X(glGetDoublev) X(glGetError) X(glGetFloatv) \
    X(glGetFrameReadback) X(glGetFrameReadbackSampled) X(glGetFramebufferAttachmentParameteriv) X(glGetHardwareStatus) X(glGetIntegerv) \
    X(glGetLightfv) X(glGetLightiv) X(glGetMapdv) X(glGetMapfv) X(glGetMapiv) \
    X(glGetMaterialfv) X(glGetMaterialiv) X(glGetPixelMapfv) X(glGetPixelMapuiv) X(glGetPixelMapusv) \
    X(glGetPointerv) X(glGetPolygonStipple) X(glGetProgramHardwareLog) X(glGetProgramInfoLog) X(glGetProgramiv) \
    X(glGetRenderbufferParameteriv) X(glGetShaderInfoLog) X(glGetShaderPrecisionFormat) X(glGetShaderSource) X(glGetShaderiv) \
    X(glGetString) X(glGetTexEnvfv) X(glGetTexEnviv) X(glGetTexGendv) X(glGetTexGenfv) \
    X(glGetTexGeniv) X(glGetTexImage) X(glGetTexLevelParameterfv) X(glGetTexLevelParameteriv) X(glGetTexParameterfv) \
    X(glGetTexParameteriv) X(glGetUniformLocation) X(glGetUniformfv) X(glGetUniformiv) X(glGetVertexAttribPointerv) \
    X(glGetVertexAttribdv) X(glGetVertexAttribfv) X(glGetVertexAttribiv) X(glHint) X(glIndexMask) \
    X(glIndexPointer) X(glIndexd) X(glIndexdv) X(glIndexf) X(glIndexfv) \
    X(glIndexi) X(glIndexiv) X(glIndexs) X(glIndexsv) X(glIndexub) \
    X(glIndexubv) X(glInitNames) X(glInterleavedArrays) X(glIsEnabled) X(glIsFramebuffer) \
    X(glIsHardwareAccelerated) X(glIsList) X(glIsProgram) X(glIsRenderbuffer) X(glIsShader) \
    X(glIsTexture) X(glLightModelf) X(glLightModelfv) X(glLightModeli) X(glLightModeliv) \
    X(glLightf) X(glLightfv) X(glLighti) X(glLightiv) X(glLineStipple) \
    X(glLineWidth) X(glLinkProgram) X(glListBase) X(glLoadIdentity) X(glLoadMatrixd) \
    X(glLoadMatrixf) X(glLoadName) X(glLogicOp) X(glMap1d) X(glMap1f) \
    X(glMap2d) X(glMap2f) X(glMapGrid1d) X(glMapGrid1f) X(glMapGrid2d) \
    X(glMapGrid2f) X(glMaterialf) X(glMaterialfv) X(glMateriali) X(glMaterialiv) \
    X(glMatrixMode) X(glMultMatrixd) X(glMultMatrixf) X(glNewList) X(glNormal3b) \
    X(glNormal3bv) X(glNormal3d) X(glNormal3dv) X(glNormal3f) X(glNormal3fv) \
    X(glNormal3i) X(glNormal3iv) X(glNormal3s) X(glNormal3sv) X(glNormalPointer) \
    X(glOrtho) X(glPassThrough) X(glPixelMapfv) X(glPixelMapuiv) X(glPixelMapusv) \
    X(glPixelStoref) X(glPixelStorei) X(glPixelTransferf) X(glPixelTransferi) X(glPixelZoom) \
    X(glPointParameteri) X(glPointParameteriv) X(glPointSize) X(glPolygonMode) X(glPolygonOffset) \
    X(glPolygonStipple) X(glPopAttrib) X(glPopClientAttrib) X(glPopMatrix) X(glPopName) \
    X(glPrioritizeTextures) X(glPushAttrib) X(glPushClientAttrib) X(glPushMatrix) X(glPushName) \
    X(glRasterPos2d) X(glRasterPos2dv) X(glRasterPos2f) X(glRasterPos2fv) X(glRasterPos2i) \
    X(glRasterPos2iv) X(glRasterPos2s) X(glRasterPos2sv) X(glRasterPos3d) X(glRasterPos3dv) \
    X(glRasterPos3f) X(glRasterPos3fv) X(glRasterPos3i) X(glRasterPos3iv) X(glRasterPos3s) \
    X(glRasterPos3sv) X(glRasterPos4d) X(glRasterPos4dv) X(glRasterPos4f) X(glRasterPos4fv) \
    X(glRasterPos4i) X(glRasterPos4iv) X(glRasterPos4s) X(glRasterPos4sv) X(glReadBuffer) \
    X(glReadPixels) X(glRectd) X(glRectdv) X(glRectf) X(glRectfv) \
    X(glRecti) X(glRectiv) X(glRects) X(glRectsv) X(glReleaseShaderCompiler) \
    X(glRenderMode) X(glRenderbufferStorage) X(glRenderbufferStorageMultisample) X(glRequestHardwareDump) X(glRotated) \
    X(glRotatef) X(glSampleCoverage) X(glScaled) X(glScalef) X(glScissor) \
    X(glSelectBuffer) X(glSetHardwarePrelude) X(glShadeModel) X(glShaderBinary) X(glShaderSource) \
    X(glStencilFunc) X(glStencilFuncSeparate) X(glStencilMask) X(glStencilMaskSeparate) X(glStencilOp) \
    X(glStencilOpSeparate) X(glSwapBuffers) X(glTexCoord1d) X(glTexCoord1dv) X(glTexCoord1f) \
    X(glTexCoord1fv) X(glTexCoord1i) X(glTexCoord1iv) X(glTexCoord1s) X(glTexCoord1sv) \
    X(glTexCoord2d) X(glTexCoord2dv) X(glTexCoord2f) X(glTexCoord2fv) X(glTexCoord2i) \
    X(glTexCoord2iv) X(glTexCoord2s) X(glTexCoord2sv) X(glTexCoord3d) X(glTexCoord3dv) \
    X(glTexCoord3f) X(glTexCoord3fv) X(glTexCoord3i) X(glTexCoord3iv) X(glTexCoord3s) \
    X(glTexCoord3sv) X(glTexCoord4d) X(glTexCoord4dv) X(glTexCoord4f) X(glTexCoord4fv) \
    X(glTexCoord4i) X(glTexCoord4iv) X(glTexCoord4s) X(glTexCoord4sv) X(glTexCoordPointer) \
    X(glTexEnvf) X(glTexEnvfv) X(glTexEnvi) X(glTexEnviv) X(glTexGend) \
    X(glTexGendv) X(glTexGenf) X(glTexGenfv) X(glTexGeni) X(glTexGeniv) \
    X(glTexImage1D) X(glTexImage2D) X(glTexParameterf) X(glTexParameterfv) X(glTexParameteri) \
    X(glTexParameteriv) X(glTexSubImage1D) X(glTexSubImage2D) X(glTranslated) X(glTranslatef) \
    X(glUniform1f) X(glUniform1fv) X(glUniform1i) X(glUniform1iv) X(glUniform2f) \
    X(glUniform2fv) X(glUniform2i) X(glUniform2iv) X(glUniform3f) X(glUniform3fv) \
    X(glUniform3i) X(glUniform3iv) X(glUniform4f) X(glUniform4fv) X(glUniform4i) \
    X(glUniform4iv) X(glUniformMatrix2fv) X(glUniformMatrix2x3fv) X(glUniformMatrix2x4fv) X(glUniformMatrix3fv) \
    X(glUniformMatrix3x2fv) X(glUniformMatrix3x4fv) X(glUniformMatrix4fv) X(glUniformMatrix4x2fv) X(glUniformMatrix4x3fv) \
    X(glUseProgram) X(glValidateProgram) X(glVertex2d) X(glVertex2dv) X(glVertex2f) \
    X(glVertex2fv) X(glVertex2i) X(glVertex2iv) X(glVertex2s) X(glVertex2sv) \
    X(glVertex3d) X(glVertex3dv) X(glVertex3f) X(glVertex3fv) X(glVertex3i) \
    X(glVertex3iv) X(glVertex3s) X(glVertex3sv) X(glVertex4d) X(glVertex4dv) \
    X(glVertex4f) X(glVertex4fv) X(glVertex4i) X(glVertex4iv) X(glVertex4s) \
    X(glVertex4sv) X(glVertexAttrib1d) X(glVertexAttrib1dv) X(glVertexAttrib1f) X(glVertexAttrib1fv) \
    X(glVertexAttrib1s) X(glVertexAttrib1sv) X(glVertexAttrib2d) X(glVertexAttrib2dv) X(glVertexAttrib2f) \
    X(glVertexAttrib2fv) X(glVertexAttrib2s) X(glVertexAttrib2sv) X(glVertexAttrib3d) X(glVertexAttrib3dv) \
    X(glVertexAttrib3f) X(glVertexAttrib3fv) X(glVertexAttrib3s) X(glVertexAttrib3sv) X(glVertexAttrib4Nbv) \
    X(glVertexAttrib4Niv) X(glVertexAttrib4Nsv) X(glVertexAttrib4Nub) X(glVertexAttrib4Nubv) X(glVertexAttrib4Nuiv) \
    X(glVertexAttrib4Nusv) X(glVertexAttrib4bv) X(glVertexAttrib4d) X(glVertexAttrib4dv) X(glVertexAttrib4f) \
    X(glVertexAttrib4fv) X(glVertexAttrib4iv) X(glVertexAttrib4s) X(glVertexAttrib4sv) X(glVertexAttrib4ubv) \
    X(glVertexAttrib4uiv) X(glVertexAttrib4usv) X(glVertexAttribPointer) X(glVertexPointer) X(glViewport) \
    /* end of the list - a line with no continuation, so the macro stops here */
// clang-format on

#endif /* OOPS_GL_PROCS_CORE_H */
