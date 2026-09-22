/*
 * Unit tests and PM4 command stream static validator for RDNA2 (GFX10.3).
 *
 * Decodes Direct Command Buffers (DCB) emitted by oops_agc_draw_primitive
 * and GPU compute dispatch, verifying packet boundaries, opcode validity,
 * register address ranges, and Depth/Color buffer state invariants before
 * sending commands to physical hardware.
 */

#include "oops/agc.h"
#include "oops/gpu.h"
#include "src/agc/agc_internal.h"
#include "GL/gl.h"
#include "src/gl/gl_internal.h"
#include "tests/test_common.h"
#include <string.h>
#include <stdio.h>

typedef struct {
  uint32_t total_packets;
  uint32_t set_context_reg_count;
  uint32_t set_sh_reg_count;
  uint32_t set_uconfig_reg_count;
  uint32_t draw_index_auto_count;
  uint32_t num_instances_count;
  uint32_t release_mem_count;
  uint32_t event_write_count;
  uint32_t nop_count;
  uint32_t other_packet_count;

  /* State tracking for hardware invariant validation */
  uint32_t db_render_control;
  uint32_t db_depth_control;
  uint32_t db_z_info;
  uint32_t db_shader_control;
  uint64_t db_z_read_base;
  uint64_t db_z_write_base;

  uint32_t cb_color0_info;
  uint32_t cb_color0_attrib2;
  uint64_t cb_color0_base;
  uint32_t cb_target_mask;
  uint32_t pa_su_sc_mode_cntl;

  uint32_t spi_shader_pos_format;
  uint32_t spi_shader_col_format;
  uint32_t spi_ps_in_control;
  uint64_t spi_shader_pgm_ps;
  uint64_t spi_shader_user_data_ps_01;

  uint32_t vgt_primitive_type;
  uint32_t last_draw_index_count;

  /* Error and diagnostics */
  uint32_t error_count;
  char last_error[256];
} oops_pm4_report_t;

/*
 * Walks and validates a raw PM4 command stream.
 * Returns 0 if valid with zero errors, or -1 if any validation error occurred.
 */
static int oops_pm4_validate_stream(const uint32_t *words, size_t word_count,
                                    oops_pm4_report_t *report) {
  if (!words || word_count == 0 || !report) {
    return -1;
  }
  memset(report, 0, sizeof(*report));

  size_t idx = 0;
  while (idx < word_count) {
    uint32_t header = words[idx];

    /* PS5 libSceAgc single-dword prefetch NOP pad (measured on hardware, D565) */
    if (header == 0xffff1000u) {
      report->nop_count++;
      report->total_packets++;
      idx++;
      continue;
    }
    uint32_t pkt_type = (header >> 30) & 0x3u;

    if (pkt_type != 3u) {
      report->error_count++;
      snprintf(report->last_error, sizeof(report->last_error),
               "Word %zu: Non-Type3 PM4 packet (type=%u, header=0x%08x)",
               idx, (unsigned)pkt_type, header);
      return -1;
    }

    uint32_t count = ((header >> 16) & 0x3fffu) + 1u; /* Payload dwords */
    uint32_t opcode = (header >> 8) & 0xffu;
    size_t pkt_end = idx + 1u + count;

    if (pkt_end > word_count) {
      report->error_count++;
      snprintf(report->last_error, sizeof(report->last_error),
               "Word %zu: Truncated packet (opcode=0x%02x, count=%u, available=%zu)",
               idx, (unsigned)opcode, (unsigned)count, word_count - idx - 1u);
      return -1;
    }

    report->total_packets++;
    const uint32_t *payload = &words[idx + 1u];

    switch (opcode) {
      case OOPS_AGC_PM4_SET_CONTEXT_REG: {
        report->set_context_reg_count++;
        if (count < 2u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: SET_CONTEXT_REG packet too small (count=%u)", idx, (unsigned)count);
          return -1;
        }
        uint32_t base_reg = payload[0] & 0xffffu;
        uint32_t num_regs = count - 1u;
        if (base_reg + num_regs > 0x400u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: Context reg out of range (base=0x%03x, num=%u)",
                   idx, (unsigned)base_reg, (unsigned)num_regs);
          return -1;
        }
        for (uint32_t r = 0; r < num_regs; r++) {
          uint32_t reg = base_reg + r;
          uint32_t val = payload[1u + r];
          if (reg == OOPS_AGC_REG_DB_RENDER_CONTROL) report->db_render_control = val;
          else if (reg == OOPS_AGC_REG_DB_DEPTH_CONTROL) report->db_depth_control = val;
          else if (reg == OOPS_AGC_REG_DB_Z_INFO) report->db_z_info = val;
          else if (reg == OOPS_AGC_REG_DB_SHADER_CONTROL) report->db_shader_control = val;
          else if (reg == OOPS_AGC_REG_DB_Z_READ_BASE) {
            report->db_z_read_base = (report->db_z_read_base & 0xffffffff00000000ULL) | (uint64_t)val;
          } else if (reg == OOPS_AGC_REG_DB_Z_READ_BASE_HI) {
            report->db_z_read_base = (report->db_z_read_base & 0x00000000ffffffffULL) | ((uint64_t)val << 32);
          } else if (reg == OOPS_AGC_REG_DB_Z_WRITE_BASE) {
            report->db_z_write_base = (report->db_z_write_base & 0xffffffff00000000ULL) | (uint64_t)val;
          } else if (reg == OOPS_AGC_REG_DB_Z_WRITE_BASE_HI) {
            report->db_z_write_base = (report->db_z_write_base & 0x00000000ffffffffULL) | ((uint64_t)val << 32);
          } else if (reg == OOPS_AGC_REG_CB_COLOR0_BASE_GFX10) {
            report->cb_color0_base = (report->cb_color0_base & 0xffffff00000000ffULL) | ((uint64_t)val << 8);
          } else if (reg == OOPS_AGC_REG_CB_COLOR0_BASE_EXT) {
            report->cb_color0_base = (report->cb_color0_base & 0x000000ffffffffffULL) | ((uint64_t)val << 40);
          } else if (reg == OOPS_AGC_REG_CB_COLOR0_INFO) report->cb_color0_info = val;
          else if (reg == OOPS_AGC_REG_CB_COLOR0_ATTRIB2) report->cb_color0_attrib2 = val;
          else if (reg == OOPS_AGC_REG_CB_TARGET_MASK) report->cb_target_mask = val;
          else if (reg == OOPS_AGC_REG_PA_SU_SC_MODE_CNTL) report->pa_su_sc_mode_cntl = val;
          else if (reg == OOPS_AGC_REG_SPI_SHADER_POS_FORMAT) report->spi_shader_pos_format = val;
          else if (reg == OOPS_AGC_REG_SPI_SHADER_COL_FORMAT) report->spi_shader_col_format = val;
          else if (reg == OOPS_AGC_REG_SPI_PS_IN_CONTROL) report->spi_ps_in_control = val;
        }
        break;
      }

      case OOPS_AGC_PM4_SET_SH_REG: {
        report->set_sh_reg_count++;
        if (count < 2u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: SET_SH_REG packet too small (count=%u)", idx, (unsigned)count);
          return -1;
        }
        uint32_t base_reg = payload[0] & 0xffffu;
        uint32_t num_regs = count - 1u;
        if (base_reg + num_regs > 0x300u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: SH reg out of range (base=0x%03x, num=%u)",
                   idx, (unsigned)base_reg, (unsigned)num_regs);
          return -1;
        }
        for (uint32_t r = 0; r < num_regs; r++) {
          uint32_t reg = base_reg + r;
          uint32_t val = payload[1u + r];
          if (reg == 0x08u) {
            report->spi_shader_pgm_ps = (report->spi_shader_pgm_ps & 0xffffff00000000ffULL) | ((uint64_t)val << 8);
          } else if (reg == 0x09u) {
            report->spi_shader_pgm_ps = (report->spi_shader_pgm_ps & 0x000000ffffffffffULL) | ((uint64_t)val << 40);
          } else if (reg == 0x0cu) {
            report->spi_shader_user_data_ps_01 = (report->spi_shader_user_data_ps_01 & 0xffffffff00000000ULL) | (uint64_t)val;
          } else if (reg == 0x0du) {
            report->spi_shader_user_data_ps_01 = (report->spi_shader_user_data_ps_01 & 0x00000000ffffffffULL) | ((uint64_t)val << 32);
          }
        }
        break;
      }

      case OOPS_AGC_PM4_SET_UCONFIG_REG:
      case OOPS_AGC_PM4_SET_UCONFIG_REG_INDEX: { /* the indexed form: the index sits in bits 31:28 of the offset word, masked below */
        report->set_uconfig_reg_count++;
        if (count < 2u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: SET_UCONFIG_REG packet too small (count=%u)", idx, (unsigned)count);
          return -1;
        }
        uint32_t base_reg = payload[0] & 0xffffu;
        uint32_t num_regs = count - 1u;
        if (base_reg + num_regs > 0x400u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: UConfig reg out of range (base=0x%03x, num=%u)",
                   idx, (unsigned)base_reg, (unsigned)num_regs);
          return -1;
        }
        if (base_reg <= OOPS_AGC_REG_VGT_PRIMITIVE_TYPE &&
            (base_reg + num_regs) > OOPS_AGC_REG_VGT_PRIMITIVE_TYPE) {
          report->vgt_primitive_type = payload[1u + (OOPS_AGC_REG_VGT_PRIMITIVE_TYPE - base_reg)];
        }
        break;
      }

      case OOPS_AGC_PM4_DRAW_INDEX_AUTO: {
        report->draw_index_auto_count++;
        if (count != 2u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: DRAW_INDEX_AUTO invalid count=%u (expected 2)", idx, (unsigned)count);
          return -1;
        }
        report->last_draw_index_count = payload[0];
        if (report->last_draw_index_count == 0u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: DRAW_INDEX_AUTO with 0 indices", idx);
          return -1;
        }
        break;
      }

      case OOPS_AGC_PM4_NUM_INSTANCES: {
        report->num_instances_count++;
        if (count != 1u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: NUM_INSTANCES invalid count=%u (expected 1)", idx, (unsigned)count);
          return -1;
        }
        if (payload[0] == 0u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: NUM_INSTANCES with 0 instances", idx);
          return -1;
        }
        break;
      }

      case OOPS_AGC_PM4_RELEASE_MEM: {
        report->release_mem_count++;
        if (count < 6u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: RELEASE_MEM too small (count=%u)", idx, (unsigned)count);
          return -1;
        }
        /* Check 8-byte address alignment: payload[2] is addr_lo, payload[3] is addr_hi */
        if ((payload[2] & 0x7u) != 0u) {
          report->error_count++;
          snprintf(report->last_error, sizeof(report->last_error),
                   "Word %zu: RELEASE_MEM destination address unaligned (0x%08x)", idx, payload[2]);
          return -1;
        }
        break;
      }

      case OOPS_AGC_PM4_EVENT_WRITE: {
        report->event_write_count++;
        break;
      }

      case OOPS_AGC_PM4_NOP: {
        report->nop_count++;
        break;
      }

      default:
        report->other_packet_count++;
        break;
    }

    idx = pkt_end;
  }

  /* Hardware Invariant Checks */

  /* Invariant 1: Depth buffer state consistency */
  if (report->db_depth_control & 0x02u) { /* Z_ENABLE */
    uint32_t z_fmt = report->db_z_info & 0x3u;
    if (z_fmt == OOPS_AGC_Z_INVALID) {
      report->error_count++;
      snprintf(report->last_error, sizeof(report->last_error),
               "Depth invariant violation: Z_ENABLE set but Z_INFO format is INVALID (0)");
      return -1;
    }
    if ((report->db_depth_control & 0x04u) && (report->db_z_write_base == 0ULL)) { /* Z_WRITE_ENABLE */
      report->error_count++;
      snprintf(report->last_error, sizeof(report->last_error),
               "Depth invariant violation: Z_WRITE_ENABLE set but Z_WRITE_BASE is 0");
      return -1;
    }
  }

  /* Invariant 2: Color buffer extent bounds */
  if (report->cb_color0_base != 0ULL) {
    uint32_t w_minus_1 = report->cb_color0_attrib2 & 0x3fffu;
    uint32_t h_minus_1 = (report->cb_color0_attrib2 >> 14) & 0x3fffu;
    if (w_minus_1 == 0u && h_minus_1 == 0u && report->cb_color0_attrib2 == 0u) {
      report->error_count++;
      snprintf(report->last_error, sizeof(report->last_error),
               "Color invariant violation: CB_COLOR0_BASE is set but CB_COLOR0_ATTRIB2 extent is 0");
      return -1;
    }
  }

  /* Invariant 3: Primitive draw prerequisites */
  if (report->draw_index_auto_count > 0u) {
    if (report->vgt_primitive_type == 0u) {
      report->error_count++;
      snprintf(report->last_error, sizeof(report->last_error),
               "Draw invariant violation: DRAW_INDEX_AUTO emitted without setting VGT_PRIMITIVE_TYPE");
      return -1;
    }
  }

  return 0;
}

/* =========================================================================
 * UNIT TESTS
 * ========================================================================= */

static void test_pm4_synthetic_valid_stream(void) {
  uint32_t stream[] = {
    /* SET_CONTEXT_REG: reg 0x200 (DB_DEPTH_CONTROL), count 1, val 0 */
    0xc0016900u, 0x200u, 0x00000000u,
    /* SET_UCONFIG_REG: reg 0x242 (VGT_PRIMITIVE_TYPE), val 4 (TRILIST) */
    0xc0017900u, 0x242u, 0x00000004u,
    /* NUM_INSTANCES: 1 */
    0xc0002f00u, 0x00000001u,
    /* DRAW_INDEX_AUTO: 3 vertices, initiator 2 */
    0xc0012d00u, 0x00000003u, 0x00000002u,
    /* RELEASE_MEM: EOP fence, 6 payload words, aligned addr */
    0xc0054900u, 0x00000000u, 0x00000000u, 0x00000000u, 0x10000000u, 0x00000000u, 0x00000001u,
  };

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(stream, sizeof(stream) / sizeof(stream[0]), &report);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.total_packets, 5u);
  ASSERT_EQ(report.set_context_reg_count, 1u);
  ASSERT_EQ(report.set_uconfig_reg_count, 1u);
  ASSERT_EQ(report.num_instances_count, 1u);
  ASSERT_EQ(report.draw_index_auto_count, 1u);
  ASSERT_EQ(report.release_mem_count, 1u);
  ASSERT_EQ(report.vgt_primitive_type, 4u);
  ASSERT_EQ(report.last_draw_index_count, 3u);
}

static void test_pm4_detects_truncated_buffer(void) {
  uint32_t truncated[] = {
    /* Header claims 4 words payload (count field 3), but only 2 follow */
    0xc0036900u, 0x200u, 0x00000000u
  };

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(truncated, sizeof(truncated) / sizeof(truncated[0]), &report);
  ASSERT_EQ(rc, -1);
  ASSERT_TRUE(report.error_count > 0);
  ASSERT_TRUE(strstr(report.last_error, "Truncated packet") != NULL);
}

static void test_pm4_detects_invalid_reg_bounds(void) {
  uint32_t bad_reg[] = {
    /* SET_CONTEXT_REG with base reg 0x405 (beyond 0x400 range) */
    0xc0016900u, 0x405u, 0x12345678u
  };

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(bad_reg, sizeof(bad_reg) / sizeof(bad_reg[0]), &report);
  ASSERT_EQ(rc, -1);
  ASSERT_TRUE(report.error_count > 0);
  ASSERT_TRUE(strstr(report.last_error, "Context reg out of range") != NULL);
}

static void test_pm4_detects_depth_invariants(void) {
  /* Test: Z_ENABLE is set (bit 1), but Z_INFO format is INVALID (0) */
  uint32_t invalid_depth[] = {
    /* SET_UCONFIG_REG: VGT_PRIMITIVE_TYPE */
    0xc0017900u, 0x242u, 0x00000004u,
    /* SET_CONTEXT_REG: DB_DEPTH_CONTROL = 0x16 (Z_ENABLE=1, Z_WRITE_ENABLE=1, ZFUNC=LESS) */
    0xc0016900u, 0x200u, 0x00000016u,
    /* SET_CONTEXT_REG: DB_Z_INFO = 0 (invalid format 0) */
    0xc0016900u, 0x010u, 0x00000000u,
    /* DRAW_INDEX_AUTO: 3 vertices */
    0xc0012d00u, 0x00000003u, 0x00000002u,
  };

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(invalid_depth, sizeof(invalid_depth) / sizeof(invalid_depth[0]), &report);
  ASSERT_EQ(rc, -1);
  ASSERT_TRUE(report.error_count > 0);
  ASSERT_TRUE(strstr(report.last_error, "Z_INFO format is INVALID") != NULL);
}

static void test_pm4_detects_zero_vertex_draw(void) {
  uint32_t zero_draw[] = {
    /* SET_UCONFIG_REG: VGT_PRIMITIVE_TYPE */
    0xc0017900u, 0x242u, 0x00000004u,
    /* DRAW_INDEX_AUTO: 0 vertices */
    0xc0012d00u, 0x00000000u, 0x00000002u,
  };

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(zero_draw, sizeof(zero_draw) / sizeof(zero_draw[0]), &report);
  ASSERT_EQ(rc, -1);
  ASSERT_TRUE(report.error_count > 0);
  ASSERT_TRUE(strstr(report.last_error, "0 indices") != NULL);
}

static void test_pm4_detects_unaligned_release_mem(void) {
  uint32_t unaligned_fence[] = {
    /* RELEASE_MEM with unaligned destination address 0x10000003 in addr_lo (payload[2]) */
    0xc0054900u, 0x00000000u, 0x00000000u, 0x10000003u, 0x00000000u, 0x00000000u, 0x00000001u,
  };

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(unaligned_fence, sizeof(unaligned_fence) / sizeof(unaligned_fence[0]), &report);
  ASSERT_EQ(rc, -1);
  ASSERT_TRUE(report.error_count > 0);
  ASSERT_TRUE(strstr(report.last_error, "unaligned") != NULL);
}

static void test_pm4_real_sdk_draw_stream(void) {
  uint32_t dcb_words[1024];
  __attribute__((aligned(8))) uint32_t fence_val = 0;
  struct oops_gpu_queue mock_queue = {
    .agc_queue_handle = (void *)0x12345678,
    .dcb_mem = (uint8_t *)dcb_words,
    .dcb_size = sizeof(dcb_words),
    .fence = &fence_val,
    .last_fence = 0,
  };

  uint32_t color_fb[64 * 64];
  uint32_t depth_fb[64 * 64];

  oops_agc_draw_desc_t desc;
  memset(&desc, 0, sizeof(desc));
  desc.color_buffer = color_fb;
  desc.width = 64;
  desc.height = 64;
  desc.depth_buffer = depth_fb;
  desc.depth_control = OOPS_AGC_DB_DEPTH_CONTROL(1, 1, OOPS_AGC_ZFUNC_LESS);
  desc.depth_format = OOPS_AGC_Z_32_FLOAT;
  desc.vertex_count = 3;
  /* Generate full SDK hardware draw stream */
  memset(dcb_words, 0, sizeof(dcb_words));
  (void)oops_agc_draw_primitive(&mock_queue, &desc);

  /* Determine exact words written into DCB */
  size_t words_written = 0;
  for (size_t i = 1024; i > 0; i--) {
    if (dcb_words[i - 1] != 0) {
      words_written = i;
      break;
    }
  }

  /* Validate the full generated DCB buffer */
  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(dcb_words, words_written, &report);
  if (rc != 0) {
    printf("\n[PM4 Validator Failure]: %s\n", report.last_error);
  }
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_TRUE(report.total_packets > 100u);
  ASSERT_EQ(report.draw_index_auto_count, 1u);
  ASSERT_EQ(report.release_mem_count, 1u);
  ASSERT_EQ(report.nop_count, 16u);
  ASSERT_EQ(report.vgt_primitive_type, OOPS_AGC_PRIM_TRILIST);
  ASSERT_EQ(report.last_draw_index_count, 3u);
  ASSERT_TRUE((report.db_depth_control & 0x02u) != 0); /* Z_ENABLE */
  ASSERT_EQ(report.db_z_info & 0x3u, OOPS_AGC_Z_32_FLOAT);
}

