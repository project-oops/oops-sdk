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
  __attribute__((aligned(8))) uint32_t fence = 0x11111111u;
  __attribute__((aligned(8))) uint32_t canary = 0xaaaaaaaa;

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));

  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = &fence;
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

static void test_pm4_gl_hardware_texture_stream(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence = 0x11111111u;
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));

  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = &fence;
  ctx->canary = &canary;
  ctx->use_hardware = GL_TRUE;
  ctx->hw_frame_active = GL_FALSE;

  /* Step 1: Draw triangle without texture (Untextured Gouraud shader at 0x300) */
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
  ASSERT_EQ(report.spi_shader_pgm_ps, payload_va + 0x300u);

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

  /* Draw textured triangle: shader should switch to 0x200, User SGPR 0/1 to descriptor table 0x900 */
  glBegin(GL_TRIANGLES);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-0.5f, -0.5f, 0.5f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(0.5f, -0.5f, 0.5f);
  glTexCoord2f(0.5f, 1.0f); glVertex3f(0.0f, 0.5f, 0.5f);
  glEnd();

  rc = oops_pm4_validate_stream(ctx->dcb_mem, ctx->dcb_words, &report);
  if (rc != 0) printf("\n[PM4 Texture Step 2 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.spi_shader_pgm_ps, payload_va + 0x200u);
  ASSERT_EQ(report.spi_shader_user_data_ps_01, payload_va + 0x900u);

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
  static _Alignas(64) uint32_t fence = 0x11111111u;
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));

  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = &fence;
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
  static _Alignas(64) uint32_t fence = 0x11111111u;
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = &fence;
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
   * `CB_COLOR0_ATTRIB3` bits 18:14 are COLOR_SW_MODE. oops-gl writes 0, because the measured
   * 0x08c6c000 from the compositor's surface carries 27 (64KB_R_X) and streaked this scratch
   * buffer when it was used. Pinned because the two values differ in one field and are easy
   * to copy across from a capture by mistake - the arithmetic is (v >> 14) & 0x1f. */
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


/* glDepthRange reaches the viewport registers, and a reversed range is accepted.
 *
 * The three cases are chosen so no two share a ZSCALE or a ZOFFSET: 0..1 gives (0.5, 0.5),
 * 0..0.5 gives (0.25, 0.25), and the reversed 1..0 gives (-0.5, 0.5). A build that swapped the
 * two registers, or that sorted the arguments, fails on one of them.
 */
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
  static _Alignas(64) uint32_t fence = 0x11111111u;
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = &fence;
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
  uint32_t *ps_tex = (uint32_t *)((char *)payload + 0x200);

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

  /* GL_ADD and GL_DECAL are stored and reported but not implemented on this path; they must
   * fall back to the multiply rather than to something plausible-looking. */
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  for (size_t i = 0; i < 4; i++)
    ASSERT_EQ(ps_tex[GL_PS_COMBINE_SLOT_TEX + i], modulate[i]);

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

static void test_pm4_gl_scissor_reaches_its_registers(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence = 0x11111111u;
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
    ctx->fence = &fence;
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

static void test_pm4_gl_depth_range_reaches_the_viewport(void) {
  oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 640, 480);
  void *ctx_handle = glContextCreate(disp);
  gl_context_t *ctx = (gl_context_t *)ctx_handle;

  static _Alignas(4096) uint32_t dcb[4096];
  static _Alignas(256) uint8_t payload[0x4000];
  static _Alignas(256) uint8_t vbo[4096];
  static _Alignas(64) uint32_t fence = 0x11111111u;
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
    ctx->fence = &fence;
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

  /* Out of range is clamped rather than refused - the specification says clamp. */
  glDepthRange(-5.0, 7.0);
  ASSERT_EQ(glGetError(), GL_NO_ERROR);

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
  static _Alignas(64) uint32_t fence = 0x11111111u;
  static _Alignas(64) uint32_t canary[16];

  memset(dcb, 0, sizeof(dcb));
  memset(payload, 0, sizeof(payload));
  memset(vbo, 0, sizeof(vbo));
  ctx->dcb_mem = dcb;
  ctx->dcb_capacity_dw = 4096;
  ctx->dcb_words = 0;
  ctx->gpu_payload = payload;
  ctx->vbo_mem = vbo;
  ctx->fence = &fence;
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
  RUN_TEST(test_pm4_gl_clip_planes_reach_their_registers);
  RUN_TEST(test_pm4_gl_scissor_reaches_its_registers);
  RUN_TEST(test_pm4_gl_depth_range_reaches_the_viewport);
  RUN_TEST(test_pm4_gl_polygon_offset_reaches_its_registers);
  RUN_TEST(test_pm4_detects_truncated_buffer);
  RUN_TEST(test_pm4_detects_invalid_reg_bounds);
  RUN_TEST(test_pm4_detects_depth_invariants);
  RUN_TEST(test_pm4_detects_zero_vertex_draw);
  RUN_TEST(test_pm4_detects_unaligned_release_mem);
}

