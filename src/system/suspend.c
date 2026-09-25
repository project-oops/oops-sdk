/* Suspend cooperation - the opt-in half of oops_system.
 *
 * Isolated from `system.c` so it is linked only by a binary that asks to prepare for suspend.
 * The pump references `sceSystemServiceReceiveEvent` weakly; carrying that reference forces a
 * consumer to *claim* the symbol (obSCEne's eboot mkmodule check rejects even a weak undef that
 * no library claims). A conformance probe or a headless utility that never suspends should not
 * pay that, so the reference lives here, in its own translation unit, and only a caller pulls it
 * in. The callers are the renderer layers that pump and drain: oops-gl (`gl_context.c` registers
 * the drain, `glut.c` pumps) and the SDL2 backend. `system.c` keeps the close-signal handler,
 * which is a different mechanism and stays always-linked.
 */

#include "oops/system.h"
#include "oops/freestd.h"

/* -------------------------------------------------------------------------
 * Being suspended without being killed for it
 *
 * The dashboard's Close and rest-mode do not only signal a title; the kernel then suspends the
 * process asynchronously and gives it **100 seconds to reach a suspend point**. A title that
 * never does is killed with `0xa0d0c00f CPU_FAULT_SUSPENDPOINT_TIMEOUT_IN_SUSPEND_ASYNC`, "No
 * suspendPoint for 100sec" - the CPU sibling of the GPU fault `0xa0d0c00c`.
 *
 * This is a **different mechanism from the close signal** in system.c and both are needed: the
 * signal is the cooperative request to stop, this is the kernel freezing what is left. A title
 * can handle the first perfectly and still die of the second.
 *
 * # What a homebrew title does not get
 *
 * The two surfaces a licensed title would use are absent here, measured rather than assumed
 * (obSCEne `141-suspend`, 2026-09-23, FW 12.40):
 *
 *   - `sceSystemServiceDeclareReadyForSuspend` and its Enable/Disable notification pair -
 *     `libSceSystemServiceSuspend` does not load, and the symbols are ENOENT at every standard
 *     path. This is the call whose entire job is to say "freeze me now", and it is not here.
 *   - The `sceApplication` lifecycle in `libSceSysCore` - nought of six resolved, and gated on
 *     an SDK version this title does not claim (ESDKVERSION).
 *
 * So neither is referenced, even weakly. What is reachable is the event pump
 * (`sceSystemServiceReceiveEvent`, `GetStatus`) and the GPU's own suspend point.
 *
 * # What is assumed about the event pump, precisely
 *
 * **Its arity and its event layout are not measured**, and this project does not invent either
 * (D008; orbistoun refuses to declare the same call for the same reason - D311). So the call is
 * made in the narrowest shape that needs neither:
 *
 *   - one out-pointer to a buffer far larger than any plausible event, zeroed;
 *   - **the return value alone is read** - zero meaning an event was taken, anything else
 *     meaning stop. No field of the event is interpreted, no "no event pending" constant is
 *     guessed, and no event type is compared against a number nobody has measured.
 *
 * That is enough for the only thing wanted here: emptying the queue so the process is servicing
 * it rather than ignoring it. What the events *say* is a separate question and stays unanswered
 * until something measures it.
 *
 * The residual assumption is the arity - that it takes one argument. If it takes more, the extra
 * registers hold whatever they held. That is the one unmeasured thing left, it is named here
 * rather than buried, and the call is guarded by `oops_symbol_is_resolved` so a build where the
 * symbol is absent never reaches it.
 */
__attribute__((weak)) int sceSystemServiceReceiveEvent(void *event);

static void (*s_suspend_drain)(void);

void oops_system_set_suspend_drain(void (*drain)(void)) { s_suspend_drain = drain; }

int oops_system_pump_events(void) {
#ifdef OOPS_HOST_BUILD
  return 0;
#else
  if (!oops_symbol_is_resolved((const void *)&sceSystemServiceReceiveEvent)) {
    return 0;
  }
  /* Far larger than any event structure this could be handed, so a callee writing its own size
     cannot run off the end of it. Zeroed each time: a partially written event must not be read
     as the tail of the last one, even though nothing here reads it at all. */
  unsigned char event[512];
  int taken = 0;
  /* Bounded. A queue that answers "took one" forever is a broken queue, and spinning in it is
     the failure this function exists to prevent rather than a way to serve it. */
  for (int i = 0; i < 64; i++) {
    for (size_t b = 0; b < sizeof(event); b++) {
      event[b] = 0;
    }
    if (sceSystemServiceReceiveEvent(event) != 0) {
      break;
    }
    taken++;
  }
  if (taken > 0) {
    oops_log_trace("SYS", "pump_events: drained %d event(s)", taken);
  }
  return taken;
#endif
}

void oops_system_prepare_for_suspend(void) {
  oops_log_info("SYS", "prepare_for_suspend: pumping events and draining renderer");
  /* The queue first: an event still owed to the process is the process still being asked
     something. */
  (void)oops_system_pump_events();
  /* Then the GPU. **This is the arm obSCEne could not settle from an inert probe** - it emitted
     `gpu-drain-required / needs-live-suspend` and said so - that an unretired submission holds
     the process out of suspend. The drain is whatever the renderer registered; oops-gl registers
     one that submits and waits on its end-of-pipe fence, bounded and sleeping, which is the wait
     it already uses everywhere else. A renderer that registered nothing simply has nothing in
     flight to wait for. */
  if (s_suspend_drain) {
    s_suspend_drain();
  }
}