static void test_pm4_gl_hardware_depth_stream(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  ASSERT_TRUE(disp != NULL);

  void *ctx_handle = glContextCreate(disp);
  ASSERT_TRUE(ctx_handle != NULL);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  uint32_t dcb[4096];
  uint8_t payload[0x4000];
  uint8_t vbo[65536];
  /* Sized to what gl_hw_flush writes, not to one word each: the fence at [0] and the GPU clock
   * at [2..3], and eight canary words reset before every submission. As a single uint32_t each,
   * this fixture was written 12 and 28 bytes past its ends by every flush - on the stack - which
   * AddressSanitizer found on 2026-09-19. */
  __attribute__((aligned(8))) uint32_t fence[4] = {0x11111111u};
  __attribute__((aligned(8))) uint32_t canary[16] = {0xaaaaaaaau};

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));

  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* Step 1: Draw triangle with default state (Depth test disabled) */
  glBegin(GL_TRIANGLES);
  glColor3f(1.0f, 0.0f, 0.0f);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Step 1 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.db_depth_control & 0x02u, 0u); /* Z_ENABLE should be 0 */
  ASSERT_EQ(report.draw_index_auto_count, 1u);

  /* Step 2: Enable GL_DEPTH_TEST with GL_LEQUAL and depth mask TRUE */
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);

  glBegin(GL_TRIANGLES);
  glColor3f(0.0f, 1.0f, 0.0f);
  glVertex3f(-0.5f, -0.5f, 0.3f);
  glVertex3f(0.5f, -0.5f, 0.3f);
  glVertex3f(0.0f, 0.5f, 0.3f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Step 2 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_TRUE((report.db_depth_control & 0x02u) != 0); /* Z_ENABLE should be 1 */
  ASSERT_TRUE((report.db_depth_control & 0x04u) != 0); /* Z_WRITE_ENABLE should be 1 */
  ASSERT_EQ((report.db_depth_control >> 4) & 0x7u, OOPS_AGC_ZFUNC_LEQUAL); /* ZFUNC == 3 */
  ASSERT_EQ(report.db_z_info & 0x3u, OOPS_AGC_Z_32_FLOAT);
  ASSERT_EQ(report.draw_index_auto_count, 2u);

  /* Step 3: Change to GL_GREATER and depth mask FALSE */
  glDepthFunc(GL_GREATER);
  glDepthMask(GL_FALSE);

  glBegin(GL_TRIANGLES);
  glColor3f(0.0f, 0.0f, 1.0f);
  glVertex3f(-0.5f, -0.5f, 0.1f);
  glVertex3f(0.5f, -0.5f, 0.1f);
  glVertex3f(0.0f, 0.5f, 0.1f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Step 3 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_TRUE((report.db_depth_control & 0x02u) != 0); /* Z_ENABLE should be 1 */
  ASSERT_EQ(report.db_depth_control & 0x04u, 0u);      /* Z_WRITE_ENABLE should be 0 */
  ASSERT_EQ((report.db_depth_control >> 4) & 0x7u, OOPS_AGC_ZFUNC_GREATER); /* ZFUNC == 4 */
  ASSERT_EQ(report.draw_index_auto_count, 3u);

  /* Step 4: Disable GL_DEPTH_TEST dynamically */
  glDisable(GL_DEPTH_TEST);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Step 4 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.db_depth_control & 0x02u, 0u); /* Z_ENABLE should be 0 */
  ASSERT_EQ(report.draw_index_auto_count, 4u);

  /* Step 5: Flush frame and verify RELEASE_MEM packet */
  uint32_t words_before_flush = ctx->dcb_words;
  gl_hw_flush(ctx);
  uint32_t total_flushed_words = words_before_flush + 32; /* two RELEASE_MEM (fence, GPU clock) + 16 NOP pads */
  rc = oops_pm4_validate_stream(ctx->dcb_mem, total_flushed_words, &report);
  if (rc != 0) printf("\n[PM4 Step 5 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.release_mem_count, 2u); /* the fence and the GPU clock */
  ASSERT_EQ(report.draw_index_auto_count, 4u);

  /* Clean up mock pointers */
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* A mipmapped texture reaches the hardware as a chain, laid out as addrlib lays out a linear
 * GFX10 surface, with the descriptor fields radeonsi fills for one.
 *
 * The layout is the part with no second chance: smallest level first and the base level last
 * (gfx10addrlib.cpp:5082-5104), each level's rows padded to 64 texels. For a 4x4 texture that is
 * level 2 (1x1) at byte 0, level 1 (2x2) at 256, level 0 (4x4) at 256 + 512 = 768, 1792 bytes in
 * all - checked here byte by byte, because the sampler finds each level by that arithmetic and
 * reads whatever is there.
 */
/*
 * **The two targets that sample with three coordinates**, on the hardware path (2026-09-20).
 * Both were drawn untextured here until that day, and this test was what said so.
 *
 * A volume is sampled with `dim:SQ_RSRC_IMG_3D`, its r interpolated from the third parameter and
 * divided by q; a cube map with `dim:SQ_RSRC_IMG_CUBE`, its face found from the direction. Both
 * therefore export at least three parameters, and both leave the sample slot holding their own
 * form until a draw with another target resets it - which the 2D draw at the end checks.
 *
 * What a volume still has no line for is the mip chain: none is built for it, so a minifying
 * filter reads the base level and the log says so once. The volume here filters GL_NEAREST, so
 * nothing is logged.
 */
static void test_pm4_gl_volume_and_cube_sample_on_hardware(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  static const GLubyte vol[2 * 2 * 2 * 4] = {0};
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_3D, t);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  glEnable(GL_TEXTURE_3D);
  ASSERT_EQ(ctx->hw_3d_logged, GL_FALSE);
  glBegin(GL_TRIANGLES);
  glTexCoord3f(0.5f, 0.5f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  /* GL_NEAREST reads no mip chain, so the one line a volume can earn is not earned. */
  ASSERT_EQ(ctx->hw_3d_logged, GL_FALSE);
  ASSERT_EQ(ctx->hw_frame_tex, t);
  uint32_t last_ps_lo = 0u;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0017600u && dcb[i + 1] == 0x08u) last_ps_lo = dcb[i + 2];
  }
  ASSERT_EQ(last_ps_lo, (uint32_t)(((uint64_t)(uintptr_t)payload + OOPS_GL_PS_TEX_OFFSET) >> 8));
  {
    /* The sample slot holds the volume form, and the draw exports the third parameter r rides
     * in. The descriptor's TYPE is 0xa and WORD4 is the last slice - one, for two slices. */
    const uint32_t *const ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX], 0xc8480b00u); /* v_interp_p1_f32 v18, attr2.w */
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 2u], 0x10241912u); /* v_mul_f32 v18, v18, v12 */
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 5u], 0xf0800f10u); /* image_sample ... dim:3D */
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 6u], 0x00610410u); /* v[16:18], dest v4 */
    ASSERT_TRUE(ctx->hw_params >= 3u);
    const gl_texture_object_t *vol_obj = gl_lookup_texture(ctx, t);
    ASSERT_TRUE(vol_obj != NULL);
    ASSERT_EQ(vol_obj->img_desc[3] >> 28, 0xau);
    ASSERT_EQ(vol_obj->img_desc[4], 1u);
  }
  /* **A mipmapped volume gets a chain** (since 2026-09-21). This block asserted the opposite
   * until then - that a volume earned the "no mip chain" line - because the chain layout was two
   * dimensional and `gl_tex_chain_levels` refused a volume outright. It halves depth with width
   * and height now, and the copy walks slices.
   *
   * `hw_failed` is cleared before each draw from here on, as the LOD-bias test does and for the
   * same reason: a change to the descriptors submits the frame, and this fence is not a real
   * one, so every submission records a failure that would silently drop the next draw. */
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
  glTexImage3D(GL_TEXTURE_3D, 1, GL_RGBA, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, vol);
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glTexCoord3f(0.5f, 0.5f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  /* **The line is earned, because no chain is built for a volume** - and since 2026-09-21 that
   * is a measured refusal rather than a structural one. The layout can describe a volume now
   * (`gl_tex_chain_layout_3d`, asserted below), and the console read the base level anyway:
   * `volume-mipmap` answered `saw 0xffff0000`, red, level 0, with `LAST_LEVEL` 1 in the
   * descriptor. `REQ-20260921T1300Z-9b73` asks where a level sits inside a 3D image. */
  ASSERT_EQ(ctx->hw_3d_logged, GL_TRUE);
  {
    const gl_texture_object_t *v2 = gl_lookup_texture(ctx, t);
    ASSERT_TRUE(v2 != NULL);
    ASSERT_EQ(v2->desc_chain, GL_FALSE);
    ASSERT_EQ(v2->img_desc[3] >> 28, 0xau); /* still a volume */
    ASSERT_EQ(v2->img_desc[4], 1u);         /* two slices */
    ASSERT_EQ((v2->img_desc[3] >> 16) & 0xfu, 0u); /* LAST_LEVEL, the base level alone */
    /* **The layout is ready for the answer.** Smallest first, each level's slices one after
     * another at that level's own `pitch * height`: a 2x2x2 base puts level 1 - one texel,
     * padded to a 64-texel row, one slice deep - at offset 0, and level 0's two slices after
     * it. A 2D chain is this with the depth term at one, which is why the old two-dimensional
     * form is now a wrapper and its chains are byte for byte what they were. */
    size_t offs[OOPS_GL_MAX_TEXTURE_LEVELS];
    uint32_t pitch[OOPS_GL_MAX_TEXTURE_LEVELS];
    const size_t total = gl_tex_chain_layout_3d(2, 2, 2, 2, offs, pitch);
    ASSERT_EQ(offs[1], 0u);
    ASSERT_EQ(offs[0], 64u * 1u * 1u * 4u);
    ASSERT_EQ(total, 64u * 1u * 1u * 4u + 64u * 2u * 2u * 4u);
    size_t offs2[OOPS_GL_MAX_TEXTURE_LEVELS];
    uint32_t pitch2[OOPS_GL_MAX_TEXTURE_LEVELS];
    ASSERT_EQ(gl_tex_chain_layout_3d(4, 4, 1, 2, offs2, pitch2),
              gl_tex_chain_layout(4, 4, 2, offs, pitch));
    ASSERT_EQ(offs2[0], offs[0]);
    ASSERT_EQ(offs2[1], offs[1]);
  }

  /* A cube map next. */
  glDisable(GL_TEXTURE_3D);
  static const GLubyte one[4] = {255, 255, 255, 255};
  GLuint cube = 0;
  glGenTextures(1, &cube);
  glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  for (int f = 0; f < 6; f++) {
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, one);
  }
  /* **A complete cube map is sampled here since 2026-09-20**: the six faces as one array, the
   * face found from the direction in the shader's sample slot. So this draw is textured, the
   * shader is the textured one, and nothing is logged about a unit being dropped. */
  glEnable(GL_TEXTURE_CUBE_MAP);
  ASSERT_EQ(gl_effective_texture_id(ctx), cube);
  ASSERT_EQ(ctx->hw_cube_logged, GL_FALSE);
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glTexCoord3f(1.0f, 0.0f, 0.0f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  /* **Three, not one** (since 2026-09-21). Binding a second texture in a frame used to submit
   * the draws built against the first one's descriptors, which reset the count; the ring gives
   * the new texture its own slot and the stream carries on, so the volume's two triangles and
   * the cube's one are all in it. That is the change, stated where it is visible. */
  ASSERT_EQ(ctx->triangles_drawn, 3u);
  ASSERT_TRUE(ctx->hw_desc_slot > 0u);
  ASSERT_EQ(ctx->hw_cube_logged, GL_FALSE);
  last_ps_lo = 0u;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0017600u && dcb[i + 1] == 0x08u) last_ps_lo = dcb[i + 2];
  }
  ASSERT_EQ(last_ps_lo, (uint32_t)(((uint64_t)(uintptr_t)payload + OOPS_GL_PS_TEX_OFFSET) >> 8));
  {
    /* The sample slot holds the cube form, and the draw exports the third parameter its
     * direction's r rides in. */
    const uint32_t *const ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX], 0xc8400400u); /* v_interp_p1_f32 v16, attr1.x */
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 22u], 0xf0800f18u); /* image_sample ... dim:CUBE */
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 23u], 0x00610410u); /* v[16:18], dest v4 */
    ASSERT_TRUE(ctx->hw_params >= 3u);
  }
  glDisable(GL_TEXTURE_CUBE_MAP);
  glDeleteTextures(1, &cube);
  /* **And a 2D draw after it stops looking for a face.** It has to be a *textured* draw: an
   * untextured one runs the untextured shader, which has no sample slot, so what the textured
   * shader holds does not matter until a texture is bound again - and then it is set. */
  {
    GLuint flat = 0;
    glGenTextures(1, &flat);
    glBindTexture(GL_TEXTURE_2D, flat);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
    glEnable(GL_TEXTURE_2D);
    ctx->hw_failed = GL_FALSE;
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();
    const uint32_t *const ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX], 0xf0800f08u); /* the 2D sample again */
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &flat);
  }

  glDisable(GL_TEXTURE_3D);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glDeleteTextures(1, &t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **A smooth point and a smooth line on the hardware path** (2026-09-20).
 *
 * The coverage slot in the untextured pixel shader is half of it; the other half is the CPU
 * putting the right numbers in the vertex, and that is the half a shader test would miss. So
 * this checks both: the slot holds the assembled words, and the widened quad's corners carry
 * their offset from the centre in the texture-coordinate parameter with the radius plus a half
 * beside it - which is the constant the shader subtracts the distance from.
 *
 * Then the two cases that must stay aliased: a textured smooth point, whose coverage has nowhere
 * to ride, and a plain triangle after a smooth one, which must stop weighing its alpha by a
 * texture coordinate.
 */
static void test_pm4_gl_smooth_points_and_lines_carry_their_coverage(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[8192];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* The payload here is the test's own and was never built into, so the slot's *initial* state
   * is not what a context creates - the "off" checks below are after a draw has set it, which is
   * the state that matters anyway. */
  const uint32_t *const ps = (const uint32_t *)(payload + OOPS_GL_PS_UNTEX_OFFSET);

  glPointSize(5.0f);
  glEnable(GL_POINT_SMOOTH);
  glBegin(GL_POINTS);
  glVertex3f(0.0f, 0.0f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->triangles_drawn, 2u); /* a point is a quad */
  ASSERT_EQ(ctx->hw_smooth_logged, GL_FALSE);
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX], 0xc8300400u);      /* v_interp_p1_f32 v12, attr1.x */
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX + 12u], 0x7c021880u); /* v_cmp_lt_f32 vcc_lo, 0, v12 */
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX + 13u], 0x877e6a7eu); /* s_and_b32 exec_lo, ... */
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX + 14u], 0x100e1907u); /* v_mul_f32 v7, v7, v12 */
  {
    /* **The vertex carries the geometry.** Each corner is 48 bytes - position, colour, the
     * texture parameter - and the parameter's x and y are the corner's offset from the centre
     * in pixels while its w is the radius plus a half. A point of size 5 widened by a pixel
     * either side gives +-3.5, and 3.0 as the constant. */
    const float *v0 = (const float *)(vbo + 32);
    ASSERT_FLOAT_NEAR(v0[0], -3.5f, 1e-4f);
    ASSERT_FLOAT_NEAR(v0[1], -3.5f, 1e-4f);
    ASSERT_FLOAT_NEAR(v0[3], 3.0f, 1e-4f);
    const float *v1 = (const float *)(vbo + 48 + 32);
    ASSERT_FLOAT_NEAR(v1[0], 3.5f, 1e-4f);
    ASSERT_FLOAT_NEAR(v1[1], -3.5f, 1e-4f);
    ASSERT_FLOAT_NEAR(v1[3], 3.0f, 1e-4f);
  }
  glDisable(GL_POINT_SMOOTH);

  /* **A plain triangle after it turns the slot off again.** */
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX] & 0xffff0000u, 0xbf820000u);

  /* **A smooth line**: the two long edges of the quad carry +-(half-width + 1) across and zero
   * along, which is what makes one shader form serve both kinds. */
  ctx->hw_vbo_cursor = 0;
  ctx->hw_failed = GL_FALSE;
  memset(vbo, 0, sizeof(vbo));
  glLineWidth(3.0f);
  glEnable(GL_LINE_SMOOTH);
  glBegin(GL_LINES);
  glVertex3f(-0.5f, 0.0f, 0.5f); glVertex3f(0.5f, 0.0f, 0.5f);
  glEnd();
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX], 0xc8300400u);
  {
    const float *v0 = (const float *)(vbo + 32);
    ASSERT_FLOAT_NEAR(v0[1], 0.0f, 1e-4f); /* nothing along - one distance decides a line */
    ASSERT_FLOAT_NEAR(v0[3], 2.0f, 1e-4f); /* 3/2 + 1/2 */
    const float across = v0[0] < 0.0f ? -v0[0] : v0[0];
    ASSERT_FLOAT_NEAR(across, 2.5f, 1e-4f); /* 3/2 + 1, the widened quad's edge */
  }
  glDisable(GL_LINE_SMOOTH);

  /* **A textured smooth point smooths too** (since 2026-09-21), in the *other* shader's slot and
   * from the *other* interpolant. This block asserted the opposite until then - that such a draw
   * was aliased and said so once - because the offset rode in the texture coordinate and a
   * textured draw reads all four of its components. It now rides in the second texture unit's
   * parameter, which a one-unit draw has spare, and the draw escalates to four parameters for
   * it: eighty bytes a vertex, the fourth vec4 at offset 64. */
  {
    static const GLubyte one[4] = {255, 255, 255, 255};
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
    glEnable(GL_TEXTURE_2D);
    glPointSize(3.0f);
    glEnable(GL_POINT_SMOOTH);
    ctx->hw_failed = GL_FALSE;
    ctx->hw_vbo_cursor = 0;
    memset(vbo, 0, sizeof(vbo));
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.5f);
    glEnd();
    /* Nothing was refused, so nothing was logged. */
    ASSERT_EQ(ctx->hw_smooth_logged, GL_FALSE);
    /* The textured shader carries the coverage, reading attr3; the untextured one branches over
     * its own slot, because a draw weighs its alpha in one shader or neither. */
    const uint32_t *const ps_tex =
        (const uint32_t *)((const char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    ASSERT_EQ(ps_tex[GL_PS_COVERAGE_SLOT_TEX], 0xc8300c00u);       /* v_interp_p1 v12, attr3.x */
    ASSERT_EQ(ps_tex[GL_PS_COVERAGE_SLOT_TEX + 4u], 0xc8380f00u);  /* v_interp_p1 v14, attr3.w */
    ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX] & 0xffff0000u, 0xbf820000u);
    /* Four parameters, so the vertex is eighty bytes and the offset is the fourth vec4's x, y
     * and w. A point of size 3 gives a radius of 3/2 + 1/2 and corners at 3/2 + 1. */
    {
      const float *p3 = (const float *)(vbo + 64);
      const float across = p3[0] < 0.0f ? -p3[0] : p3[0];
      const float along = p3[1] < 0.0f ? -p3[1] : p3[1];
      ASSERT_FLOAT_NEAR(across, 2.5f, 1e-4f);
      ASSERT_FLOAT_NEAR(along, 2.5f, 1e-4f);
      ASSERT_FLOAT_NEAR(p3[3], 2.0f, 1e-4f);
    }
    /* **Two units is the case that is still aliased**, and the one that logs: the fourth
     * parameter is the second texture's coordinate then, and only its z is spare - one float
     * where three are needed. */
    {
      GLuint t1 = 0;
      glGenTextures(1, &t1);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, t1);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
      glEnable(GL_TEXTURE_2D);
      glActiveTexture(GL_TEXTURE0);
      const GLboolean saved_mt = ctx->hw_multitex;
      ctx->hw_multitex = GL_TRUE;
      ctx->hw_failed = GL_FALSE;
      glBegin(GL_POINTS);
      glVertex3f(0.0f, 0.0f, 0.5f);
      glEnd();
      ASSERT_EQ(ctx->hw_smooth_logged, GL_TRUE);
      ASSERT_EQ(ps_tex[GL_PS_COVERAGE_SLOT_TEX] & 0xffff0000u, 0xbf820000u);
      ctx->hw_multitex = saved_mt;
      glActiveTexture(GL_TEXTURE1);
      glDisable(GL_TEXTURE_2D);
      glActiveTexture(GL_TEXTURE0);
      glDeleteTextures(1, &t1);
    }
    glPointSize(1.0f);
    glDisable(GL_POINT_SMOOTH);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &t);
  }

  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **An occlusion query counted by the GPU** (2026-09-20). Four things have to be in the stream
 * and one must not be:
 *
 *  - `DB_COUNT_CONTROL` raised to `0x11000106` - the plain recipe plus `PERFECT_ZPASS_COUNTS`
 *    and `DISABLE_CONSERVATIVE_ZPASS_COUNTS` - **after** the depth surface is bound, never
 *    before, because ZPASS_ENABLE with no depth target stalls the depth block (`-e3a7`);
 *  - a `ZPASS_DONE` `EVENT_WRITE` (`0xc0024600`, `0x00000115`) aimed at the slot base;
 *  - a second one at the base plus eight, which is where each backend's end count goes;
 *  - `DB_COUNT_CONTROL` put back to `0x11000100`, so the next frame is the stream it always was.
 *
 * And the sum: sixteen slots, `end - begin` each, bit 63 masked off as the hardware's valid
 * marker. The slots are filled here the way `-4d19` measured them - most backends carrying equal
 * work and the last two none - so a sum that only read slot 0, or that took bit 63 for count,
 * comes out wrong rather than plausible.
 */
static void test_pm4_gl_occlusion_query_counts_on_the_gpu(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  const uint64_t base = (uint64_t)(uintptr_t)payload + OOPS_GL_ZPASS_OFFSET;
  GLuint q = 0;
  glGenQueries(1, &q);

  /* GL_QUERY_COUNTER_BITS was 0 on this path and is 32 now, which is what tells a program the
   * count means something (GL 1.5, 4.1.6). */
  GLint bits = -1;
  glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS, &bits);
  ASSERT_EQ(bits, 32);

  glEnable(GL_DEPTH_TEST);
  glBeginQuery(GL_SAMPLES_PASSED, q);
  ASSERT_EQ(ctx->hw_query_counting, GL_FALSE); /* nothing until a depth surface is bound */
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->hw_query_counting, GL_TRUE);

  /* The raise comes after the depth block, which is what DB_Z_INFO (0x010) marks. */
  size_t at_zinfo = 0, at_raise = 0, at_begin = 0;
  for (size_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x010u) at_zinfo = i;
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x001u && dcb[i + 2] == 0x11000106u) at_raise = i;
    if (dcb[i] == 0xc0024600u && dcb[i + 1] == 0x00000115u &&
        dcb[i + 2] == (uint32_t)base && dcb[i + 3] == (uint32_t)(base >> 32)) {
      at_begin = i;
    }
  }
  ASSERT_TRUE(at_zinfo != 0);
  ASSERT_TRUE(at_raise != 0);
  ASSERT_TRUE(at_begin != 0);
  ASSERT_TRUE(at_raise > at_zinfo);
  ASSERT_TRUE(at_begin > at_raise);

  /* The counters as `-4d19` saw them: thirteen backends with equal work, one with half of it,
   * two with none, and bit 63 set on every slot at both ends. 13 * 0x20 + 0x10 = 0x1b0. */
  {
    uint32_t *slots = (uint32_t *)(payload + OOPS_GL_ZPASS_OFFSET);
    for (unsigned i = 0; i < OOPS_GL_ZPASS_BACKENDS; i++) {
      const unsigned w = i * (OOPS_GL_ZPASS_STRIDE / 4u);
      const uint32_t diff = (i < 13u) ? 0x20u : (i == 13u ? 0x10u : 0u);
      slots[w] = 0x100u;             /* begin, low */
      slots[w + 1] = 0x80000000u;    /* begin, high: bit 63 only */
      slots[w + 2] = 0x100u + diff;  /* end, low */
      slots[w + 3] = 0x80000000u;    /* end, high */
    }
  }

  ctx->hw_failed = GL_FALSE;
  glEndQuery(GL_SAMPLES_PASSED);
  GLuint result = 0;
  glGetQueryObjectuiv(q, GL_QUERY_RESULT, &result);
  ASSERT_EQ(result, 13u * 0x20u + 0x10u);

  /* The closing event and the restore. `dcb_words` is back to zero after the submit, so the
   * words themselves are what is read - the array was cleared before the frame. */
  size_t at_end = 0, at_restore = 0;
  for (size_t i = 0; i + 3 < 4096u; i++) {
    if (dcb[i] == 0xc0024600u && dcb[i + 1] == 0x00000115u &&
        dcb[i + 2] == (uint32_t)(base + 8u) && dcb[i + 3] == (uint32_t)((base + 8u) >> 32)) {
      at_end = i;
    }
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x001u && dcb[i + 2] == 0x11000100u &&
        i > at_raise) {
      at_restore = i;
    }
  }
  ASSERT_TRUE(at_end != 0);
  ASSERT_TRUE(at_restore > at_end);
  ASSERT_EQ(ctx->hw_query_counting, GL_FALSE);

  /* **A query whose draws never test depth does not count**, and says so once. Nothing new is
   * aimed at the slot base, which is the packet that would have hung the GPU. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  ctx->hw_failed = GL_FALSE;
  glDisable(GL_DEPTH_TEST);
  GLuint q2 = 0;
  glGenQueries(1, &q2);
  ASSERT_EQ(ctx->hw_query_logged, GL_FALSE);
  glBeginQuery(GL_SAMPLES_PASSED, q2);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  glEndQuery(GL_SAMPLES_PASSED);
  ASSERT_EQ(ctx->hw_query_logged, GL_TRUE);
  ASSERT_EQ(ctx->hw_query_counting, GL_FALSE);
  for (size_t i = 0; i + 1 < 4096u; i++) {
    ASSERT_TRUE(!(dcb[i] == 0xc0024600u && dcb[i + 1] == 0x00000115u));
  }

  glDeleteQueries(1, &q);
  glDeleteQueries(1, &q2);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **A depth texture on the hardware path** (2026-09-20), with GL 1.4's comparison and without.
 *
 * Three things have to agree for one to sample at all, and this checks each: the image format is
 * `32_FLOAT` (22) rather than `8_8_8_8_UNORM` (56), because a depth texel is one float; the
 * sampler's DEPTH_COMPARE_FUNC carries GL_TEXTURE_COMPARE_FUNC; and the sample slot asks for one
 * channel and spreads it across v4..v7 the way GL_DEPTH_TEXTURE_MODE says.
 *
 * The spread is the part worth pinning per mode, because the three forms differ in which
 * register gets what - and GL_ALPHA's has to write v7 before it zeroes v4, which is the kind of
 * ordering a reader cannot check by eye.
 */
static void test_pm4_gl_depth_texture_samples_and_compares_on_hardware(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  const uint32_t *const ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
  static const GLfloat dtex[2] = {0.25f, 0.75f};
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 2, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
               dtex);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glEnable(GL_TEXTURE_2D);

  /* **No comparison**: the stored depth itself, one channel, luminance - (v, v, v, 1). */
  glBegin(GL_TRIANGLES);
  glTexCoord3f(0.5f, 0.5f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->hw_frame_tex, t);
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX], 0xf0800108u);      /* image_sample ... dmask:0x1 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 1u], 0x00610402u); /* v[2:3], dest v4 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 2u], 0xbf8c3f70u); /* s_waitcnt vmcnt(0) */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 3u], 0x7e0a0304u); /* v_mov_b32 v5, v4 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 4u], 0x7e0c0304u); /* v_mov_b32 v6, v4 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 5u], 0x7e0e02f2u); /* v_mov_b32 v7, 1.0 */
  {
    const gl_texture_object_t *obj = gl_lookup_texture(ctx, t);
    ASSERT_TRUE(obj != NULL);
    ASSERT_EQ((obj->img_desc[1] >> 20) & 0x1ffu, 22u); /* GFX10_FORMAT_32_FLOAT */
    ASSERT_EQ((obj->samp_desc[0] >> 12) & 7u, 0u);     /* no comparison yet */
  }

  /* **The comparison**, GL_LEQUAL - the sampler's function 3, and the shader's reference. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glTexCoord3f(0.5f, 0.5f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX], 0xc8400b00u);      /* v_interp_p1_f32 v16, attr2.w */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 2u], 0x10201910u); /* v_mul_f32 v16, v16, v12 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 3u], 0x20202080u); /* v_max_f32 v16, 0, v16 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 4u], 0x1e2020f2u); /* v_min_f32 v16, 1.0, v16 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 7u], 0xf0a00108u); /* image_sample_c ... dmask:0x1 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 8u], 0x00610410u); /* v[16:18], dest v4 */
  /* The reference rides in the third parameter, so the draw exports one. */
  ASSERT_TRUE(ctx->hw_params >= 3u);
  {
    const gl_texture_object_t *obj = gl_lookup_texture(ctx, t);
    ASSERT_EQ((obj->samp_desc[0] >> 12) & 7u, 3u); /* DEPTH_COMPARE_FUNC = LEQUAL */
  }

  /* **GL_ALPHA**: (0, 0, 0, v), and v7 is written before v4 is zeroed. */
  glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_ALPHA);
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glTexCoord3f(0.5f, 0.5f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 10u], 0x7e0e0304u); /* v_mov_b32 v7, v4 - first */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 11u], 0x7e080280u); /* v_mov_b32 v4, 0 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 12u], 0x7e0a0280u); /* v_mov_b32 v5, 0 */
  ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX + 13u], 0x7e0c0280u); /* v_mov_b32 v6, 0 */

  /* **And a colour texture after it** goes back to the four-channel sample and to RGBA8 - the
   * reason every slot is set on every draw. */
  {
    static const GLubyte one[4] = {255, 255, 255, 255};
    GLuint flat = 0;
    glGenTextures(1, &flat);
    glBindTexture(GL_TEXTURE_2D, flat);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, one);
    ctx->hw_failed = GL_FALSE;
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();
    ASSERT_EQ(ps_tex[GL_PS_SAMPLE_SLOT_TEX], 0xf0800f08u); /* dmask:0xf again */
    const gl_texture_object_t *obj = gl_lookup_texture(ctx, flat);
    ASSERT_EQ((obj->img_desc[1] >> 20) & 0x1ffu, 56u); /* GFX10_FORMAT_8_8_8_8_UNORM */
    glDeleteTextures(1, &flat);
  }

  glDisable(GL_TEXTURE_2D);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glDeleteTextures(1, &t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static uint32_t last_context_reg(const uint32_t *w, size_t n, uint32_t offset);
static uint32_t last_sh_reg(const uint32_t *w, size_t n, uint32_t offset);

/* Wrap modes and the border colour as the sampler descriptor carries them - CLAMP_X/Y from
 * radeonsi's si_tex_wrap, BORDER_COLOR_TYPE from si_translate_border_color - and a border colour
 * none of the three built-in ones reaching the table TA_BC_BASE_ADDR points at. */
static void test_pm4_gl_texture_wrap_and_border_reach_the_sampler(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  static const GLubyte texel[4] = {255, 255, 255, 255};
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl_texture_object_t *obj = NULL;
  for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
    if (ctx->textures[i].used && ctx->textures[i].id == t) obj = &ctx->textures[i];
  }
  ASSERT_TRUE(obj != NULL);

  /* CLAMP_X [0,2], CLAMP_Y [3,5]: WRAP 0, MIRROR 1, CLAMP_LAST_TEXEL 2, CLAMP_HALF_BORDER 4,
   * CLAMP_BORDER 6 (gfx8.json:646-652). */
  const struct { GLenum mode; uint32_t hw; } wraps[] = {
    {GL_REPEAT, 0u}, {GL_MIRRORED_REPEAT, 1u}, {GL_CLAMP_TO_EDGE, 2u}, {GL_CLAMP, 4u},
    {GL_CLAMP_TO_BORDER, 6u},
  };
  for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wraps[i].mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wraps[i].mode);
    ASSERT_EQ(obj->samp_desc[0] & 0x3fu, wraps[i].hw | (wraps[i].hw << 3));
  }
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* BORDER_COLOR_TYPE [30,31]: the built-in colours need no table. GL_CLAMP under GL_NEAREST
   * never reads the border, so it stays transparent black whatever the colour. */
  const GLfloat opaque_black[4] = {0.0f, 0.0f, 0.0f, 1.0f}, white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const GLfloat blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_EQ(obj->samp_desc[3] >> 30, 0u); /* (0, 0, 0, 0) */
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, opaque_black);
  ASSERT_EQ(obj->samp_desc[3] >> 30, 1u);
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, white);
  ASSERT_EQ(obj->samp_desc[3] >> 30, 2u);
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, blue);
  ASSERT_EQ(obj->samp_desc[3], 3u << 30); /* the table, entry 0 */
  ASSERT_TRUE(obj->border_in_table);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  ASSERT_EQ(obj->samp_desc[3] >> 30, 0u);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  ASSERT_EQ(obj->samp_desc[3] >> 30, 3u);

  /* A draw copies the colour into the table and the frame points TA_BC_BASE_ADDR at it. */
  glEnable(GL_TEXTURE_2D);
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.5f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  const float *table = (const float *)(payload + OOPS_GL_BORDER_TABLE_OFFSET);
  ASSERT_TRUE(table[0] == 0.0f && table[1] == 0.0f && table[2] == 1.0f && table[3] == 1.0f);
  const uint64_t bc = (uint64_t)(uintptr_t)payload + OOPS_GL_BORDER_TABLE_OFFSET;
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x020u), (uint32_t)(bc >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x021u), (uint32_t)(bc >> 40) & 0xffu);

  /* GL 1.4's level-of-detail bias joins SQ_IMG_SAMP_WORD2's LOD_BIAS [0,13] at the draw - the
   * texture's 1.5 and the unit's 0.5 as 2.0 in signed fixed point with eight fraction bits,
   * 0x200 - while the texture's own descriptor stays without it. Then -1.0, 0x3f00. A change in
   * what the descriptors hold takes the next ring slot (since 2026-09-21), so each draw's bias
   * is read from the slot that draw was handed rather than from a fixed table. `hw_failed` is
   * still cleared between draws, for the submissions the ring does not remove. */
