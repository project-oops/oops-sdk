#ifndef OOPS_TARGET_H
#define OOPS_TARGET_H

/*
 * Platform Target Modes for oops-sdk.
 *
 * Configures platform generation, graphics backend, video out attributes,
 * and system library linkage.
 */

#define OOPS_TARGET_ORBIS 1    /* Base PS4 (Liverpool / GCN 2 / GFX7) */
#define OOPS_TARGET_NEO 2      /* PS4 Pro / Neo BC (Neo / GCN 4 / GFX8) */
#define OOPS_TARGET_PROSPERO 3 /* PS5 Base (Oberon / RDNA 2 / GFX10.3) */
#define OOPS_TARGET_TRINITY 4  /* PS5 Pro (Viola / RDNA 3 / GFX11-hybrid) */

#ifndef OOPS_TARGET
#define OOPS_TARGET OOPS_TARGET_PROSPERO
#endif

/* Target classification helpers */
#define OOPS_TARGET_IS_PS4                                                     \
  (OOPS_TARGET == OOPS_TARGET_ORBIS || OOPS_TARGET == OOPS_TARGET_NEO)
#define OOPS_TARGET_IS_PS5                                                     \
  (OOPS_TARGET == OOPS_TARGET_PROSPERO || OOPS_TARGET == OOPS_TARGET_TRINITY)

/*
 * OOPS_TARGET is what a binary is COMPILED FOR, fixed at build time. It is not
 * the environment the binary turns out to run in. A payload compiled `prospero`
 * can land in a previous- generation compatibility sandbox where the
 * current-generation graphics driver does not resolve at all, so the running
 * environment is measured, not compiled - obSCEne owns that as its OBS|context
 * - and the two must never be conflated.
 *
 * `neo` and `trinity` are the mid-generation Pro refreshes of `orbis` and
 * `prospero`, not synonyms: a previous-generation artifact is `orbis`, and
 * `neo` is correct only for a build that is genuinely PS4-Pro-specific;
 * likewise `prospero` is the current-generation default and `trinity` only for
 * a PS5-Pro-specific build.
 */

typedef enum oops_target {
  OOPS_ORBIS = OOPS_TARGET_ORBIS,
  OOPS_NEO = OOPS_TARGET_NEO,
  OOPS_PROSPERO = OOPS_TARGET_PROSPERO,
  OOPS_TRINITY = OOPS_TARGET_TRINITY,
} oops_target_t;

/* Returns the target the current binary was COMPILED FOR - not the environment
 * it is running in, which is measured elsewhere (see the note above
 * OOPS_TARGET_IS_PS4). */
static inline oops_target_t oops_get_target(void) {
  return (oops_target_t)OOPS_TARGET;
}

/* Returns the human-readable target name. */
static inline const char *oops_target_name(oops_target_t target) {
  switch (target) {
  case OOPS_ORBIS:
    return "orbis";
  case OOPS_NEO:
    return "neo";
  case OOPS_PROSPERO:
    return "prospero";
  case OOPS_TRINITY:
    return "trinity";
  default:
    return "unknown";
  }
}

#endif /* OOPS_TARGET_H */
