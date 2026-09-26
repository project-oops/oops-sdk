/*
 * header-check: this SDK's GL headers compiled beside a real `GL/glext.h`.
 *
 * A hosted title - one built against the Mesa sysroot - includes `<GL/gl.h>` from here
 * **and** Mesa's own `<GL/glext.h>`, because it drives both. That is the only place the
 * two meet, and two kinds of mistake only show up there:
 *
 *  - **a macro redefined with a different token sequence**, which is a diagnostic under
 *    `-Werror` even when the value is identical. Every extension spelling this header
 * adds is a name `glext.h` also defines, so `#define GL_TEXTURE_3D_EXT GL_TEXTURE_3D`
 * breaks the build while `#define GL_TEXTURE_3D_EXT 0x806F` does not;
 *  - **a declaration that conflicts**, which no amount of agreement on values will
 * hide. `glTexImage3DEXT` takes a `GLenum` internal format and the core `glTexImage3D`
 * takes a `GLint`; declaring the extension's with the core's type compiles alone and
 * fails here.
 *
 * Both of those landed on 2026-09-20, in the same change, and were found by an app
 * failing to build rather than by anything that was looking. This looks.
 *
 * `build.sh` compiles this twice: once with the SDK's headers alone, and once with
 * `glext.h` ahead of them when a sibling `oops-mesa` checkout has one. The second is
 * skipped with a line saying so when it does not - a check that looks like it ran and
 * did not is worse than none.
 */
#include "GL/gl.h"
#include "GL/glu.h"

#ifdef HEADER_CHECK_WITH_GLEXT
/*
 * Mesa's headers in the order a hosted title includes them: its `GL/gl.h` defines the
 * core, and `GL/glext.h` the extensions. The SDK's own `GL/gl.h` above has already
 * defined both, so every name here is a redefinition - which is the point.
 */
#include <GL/glext.h>
#endif

/* The header is only useful if its declarations are usable, so a few are named here: a
 * function pointer of each shape the extension spellings introduced, which will not
 * compile if the types moved. */
void (*header_check_tex_image_3d_ext)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei,
                                      GLint, GLenum, GLenum,
                                      const GLvoid *) = glTexImage3DEXT;
void (*header_check_tex_sub_image_3d_ext)(GLenum, GLint, GLint, GLint, GLint, GLsizei,
                                          GLsizei, GLsizei, GLenum, GLenum,
                                          const GLvoid *) = glTexSubImage3DEXT;
void (*header_check_begin_query_arb)(GLenum, GLuint) = glBeginQueryARB;
void (*header_check_gen_queries_arb)(GLsizei, GLuint *) = glGenQueriesARB;

/* And the alias values, checked where a port would use them. These are the same
 * assertions `tests/unit/test_gl.c` makes; they are repeated because this translation
 * unit is the one that sees `glext.h`, where a wrong literal would be a redefinition
 * rather than a silent difference. */
_Static_assert(GL_TEXTURE_3D_EXT == GL_TEXTURE_3D, "alias value");
_Static_assert(GL_TEXTURE_WRAP_R_EXT == GL_TEXTURE_WRAP_R, "alias value");
_Static_assert(GL_DEPTH_COMPONENT16_ARB == GL_DEPTH_COMPONENT16, "alias value");
_Static_assert(GL_COMPARE_R_TO_TEXTURE_ARB == GL_COMPARE_R_TO_TEXTURE, "alias value");
_Static_assert(GL_SAMPLES_PASSED_ARB == GL_SAMPLES_PASSED, "alias value");
_Static_assert(GL_TEXTURE0_ARB == GL_TEXTURE0, "alias value");

int header_check_touch(void);

int header_check_touch(void) {
    return (header_check_tex_image_3d_ext != 0) +
           (header_check_tex_sub_image_3d_ext != 0) +
           (header_check_begin_query_arb != 0) + (header_check_gen_queries_arb != 0);
}