#define SLOT_NOW ((const uint32_t *)(payload + gl_hw_desc_slot_offset(ctx->hw_desc_slot)))
  const uint32_t *slot = SLOT_NOW;
  ASSERT_EQ(slot[10] & 0x3fffu, 0u);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 1.5f);
  glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 0.5f);
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(SLOT_NOW[10] & 0x3fffu, 0x200u);
  ASSERT_EQ(obj->samp_desc[2] & 0x3fffu, 0u);
  glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, -2.5f);
  ctx->hw_failed = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(SLOT_NOW[10] & 0x3fffu, 0x3f00u);
#undef SLOT_NOW
  glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, 0.0f);

  glDisable(GL_TEXTURE_2D);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glDeleteTextures(1, &t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* A clear that keeps to a scissor box or goes through a colour mask is drawn: two triangles on
 * the frame, through CB_TARGET_MASK and the scissor registers the draw path already emits. A
 * DMA fill of the whole allocation - what the unscissored, unmasked clear still is on the
 * hardware - could do neither. */
static void test_pm4_gl_scissored_clear_is_drawn(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  glEnable(GL_SCISSOR_TEST);
  glScissor(10, 10, 20, 20);
  glClear(GL_COLOR_BUFFER_BIT);
  ASSERT_EQ(ctx->triangles_drawn, 2u);
  glDisable(GL_SCISSOR_TEST);

  /* Masked: red off. The last CB_TARGET_MASK write in the stream is the clear's, and has the
   * red bit of target 0 clear. */
  glColorMask(GL_FALSE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  ASSERT_EQ(ctx->triangles_drawn, 4u);
  uint32_t last_mask = 0xffffffffu;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x08eu) last_mask = dcb[i + 2];
  }
  ASSERT_EQ(last_mask & 0xfu, 0xeu);

  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* A linked GL 2.0 program, for the two tests below. `test_gl2.c` has a fuller version of this;
 * here it is three calls and the check that they worked, because a program that failed to link
 * would draw the fixed-function path and the register assertions would be about nothing. */
static GLuint pm4_linked_program(const char *vs_src, const char *fs_src) {
  const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  const GLuint prog = glCreateProgram();
  GLint linked = 0;
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[256] = {0};
    glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
    printf("\n    link failed: %s\n", log);
  }
  ASSERT_EQ(linked, GL_TRUE);
  return prog;
}

/* A quad through a program's own `pos` attribute. */
static void pm4_draw_quad(GLuint prog) {
  const GLint loc = glGetAttribLocation(prog, "pos");
  const float xs[6] = {-0.8f, 0.8f, 0.8f, -0.8f, 0.8f, -0.8f};
  const float ys[6] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.8f, 0.8f};
  ASSERT_TRUE(loc >= 0);
  glBegin(GL_TRIANGLES);
  for (int i = 0; i < 6; i++) {
    glVertexAttrib3f((GLuint)loc, xs[i], ys[i], 0.0f);
    glVertex3f(xs[i], ys[i], 0.0f);
  }
  glEnd();
}

/* And the same quad with no program at all, for the fixed-function path. */
static void pm4_draw_plain_quad(void) {
  const float xs[6] = {-0.8f, 0.8f, 0.8f, -0.8f, 0.8f, -0.8f};
  const float ys[6] = {-0.8f, -0.8f, 0.8f, -0.8f, 0.8f, 0.8f};
  glBegin(GL_TRIANGLES);
  for (int i = 0; i < 6; i++) glVertex3f(xs[i], ys[i], 0.0f);
  glEnd();
}

/* **A shader that discards has to tell the depth block, and `exec` is not how.**
 *
 * `DB_SHADER_CONTROL.KILL_ENABLE` (bit 6 of context register 0x203) is what says the shader can
 * throw a fragment away. Without it the block runs early Z: it tests, writes and retires the
 * pixel before the shader has run, on the understanding that the shader cannot change the
 * answer - so a discarded fragment keeps its colour *and* its depth, and the next draw behind
 * it is rejected by a depth value that should never have been written.
 *
 * That is gl2-probe's `discard` on hardware (2026-09-22): the discarded half came back holding
 * the first draw's blue instead of the second draw's yellow. `discard-in-loop` passed in the
 * same run and is the reason this is the explanation rather than "discard is broken" - it draws
 * with no depth test, so there is no early Z to retire anything and the export's mask is the
 * only thing deciding.
 *
 * From `uses_discard` in radeonsi, `si_state_shaders.cpp:1711`. Neither this register nor
 * `SPI_SHADER_Z_FORMAT` had any test before this one, which is how it stayed quiet.
 */
static void test_pm4_gl_a_discarding_shader_sets_kill_enable(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[8192];
  static _Alignas(256) uint8_t payload[0x20000];
  static _Alignas(256) uint8_t vbo[16384];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 8192;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  glContextSetVersion(2, 0);
  glEnable(GL_DEPTH_TEST);

  /* The same shape as the probe's check: a varying decides, and half the quad goes. */
  const GLuint prog = pm4_linked_program(
      "attribute vec3 pos;\n"
      "varying float side;\n"
      "void main() { side = pos.x; gl_Position = vec4(pos, 1.0); }\n",
      "varying float side;\n"
      "void main() {\n"
      "  if (side < 0.0) discard;\n"
      "  gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
      "}\n");
  ASSERT_TRUE(prog != 0u);
  glUseProgram(prog);
  pm4_draw_quad(prog);

  uint32_t last_dbsc = 0xffffffffu;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x203u) last_dbsc = dcb[i + 2];
  }
  ASSERT_TRUE(last_dbsc != 0xffffffffu);   /* it was written at all */
  ASSERT_EQ(last_dbsc & 0x40u, 0x40u);     /* KILL_ENABLE */
  /* And Z_ORDER is still EARLY_Z_THEN_LATE_Z - radeonsi's case 1. A shader that kills does not
   * move to late Z; the bit above is the whole of it. */
  ASSERT_EQ((last_dbsc >> 4) & 0x3u, 1u);
  /* No depth export, so the format register stays at zero. */
  ASSERT_EQ(last_dbsc & 0x1u, 0u);

  /* **A program that does not discard puts the bit back.** The register is state, so a draw
   * that inherits a previous draw's kill would give up early Z for nothing - and, more to the
   * point, this is what proves the emission is driven by the program and not written once. */
  const GLuint plain = pm4_linked_program(
      "attribute vec3 pos;\n"
      "void main() { gl_Position = vec4(pos, 1.0); }\n",
      "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
  ASSERT_TRUE(plain != 0u);
  glUseProgram(plain);
  ctx->dcb_words = 0;
  memset(dcb, 0, sizeof(dcb));
  pm4_draw_quad(plain);

  last_dbsc = 0xffffffffu;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x203u) last_dbsc = dcb[i + 2];
  }
  /* **This is the case the old cache could not see.** Neither program exports depth, so
   * `SPI_SHADER_Z_FORMAT` does not move between the two draws - and the register was emitted
   * only when the format changed. Keyed that way, the kill bit would still be set here. */
  ASSERT_TRUE(last_dbsc != 0xffffffffu);
  ASSERT_EQ(last_dbsc & 0x40u, 0u);

  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The fixed-function path kills as well - the alpha test and the polygon stipple both clear
 * `exec` - and had the same register wrong for the same reason. No GL 1.x check combines either
 * with a depth test, so nothing had measured it. */
static void test_pm4_gl_alpha_test_sets_kill_enable(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[8192];
  static _Alignas(256) uint8_t payload[0x20000];
  static _Alignas(256) uint8_t vbo[16384];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 8192;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0.5f);
  pm4_draw_plain_quad();

  uint32_t last_dbsc = 0xffffffffu;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x203u) last_dbsc = dcb[i + 2];
  }
  ASSERT_TRUE(last_dbsc != 0xffffffffu);
  ASSERT_EQ(last_dbsc & 0x40u, 0x40u);

  /* **`GL_ALWAYS` is not a kill.** A test that keeps every fragment has nothing to tell the
   * depth block, and saying otherwise gives up early Z on every draw for nothing. */
  glAlphaFunc(GL_ALWAYS, 0.0f);
  ctx->dcb_words = 0;
  memset(dcb, 0, sizeof(dcb));
  pm4_draw_plain_quad();

  last_dbsc = 0xffffffffu;
  for (uint32_t i = 0; i + 2 < ctx->dcb_words; i++) {
    if (dcb[i] == 0xc0016900u && dcb[i + 1] == 0x203u) last_dbsc = dcb[i + 2];
  }
  ASSERT_TRUE(last_dbsc != 0xffffffffu);
  ASSERT_EQ(last_dbsc & 0x40u, 0u);

  glDisable(GL_ALPHA_TEST);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_pm4_gl_mip_chain_reaches_the_descriptor(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* The layout function against the numbers above, and a 3x1 base where ceil and floor part. */
  size_t off[OOPS_GL_MAX_TEXTURE_LEVELS];
  uint32_t pit[OOPS_GL_MAX_TEXTURE_LEVELS];
  ASSERT_EQ(gl_tex_chain_layout(4, 4, 3, off, pit), (size_t)1792);
  ASSERT_EQ(off[2], (size_t)0);
  ASSERT_EQ(off[1], (size_t)256);
  ASSERT_EQ(off[0], (size_t)768);
  ASSERT_EQ(pit[0], 64u);
  ASSERT_EQ(gl_tex_chain_layout(3, 1, 2, off, pit), (size_t)(64 * 4 + 64 * 4));

  uint32_t red[16], green[4], blue[1];
  for (int i = 0; i < 16; i++) red[i] = 0xff0000ffu;   /* bytes R,G,B,A = ff,00,00,ff */
  for (int i = 0; i < 4; i++) green[i] = 0xff00ff00u;
  blue[0] = 0xffff0000u;
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
  glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glEnable(GL_TEXTURE_2D);
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  const gl_texture_object_t *tex = gl_lookup_texture(ctx, t);
  ASSERT_TRUE(tex && tex->chain_data && tex->desc_chain);
  const uint8_t *chain = (const uint8_t *)tex->chain_data;
  ASSERT_EQ(chain[0], 0x00); ASSERT_EQ(chain[2], 0xff);             /* level 2: blue */
  ASSERT_EQ(chain[256 + 1], 0xff);                                   /* level 1: green */
  ASSERT_EQ(chain[256 + 64 * 4 + 1], 0xff);                          /* its second row, a pitch on */
  ASSERT_EQ(chain[768 + 0], 0xff); ASSERT_EQ(chain[768 + 1], 0x00);  /* level 0: red */
  ASSERT_EQ(chain[768 + 3 * 64 * 4 + 3 * 4], 0xff);                  /* its last texel */

  /* The descriptor slot the draw loaded: base address the chain's start, LAST_LEVEL and
   * MAX_MIP 2, no custom pitch, MIP_FILTER LINEAR. */
  const uint32_t *dt = (const uint32_t *)(payload + 0x900);
  ASSERT_EQ(dt[0], (uint32_t)(tex->chain_va >> 8));
  ASSERT_EQ((dt[3] >> 16) & 0xfu, 2u);  /* LAST_LEVEL */
  ASSERT_EQ((dt[3] >> 12) & 0xfu, 0u);  /* BASE_LEVEL */
  ASSERT_EQ(dt[4], 0u);
  ASSERT_EQ((dt[5] >> 4) & 0xfu, 2u);   /* MAX_MIP */
  ASSERT_EQ((dt[8 + 2] >> 26) & 3u, 2u); /* MIP_FILTER LINEAR */

  /* A filter that reads no mipmaps: back to the base level's own storage and descriptors -
   * which is a change to the descriptors mid-frame.
   *
   * **That used to submit the frame, and since 2026-09-21 it takes the next ring slot.** The
   * draws already built keep pointing at slot 0 and sample what they were built with; this one
   * is handed slot 1. The tell is the stream's triangle count, which a submission would have
   * reset to one and which now reaches two. */
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  ASSERT_EQ(ctx->hw_desc_slot, 0u);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->triangles_drawn, 2u);
  ASSERT_EQ(ctx->hw_desc_slot, 1u);
  /* **Slot 0 still holds the chain the first draw was given.** Overwriting it is the bug the
   * submission existed to prevent, and the ring is what removes the submission without
   * reintroducing it. */
  ASSERT_EQ((dt[3] >> 16) & 0xfu, 2u);
  ASSERT_EQ((dt[8 + 2] >> 26) & 3u, 2u);
  /* The second draw's own slot carries the single-level descriptors. */
  const uint32_t *dt1 = (const uint32_t *)(payload + gl_hw_desc_slot_offset(1u));
  ASSERT_EQ((dt1[3] >> 16) & 0xfu, 0u);
  ASSERT_EQ((dt1[8 + 2] >> 26) & 3u, 0u);
  ASSERT_EQ(dt1[3], 0x90000facu); /* exactly the single-level descriptor word it always was */
  ASSERT_EQ(dt1[8 + 1], 0x00fff000u); /* MIN_LOD 0, MAX_LOD the field's maximum, by default */
  /* And the draw was handed that slot's address, not the table's. */
  ASSERT_EQ(last_sh_reg(ctx->dcb_mem, ctx->dcb_words, 0x0cu),
            (uint32_t)((uint64_t)(uintptr_t)payload + gl_hw_desc_slot_offset(1u)));

  /* **GL_TEXTURE_BASE_LEVEL 1**: a chain from level 1 - two levels, the descriptor 2x2, level 1
   * last in memory - and GL_TEXTURE_MIN_LOD / _MAX_LOD in the sampler's 4.8 fixed point
   * (ac_descriptors.c:139-140).
   *
   * Each descriptor change below takes the next ring slot rather than submitting the frame
   * (since 2026-09-21), so `DT_NOW` is what the draw just built was handed - the fixed `dt`
   * above is slot 0 and stays as the first draw left it. `hw_failed` is still cleared before
   * each draw: the ring wrapping, or any other submission, has no GPU here to write its fence,
   * and a marked failure would make the next draw do nothing. */
#define DT_NOW ((const uint32_t *)(payload + gl_hw_desc_slot_offset(ctx->hw_desc_slot)))
  ctx->hw_failed = GL_FALSE;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 1);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, 0.5f);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, 1.25f);
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  tex = gl_lookup_texture(ctx, t);
  ASSERT_TRUE(tex->chain_data != NULL);
  ASSERT_EQ(tex->chain_base, 1);
  ASSERT_EQ(tex->chain_levels, 2);
  chain = (const uint8_t *)tex->chain_data;
  ASSERT_EQ(chain[2], 0xff);        /* level 2, blue, first */
  ASSERT_EQ(chain[256 + 1], 0xff);  /* then level 1, green */
  ASSERT_EQ(DT_NOW[0], (uint32_t)(tex->chain_va >> 8));
  ASSERT_EQ(DT_NOW[1] >> 30, 1u);                  /* WIDTH_LO: width 2 */
  ASSERT_EQ((DT_NOW[2] >> 14) & 0x3fffu, 1u);      /* HEIGHT: height 2 */
  ASSERT_EQ((DT_NOW[3] >> 16) & 0xfu, 1u);         /* LAST_LEVEL */
  ASSERT_EQ((DT_NOW[5] >> 4) & 0xfu, 1u);          /* MAX_MIP */
  ASSERT_EQ(DT_NOW[8 + 1], 128u | (320u << 12));   /* MIN_LOD 0.5, MAX_LOD 1.25 */
  /* A filter reading no mipmaps from base level 1 is a chain of one level: level 1, MIP_FILTER
   * NONE - the hardware's own base image is level 0's, which is not the one to sample. */
  ctx->hw_failed = GL_FALSE;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  tex = gl_lookup_texture(ctx, t);
  ASSERT_TRUE(tex->chain_data && tex->chain_base == 1 && tex->chain_levels == 1);
  ASSERT_EQ(((const uint8_t *)tex->chain_data)[1], 0xff); /* green */
  ASSERT_EQ((DT_NOW[3] >> 16) & 0xfu, 0u);
  ASSERT_EQ((DT_NOW[8 + 2] >> 26) & 3u, 0u);
  /* GL_TEXTURE_MAX_LEVEL 1 from base 0 cuts the chain to two levels. */
  ctx->hw_failed = GL_FALSE;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  tex = gl_lookup_texture(ctx, t);
  ASSERT_TRUE(tex->chain_base == 0 && tex->chain_levels == 2);
  ASSERT_EQ((DT_NOW[3] >> 16) & 0xfu, 1u);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  /* **Five draws, four descriptor changes, one stream.** Each change took the next slot instead
   * of a submission, which is the whole point of the ring; before 2026-09-21 this test submitted
   * the frame four times over. */
  ASSERT_EQ(ctx->hw_desc_slot, 4u);
#undef DT_NOW

  glDisable(GL_TEXTURE_2D);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glDeleteTextures(1, &t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_pm4_gl_hardware_texture_stream(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));

  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* Step 1: Draw triangle without texture (untextured Gouraud shader, OOPS_GL_PS_UNTEX_OFFSET) */
  glBegin(GL_TRIANGLES);
  glColor3f(1.0f, 0.0f, 0.0f);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Texture Step 1 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  uint64_t payload_va = (uint64_t)(uintptr_t)ctx->gpu_payload;
  ASSERT_EQ(report.spi_shader_pgm_ps, payload_va + OOPS_GL_PS_UNTEX_OFFSET);

  /* Step 2: Upload 16x16 RGBA texture, bind it, enable GL_TEXTURE_2D */
  GLuint tex_id = 0;
  glGenTextures(1, &tex_id);
  glBindTexture(GL_TEXTURE_2D, tex_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  uint32_t tex_data[16 * 16];
  for (int i = 0; i < 256; i++) tex_data[i] = 0xffff00ffu; /* Magenta */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_data);
  glEnable(GL_TEXTURE_2D);

  /* Draw textured triangle: shader should switch to the textured one (0x400; 0x200 until
   * 2026-09-19), User SGPR 0/1 to descriptor table 0x900 */
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Texture Step 2 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.spi_shader_pgm_ps, payload_va + OOPS_GL_PS_TEX_OFFSET);
  ASSERT_EQ(report.spi_shader_user_data_ps_01, payload_va + OOPS_GL_DESC_TABLE_OFFSET);

  /* Verify descriptor table in gpu_payload at 0x900 */
  uint32_t *dt = (uint32_t *)((char *)ctx->gpu_payload + 0x900);
  ASSERT_EQ(dt[3], 0x90000facu); /* SQ_RSRC_IMG_2D, linear, DST_SEL = channels 0..3 */
  ASSERT_EQ(dt[9], 0x00fff000u); /* Sampler MAX_LOD */

  /* Flush and verify packet stream */
  uint32_t words_before_flush = ctx->dcb_words;
  gl_hw_flush(ctx);
  uint32_t total_flushed_words = words_before_flush + 32; /* two RELEASE_MEM (fence, GPU clock) + 16 NOP pads */
  rc = oops_pm4_validate_stream(ctx->dcb_mem, total_flushed_words, &report);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.release_mem_count, 2u); /* the fence and the GPU clock */

  glDeleteTextures(1, &tex_id);

  /* Clean up mock pointers */
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_pm4_gl_hardware_cull_and_color_mask_stream(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));

  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* Step 1: Default state (no culling, all color channels enabled) */
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  oops_pm4_report_t report;
  int rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Cull Step 1 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.pa_su_sc_mode_cntl, 0x00000240u); /* No cull, CCW front */
  ASSERT_EQ(report.cb_target_mask, 0x0fu);            /* All RGBA channels */

  /* Step 2: Enable GL_CULL_FACE with default GL_BACK and GL_CCW */
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glFrontFace(GL_CCW);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Cull Step 2 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.pa_su_sc_mode_cntl, 0x00000242u); /* CULL_BACK (bit 1), CCW */

  /* Step 3: Change front face to GL_CW */
  glFrontFace(GL_CW);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Cull Step 3 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.pa_su_sc_mode_cntl, 0x00000246u); /* CULL_BACK (bit 1) | CW (bit 2) */

  /* Step 4: Change cull mode to GL_FRONT */
  glCullFace(GL_FRONT);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Cull Step 4 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.pa_su_sc_mode_cntl, 0x00000245u); /* CULL_FRONT (bit 0) | CW (bit 2) */

  /* Step 5: Test dynamic color mask */
  glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE); /* G and A only */

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Color Mask Step 5 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.cb_target_mask, 0x0au); /* Bit 1 (G) | Bit 3 (A) = 0xa */

  /* Step 6: Disable culling and set single red channel mask */
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.0f);
  glVertex3f(0.5f, -0.5f, 0.0f);
  glVertex3f(0.0f, 0.5f, 0.0f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Step 6 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.pa_su_sc_mode_cntl, 0x00000244u); /* CW front, no cull */
  ASSERT_EQ(report.cb_target_mask, 0x01u);            /* Bit 0 (R) */

  /* Step 7: Flush frame and verify RELEASE_MEM packet */
  uint32_t words_before_flush = ctx->dcb_words;
  gl_hw_flush(ctx);
  uint32_t total_flushed_words = words_before_flush + 32; /* two RELEASE_MEM (fence, GPU clock) + 16 NOP pads */
  rc = oops_pm4_validate_stream(ctx->dcb_mem, total_flushed_words, &report);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.release_mem_count, 2u); /* the fence and the GPU clock */

  /* Clean up mock pointers */
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The last value written to one context register in a stream, or `absent` if never written.
 *
 * Walks the type-3 packets rather than searching for a value, so a register offset that
 * happens to appear as somebody else's payload cannot be mistaken for a write to it. */
#define REG_ABSENT 0xdeadbeefu
static uint32_t last_context_reg(const uint32_t *w, size_t n, uint32_t offset) {
  uint32_t found = REG_ABSENT;
  size_t i = 0;
  while (i < n) {
    uint32_t hdr = w[i];
    if ((hdr >> 30) != 3u) break;
    uint32_t count = ((hdr >> 16) & 0x3fffu) + 1u;
    uint32_t op = (hdr >> 8) & 0xffu;
    if (i + 1u + count > n) break;
    if (op == 0x69u && count >= 2u) { /* SET_CONTEXT_REG: base, then values */
      uint32_t base = w[i + 1];
      for (uint32_t k = 1; k < count; k++) {
        if (base + k - 1u == offset) found = w[i + 1u + k];
      }
    }
    i += 1u + count;
  }
  return found;
}

