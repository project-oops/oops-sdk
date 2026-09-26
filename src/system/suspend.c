/* Suspend cooperation - the opt-in half of oops_system.
 *
 * Kept out of `system.c` so only a binary that prepares for suspend links it: the weak
 * reference to `sceSystemServiceReceiveEvent` must be claimed by a library (obSCEne's
 * mkmodule check rejects an unclaimed weak undef), and a probe or headless utility
 * should not carry it. The callers are the renderer layers: oops-gl (`gl_context.c`
 * registers the drain, `glut.c` pumps) and the SDL2 backend. The close-signal handler
 * stays in `system.c`.
 */

#include "oops/system.h"
#include "oops/freestd.h"

/* -------------------------------------------------------------------------
 * Reaching a suspend point
 *
 * After the dashboard's Close or rest mode, the kernel suspends the process
 * asynchronously and gives it 100 seconds to reach a suspend point; a title that does
 * not is killed with `0xa0d0c00f CPU_FAULT_SUSPENDPOINT_TIMEOUT_IN_SUSPEND_ASYNC`. This
 * is separate from the close signal in system.c, and a title needs both.
 *
 * A homebrew title cannot reach the licensed surfaces (obSCEne probe `141-suspend`, FW
 * 12.40): `libSceSystemServiceSuspend` does not load, so
 * `sceSystemServiceDeclareReadyForSuspend` is absent, and the `sceApplication`
 * lifecycle in `libSceSysCore` is gated on an SDK version (ESDKVERSION). Neither is
 * referenced. What is reachable is the event pump and the GPU's own suspend point.
 *
 * The pump's arity and event layout are unmeasured, so the call is made in the
 * narrowest shape: one out-pointer to a large zeroed buffer, and only the return value
 * is read - zero means an event was taken, anything else means stop. The one-argument
 * arity is the remaining assumption; `oops_symbol_is_resolved` guards the call.
 */
__attribute__((weak)) int sceSystemServiceReceiveEvent(void *event);

static void (*s_suspend_drain)(void);

void oops_system_set_suspend_drain(void (*drain)(void)) {
    s_suspend_drain = drain;
}

int oops_system_pump_events(void) {
#ifdef OOPS_HOST_BUILD
    return 0;
#else
    if (!oops_symbol_is_resolved((const void *)&sceSystemServiceReceiveEvent)) {
        return 0;
    }
    /* Far larger than any event structure, so a callee writing its own size cannot run
       off the end. Zeroed each time so a partial write never carries the last event. */
    unsigned char event[512];
    int taken = 0;
    /* Bounded: a queue that answers "took one" forever must not hold the process here.
     */
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
    /* The event queue first, then the GPU: an unretired submission is assumed to hold
       the process out of suspend (unmeasured). The drain is whatever the renderer
       registered; oops-gl's submits and waits, bounded, on its end-of-pipe fence. */
    (void)oops_system_pump_events();
    if (s_suspend_drain) {
        s_suspend_drain();
    }
}
