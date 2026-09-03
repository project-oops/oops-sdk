#include "oops/input.h"
#include <stddef.h>

/* Platform symbols from libScePad and libSceUserService */
__attribute__((weak)) int scePadInit(void);
__attribute__((weak)) int scePadOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int scePadClose(int handle);
__attribute__((weak)) int scePadReadState(int handle, void *state);
__attribute__((weak)) int scePadSetVibration(int handle, const void *param);
__attribute__((weak)) int scePadSetLightBar(int handle, const void *param);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);

static int s_pad_handle = -1;
static int s_initialized = 0;

int oops_input_init(void) {
    if (s_initialized) return 0;

    if (scePadInit) {
        scePadInit();
    }

    int32_t user = -1;
    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
            sceUserServiceInitialize(NULL);
            (void)sceUserServiceGetInitialUser(&user);
        }
    }

    if (user >= 0 && scePadOpen) {
        s_pad_handle = scePadOpen(user, 0, 0, NULL);
    }

    s_initialized = 1;
    return (s_pad_handle >= 0) ? 0 : -1;
}

int oops_input_poll(unsigned int port, oops_pad_state_t *out_state) {
    (void)port;
    if (!out_state) return -1;
    for (size_t i = 0; i < sizeof(*out_state); i++) {
        ((unsigned char *)out_state)[i] = 0;
    }

    if (s_pad_handle < 0 || !scePadReadState) {
        return -1;
    }

    /* Oversized buffer to safely receive libScePad state (approx 128 bytes) */
    unsigned char raw_state[256];
    for (size_t i = 0; i < sizeof(raw_state); i++) raw_state[i] = 0;

    if (scePadReadState(s_pad_handle, raw_state) != 0) {
        return -1;
    }

    /* Standard ScePad data mapping:
     * offset 0x00: uint32_t buttons
     * offset 0x04: uint8_t left_x, left_y, right_x, right_y
     * offset 0x08: uint8_t l2, r2
     * offset 0x30+: connection state
     */
    uint32_t raw_buttons = *(const uint32_t *)(raw_state + 0);
    out_state->buttons = raw_buttons;
    out_state->left_stick_x  = (int8_t)(raw_state[4] - 128);
    out_state->left_stick_y  = (int8_t)(raw_state[5] - 128);
    out_state->right_stick_x = (int8_t)(raw_state[6] - 128);
    out_state->right_stick_y = (int8_t)(raw_state[7] - 128);
    out_state->l2_trigger    = raw_state[8];
    out_state->r2_trigger    = raw_state[9];
    out_state->connected    = (raw_state[38] & 1) ? 1 : 1; /* Connected */

    return 0;
}

int oops_input_set_rumble(unsigned int port, uint8_t small_motor, uint8_t large_motor) {
    (void)port;
    if (s_pad_handle < 0 || !scePadSetVibration) return -1;
    struct {
        uint8_t small;
        uint8_t large;
        uint8_t pad[6];
    } vib = { small_motor, large_motor, {0} };
    return scePadSetVibration(s_pad_handle, &vib);
}

int oops_input_set_lightbar(unsigned int port, uint8_t r, uint8_t g, uint8_t b) {
    (void)port;
    if (s_pad_handle < 0 || !scePadSetLightBar) return -1;
    struct {
        uint8_t r, g, b, a;
        uint8_t pad[4];
    } col = { r, g, b, 255, {0} };
    return scePadSetLightBar(s_pad_handle, &col);
}

void oops_input_close(void) {
    if (s_pad_handle >= 0 && scePadClose) {
        scePadClose(s_pad_handle);
        s_pad_handle = -1;
    }
    s_initialized = 0;
}