/*
 * **The gl-cube oracle record's facts, asserted against the stream oops-gl emits today.**
 *
 * `docs/hardware/agc-gl-cube-oracle-fw1240.md` is the whole reason this subsystem exists
 * (D007): a frame measured on retail firmware 12.40, with the register facts that made it
 * draw written underneath it. Every one of those facts was prose and nothing checked that the
 * code still honoured them - so a register could be edited and the document would go on
 * describing a frame the code no longer produces, which is the documented-but-unchecked
 * failure this repository keeps meeting.
 *
 * Each assertion below is one line of that record. The display is 640x480 on purpose: it is
 * not square, so the extent fields cannot be transposed without this failing, which is
 * exactly the bug the record says was measured and fixed once already.
 */
static void test_pm4_gl_honours_the_gl_cube_oracle_record(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* Depth on, because the depth block is emitted only when it is - without this the
   * DB_Z_INFO line of the record has nothing to check and the test would quietly cover five
   * facts while claiming six. */
  glEnable(GL_DEPTH_TEST);

  glBegin(GL_TRIANGLES);
  glColor3f(1.0f, 0.0f, 0.0f);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  const uint32_t *w = ctx->dcb_mem;
  const size_t n = ctx->dcb_words;
  ASSERT_TRUE(n > 0);

  /* Record: "CB_COLOR0_ATTRIB2: width - 1 in bits 27:14, height - 1 in bits 13:0; the colour
   * block takes the row pitch from bits 27:14." Measured 2026-09-14 by putting them the other
   * way round and watching a 1920x1080 target get a 1088-pixel row pitch. */
  uint32_t attrib2 = last_context_reg(w, n, 0x3b0u);
  ASSERT_NE(attrib2, REG_ABSENT);
  ASSERT_EQ((attrib2 >> 14) & 0x3fffu, 639u); /* width - 1, NOT height */
  ASSERT_EQ(attrib2 & 0x3fffu, 479u);         /* height - 1 */

  /* Record: "CB_COLOR0_INFO COMP_SWAP=ALT stores bytes B, G, R, A" - which is what makes the
   * framebuffer read as 0xAARRGGBB. */
  uint32_t cb_info = last_context_reg(w, n, 0x31cu);
  ASSERT_NE(cb_info, REG_ABSENT);
  ASSERT_EQ((cb_info >> 11) & 0x3u, 1u); /* COMP_SWAP = SWAP_ALT */

  /* Record: "BLEND_BYPASS must be clear on an 8_8_8_8 UNORM target, or the blender is skipped
   * whatever CB_BLEND0_CONTROL says."
   *
   * This bit was set here until 2026-09-17, inherited from the measured primitive-draw recipe,
   * and it cost three identical hardware runs: gl1-probe's `blend` and `blend-equation` failed
   * while clear, rect and depth passed, and correcting CB_BLEND0_CONTROL changed nothing
   * because the bypass is read first. Mesa derives the pair together and never emits this
   * combination - mesa/src/amd/common/ac_descriptors.c:1426-1438 sets blend_clamp for
   * NORM/SRGB types and blend_bypass only for UINT/SINT or the 8_24/24_8/X24_8_32_FLOAT
   * formats, clearing blend_clamp when it does.
   *
   * Asserted by name as well as by value: the whole-register check below catches a regression,
   * but prints two integers. This says which bit moved. */
  ASSERT_EQ((cb_info >> 16) & 0x1u, 0u); /* BLEND_BYPASS clear  */
  ASSERT_EQ((cb_info >> 15) & 0x1u, 1u); /* BLEND_CLAMP set     */
  ASSERT_EQ(cb_info, 0x000088a8u);

  /* Record: "PA_CL_VPORT_YSCALE negative for GL's upward NDC y". Asserted as the sign bit of
   * the float rather than an exact value, because the magnitude is half the height and that
   * is allowed to change with the display. */
  uint32_t yscale = last_context_reg(w, n, 0x111u);
  ASSERT_NE(yscale, REG_ABSENT);
  ASSERT_TRUE((yscale & 0x80000000u) != 0u);
  ASSERT_NE(yscale, 0x80000000u); /* negative *zero* would pass the sign test and draw nothing */

  /* Record: "VGT_GS_OUT_PRIM_TYPE must be TRISTRIP (2) for triangles; POINTLIST turns each
   * triangle into a point and starves the compositor."
   *
   * There is now a measured negative behind this too: obSCEne's `166-agc/primitive-draw`
   * (sweep 20260916-223136) sets this register to 0, submits three vertices, retires its
   * fence, and its 64x64 target comes back with 4,095 background pixels and exactly one red
   * one. A point, from a triangle, exactly as the record says. */
  uint32_t gs_out = last_context_reg(w, n, 0x29bu);
  ASSERT_NE(gs_out, REG_ABSENT);
  ASSERT_EQ(gs_out, 2u);

  /* Record: "DB_Z_INFO with SW_MODE 64KB_Z_X and no HTILE (TILE_SURFACE_ENABLE 0,
   * DB_HTILE_DATA_BASE 0) gives a working depth test." */
  uint32_t z_info = last_context_reg(w, n, 0x010u);
  ASSERT_NE(z_info, REG_ABSENT);
  ASSERT_EQ(z_info & 0x3u, 3u);          /* Z_32_FLOAT */
  ASSERT_EQ((z_info >> 4) & 0x1fu, 24u); /* SW_MODE = 24, 64KB_Z_X */
  ASSERT_EQ((z_info >> 29) & 0x1u, 0u);  /* TILE_SURFACE_ENABLE: no HTILE */

  /* **The colour target is LINEAR, and that is a deliberate difference from the capture.**
   *
   * `CB_COLOR0_ATTRIB3` bits 18:14 are COLOR_SW_MODE. oops-gl writes 0 on this, the linear
   * path the oracle frame was recorded on, because 0x08c6c000 carries 27 (64KB_R_X) and
   * streaked this scratch buffer when it was used. That value is agc_draw.c's, and no log
   * records where it came from. Pinned because the two values differ in one field and are easy
   * to copy across by mistake - the arithmetic is (v >> 14) & 0x1f. The scanout path writes 27
   * on purpose, into a scanout buffer; test_pm4_gl_scanout_path_targets pins that. */
  uint32_t attrib3 = last_context_reg(w, n, 0x3b8u);
  ASSERT_NE(attrib3, REG_ABSENT);
  ASSERT_EQ((attrib3 >> 14) & 0x1fu, 0u);
  ASSERT_NE((attrib3 >> 14) & 0x1fu, 27u);

  /* **The depth range the oracle frame was recorded with.**
   *
   * `PA_CL_VPORT_ZSCALE` and `ZOFFSET` were literal `0.5f` constants when that frame was
   * captured; they are now computed from `glDepthRange`, whose default is 0..1. The arithmetic
   * is `(far - near) / 2` and `(far + near) / 2`, so the default has to come back to the same
   * two words - and if it does not, every depth-tested frame differs from the record while
   * nothing else in this test would notice. */
  ASSERT_EQ(last_context_reg(w, n, 0x113u), 0x3f000000u); /* 0.5f */
  ASSERT_EQ(last_context_reg(w, n, 0x114u), 0x3f000000u); /* 0.5f */

  /* **The shader interface: what the vertex shader exports and what the pixel shader is handed.**
   *
   * This block was missing. `docs/GL_ROADMAP.md` claimed these registers were pinned here and
   * used that as the reason fog and multitexture are blocked - "that test failing is the point".
   * They were not pinned, by this test or any other, so the safety net the document described
   * did not exist: moving the shader interface would have passed the whole suite and produced a
   * wrong frame on hardware, which is the one place nothing here can check.
   *
   * It matters because **the shaders are hand-written binaries**. Adding an interpolated input
   * renumbers the VGPRs the pixel shader already uses, and adding a parameter export changes
   * what the vertex shader has to write - so these four registers and the shader code have to
   * move together or not at all. Pinning them makes that a build failure rather than a bad
   * frame, and a change that deliberately moves them updates this test and re-records the
   * oracle frame on hardware.
   */
  uint32_t vs_out = last_context_reg(w, n, 0x1b1u); /* SPI_VS_OUT_CONFIG */
  ASSERT_NE(vs_out, REG_ABSENT);
  ASSERT_EQ((vs_out >> 1) & 0x1fu, 1u); /* VS_EXPORT_COUNT = 1, meaning two parameters */

  uint32_t ps_in = last_context_reg(w, n, 0x1b6u); /* SPI_PS_IN_CONTROL */
  ASSERT_NE(ps_in, REG_ABSENT);
  ASSERT_EQ(ps_in & 0x3fu, 2u); /* NUM_INTERP = 2: colour and texcoord, and nothing else */

  /* PERSP_CENTER_ENA and nothing else. Enabling POS_Z here - which is what fog would need -
   * adds VGPRs *before* the ones the pixel shader already reads, so the colour it currently
   * finds in v4..v7 would move. */
  uint32_t ps_ena = last_context_reg(w, n, 0x1b3u); /* SPI_PS_INPUT_ENA */
  ASSERT_NE(ps_ena, REG_ABSENT);
  ASSERT_EQ(ps_ena, 0x00000002u);

  /* The two interpolation slots, in the order the shaders assume: parameter 0 is the colour,
   * parameter 1 is the texture coordinate. Swapping them draws a texture-coloured triangle
   * with its colour used as coordinates and nothing else in this test would notice. */
  ASSERT_EQ(last_context_reg(w, n, 0x191u), 0x00000000u); /* SPI_PS_INPUT_CNTL_0: param 0 */
  ASSERT_EQ(last_context_reg(w, n, 0x192u), 0x00000001u); /* SPI_PS_INPUT_CNTL_1: param 1 */

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}


/* glScissor reaches PA_SC_VPORT_SCISSOR_0_TL/_BR.
 *
 * Until 2026-09-17 all four scissor rectangles were patched to the render target's extent and
 * ctx->cap_scissor_test was read only by the software rasteriser, so the box was silently
 * ignored on hardware - the same shape of bug as the viewport, and gl1-probe's `scissor` check
 * failed on the console while passing on the host for exactly that reason.
 *
 * The target here is 640x480, and GL measures its box from the bottom-left while the scan
 * converter's rectangle is y-down, so the y coordinates come back flipped against 480.
 */
/* The texture environment reaches the textured pixel shader's combine slot.
 *
 * Until 2026-09-17 `tex_env_mode` was read only by the software rasteriser, so the hardware path
 * multiplied whatever the mode said - GL_REPLACE returned the texel scaled by the vertex colour.
 * That is invisible whenever the colour is white, which is why gl-cube never showed it and
 * gl1-probe's `tex-env-modes` did: it draws the same white texel under REPLACE and MODULATE with
 * a dark red colour and requires the two to differ.
 */
/* User clip planes reach PA_CL_UCP_0_X and PA_CL_CLIP_CNTL.
 *
 * Two registers and no shader change, because the fixed-function clipper applies whenever the
 * vertex shader exports no clip distances - which these hand-written shaders do not
 * (mesa/src/gallium/drivers/radeonsi/si_state.c, si_emit_clip_regs).
 *
 * **The plane the hardware wants is in clip space, not eye space** - "Clip-Space Plane =
 * Eye-Space Plane * Projection Matrix", mesa/src/mesa/main/clip.c:40-51. With an identity
 * projection the two coincide, which is why this test also checks a non-identity one: an
 * implementation that skipped the projection transform entirely would pass the first and fail
 * the second.
 */
static void test_pm4_gl_clip_planes_reach_their_registers(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  /* No plane enabled: the enable register is zero and nothing is clipped. */
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x204u), 0u);

  /* Plane 0 enabled, identity projection: the clip-space plane equals the eye-space one. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  const GLdouble px[4] = {1.0, 0.0, 0.0, 0.0};
  glClipPlane(GL_CLIP_PLANE0, px);
  glEnable(GL_CLIP_PLANE0);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x204u), 1u); /* UCP_ENA_0 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x16fu), 0x3f800000u); /* x = 1.0 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x170u), 0u);          /* y = 0   */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x171u), 0u);          /* z = 0   */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x172u), 0u);          /* w = 0   */

  /* A second plane sets its own enable bit, and lands four registers along. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  const GLdouble py[4] = {0.0, 1.0, 0.0, 0.0};
  glClipPlane(GL_CLIP_PLANE2, py);
  glEnable(GL_CLIP_PLANE2);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  /* Plane n occupies four consecutive registers from 0x16f + 4n, so plane 2 starts at 0x177 -
   * **not** 0x178, which is its y. Getting that wrong reads a neighbouring component and passes
   * or fails for the wrong reason. */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x204u), 0x5u); /* planes 0 and 2 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x177u), 0u);          /* 2.x = 0 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x178u), 0x3f800000u); /* 2.y = 1 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x179u), 0u);          /* 2.z = 0 */

  /* **A non-identity projection must change the register**, because the plane is transformed
   * into clip space. glScalef on the projection by 2 in x halves the x term. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glDisable(GL_CLIP_PLANE2);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glScalef(2.0f, 1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glClipPlane(GL_CLIP_PLANE0, px);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x16fu), 0x3f000000u); /* x = 0.5 */

  glDisable(GL_CLIP_PLANE0);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_pm4_gl_tex_env_reaches_the_combine_slot(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(256) uint8_t payload[0x4000];
  memset(payload, 0, sizeof(payload));
  ctx->gpu_payload = payload;
  uint32_t *ps_tex = (uint32_t *)((char *)payload + OOPS_GL_PS_TEX_OFFSET);

  /* The multiply the shader is assembled with: one per channel, texel * colour. */
  static const uint32_t modulate[4] = {0x10081104u, 0x100a1305u, 0x100c1506u, 0x100e1707u};

  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  for (size_t i = 0; i < 4; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], modulate[i]);

  /* Replace is four s_nop: the texel already sits in the registers the export reads, so
   * RGBA replace is exactly "leave it alone" - C = Cs, A = As. */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  for (size_t i = 0; i < 4; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], 0xbf800000u);

  /* And back, because a mode is not a one-way door. */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  for (size_t i = 0; i < 4; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], modulate[i]);

  /* GL_ADD adds the colour and multiplies the alpha (tools/shader/tex-env.s), and since the slot
   * grew clamps the sums: v_min_f32 v4..v6, 1.0 (tools/shader/combine.s), then the branch over the
   * slot's other 56 words. GL_DECAL of an RGBA texture is a program since 2026-09-19 - it modulated
   * until then - which test_pm4_gl_combine_programs_compute_what_software_does runs. */
  static const uint32_t add_rgb[3] = {0x06081104u, 0x060a1305u, 0x060c1506u};
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  for (size_t i = 0; i < 3; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], add_rgb[i]);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 3], modulate[3]);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 4], 0x1e0808f2u);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 6], 0x1e0c0cf2u);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 7], 0xbf820038u); /* s_branch over 56 */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
  ASSERT_TRUE(ps_tex[GL_PS_COMBINE_SLOT_TEX + 0] != modulate[0]);
  ASSERT_EQ(gl_ps_patch_tex_env(ctx), GL_TRUE);

  /* **The texture's base format picks the words too.** An RGB texture keeps the fragment's alpha
   * under GL_REPLACE - v_mov_b32 v7, v11 - and an alpha texture its colour under GL_MODULATE. */
  static const uint8_t texel[4] = {255, 255, 255, 255};
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glEnable(GL_TEXTURE_2D);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  for (size_t i = 0; i < 3; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], 0xbf800000u);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 3], 0x7e0e030bu);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  static const uint32_t keep_rgb[3] = {0x7e080308u, 0x7e0a0309u, 0x7e0c030au};
  for (size_t i = 0; i < 3; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], keep_rgb[i]);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 3], modulate[3]);
  /* GL_ADD of an intensity texture adds alpha as well. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_INTENSITY, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + 3], 0x060e1707u);
  glDisable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDeleteTextures(1, &t);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

  /* glPopAttrib restores the mode, so it has to restore the instructions with it. */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glPushAttrib(GL_TEXTURE_BIT);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  for (size_t i = 0; i < 4; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], modulate[i]);
  glPopAttrib();
  for (size_t i = 0; i < 4; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], 0xbf800000u);

  ctx->gpu_payload = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The general combine's encoder against the assembler: each call below, beside the words
 * `tools/shader/combine.s` assembled to for the same instruction, in that file's order. This is
 * what lets the generator compose words rather than look them up - every form and operand kind it
 * emits is here. */
static void test_pm4_gl_combine_encoder_matches_the_assembler(void) {
  const struct { uint32_t encoded, assembled; } lines[] = {
    {gl_ps_v_mov(16u, GL_PS_SRC_V(4u)),                              0x7e200304u},
    {gl_ps_v_mov(27u, GL_PS_SRC_V(11u)),                             0x7e36030bu},
    {gl_ps_v_mov(20u, GL_PS_SRC_LITERAL),                            0x7e2802ffu},
    {gl_ps_vop2(GL_PS_V_SUB, 16u, GL_PS_SRC_ONE, 4u),                0x082008f2u},
    {gl_ps_vop2(GL_PS_V_SUB, 27u, GL_PS_SRC_ONE, 11u),               0x083616f2u},
    {gl_ps_vop2(GL_PS_V_MUL, 4u, GL_PS_SRC_V(16u), 20u),             0x10082910u},
    {gl_ps_vop2(GL_PS_V_ADD, 5u, GL_PS_SRC_V(17u), 21u),             0x060a2b11u},
    {gl_ps_vop2(GL_PS_V_SUB, 6u, GL_PS_SRC_V(18u), 22u),             0x080c2d12u},
    {gl_ps_vop2(GL_PS_V_SUB, 13u, GL_PS_SRC_V(19u), 23u),            0x081a2f13u},
    {gl_ps_vop2(GL_PS_V_FMAC, 7u, GL_PS_SRC_V(13u), 27u),            0x560e370du},
    {gl_ps_vop2(GL_PS_V_ADD, 4u, GL_PS_SRC_NEG_HALF, 4u),            0x060808f1u},
    {gl_ps_vop2(GL_PS_V_ADD, 13u, GL_PS_SRC_NEG_HALF, 16u),          0x061a20f1u},
    {gl_ps_vop2(GL_PS_V_MUL, 4u, GL_PS_SRC_TWO, 4u),                 0x100808f4u},
    {gl_ps_vop2(GL_PS_V_MUL, 7u, GL_PS_SRC_FOUR, 7u),                0x100e0ef6u},
    {gl_ps_vop2(GL_PS_V_MAX, 4u, GL_PS_SRC_ZERO, 4u),                0x20080880u},
    {gl_ps_vop2(GL_PS_V_MIN, 7u, GL_PS_SRC_ONE, 7u),                 0x1e0e0ef2u},
    {gl_ps_v_mov(5u, GL_PS_SRC_V(4u)),                               0x7e0a0304u},
    {gl_ps_s_branch(0u),                                             0xbf820000u},
    {gl_ps_s_branch(2u),                                             0xbf820002u},
    {gl_ps_s_branch(40u),                                            0xbf820028u},
  };
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
    if (lines[i].encoded != lines[i].assembled) printf("\n[combine.s line %zu]\n", i);
    ASSERT_EQ(lines[i].encoded, lines[i].assembled);
  }
}

/* A reader for exactly the words the combine slot can hold - s_nop, s_branch, v_mov_b32 and the
 * six VOP2 operations - run over v[0..31]. Answers GL_FALSE on anything else, or on leaving the
 * slot anywhere but its end. The decoding is the encoder's field layout, which the test above
 * ties to the assembler. */
static GLboolean run_combine_slot(const uint32_t *w, size_t n, float v[32]) {
  size_t pc = 0;
  while (pc < n) {
    const uint32_t x = w[pc++];
    if (x == 0xbf800000u) continue;
    if ((x & 0xffff0000u) == 0xbf820000u) {
      pc += x & 0xffffu;
      continue;
    }
    const uint32_t src0 = x & 0x1ffu;
    float s0;
    if (src0 >= 256u && src0 < 288u) s0 = v[src0 - 256u];
    else if (src0 == GL_PS_SRC_ZERO) s0 = 0.0f;
    else if (src0 == GL_PS_SRC_NEG_HALF) s0 = -0.5f;
    else if (src0 == GL_PS_SRC_ONE) s0 = 1.0f;
    else if (src0 == GL_PS_SRC_TWO) s0 = 2.0f;
    else if (src0 == GL_PS_SRC_FOUR) s0 = 4.0f;
    else if (src0 == GL_PS_SRC_LITERAL && pc < n) {
      const uint32_t bits = w[pc++];
      memcpy(&s0, &bits, sizeof(s0));
    } else {
      return GL_FALSE;
    }
    const uint32_t vdst = (x >> 17) & 0xffu;
    if (vdst >= 32u) return GL_FALSE;
    if ((x & 0xfe01fe00u) == 0x7e000200u) { /* v_mov_b32 */
      v[vdst] = s0;
      continue;
    }
    if (x & 0x80000000u) return GL_FALSE;
    const uint32_t vsrc1 = (x >> 9) & 0xffu;
    if (vsrc1 >= 32u) return GL_FALSE;
    const float s1 = v[vsrc1];
    switch (x >> 25) {
      case GL_PS_V_ADD:  v[vdst] = s0 + s1; break;
      case GL_PS_V_SUB:  v[vdst] = s0 - s1; break;
      case GL_PS_V_MUL:  v[vdst] = s0 * s1; break;
      case GL_PS_V_MIN:  v[vdst] = (s0 < s1) ? s0 : s1; break;
      case GL_PS_V_MAX:  v[vdst] = (s0 > s1) ? s0 : s1; break;
      case GL_PS_V_FMAC: v[vdst] = s0 * s1 + v[vdst]; break;
      default: return GL_FALSE;
    }
  }
  return (pc == n) ? GL_TRUE : GL_FALSE;
}

/* **The console's combine computes what the software rasteriser's does.** Every texture function
 * on every base format, and GL_COMBINE through each of its functions, sources, operands and scales:
 * the slot gl_ps_patch_tex_env writes is run by the reader above on a texel and a fragment colour,
 * and the result compared with gl_tex_env_apply's on the same two. The short forms are held to it
 * too, which is how GL_ADD's missing clamp showed. */
