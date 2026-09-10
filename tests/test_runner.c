#include "tests/test_common.h"

int g_tests_run = 0;
int g_tests_passed = 0;
int g_tests_failed = 0;

/* Weak stubs for display getters when running host test runner */
#include "oops/display.h"
__attribute__((weak))
uint32_t *oops_display_get_framebuffer(oops_display_t *disp) {
  (void)disp;
  return NULL;
}
__attribute__((weak)) unsigned int
oops_display_get_width(const oops_display_t *disp) {
  (void)disp;
  return 1920;
}
__attribute__((weak)) unsigned int
oops_display_get_height(const oops_display_t *disp) {
  (void)disp;
  return 1080;
}
__attribute__((weak)) int
oops_display_is_gpu_accelerated(const oops_display_t *disp) {
  (void)disp;
  return 0;
}
#include "agc/display.h"
__attribute__((weak)) int
agc_display_is_gpu_accelerated(const agc_display_t *disp) {
  (void)disp;
  return 0;
}

/* Declarations of unit test suites */
void run_unit_tests_agc_tiler(void);
void run_unit_tests_draw(void);
void run_unit_tests_input(void);
void run_unit_tests_audio(void);
void run_unit_tests_videodec(void);
void run_unit_tests_audiodec(void);
void run_unit_tests_memory(void);
void run_unit_tests_system(void);
void run_unit_tests_offsets(void);
void run_unit_tests_sysmodule(void);
void run_unit_tests_dialog(void);
void run_unit_tests_netctl(void);
void run_unit_tests_savedata(void);
void run_unit_tests_escalate(void);
void run_unit_tests_pkg(void);
void run_unit_tests_time(void);
void run_unit_tests_thread(void);
void run_unit_tests_net(void);
void run_unit_tests_freestd(void);
void run_unit_tests_krw(void);
void run_unit_tests_inject(void);
void run_unit_tests_gpu(void);

/* Declarations of integration test suites */
void run_integration_tests_pipeline(void);
void run_integration_tests_memory(void);
void run_integration_tests_thread_pool(void);
void run_integration_tests_net_loopback(void);

int main(int argc, char **argv) {
  bool run_unit = true;
  bool run_int = true;

  if (argc > 1) {
    if (strcmp(argv[1], "unit") == 0) {
      run_int = false;
    } else if (strcmp(argv[1], "int") == 0) {
      run_unit = false;
    }
  }

  printf("\n=======================================================\n");
  printf("         OOPS-SDK COMPREHENSIVE TEST SUITE             \n");
  printf("=======================================================\n");

  if (run_unit) {
    printf("\n>>> RUNNING UNIT TESTS <<<\n");
    run_unit_tests_agc_tiler();
    run_unit_tests_draw();
    run_unit_tests_input();
    run_unit_tests_audio();
    run_unit_tests_videodec();
    run_unit_tests_audiodec();
    run_unit_tests_memory();
    run_unit_tests_system();
    run_unit_tests_offsets();
    run_unit_tests_sysmodule();
    run_unit_tests_dialog();
    run_unit_tests_netctl();
    run_unit_tests_savedata();
    run_unit_tests_escalate();
    run_unit_tests_pkg();
    run_unit_tests_time();
    run_unit_tests_thread();
    run_unit_tests_net();
    run_unit_tests_freestd();
    run_unit_tests_krw();
    run_unit_tests_inject();
    run_unit_tests_gpu();
  }

  if (run_int) {
    printf("\n>>> RUNNING INTEGRATION TESTS <<<\n");
    run_integration_tests_pipeline();
    run_integration_tests_memory();
    run_integration_tests_thread_pool();
    run_integration_tests_net_loopback();
  }

  printf("\n=======================================================\n");
  printf(" TEST SUMMARY: %d Ran | \033[32m%d Passed\033[0m | \033[%sm%d "
         "Failed\033[0m\n",
         g_tests_run, g_tests_passed, g_tests_failed > 0 ? "31" : "32",
         g_tests_failed);
  printf("=======================================================\n\n");

  return (g_tests_failed == 0) ? 0 : 1;
}
