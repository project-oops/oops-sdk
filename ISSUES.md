# Issues

Open defects, gaps and unmeasured facts, one line each. Delete a line when it is fixed.

## Needs hardware confirmation

- The base texture's LOD bias on unit 1 (3c1466e) and the alpha-test slot mirror (35f0f1a); Neverball's material is on unit 1.
- Whether `gnm_display` handle 0 ever occurs.

## Gaps

- `glUseProgram`, `glUniform*` and `glVertexAttrib*` execute immediately during display-list compile instead of being recorded (GL 2.0 section 5.4).
- The HTML container never fills its image cache and never applies clip rectangles.
- `glCopyTexSubImage` reads back and flushes once per row.
- `glContextCreate` is 811 lines; `gl_draw_triangle_pv_body` (212), `gl_hw_tri_census` (162), `gl_hw_tri_shader` (152) and `gen_builtin_inverse_trig` (170) are over 150.
- `gl_list.c` has no way for tests to capture log output.

## Build and tests

- `test_html`, `test_webview` and `test_js` are not built by `make test`.
- Files outside the formatter and comment pass: `include/libc/{inttypes,math,stdio,time}.h`, `include/oops/system.h`, `src/agc/agc_display.c`, `src/math/math.c`, `src/system/{fs,libc,system}.c`, `src/time/time.c`, `src/net/net.c`, `src/draw/png.c`, `tests/unit/test_system.c`, `tools/libc-check/libc_check.c`, and the permission-restricted source files.
- `README.md` and `docs/API_REFERENCE.md` contain em-dashes.
- The shared helpers in oops-apps `common/` (`probe_px.h`, `cube_frame.h`, `app_pad.h`, `app_ui.h`) are candidates for the SDK.