static void test_pm4_gl_combine_programs_compute_what_software_does(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(256) uint8_t payload[0x4000];
  memset(payload, 0, sizeof(payload));
  ctx->gpu_payload = payload;
  const uint32_t *slot = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET) + GL_PS_COMBINE_SLOT_TEX;

  static const GLenum bases[6] = {GL_RGBA, GL_RGB, GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA,
                                  GL_INTENSITY};
  static const GLenum modes[5] = {GL_MODULATE, GL_REPLACE, GL_ADD, GL_DECAL, GL_BLEND};
  /* Two texel/colour pairs, one with sums above 1 so the clamps matter. */
  static const float texels[2][4] = {{0.25f, 0.5f, 0.75f, 0.6f}, {0.9f, 0.8f, 0.7f, 0.95f}};
  static const float colours[2][4] = {{0.8f, 0.3f, 0.1f, 0.5f}, {0.6f, 0.9f, 0.4f, 0.7f}};
  const GLfloat env[4] = {0.2f, 0.4f, 0.9f, 0.35f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
  static const uint8_t white[4] = {255, 255, 255, 255};
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glEnable(GL_TEXTURE_2D);

  int checked = 0;
  #define CHECK_SLOT(what) do { \
      for (int p = 0; p < 2; p++) { \
        float t[4], expect[4], v[32]; \
        const GLenum base_ = gl_tex_sample_format(gl_lookup_texture(ctx, tex)); \
        /* The texel as the descriptor's swizzle expands it for the base format. */ \
        for (int k = 0; k < 4; k++) t[k] = texels[p][k]; \
        if (base_ == GL_ALPHA) { t[0] = t[1] = t[2] = 0.0f; } \
        else if (base_ == GL_LUMINANCE) { t[1] = t[2] = t[0]; t[3] = 1.0f; } \
        else if (base_ == GL_LUMINANCE_ALPHA) { t[1] = t[2] = t[0]; } \
        else if (base_ == GL_INTENSITY) { t[1] = t[2] = t[3] = t[0]; } \
        else if (base_ == GL_RGB) { t[3] = 1.0f; } \
        for (int k = 0; k < 4; k++) expect[k] = colours[p][k]; \
        gl_unit_texels_t tx_; \
        memset(&tx_, 0, sizeof(tx_)); \
        for (int k = 0; k < 4; k++) tx_.t[0][k] = t[k]; \
        gl_tex_env_apply(ctx, 0u, base_, &tx_, colours[p], expect); \
        for (int k = 0; k < 32; k++) v[k] = -7.0f; \
        for (int k = 0; k < 4; k++) { v[4 + k] = t[k]; v[8 + k] = colours[p][k]; } \
        ASSERT_EQ(gl_ps_patch_tex_env(ctx), GL_TRUE); \
        ASSERT_EQ(run_combine_slot(slot, GL_PS_COMBINE_WORDS, v), GL_TRUE); \
        for (int k = 0; k < 4; k++) { \
          const float d = v[4 + k] - expect[k]; \
          if (d > 1e-5f || d < -1e-5f) \
            printf("\n[%s pair %d channel %d: slot %f, software %f]\n", what, p, k, \
                   (double)v[4 + k], (double)expect[k]); \
          ASSERT_TRUE(d <= 1e-5f && d >= -1e-5f); \
        } \
        checked++; \
      } \
    } while (0)

  for (int b = 0; b < 6; b++) {
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)bases[b], 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    for (int m = 0; m < 5; m++) {
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)modes[m]);
      CHECK_SLOT("texture function");
    }
  }

  /* GL_COMBINE on an RGBA texture: each function, with arguments drawn from every source and
   * every operand, and the three scales. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
  static const GLenum fns[8] = {GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE,
                                GL_SUBTRACT, GL_DOT3_RGB, GL_DOT3_RGBA};
  static const GLenum sources[4] = {GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS};
  static const GLenum rgb_ops[4] = {GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                    GL_ONE_MINUS_SRC_ALPHA};
  static const GLenum alpha_ops[2] = {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA};
  static const float scales[3] = {1.0f, 2.0f, 4.0f};
  for (int f = 0; f < 8; f++) {
    for (int s = 0; s < 4; s++) {
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, (GLint)fns[f]);
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, (GLint)fns[(f + s) % 6]);
      for (int a = 0; a < 3; a++) {
        glTexEnvi(GL_TEXTURE_ENV, (GLenum)(GL_SOURCE0_RGB + a), (GLint)sources[(s + a) % 4]);
        glTexEnvi(GL_TEXTURE_ENV, (GLenum)(GL_OPERAND0_RGB + a), (GLint)rgb_ops[(s + 2 * a) % 4]);
        glTexEnvi(GL_TEXTURE_ENV, (GLenum)(GL_SOURCE0_ALPHA + a), (GLint)sources[(s + a + 1) % 4]);
        glTexEnvi(GL_TEXTURE_ENV, (GLenum)(GL_OPERAND0_ALPHA + a), (GLint)alpha_ops[(s + a) % 2]);
      }
      glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, scales[(f + s) % 3]);
      glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, scales[(f + 2 * s) % 3]);
      ASSERT_EQ(glGetError(), GL_NO_ERROR);
      CHECK_SLOT("combine");
    }
  }
  #undef CHECK_SLOT
  ASSERT_EQ(checked, 2 * (6 * 5 + 8 * 4));

  glDisable(GL_TEXTURE_2D);
  glDeleteTextures(1, &tex);
  ctx->gpu_payload = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static void test_pm4_gl_scissor_reaches_its_registers(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  const struct {
    GLboolean enable;
    GLint x, y;
    GLsizei w, h;
    uint32_t tl, br;
    const char *what;
  } cases[] = {
    /* Disabled: the full target, which is what the measured recipe wrote unconditionally. */
    {GL_FALSE, 0, 0, 0, 0, 0x80000000u, 0x01e00280u, "disabled"},
    /* The lower-left quarter in GL terms is the *bottom* rows in window terms. */
    {GL_TRUE, 0, 0, 320, 240, 0x80f00000u, 0x01e00140u, "lower-left quarter"},
    /* An interior box: x 100..300, GL y 50..150 -> window y 330..430. */
    {GL_TRUE, 100, 50, 200, 100, 0x814a0064u, 0x01ae012cu, "interior box"},
    /* Hanging off the left and bottom. The register fields are unsigned, so a negative
     * coordinate left alone would wrap to an enormous one and the box would vanish. */
    {GL_TRUE, -50, -50, 100, 100, 0x81ae0000u, 0x01e00032u, "clamped to the target"},
  };

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    memset(dcb, 0, sizeof(dcb));
    memset(payload, 0, sizeof(payload));
    memset(vbo, 0, sizeof(vbo));
    ctx->dcb_mem = dcb;
    ctx->dcb_capacity_dw = 4096;
    ctx->dcb_words = 0;
    ctx->gpu_payload = payload;
    ctx->vbo_mem = vbo;
    ctx->fence = fence;
    ctx->canary = &canary;
    ctx->use_hardware = GL_TRUE;
    ctx->hw_frame_active = GL_FALSE;

    if (cases[c].enable) {
      glEnable(GL_SCISSOR_TEST);
      glScissor(cases[c].x, cases[c].y, cases[c].w, cases[c].h);
    } else {
      glDisable(GL_SCISSOR_TEST);
    }

    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();

    if (last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x094u) != cases[c].tl)
      printf("\n[scissor case: %s]\n", cases[c].what);
    ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x094u), cases[c].tl);
    ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x095u), cases[c].br);

    /* The other three rectangles are the surface bounds and must not follow the GL box. */
    ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x082u), 0x01e00280u);
    ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x091u), 0x01e00280u);
  }

  /* A box changed *after* the frame opened has to be re-emitted before the next draw, or the
   * draw runs against the rectangle the frame started with. Same failure the viewport had. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glDisable(GL_SCISSOR_TEST);

  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x094u), 0x80000000u);

  /* Frame still open - no hw_frame_active reset here, which is the whole point. */
  glEnable(GL_SCISSOR_TEST);
  glScissor(10, 20, 30, 40);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x094u), 0x81a4000au);
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x095u), 0x01cc0028u);

  glDisable(GL_SCISSOR_TEST);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glDepthRange reaches the viewport registers, and a reversed range is accepted.
 *
 * The three cases are chosen so no two share a ZSCALE or a ZOFFSET: 0..1 gives (0.5, 0.5),
 * 0..0.5 gives (0.25, 0.25), and the reversed 1..0 gives (-0.5, 0.5). A build that swapped the
 * two registers, or that sorted the arguments, fails on one of them.
 *
 * **And a range changed inside a frame reaches the next draw.** Every case above opens a fresh
 * frame, which is the one situation the frame's register table covers by itself - so this test
 * passed while a mid-frame glDepthRange did nothing on hardware until the frame after.
 */
static void test_pm4_gl_depth_range_reaches_the_viewport(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  const struct { double near_val, far_val; uint32_t scale, offset; } cases[] = {
    {0.0, 1.0, 0x3f000000u, 0x3f000000u},  /*  0.5,  0.5 - the default */
    {0.0, 0.5, 0x3e800000u, 0x3e800000u},  /* 0.25, 0.25 */
    {1.0, 0.0, 0xbf000000u, 0x3f000000u},  /* -0.5,  0.5 - reversed, and legal */
  };

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    memset(dcb, 0, sizeof(dcb));
    memset(payload, 0, sizeof(payload));
    memset(vbo, 0, sizeof(vbo));
    ctx->dcb_mem = dcb;
    ctx->dcb_capacity_dw = 4096;
    ctx->dcb_words = 0;
    ctx->gpu_payload = payload;
    ctx->vbo_mem = vbo;
    ctx->fence = fence;
    ctx->canary = &canary;
    ctx->use_hardware = GL_TRUE;
    ctx->hw_frame_active = GL_FALSE;

    glDepthRange(cases[c].near_val, cases[c].far_val);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0.5f);
    glVertex3f(0.5f, -0.5f, 0.5f);
    glVertex3f(0.0f, 0.5f, 0.5f);
    glEnd();

    ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x113u), cases[c].scale);
    ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x114u), cases[c].offset);
  }

  /* The last case left the frame open with the reversed range. Narrow it without reopening -
   * no hw_frame_active reset, which is the point - and the next draw must carry 0..0.5. */
  ASSERT_EQ(ctx->hw_frame_active, GL_TRUE);
  glDepthRange(0.0, 0.5);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x113u), 0x3e800000u); /* 0.25 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x114u), 0x3e800000u); /* 0.25 */

  /* A glPopAttrib of GL_VIEWPORT_BIT is the other way a range changes mid-frame. The draw in
   * between puts the reversed range on the hardware, so the one after the pop only reads 0.25
   * if the pop itself sent it. */
  glPushAttrib(GL_VIEWPORT_BIT);
  glDepthRange(1.0, 0.0);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x113u), 0xbf000000u); /* -0.5 */
  glPopAttrib();
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x113u), 0x3e800000u);
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x114u), 0x3e800000u);

  /* Out of range is clamped rather than refused - the specification says clamp. */
  glDepthRange(-5.0, 7.0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

static size_t count_context_reg_writes(const uint32_t *w, size_t n, uint32_t offset) {
  size_t hits = 0;
  size_t i = 0;
  while (i < n) {
    uint32_t hdr = w[i];
    if ((hdr >> 30) != 3u) break;
    uint32_t count = ((hdr >> 16) & 0x3fffu) + 1u;
    uint32_t op = (hdr >> 8) & 0xffu;
    if (i + 1u + count > n) break;
    if (op == 0x69u && count >= 2u) {
      uint32_t base = w[i + 1];
      for (uint32_t k = 1; k < count; k++) {
        if (base + k - 1u == offset) hits++;
      }
    }
    i += 1u + count;
  }
  return hits;
}

static void pm4_draw_one_triangle(void) {
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
}

/*
 * **Two depth ranges in one frame reach the GPU as two writes**, which is what gl1-probe's
 * `depth-range-in-frame` asks of a console and has now failed there twice.
 *
 * The check draws a red rectangle under `glDepthRange(0.5, 1.0)` and a green one under
 * `glDepthRange(0.0, 0.5)` at the same NDC z, with `GL_LESS`. Window z is `z_ndc * ZSCALE +
 * ZOFFSET`, so at z_ndc 0 the red lands at 0.75 and the green at 0.25, and the green wins. Its
 * `saw` on hardware is `0xffff0000` - **red**, the colour that survives when both rectangles
 * land at the same depth and the second fails `GL_LESS` on equality. That is the pixel a lost
 * second write produces.
 *
 * ZSCALE is `(f - n) / 2` and ZOFFSET `(f + n) / 2`, so between those two calls the scale is
 * 0.25 both times and only the offset moves, 0.75 to 0.25. A test that watched the scale would
 * see nothing.
 */
static void test_pm4_gl_depth_range_changes_within_a_frame(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);

  glDepthRange(0.5, 1.0);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x113u), 0x3e800000u); /* 0.25 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x114u), 0x3f400000u); /* 0.75 */

  /* The second range, in the same frame - no flush between, as in the check. */
  const uint32_t before = ctx->dcb_words;
  glDepthRange(0.0, 0.5);
  pm4_draw_one_triangle();
  ASSERT_TRUE(ctx->hw_frame_active); /* still one frame, or this measures nothing */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x113u), 0x3e800000u); /* 0.25 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x114u), 0x3e800000u); /* 0.25 */
  /* And it went out with the second draw, not carried from the first. */
  ASSERT_NE(last_context_reg(ctx->dcb_mem + before, ctx->dcb_words - before, 0x114u), REG_ABSENT);

  glDepthRange(0.0, 1.0);
  glDisable(GL_DEPTH_TEST);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **GL_POLYGON_SMOOTH's three edge distances** (since 2026-09-21), and the widening that gives
 * the fade somewhere to land.
 *
 * The roadmap said this could not be done, for two reasons that were both true when written: a
 * coverage of three edge fades is a different shape of slot from one distance, and the outer
 * half of every fade is on pixels the hardware rasteriser never raises. The slot is twenty-eight
 * words now and holds either form, and the CPU widens the triangle by a pixel about its
 * incenter - the same trick that already widens a point and a line into a quad, which moves
 * every edge outward by the same amount.
 *
 * The attribute is one-hot on purpose: the signed distance to edge *i* is `λ_i * h_i`, so vertex
 * *j* carries its own height in component *j* and zero in the others, and the interpolator does
 * the rest. Times `w`, with `w` in the fourth slot, because `v_interp` is perspective-correct
 * and a distance to a line is not.
 *
 * A right triangle with legs along the axes is used because its heights are arithmetic worth
 * asserting: in a 640x480 viewport the unit square from NDC (-1,-1) to (0,0) is 320 by 240
 * pixels, so the two legs are the heights from the far vertices.
 */
static void test_pm4_gl_smooth_polygon_carries_three_edge_distances(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[8192];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  ctx->hw_vbo_cursor = 0;

  /* The payload is this test's own zeroed array rather than the one context creation laid the
   * shaders into, as every test here does - so the slot is zero until a draw patches it, and
   * what is asserted is what the draws write. */
  const uint32_t *const ps = (const uint32_t *)(payload + OOPS_GL_PS_UNTEX_OFFSET);

  glEnable(GL_POLYGON_SMOOTH);
  glBegin(GL_TRIANGLES);
  glVertex3f(-1.0f, -1.0f, 0.5f);
  glVertex3f(0.0f, -1.0f, 0.5f);
  glVertex3f(-1.0f, 0.0f, 0.5f);
  glEnd();
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The polygon form, reading attr3 - the same first word in either shader, because one program
   * serves both. */
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX], 0xc8300c00u);      /* v_interp_p1 v12, attr3.x */
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX + 6u], 0xc83c0f00u); /* v_interp_p1 v15, attr3.w */
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX + 8u], 0x7e1e550fu); /* v_rcp_f32 v15, v15 */
  /* Four parameters, so an eighty-byte vertex with the distances in the fourth vec4. */
  ASSERT_EQ(ctx->hw_params, 4u);
  {
    const float *a0 = (const float *)(vbo + 64);
    const float *a1 = (const float *)(vbo + 80 + 64);
    const float *a2 = (const float *)(vbo + 160 + 64);
    /* w is 1 here - an untransformed vertex through an identity projection - so `d*w` is `d`. */
    ASSERT_FLOAT_NEAR(a0[3], 1.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a1[3], 1.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a2[3], 1.0f, 1e-3f);
    /* One-hot: vertex j carries its own height in component j and nothing in the others. */
    ASSERT_FLOAT_NEAR(a0[1], 0.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a0[2], 0.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a1[0], 0.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a1[2], 0.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a2[0], 0.0f, 1e-3f);
    ASSERT_FLOAT_NEAR(a2[1], 0.0f, 1e-3f);
    /* Vertex 1 and vertex 2 sit at the ends of the two legs, 320 and 240 pixels from the
     * opposite edge - so those are their heights, before the widening moves them. The widening
     * scales about the incenter, which lengthens both by the same ratio, so each is at least
     * its original height. */
    ASSERT_TRUE(a1[1] >= 320.0f && a1[1] < 340.0f);
    ASSERT_TRUE(a2[2] >= 240.0f && a2[2] < 260.0f);
    /* The right angle is at vertex 0, whose height to the hypotenuse is the usual product over
     * the hypotenuse: 320*240/400 = 192. */
    ASSERT_TRUE(a0[0] >= 192.0f && a0[0] < 212.0f);
  }
  /* **The triangle was widened**, so its vertices are outside the ones GL was given - which is
   * what puts a fragment on the pixels the edges only partly cover. */
  {
    const float *p0 = (const float *)(vbo + 0);
    ASSERT_TRUE(p0[0] < -1.0f && p0[1] < -1.0f);
  }

  /* Off again on the next draw: a triangle after a smooth polygon must stop weighing its alpha
   * by an interpolant it no longer exports. */
  glDisable(GL_POLYGON_SMOOTH);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ps[GL_PS_COVERAGE_SLOT_UNTEX] & 0xffff0000u, 0xbf820000u);

  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **The descriptor ring wraps into a submission**, which is the one thing that must still work
 * the old way.
 *
 * A frame gets 64 textures - slot 0 at the original table and 63 in the ring - and the 65th
 * change has nowhere to go. That case has to submit, exactly as *every* change did before
 * 2026-09-21, because the alternative is overwriting a slot some built draw still points at.
 * Driven by setting the cursor rather than by binding 64 textures: what is under test is the
 * boundary, and 64 uploads would test the texture table instead.
 */
static void test_pm4_gl_descriptor_ring_wraps_into_a_submission(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[8192];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* **The ring fits inside the payload.** The textured pixel shader ends at 0x1500 and the
   * allocation is 0x4000, so the last slot must land between them - the arithmetic that keeps
   * the ring from writing over a shader. */
  ASSERT_TRUE(gl_hw_desc_slot_offset(OOPS_GL_DESC_RING_SLOTS) + OOPS_GL_DESC_SLOT_STRIDE <=
              0x4000u);
  ASSERT_TRUE(gl_hw_desc_slot_offset(1u) >= 0x1500u);
  /* Slot 0 is the original table, which is what keeps a one-texture frame's stream unchanged. */
  ASSERT_EQ(gl_hw_desc_slot_offset(0u), OOPS_GL_DESC_TABLE_OFFSET);

  /* **Two different sizes, on purpose.** The slot is compared against the texture's descriptors,
   * and two 1x1 textures with the same parameters describe identically here - the host's
   * allocator hands them the same address, so there is no difference to notice and no slot is
   * taken. A console would differ in the address alone; this differs in WIDTH and HEIGHT, which
   * is a difference on both. */
  static const GLubyte red[4] = {255, 0, 0, 255};
  static const GLubyte green[2 * 2 * 4] = {0, 255, 0, 255, 0, 255, 0, 255,
                                           0, 255, 0, 255, 0, 255, 0, 255};
  GLuint t[2] = {0, 0};
  glGenTextures(2, t);
  for (int i = 0; i < 2; i++) {
    glBindTexture(GL_TEXTURE_2D, t[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, i ? 2 : 1, i ? 2 : 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, i ? green : red);
  }
  glEnable(GL_TEXTURE_2D);
  #define TRI() do { glBegin(GL_TRIANGLES); \
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f); \
    glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f); \
    glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f); glEnd(); } while (0)

  glBindTexture(GL_TEXTURE_2D, t[0]);
  ctx->hw_failed = GL_FALSE;
  TRI();
  ASSERT_EQ(ctx->hw_desc_slot, 0u);
  /* One change, one slot, no submission: the stream keeps its triangle. */
  glBindTexture(GL_TEXTURE_2D, t[1]);
  ctx->hw_failed = GL_FALSE;
  TRI();
  ASSERT_EQ(ctx->hw_desc_slot, 1u);
  ASSERT_EQ(ctx->triangles_drawn, 2u);

  /* **At the cap it submits instead.** The cursor is put at the last slot by hand and one more
   * texture asked for; the frame runs, and the new frame starts back at slot 0. */
  ctx->hw_desc_slot = OOPS_GL_DESC_RING_SLOTS;
  glBindTexture(GL_TEXTURE_2D, t[0]);
  ctx->hw_failed = GL_FALSE;
  TRI();
  ASSERT_EQ(ctx->hw_desc_slot, 0u);
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  #undef TRI

  glDisable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDeleteTextures(2, t);
  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glLogicOp reaches CB_COLOR_CONTROL and glBlendColor reaches CB_BLEND_RED..ALPHA.
 *
 * The logic op's register value is radeonsi's (si_state.c:341, :365-368, :545-549): the Mesa
 * four-bit mode repeated into both halves of ROP3, DISABLE_DUAL_QUAD with it, and GL_COPY the same
 * as off. The mode is **not** `opcode - GL_CLEAR` - GL's enum orders the truth table the other
 * way - so the cases here are the ones where that mistake shows: GL_XOR is 6 either way and
 * proves nothing on its own, GL_AND_REVERSE is 2 in GL's order and 4 in the table.
 *
 * The blend constant is not in the frame's register table, so the stream gl-cube's oracle
 * recorded is unchanged: it goes out only for a draw that reads it, and only once per value.
 */
static void test_pm4_gl_logic_op_and_blend_constant_reach_their_registers(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* Default: the constant the frame table always carried, and no blend constant at all. */
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00cc0010u);
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x105u), REG_ABSENT);

  /* Mid-frame logic op. */
  glEnable(GL_COLOR_LOGIC_OP);
  glLogicOp(GL_XOR);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00660011u);

  glLogicOp(GL_AND_REVERSE); /* COLOR_LOGICOP_AND_REVERSE = 4, so ROP3 0x44 */
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00440011u);

  glLogicOp(GL_INVERT); /* 5, where opcode - GL_CLEAR would be 10 */
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00550011u);

  /* Blending on as well: the logic op wins, so the blender is off. */
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x1e0u), 0u);

  /* GL_COPY is enabled-but-identity: ROP3_COPY and no DISABLE_DUAL_QUAD. */
  glLogicOp(GL_COPY);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00cc0010u);

  /* Off again: back to the constant, and the blender back on. */
  glDisable(GL_COLOR_LOGIC_OP);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00cc0010u);
  ASSERT_NE(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x1e0u), 0u);

  /* Blending without a constant factor still sends no constant. */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x105u), REG_ABSENT);

  /* A constant factor: BLEND_CONSTANT_COLOR is 13 and BLEND_ONE_MINUS_CONSTANT_ALPHA 20
   * (gfx103.json, enum BlendOp), and the four floats land at 0x105..0x108. */
  glBlendColor(0.25f, 0.5f, 0.75f, 1.0f);
  glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_ALPHA);
  pm4_draw_one_triangle();
  uint32_t bc = last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x1e0u);
  ASSERT_EQ(bc & 0x1fu, 13u);
  ASSERT_EQ((bc >> 8) & 0x1fu, 20u);
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x105u), 0x3e800000u); /* 0.25 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x106u), 0x3f000000u); /* 0.5 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x107u), 0x3f400000u); /* 0.75 */

  /*
   * **`0x108` carries green here, not alpha**, and this line used to expect 1.0.
   *
   * obSCEne measured (`-2e9f`, sweep 20260921-run17, firmware 12.40) that the green channel of a
   * `BLEND_CONSTANT_COLOR` blend reads `CB_BLEND_ALPHA` at `0x108` and ignores `CB_BLEND_GREEN`
   * at `0x106` - four arms, same pixel every time. So `gl_draw.c` writes green into the alpha
   * slot for any draw that reads the colour constant, which is the only way `glBlendColor` can
   * mean what GL says.
   *
   * `0x106` above is still written with green, and is still ignored by the part. It goes out
   * because it is the register GL names and because a future part may read it.
   */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x108u), 0x3f000000u); /* green, 0.5 */

  /*
   * **And this pair cannot be served, so it is refused.** The draw above asks for
   * `GL_CONSTANT_COLOR` as source and `GL_ONE_MINUS_CONSTANT_ALPHA` as destination: the first
   * needs green in `0x108` and the second needs alpha there. Legal GL, impossible on this part,
   * and `D009` says a wrong channel is worse than a loud failure.
   *
   * The test used to pass because it assumed both halves worked. They never did.
   */
  ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

  /* Once per value, not once per draw. */
  pm4_draw_one_triangle();
  ASSERT_EQ(count_context_reg_writes(ctx->dcb_mem, ctx->dcb_words, 0x105u), 1u);

  /* A new value goes out with the next draw, clamped as glBlendColor stored it. */
  glBlendColor(2.0f, -1.0f, 0.5f, 0.0f);
  pm4_draw_one_triangle();
  ASSERT_EQ(count_context_reg_writes(ctx->dcb_mem, ctx->dcb_words, 0x105u), 2u);
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x105u), 0x3f800000u); /* 1.0 */
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x106u), 0u);          /* 0.0 */

  /* And a new frame sends it again, because nothing says the registers survived. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  pm4_draw_one_triangle();
  ASSERT_EQ(count_context_reg_writes(ctx->dcb_mem, ctx->dcb_words, 0x105u), 1u);

  /*
   * **One constant family at a time is served, and the choice follows the blend function.**
   *
   * With only the alpha constant read, `0x108` carries alpha again rather than green - so the
   * value in that register depends on `glBlendFunc` and not only on `glBlendColor`, which is why
   * both setters mark the constant dirty. Without that, this draw would reuse the green the
   * colour-constant draws above left there, and blend against the wrong channel with nothing to
   * show for it.
   */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  /* The draws above kept asking for the impossible pair, and a GL error latches until it is
     read. Drain it, so the assertions below are about these draws and not those. */
  (void)glGetError();
  glBlendColor(0.25f, 0.5f, 0.75f, 1.0f);
  glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x108u), 0x3f800000u); /* 1.0, alpha */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* And switching family re-sends it, with green in the alpha slot this time. */
  glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x108u), 0x3f000000u); /* 0.5, green */
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* Put the state back for the rest of the test. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glBlendColor(2.0f, -1.0f, 0.5f, 0.0f);
  glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_ALPHA);
  pm4_draw_one_triangle();
  (void)glGetError(); /* the impossible pair again; asserted where it is introduced */

  /* A logic op set before the frame opens goes out in the frame's own table. */
  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glEnable(GL_COLOR_LOGIC_OP);
  glLogicOp(GL_SET);
  pm4_draw_one_triangle();
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x202u), 0x00ff0011u);

  glDisable(GL_COLOR_LOGIC_OP);
  glDisable(GL_BLEND);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* A frame of more triangles than the vertex ring holds is submitted before the ring is reused.
 *
 * Each triangle's vertices sit in their own ring slot until the stream runs at the flush, so the
 * 451st triangle of a stream must not land in the 1st one's slot while the 1st is still unread.
 * On the host there is no driver, so the flush is observed by what it resets - the stream's
 * triangle count and its length - rather than by a submission.
 */
