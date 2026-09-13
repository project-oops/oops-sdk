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

      case OOPS_AGC_PM4_SET_UCONFIG_REG: {
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
  uint32_t total_flushed_words = words_before_flush + 24; /* 8 RELEASE_MEM + 16 NOP pads */
  rc = oops_pm4_validate_stream(ctx->dcb_mem, total_flushed_words, &report);
  if (rc != 0) printf("\n[PM4 Step 5 error]: %s\n", report.last_error);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.release_mem_count, 1u);
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
  ASSERT_EQ(dt[3], 0x90000688u); /* SQ_RSRC_IMG_2D */
  ASSERT_EQ(dt[9], 0x00fff000u); /* Sampler MAX_LOD */

  /* Flush and verify packet stream */
  uint32_t words_before_flush = ctx->dcb_words;
  gl_hw_flush(ctx);
  uint32_t total_flushed_words = words_before_flush + 24;
  rc = oops_pm4_validate_stream(ctx->dcb_mem, total_flushed_words, &report);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(report.error_count, 0u);
  ASSERT_EQ(report.release_mem_count, 1u);

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

void run_unit_tests_pm4(void);

void run_unit_tests_pm4(void) {
  TEST_SUITE_BEGIN("PM4 Static Command Stream Validator (RDNA2 GFX10.3)");
  RUN_TEST(test_pm4_synthetic_valid_stream);
  RUN_TEST(test_pm4_real_sdk_draw_stream);
  RUN_TEST(test_pm4_gl_hardware_depth_stream);
  RUN_TEST(test_pm4_gl_hardware_texture_stream);
  RUN_TEST(test_pm4_detects_truncated_buffer);
  RUN_TEST(test_pm4_detects_invalid_reg_bounds);
  RUN_TEST(test_pm4_detects_depth_invariants);
  RUN_TEST(test_pm4_detects_zero_vertex_draw);
  RUN_TEST(test_pm4_detects_unaligned_release_mem);
}