static void test_pm4_gl_vertex_ring_submits_before_it_wraps(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[32768];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[65536];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 32768;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  for (uint32_t i = 0; i < OOPS_GL_VBO_RING_TRIANGLES; i++) pm4_draw_one_triangle();
  /* Full, and still one stream: the command buffer is nowhere near its own limit. */
  ASSERT_EQ(ctx->triangles_drawn, OOPS_GL_VBO_RING_TRIANGLES);
  const uint32_t words_full = ctx->dcb_words;
  ASSERT_TRUE(words_full + OOPS_GL_DCB_DRAW_MAX_DW + OOPS_GL_DCB_TRAILER_DW < ctx->dcb_capacity_dw);

  /* One more: the full stream is submitted and the new triangle opens the next one. */
  pm4_draw_one_triangle();
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  ASSERT_TRUE(ctx->dcb_words < words_full);

  ctx->use_hardware = GL_FALSE;
  ctx->dcb_mem = NULL;
  ctx->gpu_payload = NULL;
  ctx->vbo_mem = NULL;
  ctx->fence = NULL;
  ctx->canary = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* glPolygonOffset reaches its four registers, and the enable reaches PA_SU_SC_MODE_CNTL.
 *
 * The factor is scaled by 16 and the units are not, which is the part worth pinning: those are
 * different multipliers taken from Mesa's radeonsi for a 32-bit float z-buffer, and a build that
 * applied one scaling to both would pass a test that used equal values. So factor and units are
 * deliberately different here, and neither is a value the other's scaling could produce.
 */
static void test_pm4_gl_polygon_offset_reaches_its_registers(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u}; /* flush writes [0] and the clock at [2..3] */
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(2.0f, 3.0f);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  const uint32_t *w = ctx->dcb_mem;
  const size_t n = ctx->dcb_words;
  /* factor 2.0 * 16 = 32.0 */
  ASSERT_EQ(last_context_reg(w, n, 0x2e0u), 0x42000000u); /* FRONT_SCALE */
  ASSERT_EQ(last_context_reg(w, n, 0x2e2u), 0x42000000u); /* BACK_SCALE */
  /* units 3.0, unscaled on the float z path */
  ASSERT_EQ(last_context_reg(w, n, 0x2e1u), 0x40400000u); /* FRONT_OFFSET */
  ASSERT_EQ(last_context_reg(w, n, 0x2e3u), 0x40400000u); /* BACK_OFFSET */
  ASSERT_EQ(last_context_reg(w, n, 0x2dfu), 0u);          /* CLAMP: GL has none */
  /* The DB format this was derived for has not moved. */
  ASSERT_EQ(last_context_reg(w, n, 0x2deu), 0x000001e9u);

  /* Enabled: front, back and para bits all set (11, 12, 13). */
  uint32_t mode = last_context_reg(w, n, 0x205u);
  ASSERT_NE(mode, REG_ABSENT);
  ASSERT_EQ((mode >> 11) & 0x7u, 0x7u);

  /* Disabled: the offsets stay programmed and the enables go. GL keeps the values across a
   * disable, so a re-enable does not need the call repeated. */
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glDisable(GL_POLYGON_OFFSET_FILL);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  mode = last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x205u);
  ASSERT_EQ((mode >> 11) & 0x7u, 0x0u);
  ASSERT_EQ(last_context_reg(ctx->dcb_mem, ctx->dcb_words, 0x2e0u), 0x42000000u);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* GL_COLOR_SUM on the hardware path: the pixel shader has one colour interpolant, so the
 * secondary colour is summed into each vertex's colour before it reaches the vertex ring. The
 * colour is the second 16 bytes of each 48-byte vertex. */
static void test_pm4_gl_color_sum_reaches_the_vertex_colour(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  float col[4];
  glColor3f(1.0f, 0.0f, 0.0f);
  glSecondaryColor3f(0.0f, 0.0f, 0.5f);
  glEnable(GL_COLOR_SUM);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_TRUE(ctx->triangles_drawn >= 1u);
  memcpy(col, vbo + (size_t)(ctx->triangles_drawn - 1u) * 144u + 16u, sizeof(col));
  ASSERT_TRUE(col[0] == 1.0f && col[1] == 0.0f && col[2] == 0.5f && col[3] == 1.0f);

  /* Off: the primary alone. */
  glDisable(GL_COLOR_SUM);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  memcpy(col, vbo + (size_t)(ctx->triangles_drawn - 1u) * 144u + 16u, sizeof(col));
  ASSERT_TRUE(col[0] == 1.0f && col[1] == 0.0f && col[2] == 0.0f && col[3] == 1.0f);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* Fog on the console: the factor per vertex in the texture coordinate's z, and the blend in both
 * pixel shaders' fog slots with the fog colour as its three literals - the words
 * `tools/shader/fog.s` assembles to. Off, the slots are s_nop and nothing else moved. */
static void test_pm4_gl_fog_reaches_both_shaders_and_the_vertex(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  const uint32_t *ps_untex = (const uint32_t *)(payload + OOPS_GL_PS_UNTEX_OFFSET);
  const uint32_t *ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);

  /* Linear fog from 0 to 4, by GL 1.4's fog coordinate so the factors are exact: a coordinate c
   * has factor (4 - c) / 4. */
  const GLfloat fog_colour[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  glFogi(GL_FOG_MODE, GL_LINEAR);
  glFogf(GL_FOG_START, 0.0f);
  glFogf(GL_FOG_END, 4.0f);
  glFogfv(GL_FOG_COLOR, fog_colour);
  glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
  glEnable(GL_FOG);
  glColor3f(1.0f, 0.0f, 0.0f);
  glBegin(GL_TRIANGLES);
  glFogCoordf(1.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glFogCoordf(2.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glFogCoordf(3.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_TRUE(ctx->triangles_drawn >= 1u);
  const uint8_t *tri = vbo + (size_t)(ctx->triangles_drawn - 1u) * 144u;
  float uv[4];
  static const float factors[3] = {0.75f, 0.5f, 0.25f};
  for (int k = 0; k < 3; k++) {
    memcpy(uv, tri + (size_t)k * 48u + 32u, sizeof(uv));
    ASSERT_TRUE(uv[2] > factors[k] - 1e-6f && uv[2] < factors[k] + 1e-6f);
  }

  static const uint32_t fog_words[GL_PS_FOG_WORDS] = {
    0xc8340600u, 0xc8350601u, 0x081c1af2u,
    0x10081b04u, 0x56081cffu, 0x3e800000u, /* 0.25 */
    0x100a1b05u, 0x560a1cffu, 0x3f000000u, /* 0.5 */
    0x100c1b06u, 0x560c1cffu, 0x3f400000u, /* 0.75 */
  };
  for (size_t i = 0; i < GL_PS_FOG_WORDS; i++) {
    ASSERT_EQ(ps_untex[GL_PS_FOG_SLOT_UNTEX + i], fog_words[i]);
    ASSERT_EQ(ps_tex[GL_PS_FOG_SLOT_TEX + i], fog_words[i]);
  }
  /* Nothing spilled into the alpha test's slots after them. */
  ASSERT_EQ(ps_untex[GL_PS_ALPHA_SLOT_UNTEX], 0u);
  ASSERT_EQ(ps_tex[GL_PS_ALPHA_SLOT_TEX], 0u);

  /* A new colour is a new literal. */
  const GLfloat red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  glFogfv(GL_FOG_COLOR, red);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ps_untex[GL_PS_FOG_SLOT_UNTEX + 5], 0x3f800000u);
  ASSERT_EQ(ps_untex[GL_PS_FOG_SLOT_UNTEX + 8], 0x00000000u);

  /* Off: s_nop in both, and a factor of 1 in the vertex. */
  glDisable(GL_FOG);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  for (size_t i = 0; i < GL_PS_FOG_WORDS; i++) {
    ASSERT_EQ(ps_untex[GL_PS_FOG_SLOT_UNTEX + i], 0xbf800000u);
    ASSERT_EQ(ps_tex[GL_PS_FOG_SLOT_TEX + i], 0xbf800000u);
  }
  tri = vbo + (size_t)(ctx->triangles_drawn - 1u) * 144u;
  memcpy(uv, tri + 32u, sizeof(uv));
  ASSERT_TRUE(uv[2] == 1.0f);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}
/* **The stencil test on the console** (since 2026-09-19; written, not yet seen - gl1-probe's
 * `stencil` is the measurement). A frame that tests stencil binds the stencil surface live -
 * DB_STENCIL_INFO FORMAT STENCIL_8 at the depth surface's 64KB_Z_X, and its bases - and every
 * stencil-tested draw carries the function in DB_DEPTH_CONTROL and the operations, reference and
 * masks in DB_STENCIL_CONTROL and DB_STENCILREFMASK(_BF); a frame that does not leaves the
 * measured recipe's stencil registers as they were. A whole stencil clear is a fill, and the
 * pixel operations read and write the surfaces through their tiling. */
static void test_pm4_gl_stencil_reaches_its_registers(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  #define TRI() do { glBegin(GL_TRIANGLES); glVertex3f(-0.5f, -0.5f, 0.5f); \
    glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f); glEnd(); } while (0)

  /* No stencil test: nothing stencil in the stream. */
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x011u), REG_ABSENT);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x10bu), REG_ABSENT);

  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_EQUAL, 1, 0x0f);
  glStencilOp(GL_KEEP, GL_INCR, GL_REPLACE);
  glStencilMask(0x3c);
  TRI();
  const uint64_t s = (uint64_t)(uintptr_t)ctx->stencil_buffer;
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x011u), 0x20000181u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x013u), (uint32_t)(s >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x015u), (uint32_t)(s >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x01bu), (uint32_t)(s >> 40));
  /* The depth block came first: DB_DEPTH_SIZE_XY is the extent the stencil surface shares. */
  ASSERT_TRUE(last_context_reg(dcb, ctx->dcb_words, 0x007u) != REG_ABSENT);
  const uint32_t dc = last_context_reg(dcb, ctx->dcb_words, 0x200u);
  ASSERT_EQ(dc & 1u, 1u);               /* STENCIL_ENABLE */
  ASSERT_EQ((dc >> 8) & 7u, 2u);        /* STENCILFUNC FRAG_EQUAL */
  ASSERT_EQ(dc & 2u, 0u);               /* the depth test stays off */
  /* **BACKFACE_ENABLE is set, and the back fields carry the back state.** `glStencilFunc` and
   * `glStencilOp` write *both* faces - that is what the non-separate entry points mean - so the
   * two halves are identical here and the register values below are unchanged by it. What the
   * bit buys is the separate arm further down, where they differ. */
  ASSERT_EQ((dc >> 7) & 1u, 1u);
  ASSERT_EQ((dc >> 20) & 7u, 2u);       /* STENCILFUNC_BF, the same FRAG_EQUAL */
  /* fail KEEP 0, zpass REPLACE_TEST 3, zfail ADD_CLAMP 5; the back-face copies at +12. */
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x10bu), 0x00530530u);
  /* ref 1, value mask 0x0f, write mask 0x3c, STENCILOPVAL 1 - front and back. */
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x10cu), 0x013c0f01u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x10du), 0x013c0f01u);

  /* **The two faces apart**, which is GL 2.0's `glStencilOpSeparate` and the whole reason the
   * back fields exist. The shadow-volume idiom is the case: a front face increments where it
   * passes and a back face decrements, so the stencil returns to where it started over a
   * rectangle covered by both. A path that copied the front state into the back fields
   * increments twice and leaves 2 - which is what gl2-probe's `separate-stencil` measured on
   * hardware before this. */
  /* The separate entry points are GL 2.0's and `gl_require_version` gates them, so this context
   * has to say what it targets first - otherwise the calls below are refused with
   * GL_INVALID_OPERATION and the registers keep the values above, which is what a first run of
   * this arm measured. */
  glContextSetVersion(2, 0);
  glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
  glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);
  glStencilFuncSeparate(GL_FRONT, GL_ALWAYS, 0, 0xffu);
  glStencilFuncSeparate(GL_BACK, GL_NEVER, 0, 0xffu);
  TRI();
  {
    const uint32_t sc = last_context_reg(dcb, ctx->dcb_words, 0x10bu);
    /* zpass INCR is ADD_CLAMP 5 at bits 7:4; the back's DECR is SUB_CLAMP 6 at 19:16. */
    ASSERT_EQ((sc >> 4) & 0xfu, 5u);
    ASSERT_EQ((sc >> 16) & 0xfu, 6u);
    ASSERT_TRUE(((sc >> 4) & 0xfu) != ((sc >> 16) & 0xfu));
    const uint32_t dc2 = last_context_reg(dcb, ctx->dcb_words, 0x200u);
    ASSERT_EQ((dc2 >> 7) & 1u, 1u);      /* BACKFACE_ENABLE, or the back fields are ignored */
    ASSERT_EQ((dc2 >> 8) & 7u, 7u);      /* front GL_ALWAYS */
    ASSERT_EQ((dc2 >> 20) & 7u, 0u);     /* back GL_NEVER - and not a copy of the front */
  }
  glStencilFunc(GL_EQUAL, 1, 0x0f);
  glStencilOp(GL_KEEP, GL_INCR, GL_REPLACE);
  TRI();
  /* Bound once a frame: a second draw sets the operations again but not the surface. */
  const uint32_t before = ctx->dcb_words;
  glStencilOp(GL_ZERO, GL_INVERT, GL_DECR_WRAP);
  TRI();
  ASSERT_EQ(last_context_reg(dcb + before, ctx->dcb_words - before, 0x011u), REG_ABSENT);
  /* glStencilOp is (fail, zfail, zpass): ZERO 1 | zpass DECR_WRAP 9 << 4 | zfail INVERT 7 << 8. */
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x10bu), 0x00791791u);

  /* A whole stencil clear: a fill of the clear byte, four times over, at the stencil surface. */
  glClearStencil(0x5a);
  const uint32_t at = ctx->dcb_words;
  gl_hw_clear(ctx, GL_STENCIL_BUFFER_BIT, 0u, 0.0f);
  ASSERT_EQ(dcb[at], 0xc0055000u);
  ASSERT_EQ(dcb[at + 2], 0x5a5a5a5au);
  ASSERT_EQ(dcb[at + 4], (uint32_t)s);
  ASSERT_EQ(dcb[at + 6] & 0x3ffffffu, (uint32_t)ctx->stencil_px & 0x3ffffffu);

  #undef TRI
  glDisable(GL_STENCIL_TEST);

  /* **The pixel operations address the surfaces as the DB tiles them** (since 2026-09-19). The
   * mock's buffers are read as the console's here. A value drawn at a window pixel lands at
   * gl_zs_tiling.h's offset for surface row 479 - y, and only there. The stencil surface's pitch
   * is 640 padded to 768, depth's to 640. The value reads back and copies elsewhere the same
   * way. */
  ctx->zs_tiled = GL_TRUE;
  memset(ctx->stencil_buffer, 0, 768u * 512u);
  glStencilMask(0xff);
  const GLubyte sv = 0xa5;
  glWindowPos2i(3, 5);
  glDrawPixels(1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &sv);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(ctx->stencil_buffer[gl_zs_stencil_offset(768u, 3u, 474u)], 0xa5u);
  size_t set = 0;
  for (size_t i = 0; i < 768u * 512u; i++) set += ctx->stencil_buffer[i] != 0u;
  ASSERT_EQ(set, 1u);
  GLubyte px = 0;
  glReadPixels(3, 5, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, &px);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(px, 0xa5u);
  glWindowPos2i(300, 300);
  glCopyPixels(3, 5, 1, 1, GL_STENCIL);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(ctx->stencil_buffer[gl_zs_stencil_offset(768u, 300u, 179u)], 0xa5u);

  /* Depth the same, in the second column of blocks and the third row; the depth test on and
   * passing, as a depth write needs. */
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_ALWAYS);
  const GLfloat zv = 0.25f;
  glWindowPos2i(130, 200);
  glDrawPixels(1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &zv);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  const float *zat = (const float *)(const void *)((const char *)ctx->depth_buffer +
                                                   gl_zs_depth_offset(640u, 130u, 279u));
  ASSERT_TRUE(*zat == 0.25f);
  GLfloat zr = 0.0f;
  glReadPixels(130, 200, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &zr);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_TRUE(zr == 0.25f);
  glDisable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  ctx->zs_tiled = GL_FALSE;

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **gl_zs_tiling.h's vectors address a 64 KiB block one to one**: every pixel of a 128 x 128
 * depth block has its own float and every pixel of a 256 x 256 stencil block its own byte. That
 * is what a swizzle is; tools/zs-tiling checks the vectors against addrlib itself. Pinned
 * alongside are a few offsets read off the vectors, and the blocks' row-by-row order. */
static void test_pm4_gl_zs_tiling_is_a_permutation(void) {
  static uint8_t seen[65536];
  memset(seen, 0, sizeof(seen));
  for (uint32_t y = 0; y < 128u; y++) {
    for (uint32_t x = 0; x < 128u; x++) {
      const size_t o = gl_zs_depth_offset(128u, x, y);
      ASSERT_TRUE(o < 65536u && (o & 3u) == 0u && !seen[o]);
      seen[o] = 1u;
    }
  }
  memset(seen, 0, sizeof(seen));
  for (uint32_t y = 0; y < 256u; y++) {
    for (uint32_t x = 0; x < 256u; x++) {
      const size_t o = gl_zs_stencil_offset(256u, x, y);
      ASSERT_TRUE(o < 65536u && !seen[o]);
      seen[o] = 1u;
    }
  }
  ASSERT_EQ(gl_zs_depth_offset(384u, 1u, 0u), 0x4u);
  ASSERT_EQ(gl_zs_depth_offset(384u, 0u, 1u), 0x8u);
  ASSERT_EQ(gl_zs_depth_offset(384u, 16u, 0u), 0x2200u);
  ASSERT_EQ(gl_zs_depth_offset(384u, 127u, 127u), 0xf0fcu);
  ASSERT_EQ(gl_zs_depth_offset(384u, 128u, 0u), 0x10000u);
  ASSERT_EQ(gl_zs_depth_offset(384u, 0u, 128u), 0x30000u);
  ASSERT_EQ(gl_zs_stencil_offset(768u, 1u, 0u), 0x1u);
  ASSERT_EQ(gl_zs_stencil_offset(768u, 8u, 16u), 0x0140u ^ 0x0280u);
  ASSERT_EQ(gl_zs_stencil_offset(768u, 256u, 256u), 0x40000u);
}

/* The last value a SET_SH_REG in the stream wrote to shader register `offset`. */
static uint32_t last_sh_reg(const uint32_t *w, size_t n, uint32_t offset) {
  uint32_t found = REG_ABSENT;
  size_t i = 0;
  while (i < n) {
    const uint32_t hdr = w[i];
    if ((hdr >> 30) != 3u) break;
    const uint32_t count = ((hdr >> 16) & 0x3fffu) + 1u;
    if (i + 1u + count > n) break;
    if (((hdr >> 8) & 0xffu) == 0x76u && count >= 2u) {
      for (uint32_t k = 1; k < count; k++) {
        if (w[i + 1] + k - 1u == offset) found = w[i + 1u + k];
      }
    }
    i += 1u + count;
  }
  return found;
}

/* **The three-parameter vertex shader is tools/shader/vs-param3.s, assembled** (since
 * 2026-09-19). These are the words `clang -mcpu=gfx1030` gave for the lines that differ from the
 * two-parameter shader and for its ends, with the four address literals where the builder
 * patches them. */
static void test_pm4_gl_param3_vertex_shader_is_the_assembled_one(void) {
  static uint32_t vs[OOPS_GL_VS_P3_WORDS];
  gl_vs_build_param3(vs, 0x0000001122334400ull, 0x0000005566778800ull);
  ASSERT_EQ(vs[0], 0xbfa00001u);  /* s_inst_prefetch 0x1 */
  ASSERT_EQ(vs[15], 0x341e1c82u); /* v_lshlrev_b32 v15, 2, v14 */
  ASSERT_EQ(vs[16], 0x341c1c86u); /* v_lshlrev_b32 v14, 6, v14: a 64-byte vertex */
  ASSERT_EQ(vs[19], 0x22334400u); /* the vertex buffer */
  ASSERT_EQ(vs[21], 0x00000011u);
  ASSERT_EQ(vs[32], 0xdc388030u); /* global_load_dwordx4 v[20:23], v[18:19], off offset:48 */
  ASSERT_EQ(vs[33], 0x147d0012u);
  ASSERT_EQ(vs[36], 0x66778800u); /* the canary */
  ASSERT_EQ(vs[38], 0x00000055u);
  ASSERT_EQ(vs[39], 0xd70f6a12u); /* v_add_co_u32 v18, vcc_lo, s6, v15 */
  ASSERT_EQ(vs[40], 0x00021e06u);
  ASSERT_EQ(vs[49], 0xf800022fu); /* exp param2, v20, v21, v22, v23 */
  ASSERT_EQ(vs[50], 0x17161514u);
  ASSERT_EQ(vs[51], 0xf80008cfu); /* exp pos0 ... done, after every parameter */
  ASSERT_EQ(vs[63], 0xbf810000u); /* s_endpgm, the last of 64 */
}

/*
 * The descriptors for a 3D image, a cube map and a depth comparison - **checked against the words
 * obSCEne sampled with**, not against a reading of the register tables.
 *
 * `REQ-20260920T0745Z-6c80` (sweep `20260920-103636`, `166-agc/texture-extended`) sampled all
 * three on the part and reported every descriptor and sampler word. Its 3D arm and its 2D
 * control describe the *same memory*, so the only difference between a green texel and nothing
 * is the TYPE field; its two depth arms put one reference either side of the stored depth and
 * came back passing and failing. Those are the numbers below.
 */
static void test_pm4_gl_extended_texture_descriptors_match_the_measured_words(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static const GLubyte texel[4 * 8] = {0};

  /* 2D, the control: TYPE 9, and a sampler with no third clamp and no comparison. */
  GLuint t2d = 0;
  glGenTextures(1, &t2d);
  glBindTexture(GL_TEXTURE_2D, t2d);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  const gl_texture_object_t *o2d = gl_lookup_texture(ctx, t2d);
  ASSERT_TRUE(o2d != (const gl_texture_object_t *)0);
  ASSERT_EQ(o2d->img_desc[3] & 0xf0000fffu, 0x90000facu);
  ASSERT_EQ(o2d->samp_desc[0], 0x12u); /* the control's sampler word, exactly */

  /* 3D: TYPE 0xa, the last slice in WORD4, and CLAMP_Z alongside the other two. */
  GLuint t3d = 0;
  glGenTextures(1, &t3d);
  glBindTexture(GL_TEXTURE_3D, t3d);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, 1, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  const gl_texture_object_t *o3d = gl_lookup_texture(ctx, t3d);
  ASSERT_TRUE(o3d != (const gl_texture_object_t *)0);
  ASSERT_EQ(o3d->img_desc[3] & 0xf0000fffu, 0xa0000facu);
  ASSERT_EQ(o3d->img_desc[4] & 0x1fffu, 1u); /* two slices, so the last is 1 */
  ASSERT_EQ(o3d->samp_desc[0], 0x92u);       /* the 3D arm's sampler word, exactly */

  /*
   * A cube map gets TYPE 0xb **when it has an image to describe, and it has none yet**. Its six
   * faces live on the CPU (`gl_texture_object_t::cube`, and the object's own image fields stay
   * empty for one), so there is no GPU storage for a descriptor to point at - which is why a
   * cube-textured draw is still dropped on this path with a line in the log. `-6c80` settles the
   * *hardware* half - TYPE 0xb samples, and it named the face its texel came from - so what is
   * left is uploading the faces as one array, not finding out what the field is.
   *
   * This asserts the gap rather than the descriptor, because asserting words for an image that
   * does not exist would pass and mean nothing.
   */
  GLuint tcm = 0;
  glGenTextures(1, &tcm);
  glBindTexture(GL_TEXTURE_CUBE_MAP, tcm);
  for (int f = 0; f < 6; f++) {
    glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, texel);
  }
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  const gl_texture_object_t *ocm = gl_lookup_texture(ctx, tcm);
  ASSERT_TRUE(ocm != (const gl_texture_object_t *)0);
  ASSERT_EQ(ocm->target, (GLenum)GL_TEXTURE_CUBE_MAP);
  ASSERT_TRUE(ocm->cube != (gl_tex_level_t *)0); /* the faces are there */
  ASSERT_EQ(ocm->width, 0);                      /* and the object's own image fields stay empty */

  /* A depth comparison: the function in the sampler, and nothing there without the mode. */
  glBindTexture(GL_TEXTURE_2D, t2d);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  ASSERT_EQ(o2d->samp_desc[0], 0x12u); /* the function alone changes nothing */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
  ASSERT_EQ(o2d->samp_desc[0], 0x3012u); /* the depth arms' sampler word, exactly */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_GREATER);
  ASSERT_EQ((o2d->samp_desc[0] >> 12) & 7u, 4u);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  ASSERT_EQ(o2d->samp_desc[0], 0x12u); /* and turning the mode off takes it away again */

  glDeleteTextures(1, &t2d);
  glDeleteTextures(1, &t3d);
  glDeleteTextures(1, &tcm);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * **The six faces uploaded as one array** (2026-09-20). A cube map's faces arrive one at a time
 * as their own images, because that is what the software rasteriser samples; the hardware wants
 * one image of six slices. `gl_tex_hw_prepare` builds it.
 *
 * Each face is given a colour of its own, so the check is where each one *landed*: face f at
 * slice f, in GL's order. A single-colour cube would pass with every face written to slice 0,
 * which is exactly the mistake worth catching - and the slice stride this reads is derived
 * rather than measured (see gl_tex_cube_upload), so it is the thing most worth pinning here.
 */
static void test_pm4_gl_cube_faces_upload_as_one_array(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  enum { DIM = 2 };
  GLuint tcm = 0;
  glGenTextures(1, &tcm);
  glBindTexture(GL_TEXTURE_CUBE_MAP, tcm);

  /* Face f is filled with (f+1, f+1, f+1, 255) - six values nothing else in the image has. */
  for (int f = 0; f < 6; f++) {
    GLubyte face[DIM * DIM * 4];
    for (int p = 0; p < DIM * DIM; p++) {
      face[p * 4 + 0] = (GLubyte)(f + 1);
      face[p * 4 + 1] = (GLubyte)(f + 1);
      face[p * 4 + 2] = (GLubyte)(f + 1);
      face[p * 4 + 3] = 255;
    }
    glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0, GL_RGBA, DIM, DIM, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, face);
  }
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  gl_texture_object_t *o = (gl_texture_object_t *)gl_lookup_texture(ctx, tcm);
  ASSERT_TRUE(o != (gl_texture_object_t *)0);
  /* Nothing is built until a draw needs it: six faces arriving must not each allocate. */
  ASSERT_TRUE(o->garlic_data == (void *)0);
  ASSERT_EQ(o->cube_hw_dirty, GL_TRUE);

  gl_tex_hw_prepare(ctx, o);
  ASSERT_TRUE(o->garlic_data != (void *)0);
  ASSERT_EQ(o->cube_hw_dirty, GL_FALSE);
  ASSERT_EQ(o->cube_hw_dim, (GLsizei)DIM);
  ASSERT_EQ(o->pitch, 64u); /* the 256-byte row pitch every image here has */

  /* Face f at slice f, each slice `pitch * dim` pixels along. */
  const GLubyte *img = (const GLubyte *)o->garlic_data;
  const size_t slice_px = (size_t)o->pitch * (size_t)DIM;
  for (int f = 0; f < 6; f++) {
    for (int y = 0; y < DIM; y++) {
      for (int x = 0; x < DIM; x++) {
        const size_t at = ((size_t)f * slice_px + (size_t)y * o->pitch + (size_t)x) * 4u;
        ASSERT_EQ(img[at + 0], (GLubyte)(f + 1));
        ASSERT_EQ(img[at + 3], 255);
      }
    }
  }

  /* And the descriptor now describes that array rather than nothing: TYPE 0xb, six slices, the
   * face size, and the image's own address. */
  ASSERT_EQ(o->img_desc[3] & 0xf0000fffu, 0xb0000facu);
  ASSERT_EQ(o->img_desc[4] & 0x1fffu, 5u);
  ASSERT_EQ(o->img_desc[0], (uint32_t)(o->garlic_va >> 8));
  ASSERT_EQ((o->img_desc[1] >> 30) & 3u, ((uint32_t)DIM - 1u) & 3u); /* WIDTH_LO */

  /* A face given again marks the array stale, and rebuilding it takes the new face. */
  GLubyte again[DIM * DIM * 4];
  for (int p = 0; p < DIM * DIM; p++) {
    again[p * 4 + 0] = 200;
    again[p * 4 + 1] = 200;
    again[p * 4 + 2] = 200;
    again[p * 4 + 3] = 255;
  }
  glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, 0, GL_RGBA, DIM, DIM, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, again);
  ASSERT_EQ(o->cube_hw_dirty, GL_TRUE);
  gl_tex_hw_prepare(ctx, o);
  img = (const GLubyte *)o->garlic_data;
  ASSERT_EQ(img[5u * slice_px * 4u], 200); /* -Z is slice 5 */
  ASSERT_EQ(img[0], 1);                    /* and +X is untouched at slice 0 */

  glDeleteTextures(1, &tcm);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * The second texture unit's sample slot (tools/shader/tex-prolog2.s).
 *
 * This drives the patcher directly, because a draw's own path reaches it only with two textures
 * bound. What it pins is the placement, which is the part that would be wrong
 * silently: the slot must sit *before* the prolog's exec restore, because a sample taken outside
 * whole-quad mode has no helper pixels and its level of detail is wrong along every quad edge -
 * a seam of the wrong mip level, not a missing picture. Off, the slot is a branch over itself
 * rather than twenty nops, because every fragment of every textured draw runs through it.
 */
static void test_pm4_gl_second_unit_samples_inside_whole_quad_mode(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(256) uint8_t payload[0x4000];
  memset(payload, 0, sizeof(payload));
  ctx->gpu_payload = payload;
  uint32_t *const ps = (uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
  gl_ps_build_textured(ps, 0x0000000123456700ull);

  /* The slot ends before the restore, and the whole-quad mode it is inside began in the prolog. */
  ASSERT_EQ(ps[GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS], 0xbe90037eu);     /* s16, exec_lo */
  ASSERT_EQ(ps[GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS + 1u], 0xbefe097eu); /* s_wqm_b32 */
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX + GL_PS_UNIT1_WORDS], 0xbefe0310u);     /* exec_lo, s16 */

  /* Off: one branch over the slot. */
  gl_ps_patch_unit1(ctx, GL_FALSE);
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX], gl_ps_s_branch(GL_PS_UNIT1_WORDS - 1u));

  /* On: unit 1's coordinate from the fourth parameter, its own descriptor pair at +0x40 and
   * +0x60, and the texel in v16..v19. */
  gl_ps_patch_unit1(ctx, GL_TRUE);
  const uint32_t *const s = ps + GL_PS_UNIT1_SLOT_TEX;
  ASSERT_EQ(s[0], 0xc8080c00u);  /* v_interp_p1_f32 v2, v0, attr3.x */
  ASSERT_EQ(s[6], 0x7e1a550du);  /* v_rcp_f32 v13, v13 */
  ASSERT_EQ(s[9], 0xf40c0500u);  /* s_load_dwordx8 s[20:27], s[0:1], 0x40 */
  ASSERT_EQ(s[10], 0xfa000040u); /* the second unit's image, one stride along the table */
  ASSERT_EQ(s[11], 0xf4080700u); /* s_load_dwordx4 s[28:31], s[0:1], 0x60 */
  ASSERT_EQ(s[12], 0xfa000060u);
  ASSERT_EQ(s[14], 0xf0800f08u); /* image_sample - the same opcode word unit 0 uses */
  /* Into v28..v31, clear of v16..v27 where the general combine form gathers its arguments: a
   * texel left there is overwritten by unit 0's own combine under GL_COMBINE, GL_BLEND and
   * GL_DECAL, and survives under GL_MODULATE - right in the common case, wrong in the ones that
   * need a program. The destination is the operand word's low byte of the second field. */
  ASSERT_EQ(s[15], 0x00e51c02u);
  ASSERT_EQ((s[15] >> 8) & 0xffu, 28u);
  ASSERT_EQ(s[16], 0xbf8c3f70u); /* and the wait, still inside whole-quad mode */
  /* The offsets are the table's own stride, not a number picked here. */
  ASSERT_EQ(s[10] & 0xffu, (uint32_t)OOPS_GL_DESC_UNIT_STRIDE);
  ASSERT_EQ(s[12] & 0xffu, (uint32_t)(OOPS_GL_DESC_UNIT_STRIDE + 0x20u));

  /* And back off again, because a draw that stops using the second unit must stop sampling it. */
  gl_ps_patch_unit1(ctx, GL_FALSE);
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX], gl_ps_s_branch(GL_PS_UNIT1_WORDS - 1u));

  /* **The second combine stage** reads what the first one left. Its slot follows the first, and
   * off it is a branch over itself. */
  ASSERT_TRUE(GL_PS_COMBINE2_SLOT_TEX >= GL_PS_COMBINE_SLOT_TEX + GL_PS_COMBINE_WORDS);
  ASSERT_EQ(ps[GL_PS_COMBINE2_SLOT_TEX], gl_ps_s_branch(GL_PS_COMBINE_WORDS - 1u));
  static const GLubyte white[4] = {255, 255, 255, 255};
  GLuint t1 = 0;
  glGenTextures(1, &t1);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, t1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glActiveTexture(GL_TEXTURE0);
  gl_ps_patch_tex_env_unit1(ctx, GL_TRUE);
  /* GL_MODULATE of unit 1: its texel times what came in. Every argument this gathers must name
   * v28..v31 or v4..v7 - unit 1's own texel, or unit 0's result as GL_PREVIOUS - and never
   * v8..v11 except where the primary colour is asked for by name. The registers are the whole
   * difference between the two stages, so this reads them out of the emitted words. */
  int saw_texel = 0, saw_prev = 0, saw_unit0_texel = 0;
  for (size_t i = 0; i < GL_PS_COMBINE_WORDS; i++) {
    const uint32_t word = ps[GL_PS_COMBINE2_SLOT_TEX + i];
    /* The gather moves an argument's channel into v16.. or v20..: v_mov_b32 v<16+k>, v<src>. */
    for (uint32_t k = 0; k < 8u; k++) {
      if (word == gl_ps_v_mov(16u + k, GL_PS_SRC_V(28u + (k & 3u)))) saw_texel = 1;
      if (word == gl_ps_v_mov(16u + k, GL_PS_SRC_V(4u + (k & 3u)))) saw_prev = 1;
      /* v8..v11 is the primary colour - unit 0's *stage* is v4..v7, and reading the colour here
       * would be the stage-0 mapping left in place. */
      if (word == gl_ps_v_mov(16u + k, GL_PS_SRC_V(8u + (k & 3u)))) saw_unit0_texel = 1;
    }
  }
  ASSERT_TRUE(saw_texel);
  ASSERT_TRUE(saw_prev);
  ASSERT_TRUE(!saw_unit0_texel);
  /* Off again: the branch, so a draw that drops to one unit stops combining a texel it no longer
   * samples - the same hazard the colour sum's slot had. */
  gl_ps_patch_tex_env_unit1(ctx, GL_FALSE);
  ASSERT_EQ(ps[GL_PS_COMBINE2_SLOT_TEX], gl_ps_s_branch(GL_PS_COMBINE_WORDS - 1u));
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * A two-unit draw end to end, with the gate opened the way only a test may.
 *
 * `ctx->hw_multitex` is `OOPS_GL_MULTITEX_MEASURED` in every real context, so no draw takes this
 * path on a console yet (gl_multitex.h). Setting it here is what lets the whole thing be checked
 * before the measurement lands: the four-parameter shader bound, the registers `-8b1c` retired
 * with, an 80-byte vertex carrying the second unit's coordinate, and the second descriptor pair
 * where tex-prolog2.s loads it from.
 */
static void test_pm4_gl_two_unit_draw_binds_the_fourth_parameter(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[65536];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  /* Measured since 2026-09-20 (REQ-9a41), so a real context has this on; it was set by hand here
   * while the gate was closed. */
  ASSERT_EQ(ctx->hw_multitex, (GLboolean)(OOPS_GL_MULTITEX_MEASURED != 0));
  ctx->hw_multitex = GL_TRUE;

  static const GLubyte white[4] = {255, 255, 255, 255};
  GLuint t[2] = {0, 0};
  glGenTextures(2, t);
  for (int u = 0; u < 2; u++) {
    glActiveTexture((GLenum)(GL_TEXTURE0 + u));
    glBindTexture(GL_TEXTURE_2D, t[u]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glEnable(GL_TEXTURE_2D);
  }
  glActiveTexture(GL_TEXTURE0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glMultiTexCoord2f(GL_TEXTURE0, 0.0f, 0.0f);
  glMultiTexCoord2f(GL_TEXTURE1, 0.25f, 0.5f);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glMultiTexCoord2f(GL_TEXTURE0, 1.0f, 0.0f);
  glMultiTexCoord2f(GL_TEXTURE1, 0.75f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glMultiTexCoord2f(GL_TEXTURE0, 0.5f, 1.0f);
  glMultiTexCoord2f(GL_TEXTURE1, 0.5f, 1.0f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_TRUE(ctx->triangles_drawn >= 1u);

  /* Four parameters: the shader and the three registers, as `-8b1c` set them. */
  ASSERT_EQ(ctx->hw_params, 4u);
  const uint64_t vs4 = ((uint64_t)(uintptr_t)payload + OOPS_GL_VS_P4_OFFSET) >> 8;
  ASSERT_EQ(last_sh_reg(dcb, ctx->dcb_words, 0x88u), (uint32_t)vs4);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b1u), 0x6u); /* SPI_VS_OUT_CONFIG */
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b6u), 0x4u); /* SPI_PS_IN_CONTROL */
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x194u), 0x3u); /* SPI_PS_INPUT_CNTL_3 */

  /* An 80-byte vertex, the second unit's coordinate at offset 64 with q defaulted to 1. */
  float tc1[4];
  memcpy(tc1, vbo + 64u, sizeof(tc1));
  ASSERT_FLOAT_NEAR(tc1[0], 0.25f, 1e-6f);
  ASSERT_FLOAT_NEAR(tc1[1], 0.5f, 1e-6f);
  ASSERT_FLOAT_NEAR(tc1[3], 1.0f, 1e-6f);
  memcpy(tc1, vbo + 80u + 64u, sizeof(tc1)); /* the second vertex, one 80-byte stride along */
  ASSERT_FLOAT_NEAR(tc1[0], 0.75f, 1e-6f);

  /* Both descriptor pairs filled, the second one stride along. */
  const uint32_t *const dt = (const uint32_t *)(payload + OOPS_GL_DESC_TABLE_OFFSET);
  const uint32_t *const dt1 = (const uint32_t *)(payload + OOPS_GL_DESC_TABLE_OFFSET +
                                                 OOPS_GL_DESC_UNIT_STRIDE);
  ASSERT_TRUE(dt[0] != 0u || dt[1] != 0u);
  ASSERT_TRUE(dt1[0] != 0u || dt1[1] != 0u);

  /* And the shader's two slots are the sampling and combining forms, not branches. */
  const uint32_t *const ps = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX], 0xc8080c00u); /* v_interp_p1_f32 v2, v0, attr3.x */
  ASSERT_TRUE(ps[GL_PS_COMBINE2_SLOT_TEX] != gl_ps_s_branch(GL_PS_COMBINE_WORDS - 1u));

  /* Dropping the second unit puts both slots back and rebinds the shorter shader, because a
   * stale slot would sample a descriptor pair this draw no longer writes. */
  glActiveTexture(GL_TEXTURE1);
  glDisable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  ctx->dcb_words = 0;
  ctx->hw_frame_active = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f);
  glVertex3f(0.5f, -0.5f, 0.5f);
  glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->hw_params, 2u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x194u), 0x0u);
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX], gl_ps_s_branch(GL_PS_UNIT1_WORDS - 1u));
  ASSERT_EQ(ps[GL_PS_COMBINE2_SLOT_TEX], gl_ps_s_branch(GL_PS_COMBINE_WORDS - 1u));

  glDeleteTextures(2, t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * The four-parameter vertex shader, for a second texture unit: the same program with an 80-byte
 * vertex and a fifth vec4 exported as param3 (tools/shader/vs-param4.s).
 *
 * **This test passed for a day while the program was never in the payload** - `glContextCreate`
 * built the three-parameter shader and not this one, so a draw needing four parameters jumped
 * into an unwritten region and took the GPU down on 2026-09-20. A builder checked against a
 * scratch buffer says the words are right and nothing about whether anyone writes them; that is
 * the gap, and it is named here because this test is where a reader would expect it to be
 * closed. The 80-byte stride is the part worth pinning: it is not a
 * shift, so it is lane * 64 plus lane * 16, and a program that kept the 64-byte shift would read
 * every vertex but the first from the wrong place.
 */
static void test_pm4_gl_param4_vertex_shader_is_the_assembled_one(void) {
  static uint32_t vs[OOPS_GL_VS_P4_WORDS];
  gl_vs_build_param4(vs, 0x0000001122334400ull, 0x0000005566778800ull);
  ASSERT_EQ(vs[0], 0xbfa00001u);  /* s_inst_prefetch 0x1, as the three-parameter program */
  ASSERT_EQ(vs[15], 0x341e1c82u); /* v_lshlrev_b32 v15, 2, v14 */
  ASSERT_EQ(vs[16], 0x34201c84u); /* v_lshlrev_b32 v16, 4, v14  - lane * 16 */
  ASSERT_EQ(vs[17], 0x341c1c86u); /* v_lshlrev_b32 v14, 6, v14  - lane * 64 */
  ASSERT_EQ(vs[18], 0x4a1c1d10u); /* v_add_nc_u32 v14, v16, v14 - lane * 80 */
  ASSERT_EQ(vs[19], 0x4a1c1c08u); /* v_add_nc_u32 v14, s8, v14 */
  ASSERT_EQ(vs[21], 0x22334400u); /* the vertex buffer, two words later than in param3 */
  ASSERT_EQ(vs[23], 0x00000011u);
  ASSERT_EQ(vs[36], 0xdc388040u); /* global_load_dwordx4 v[24:27], v[18:19], off offset:64 */
  ASSERT_EQ(vs[37], 0x187d0012u);
  ASSERT_EQ(vs[40], 0x66778800u); /* the canary */
  ASSERT_EQ(vs[42], 0x00000055u);
  ASSERT_EQ(vs[53], 0xf800022fu); /* exp param2 */
  ASSERT_EQ(vs[55], 0xf800023fu); /* exp param3, v24, v25, v26, v27 */
  ASSERT_EQ(vs[56], 0x1b1a1918u);
  ASSERT_EQ(vs[57], 0xf80008cfu); /* exp pos0 ... done, after every parameter */
  ASSERT_EQ(vs[69], 0xbf810000u); /* s_endpgm, the last of 70 */
}

/* **The colour sum after texturing, through the third parameter** (since 2026-09-19). A textured
 * draw with a secondary colour runs the three-parameter vertex shader - the program address, then
 * SPI_VS_OUT_CONFIG 0x4, SPI_PS_IN_CONTROL 0x3 and SPI_PS_INPUT_CNTL_2 0x2, the unpacked
 * interface obSCEne measured (REQ-20260919T1745Z-9c3e). Its vertices are 64 bytes, the secondary
 * colour in the fourth vec4, and the textured shader's sum slot holds tools/shader/colour-sum.s.
 * A draw after it without a secondary colour switches back. A frame that never sums emits none of
 * it. */
static void test_pm4_gl_colour_sum_takes_the_third_parameter(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  gl_ps_build_textured((uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET), 0u);
  const uint32_t *ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
  const uint64_t pv = (uint64_t)(uintptr_t)payload;

  static const GLubyte texel[4] = {255, 255, 255, 255};
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glEnable(GL_TEXTURE_2D);
  #define TRI() do { glBegin(GL_TRIANGLES); glTexCoord3f(0.0f, 0.0f, 0.75f); \
    glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f); \
    glEnd(); } while (0)

  /* A textured frame without the sum: two parameters, and nothing of the switch. */
  TRI();
  ASSERT_EQ(ctx->hw_params, 2u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x193u), 0u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b6u), 2u);
  ASSERT_EQ(last_sh_reg(dcb, ctx->dcb_words, 0xc8u), (uint32_t)(pv >> 8));
  ASSERT_EQ(ctx->hw_vbo_cursor, 144u);
  for (size_t i = 0; i < GL_PS_SUM_WORDS; i++) {
    ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX + i], 0xbf800000u);
  }

  /* With GL_COLOR_SUM and a secondary colour: the switch, the slot, the wider vertex. */
  glEnable(GL_COLOR_SUM);
  glSecondaryColor3f(0.5f, 0.25f, 0.0f);
  TRI();
  ASSERT_EQ(ctx->hw_params, 3u);
  ASSERT_EQ(last_sh_reg(dcb, ctx->dcb_words, 0xc8u), (uint32_t)((pv + OOPS_GL_VS_P3_OFFSET) >> 8));
  ASSERT_EQ(last_sh_reg(dcb, ctx->dcb_words, 0x88u), (uint32_t)((pv + OOPS_GL_VS_P3_OFFSET) >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b1u), 4u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b6u), 3u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x193u), 2u);
  ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX], 0xc8300800u);      /* v_interp_p1_f32 v12, v0, attr2.x */
  ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX + 6], 0xd5038004u);  /* v_add_f32_e64 v4, v4, v12 clamp */
  ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX + 7], 0x00021904u);
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX], 0xf800180fu);        /* the export, after fog */
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX + 2], 0xbf810000u);
  /* This triangle starts where the first ended, its vertices 64 bytes apart. The primary colour
   * is not summed into on the CPU: the shader adds after the texture. */
  ASSERT_EQ(last_sh_reg(dcb, ctx->dcb_words, 0x8cu), 144u);
  ASSERT_EQ(ctx->hw_vbo_cursor, 144u + 192u);
  for (int k = 0; k < 3; k++) {
    float p2[4], colour[4];
    memcpy(p2, vbo + 144 + 64 * k + 48, sizeof(p2));
    memcpy(colour, vbo + 144 + 64 * k + 16, sizeof(colour));
    ASSERT_TRUE(p2[0] == 0.5f && p2[1] == 0.25f && p2[2] == 0.0f && p2[3] == 0.75f);
    ASSERT_TRUE(colour[0] == 1.0f && colour[1] == 1.0f && colour[2] == 1.0f);
  }

  /* No secondary colour: back to two parameters, and the slot is nothing again. */
  glSecondaryColor3f(0.0f, 0.0f, 0.0f);
  TRI();
  ASSERT_EQ(ctx->hw_params, 2u);
  ASSERT_EQ(last_sh_reg(dcb, ctx->dcb_words, 0xc8u), (uint32_t)(pv >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b1u), 2u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x193u), 0u);
  ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX], 0xbf800000u);
  /* And the sum switched off after a draw that summed: the slot empties with it, or it would
   * add whatever a two-parameter draw leaves in attr2. */
  glSecondaryColor3f(0.5f, 0.25f, 0.0f);
  TRI();
  ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX], 0xc8300800u);
  glDisable(GL_COLOR_SUM);
  TRI();
  ASSERT_EQ(ctx->hw_params, 2u);
  ASSERT_EQ(ps_tex[GL_PS_SUM_SLOT_TEX], 0xbf800000u);
  glEnable(GL_COLOR_SUM);

  /* Untextured, the sum stays per vertex and the vertex stage keeps two parameters. */
  glDisable(GL_TEXTURE_2D);
  glColor3f(0.25f, 0.0f, 0.0f);
  glSecondaryColor3f(0.5f, 0.0f, 0.0f);
  const uint32_t before = ctx->dcb_words;
  const uint32_t cursor = ctx->hw_vbo_cursor;
  TRI();
  ASSERT_EQ(ctx->hw_params, 2u);
  ASSERT_EQ(last_context_reg(dcb + before, ctx->dcb_words - before, 0x193u), REG_ABSENT);
  float summed[4];
  memcpy(summed, vbo + cursor + 16, sizeof(summed));
  ASSERT_TRUE(summed[0] == 0.75f);
  #undef TRI

  glDisable(GL_COLOR_SUM);
  glDeleteTextures(1, &t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **The scanout path** (gl_rx.h; written 2026-09-19, off on the console until REQ-7e21 reports).
 * With the colour buffers made scanout buffers, in the GPU's 64KB_R_X swizzle:
 * - a frame draws into the back one, `CB_COLOR0_ATTRIB3` taking OOPS_GL_RX_ATTRIB3 (the linear
 *   path keeps 0);
 * - a clear fills it whole, padding to 128 x 128 blocks included;
 * - every CPU colour path addresses it through the display tiler's vectors: pixel rectangles,
 *   reads, copies, the accumulation buffer, the front, and the CP's copy, which
 *   glGetFrameReadback detiles for callers that index rows.
 * The display is 200 x 150, so blocks are partly outside it in both directions. */
static void test_pm4_gl_scanout_path_targets(void) {
  enum { W = 200, H = 150, WORDS = 4 * 16384 };
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, W, H);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  static _Alignas(65536) uint32_t back[WORDS], front[WORDS], copy[WORDS];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(back, 0, sizeof(back));
  memset(front, 0, sizeof(front));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  #define TRI() do { glBegin(GL_TRIANGLES); glVertex3f(-0.5f, -0.5f, 0.5f); \
    glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f); glEnd(); } while (0)
  #define AT(buf, x, y) (buf)[(size_t)(((H - 1 - (y)) >> 7) * 2 + ((x) >> 7)) * 16384u + \
    agc_tile_pixel((uint32_t)(x) & 127u, (uint32_t)(H - 1 - (y)) & 127u)]

  /* The linear path first: COLOR_SW_MODE 0. */
  TRI();
  ASSERT_EQ((last_context_reg(dcb, ctx->dcb_words, 0x3b8u) >> 14) & 0x1fu, 0u);

  /* Onto the scanout path, as gl_scanout_begin puts a console there. */
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  ctx->hw_rx = GL_TRUE;
  ctx->color_tiled = GL_TRUE;
  ctx->back_fb = back;
  ctx->front_fb = front;
  gl_draw_targets(ctx);
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x318u), (uint32_t)((uintptr_t)back >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x3b8u), OOPS_GL_RX_ATTRIB3);
  ASSERT_EQ((OOPS_GL_RX_ATTRIB3 >> 14) & 0x1fu, 27u); /* ADDR_SW_64KB_R_X */

  /* A whole clear fills the padded buffer: 2 x 2 blocks, 256 KiB. */
  const uint32_t at = ctx->dcb_words;
  gl_hw_clear(ctx, GL_COLOR_BUFFER_BIT, 0xff000000u, 0.0f);
  ASSERT_EQ(dcb[at], 0xc0055000u);
  ASSERT_EQ(dcb[at + 6] & 0x3ffffffu, (uint32_t)WORDS * 4u);
  #undef TRI

  /* A pixel rectangle lands where the display tiler says, in the second column and second row
   * of blocks, and nowhere else. */
  const GLubyte red[4] = {255, 0, 0, 255};
  glWindowPos2i(130, 10);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  ASSERT_EQ(AT(back, 130, 10), 0xffff0000u);
  size_t set = 0;
  for (size_t i = 0; i < WORDS; i++) set += back[i] != 0u;
  ASSERT_EQ(set, 1u);
  GLubyte out[4] = {0, 0, 0, 0};
  glReadPixels(130, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(out[0] == 255u && out[1] == 0u && out[2] == 0u);
  /* Copied within the buffer, into the first block. */
  glWindowPos2i(5, 140);
  glCopyPixels(130, 10, 1, 1, GL_COLOR);
  ASSERT_EQ(AT(back, 5, 140), 0xffff0000u);

  /* The accumulation buffer reads and writes through the same addressing: loaded from the
   * back, returned into the front. */
  glReadBuffer(GL_BACK);
  glAccum(GL_LOAD, 1.0f);
  glDrawBuffer(GL_FRONT);
  ASSERT_TRUE(ctx->framebuffer == front);
  glAccum(GL_RETURN, 1.0f);
  ASSERT_EQ(AT(front, 130, 10), 0xffff0000u);
  ASSERT_EQ(AT(front, 5, 140), 0xffff0000u);
  glReadBuffer(GL_FRONT);
  glReadPixels(5, 140, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(out[0] == 255u && out[2] == 0u);
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  /* The CP's copy is the back's bytes, still tiled; glGetFrameReadback hands out rows. The
   * sampled read detiles the first word of every fourth cache line - index 0 is one. */
  const GLubyte blue[4] = {0, 0, 255, 255};
  glWindowPos2i(0, H - 1);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, blue);
  memcpy(copy, back, sizeof(copy));
  ctx->readback = copy;
  ctx->hw_frames_confirmed = 1u;
  ctx->readback_of = back;
  const GLuint *lin = glGetFrameReadback();
  ASSERT_TRUE(lin != NULL && lin != copy);
  ASSERT_EQ(lin[(size_t)(H - 1 - 10) * W + 130], 0xffff0000u);
  ASSERT_EQ(lin[(size_t)(H - 1 - 140) * W + 5], 0xffff0000u);
  ASSERT_EQ(lin[0], 0xff0000ffu);
  const GLuint *sampled = glGetFrameReadbackSampled(4u);
  ASSERT_EQ(sampled[0], 0xff0000ffu);
  /* And glReadPixels through the copy reads the tiled bytes the same way. */
  glReadPixels(130, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(out[0] == 255u && out[2] == 0u);

  /*
   * **A copy that is no longer current must not be handed out.**
   *
   * The CP makes it at a submit, and a CPU pixel rectangle builds no command stream - so nothing
   * re-copies the buffer after one, and the copy is a picture of the frame *before* those
   * pixels. The `memcpy` above hides that by syncing it by hand; poisoning it here is what the
   * console does by simply not submitting.
   *
   * This is what six of gl1-probe's eight hardware failures were - `raster-ops`,
   * `pixel-transfer`, `pixel-fragments`, `index-pixels`, `accumulation` and `array-types`, all
   * reading through `frame()` - while `read-pixels` passed beside them, because glReadPixels
   * already asked `gl_color_read_source` which buffer was current and this did not.
   */
  for (size_t i = 0; i < sizeof(copy) / sizeof(copy[0]); i++) copy[i] = 0xdeadbeefu;
  ctx->readback_of = back; /* the copy claims to be current ... */
  const GLubyte green[4] = {0, 255, 0, 255};
  glWindowPos2i(2, H - 1);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green); /* ... and this is what makes it not */
  const GLuint *fresh = glGetFrameReadback();
  ASSERT_TRUE(fresh != NULL);
  ASSERT_EQ(fresh[2], 0xff00ff00u);
  ASSERT_EQ(fresh[0], 0xff0000ffu); /* and the pixel before it is still there */
  #undef AT

  ctx->readback = NULL;
  ctx->hw_frames_confirmed = 0u;
  ctx->hw_rx = GL_FALSE;
  ctx->color_tiled = GL_FALSE;
  ctx->front_fb = NULL;
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* **The front buffer on the console** (since 2026-09-19). A frame begun after glDrawBuffer(GL_FRONT)
 * draws into the front surface: CB_COLOR0_BASE is its address. A clear under GL_FRONT_AND_BACK
 * fills both buffers. And the CP's copy of a buffer goes stale when the CPU writes into it, so a
 * glReadPixels after a glDrawPixels reads the buffer, not a copy made before the write - which
 * it did until then. */
static void test_pm4_gl_front_buffer_targets(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  static uint32_t readback[64 * 64];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  #define TRI() do { glBegin(GL_TRIANGLES); glVertex3f(-0.5f, -0.5f, 0.5f); \
    glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f); glEnd(); } while (0)

  const uint64_t back = (uint64_t)(uintptr_t)ctx->back_fb;
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x318u), (uint32_t)(back >> 8));

  glDrawBuffer(GL_FRONT);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  const uint64_t front = (uint64_t)(uintptr_t)ctx->front_fb;
  ASSERT_TRUE(front != 0u && front != back);
  /* The console submits the open frame here; the mock has no queue, so its frame is ended by
   * hand, and the next draw begins one on the front. */
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x318u), (uint32_t)(front >> 8));

  /* Both: a clear is two colour fills, the back's and the front's. */
  glDrawBuffer(GL_FRONT_AND_BACK);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  const uint32_t at = ctx->dcb_words;
  gl_hw_clear(ctx, GL_COLOR_BUFFER_BIT, 0xff102030u, 0.0f);
  size_t fills = 0;
  GLboolean saw_back = GL_FALSE, saw_front = GL_FALSE;
  for (uint32_t i = at; i + 6 < ctx->dcb_words; i++) {
    if (dcb[i] != 0xc0055000u || dcb[i + 2] != 0xff102030u) continue;
    fills++;
    if (dcb[i + 4] == (uint32_t)back) saw_back = GL_TRUE;
    if (dcb[i + 4] == (uint32_t)front) saw_front = GL_TRUE;
  }
  ASSERT_EQ(fills, 2u);
  ASSERT_TRUE(saw_back && saw_front);
  /* **And a draw reaches both**, since 2026-09-20: the second colour target is bound to the
   * front, both masks carry MRT1, the export format carries COL1, and the pixel shaders export
   * twice. The values are the ones obSCEne's `-3f62` drew with. The clear above left a frame
   * open, and the colour targets are bound where a frame begins, so it is ended by hand here as
   * it was for the front. */
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  const uint32_t *const ps_tex = (const uint32_t *)((const char *)ctx->gpu_payload +
                                                    OOPS_GL_PS_TEX_OFFSET);
  const uint32_t *const ps_untex = (const uint32_t *)((const char *)ctx->gpu_payload +
                                                      OOPS_GL_PS_UNTEX_OFFSET);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x327u), (uint32_t)(front >> 8));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x391u), (uint32_t)(front >> 40));
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x32bu), 0x000088a8u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x08eu), 0x000000ffu);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x08fu), 0x000000ffu);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1c5u), 0x00000099u);
  /* Two exports, `done` on the second only - the first would end the wave otherwise. */
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX], 0xf800100fu);
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX + 2u], 0xf800181fu);
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX + 4u], 0xbf810000u);
  ASSERT_EQ(ps_untex[GL_PS_EXPORT_UNTEX], 0xf800100fu);
  ASSERT_EQ(ps_untex[GL_PS_EXPORT_UNTEX + 2u], 0xf800181fu);

  /* **And it blends there too** (since 2026-09-21). Blending is per target on this part - one
   * CB_BLENDn_CONTROL each, `R_028780_CB_BLEND0_CONTROL + i * 4` in radeonsi's own loop - and
   * only MRT0's was ever written, so a draw under GL_FRONT_AND_BACK blended into the back and
   * *replaced* in the front. That is what gl1-probe's `front-and-back` measured on hardware:
   * its back was the colour GL asks for and its front was not. Both registers now carry the one
   * blend state GL has. */
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  const uint32_t blend0 = last_context_reg(dcb, ctx->dcb_words, 0x1e0u);
  ASSERT_TRUE(blend0 != 0u && blend0 != REG_ABSENT);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1e1u), blend0);

  /* Back to one buffer: the second target is unbound - out of both masks and with no format -
   * and the export is the single one again. A stale second target would keep writing a buffer
   * GL no longer names. Its blend control goes back to zero with it, for the same reason the
   * format does: no register left pointing at a buffer GL stopped naming. */
  glDrawBuffer(GL_BACK);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1e0u), blend0);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1e1u), 0u);
  glDisable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ZERO);

  glDrawBuffer(GL_BACK);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x32bu), 0u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x08eu), 0x0000000fu);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x08fu), 0x0000000fu);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1c5u), 0x00000009u);
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX], 0xf800180fu);
  ASSERT_EQ(ps_tex[GL_PS_EXPORT_TEX + 2u], 0xbf810000u);
  ASSERT_EQ(ps_untex[GL_PS_EXPORT_UNTEX], 0xf800180fu);
  ASSERT_EQ(ps_untex[GL_PS_EXPORT_UNTEX + 2u], 0xbf810000u);
  #undef TRI

  /* A CPU write makes the CP's copy stale. The copy here holds a sentinel. After glDrawPixels
   * into the back, glReadPixels has to read the back itself. */
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);
  for (size_t i = 0; i < 64u * 64u; i++) readback[i] = 0xff00ff00u;
  ctx->readback = readback;
  ctx->hw_frames_confirmed = 1u;
  ctx->readback_of = ctx->back_fb;
  const GLubyte red[4] = {255, 0, 0, 255};
  glWindowPos2i(5, 5);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
  ASSERT_TRUE(ctx->readback_of == NULL);
  GLubyte out[4] = {0, 0, 0, 0};
  glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(out[0] == 255u && out[1] == 0u && out[2] == 0u);
  /* And a copy that is current is what a read takes - the front's is not the back's. */
  ctx->readback_of = ctx->back_fb;
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(out[0] == 0u && out[1] == 255u && out[2] == 0u);
  glReadBuffer(GL_FRONT);
  glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
  ASSERT_TRUE(!(out[0] == 0u && out[1] == 255u && out[2] == 0u));
  glReadBuffer(GL_BACK);
  ctx->readback = NULL;
  ctx->hw_frames_confirmed = 0u;
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* The textured pixel shader samples with a level of detail and divides q per fragment (since
 * 2026-09-19). Its sampling words are `tools/shader/tex-prolog.s`'s, assembled - the file also
 * assembles the image_sample_lz this replaced to the word the tree had - and the vertex carries
 * s, t and q undivided, q in the texture parameter's w. */
static void test_pm4_gl_textured_shader_samples_with_lod_and_divides_q(void) {
  static uint32_t ps[OOPS_GL_PS_TEX_WORDS];
  gl_ps_build_textured(ps, 0x0000000123456700ull);
  static const uint32_t prolog[28] = {
    0xbe90037eu, 0xbefe097eu,                         /* s_mov_b32 s16, exec_lo; s_wqm_b32 */
    0xc8080400u, 0xc8090401u, 0xc80c0500u, 0xc80d0501u, /* s, t */
    0xc8300700u, 0xc8310701u, 0x7e18550cu,             /* q, and its reciprocal */
    0x10041902u, 0x10061903u,                          /* s/q, t/q */
    0xc8200000u, 0xc8210001u, 0xc8240100u, 0xc8250101u, /* the colour */
    0xc8280200u, 0xc8290201u, 0xc82c0300u, 0xc82d0301u,
    0xf40c0100u, 0xfa000000u, 0xf4080300u, 0xfa000020u, /* the descriptors */
    0xbf8cc07fu,
  };
  /* The prolog follows the stipple slot, which follows the canary block. */
  const size_t pro_at = GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS;
  for (size_t i = 0; i < 24; i++) {
    if (ps[pro_at + i] != prolog[i]) printf("\n[prolog word %zu]\n", pro_at + i);
    ASSERT_EQ(ps[pro_at + i], prolog[i]);
  }
  /* Then the sample slot - the 2D form, which is what a context with no cube map draws - and
   * the wait after it, whichever sample ran. */
  ASSERT_EQ(pro_at + 24u, (size_t)GL_PS_SAMPLE_SLOT_TEX);
  ASSERT_EQ(ps[GL_PS_SAMPLE_SLOT_TEX], 0xf0800f08u); /* image_sample, not _lz */
  ASSERT_EQ(ps[GL_PS_SAMPLE_SLOT_TEX + 1u], 0x00610402u);
  ASSERT_EQ(ps[GL_PS_SAMPLE_SLOT_TEX + GL_PS_SAMPLE_WORDS], 0xbf8c3f70u); /* the wait */
  /* Then the second unit's slot - a branch over it, one unit being what this context draws -
   * and only after it the restore, so that both samples are taken in whole-quad mode. */
  ASSERT_EQ(GL_PS_SAMPLE_SLOT_TEX + GL_PS_SAMPLE_WORDS + 1u, (size_t)GL_PS_UNIT1_SLOT_TEX);
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX], gl_ps_s_branch(GL_PS_UNIT1_WORDS - 1u));
  ASSERT_EQ(ps[GL_PS_UNIT1_SLOT_TEX + GL_PS_UNIT1_WORDS], 0xbefe0310u); /* exec_lo, s16 */
  for (size_t i = 0; i < OOPS_GL_PS_TEX_WORDS; i++) ASSERT_TRUE(ps[i] != 0xf09c0f08u); /* no _lz */
  ASSERT_EQ(ps[5], 0x23456700u); /* the canary address, low and high */
  ASSERT_EQ(ps[7], 0x00000001u);
  /* The slots where their patchers write them, and the export after the last. */
  ASSERT_EQ(ps[GL_PS_COMBINE_SLOT_TEX], 0x10081104u);
  ASSERT_EQ(ps[GL_PS_COMBINE_SLOT_TEX + 4], gl_ps_s_branch(GL_PS_COMBINE_WORDS - 5u));
  ASSERT_EQ(ps[GL_PS_FOG_SLOT_TEX], 0xbf800000u);
  ASSERT_EQ(ps[GL_PS_ALPHA_SLOT_TEX], 0xbf800000u);
  ASSERT_EQ(ps[GL_PS_ALPHA_SLOT_TEX + 4], 0xf800180fu);
  ASSERT_EQ(ps[GL_PS_ALPHA_SLOT_TEX + 6], 0xbf810000u);

  /* The vertex: s and t as given, q in w - and a q of 0 as 1, gl_q_inv's rule. */
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  glBegin(GL_TRIANGLES);
  glTexCoord4f(2.0f, 4.0f, 0.0f, 2.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord4f(1.0f, 3.0f, 0.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 0.25f);            glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  float uv[4];
  memcpy(uv, vbo + 32u, sizeof(uv));
  ASSERT_TRUE(uv[0] == 2.0f && uv[1] == 4.0f && uv[3] == 2.0f);
  memcpy(uv, vbo + 48u + 32u, sizeof(uv));
  ASSERT_TRUE(uv[0] == 1.0f && uv[1] == 3.0f && uv[3] == 1.0f);
  memcpy(uv, vbo + 96u + 32u, sizeof(uv));
  ASSERT_TRUE(uv[0] == 0.5f && uv[1] == 0.25f && uv[3] == 1.0f);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/*
 * The polygon stipple on the console: the discard in both pixel shaders, the fragment's position
 * turned on for it, and the mask in the form the shader reads.
 *
 * The mask check is the one that matters. The shader does two operations where the software
 * rasteriser does four, because gl_ps_patch_stipple rotates the rows for the window height and
 * reverses each row's bits first. This asserts the two agree for every pixel of a 32x32 window
 * of the framebuffer - if the rotation or the reversal were dropped, a stippled polygon would
 * come out stippled *differently* on the console, which is exactly the kind of wrong that looks
 * right in a screenshot.
 */
static void test_pm4_gl_polygon_stipple_discards_in_the_shader(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;
  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;
  #define TRI() do { glBegin(GL_TRIANGLES); glVertex3f(-0.9f, -0.9f, 0.5f); \
    glVertex3f(0.9f, -0.9f, 0.5f); glVertex3f(0.0f, 0.9f, 0.5f); glEnd(); } while (0)

  /* A mask with no symmetry that could hide a rotation or a reversal: every row different, and
   * no row a palindrome. */
  GLubyte mask[128];
  for (int r = 0; r < 32; r++) {
    const uint32_t row = 0x8f1c3a00u ^ ((uint32_t)r * 0x01010107u);
    for (int b = 0; b < 4; b++) mask[r * 4 + b] = (GLubyte)(row >> (24 - b * 8));
  }
  glPolygonStipple(mask);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);
  glEnable(GL_POLYGON_STIPPLE);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();

  const uint32_t *const ps_tex = (const uint32_t *)(payload + OOPS_GL_PS_TEX_OFFSET);
  const uint32_t *const ps_untex = (const uint32_t *)(payload + OOPS_GL_PS_UNTEX_OFFSET);
  const uint32_t *const table = (const uint32_t *)(payload + OOPS_GL_STIPPLE_OFFSET);
  /* The fragment's position, in both registers, as `-c7d4` set them. */
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b3u), 0x00000302u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b4u), 0x00000302u);
  /* The discard, in both shaders: tools/shader/polygon-stipple.s, with the table's address in
   * the two literals. */
  const uint64_t table_va = (uint64_t)(uintptr_t)table;
  const uint32_t *const slots[2] = {ps_tex + GL_PS_STIPPLE_SLOT, ps_untex + GL_PS_STIPPLE_SLOT};
  for (int s = 0; s < 2; s++) {
    ASSERT_EQ(slots[s][0], 0x7e040f02u);  /* v_cvt_u32_f32_e32 v2, v2 */
    ASSERT_EQ(slots[s][1], 0x7e060f03u);  /* v_cvt_u32_f32_e32 v3, v3 */
    ASSERT_EQ(slots[s][5], (uint32_t)table_va);
    ASSERT_EQ(slots[s][7], (uint32_t)(table_va >> 32));
    ASSERT_EQ(slots[s][8], 0xdc308000u);  /* global_load_dword v3, v3, s[2:3] */
    ASSERT_EQ(slots[s][11], 0xbf8c3f70u); /* the wait, a word both shaders already carry */
    ASSERT_EQ(slots[s][15], 0x877e6a7eu); /* s_and_b32 exec_lo, exec_lo, vcc_lo */
  }
  /* It runs before the interpolation that uses v2 and v3 as scratch. */
  ASSERT_TRUE(GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS <= GL_PS_COMBINE_SLOT_TEX);

  /* **The table says what the software rasteriser says.** The shader keeps the fragment at
   * (x, y) when bit `x & 31` of row `y & 31` is set, with y counting down from the top; the
   * rasteriser keeps it when bit `31 - (x & 31)` of `polygon_stipple[(height - 1 - y) & 31]` is.
   */
  for (uint32_t y = 0; y < 32u; y++) {
    const uint32_t soft = ctx->polygon_stipple[((uint32_t)ctx->height - 1u - y) & 31u];
    for (uint32_t x = 0; x < 32u; x++) {
      ASSERT_EQ((table[y & 31u] >> (x & 31u)) & 1u, (soft >> (31u - (x & 31u))) & 1u);
    }
  }

  /* Off again: no discard, and the position is not asked for. A draw that kept the slot would
   * stipple a polygon GL no longer stipples. */
  glDisable(GL_POLYGON_STIPPLE);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b3u), 0x00000002u);
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b4u), 0x00000002u);
  ASSERT_EQ(ps_tex[GL_PS_STIPPLE_SLOT], 0xbf800000u);
  ASSERT_EQ(ps_untex[GL_PS_STIPPLE_SLOT], 0xbf800000u);

  /* And a stipple GL does not apply to a polygon's outline (3.5.2) is not applied here either. */
  glEnable(GL_POLYGON_STIPPLE);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  ctx->hw_frame_active = GL_FALSE;
  ctx->dcb_words = 0;
  TRI();
  ASSERT_EQ(last_context_reg(dcb, ctx->dcb_words, 0x1b3u), 0x00000002u);
  ASSERT_EQ(ps_tex[GL_PS_STIPPLE_SLOT], 0xbf800000u);
  #undef TRI
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}

/* A second texture unit on the console: left out of the draw, said once - the pixel shader samples
 * one texture until it takes a second coordinate. Unit 0 still draws as it did. */
static void test_pm4_gl_unit1_is_the_base_when_unit0_has_no_texture(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence[4] = {0x11111111u};
  static _Alignas(64) uint32_t canary[16];
  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  static const GLubyte white[4] = {255, 255, 255, 255};
  GLuint t = 0;
  glGenTextures(1, &t);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glEnable(GL_TEXTURE_2D);
  glActiveTexture(GL_TEXTURE0);
  ASSERT_EQ(ctx->hw_unit_logged, GL_FALSE);
  glBegin(GL_TRIANGLES);
  glVertex3f(-0.5f, -0.5f, 0.5f); glVertex3f(0.5f, -0.5f, 0.5f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();
  ASSERT_EQ(ctx->triangles_drawn, 1u);
  /* **Unit 1 is the base stage here, and the draw is textured.**
   *
   * This asserted the opposite until 2026-09-22 - that the unit was logged as left out and the
   * draw went untextured - which recorded the old behaviour as though it were intended. It was
   * a bug: a disabled unit passes the fragment colour through, so unit 0 need not be textured
   * for unit 1 to apply. Neverball puts its surface texture on unit 1 and its whole world drew
   * flat because of this. */
  ASSERT_EQ(ctx->hw_unit_logged, GL_FALSE); /* nothing was left out */
  ASSERT_NE(ctx->hw_frame_tex, 0u);         /* the draw sampled unit 1's texture */

  glDeleteTextures(1, &t);
  glContextDestroy(ctx_handle);
  oops_display_close(disp);
}
void run_unit_tests_pm4(void);

void run_unit_tests_pm4(void) {
  TEST_SUITE_BEGIN("PM4 Static Command Stream Validator (RDNA2 GFX10.3)");
  RUN_TEST(test_pm4_synthetic_valid_stream);
  RUN_TEST(test_pm4_real_sdk_draw_stream);
  RUN_TEST(test_pm4_gl_hardware_depth_stream);
  RUN_TEST(test_pm4_gl_hardware_texture_stream);
  RUN_TEST(test_pm4_gl_hardware_cull_and_color_mask_stream);
  RUN_TEST(test_pm4_gl_honours_the_gl_cube_oracle_record);
  RUN_TEST(test_pm4_gl_tex_env_reaches_the_combine_slot);
  RUN_TEST(test_pm4_gl_combine_encoder_matches_the_assembler);
  RUN_TEST(test_pm4_gl_combine_programs_compute_what_software_does);
  RUN_TEST(test_pm4_gl_color_sum_reaches_the_vertex_colour);
  RUN_TEST(test_pm4_gl_fog_reaches_both_shaders_and_the_vertex);
  RUN_TEST(test_pm4_gl_unit1_is_the_base_when_unit0_has_no_texture);
  RUN_TEST(test_pm4_gl_textured_shader_samples_with_lod_and_divides_q);
  RUN_TEST(test_pm4_gl_stencil_reaches_its_registers);
  RUN_TEST(test_pm4_gl_zs_tiling_is_a_permutation);
  RUN_TEST(test_pm4_gl_front_buffer_targets);
  RUN_TEST(test_pm4_gl_polygon_stipple_discards_in_the_shader);
  RUN_TEST(test_pm4_gl_scanout_path_targets);
  RUN_TEST(test_pm4_gl_param3_vertex_shader_is_the_assembled_one);
  RUN_TEST(test_pm4_gl_param4_vertex_shader_is_the_assembled_one);
  RUN_TEST(test_pm4_gl_extended_texture_descriptors_match_the_measured_words);
  RUN_TEST(test_pm4_gl_cube_faces_upload_as_one_array);
  RUN_TEST(test_pm4_gl_second_unit_samples_inside_whole_quad_mode);
  RUN_TEST(test_pm4_gl_two_unit_draw_binds_the_fourth_parameter);
  RUN_TEST(test_pm4_gl_colour_sum_takes_the_third_parameter);
  RUN_TEST(test_pm4_gl_clip_planes_reach_their_registers);
  RUN_TEST(test_pm4_gl_scissor_reaches_its_registers);
  RUN_TEST(test_pm4_gl_depth_range_reaches_the_viewport);
  RUN_TEST(test_pm4_gl_smooth_polygon_carries_three_edge_distances);
  RUN_TEST(test_pm4_gl_descriptor_ring_wraps_into_a_submission);
  RUN_TEST(test_pm4_gl_depth_range_changes_within_a_frame);
  RUN_TEST(test_pm4_gl_logic_op_and_blend_constant_reach_their_registers);
  RUN_TEST(test_pm4_gl_vertex_ring_submits_before_it_wraps);
  RUN_TEST(test_pm4_gl_a_discarding_shader_sets_kill_enable);
  RUN_TEST(test_pm4_gl_alpha_test_sets_kill_enable);
  RUN_TEST(test_pm4_gl_mip_chain_reaches_the_descriptor);
  RUN_TEST(test_pm4_gl_scissored_clear_is_drawn);
  RUN_TEST(test_pm4_gl_volume_and_cube_sample_on_hardware);
  RUN_TEST(test_pm4_gl_depth_texture_samples_and_compares_on_hardware);
  RUN_TEST(test_pm4_gl_occlusion_query_counts_on_the_gpu);
  RUN_TEST(test_pm4_gl_smooth_points_and_lines_carry_their_coverage);
  RUN_TEST(test_pm4_gl_texture_wrap_and_border_reach_the_sampler);
  RUN_TEST(test_pm4_gl_polygon_offset_reaches_its_registers);
  RUN_TEST(test_pm4_detects_truncated_buffer);
  RUN_TEST(test_pm4_detects_invalid_reg_bounds);
  RUN_TEST(test_pm4_detects_depth_invariants);
  RUN_TEST(test_pm4_detects_zero_vertex_draw);
  RUN_TEST(test_pm4_detects_unaligned_release_mem);
}

