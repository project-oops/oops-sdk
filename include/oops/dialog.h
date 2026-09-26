#ifndef OOPS_DIALOG_H
#define OOPS_DIALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* On-Screen Virtual Keyboard (libSceImeDialog)                               */
/* ========================================================================= */

typedef enum oops_ime_type {
    OOPS_IME_TYPE_DEFAULT = 0,
    OOPS_IME_TYPE_BASIC_LATIN = 1,
    OOPS_IME_TYPE_URL = 2,
    OOPS_IME_TYPE_MAIL = 3,
    OOPS_IME_TYPE_NUMBER = 4
} oops_ime_type_t;

typedef enum oops_ime_status {
    OOPS_IME_STATUS_NONE = 0,
    OOPS_IME_STATUS_RUNNING = 1,
    OOPS_IME_STATUS_FINISHED = 2
} oops_ime_status_t;

typedef enum oops_ime_result {
    OOPS_IME_RESULT_OK = 0,
    OOPS_IME_RESULT_CANCELED = 1,
    OOPS_IME_RESULT_ABORTED = 2
} oops_ime_result_t;

typedef struct oops_ime_param {
    int32_t user_id; /* -1 for initial user */
    oops_ime_type_t type;
    const char *title;        /* UTF-8 title or NULL */
    const char *placeholder;  /* UTF-8 placeholder or NULL */
    const char *initial_text; /* UTF-8 initial text or NULL */
    uint32_t max_text_len;    /* Max chars allowed */
    float pos_x;
    float pos_y;
} oops_ime_param_t;

int oops_dialog_ime_open(const oops_ime_param_t *param);
oops_ime_status_t oops_dialog_ime_poll(void);
int oops_dialog_ime_get_result(char *out_text, size_t max_len,
                               oops_ime_result_t *out_result);
void oops_dialog_ime_abort(void);
void oops_dialog_ime_close(void);

/* ========================================================================= */
/* System Message Dialog (libSceMsgDialog)                                   */
/* ========================================================================= */

typedef enum oops_msg_dialog_button {
    OOPS_MSG_DIALOG_BTN_OK = 0,
    OOPS_MSG_DIALOG_BTN_YESNO = 1,
    OOPS_MSG_DIALOG_BTN_NONE = 2,
    OOPS_MSG_DIALOG_BTN_OKCANCEL = 3
} oops_msg_dialog_button_t;

typedef enum oops_msg_dialog_result {
    OOPS_MSG_DIALOG_RES_INVALID = 0,
    OOPS_MSG_DIALOG_RES_OK = 1, /* Or Yes */
    OOPS_MSG_DIALOG_RES_NO = 2  /* Or Cancel */
} oops_msg_dialog_result_t;

int oops_dialog_message_show(const char *message, oops_msg_dialog_button_t buttons);
int oops_dialog_message_poll(oops_msg_dialog_result_t *out_result);
void oops_dialog_message_close(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_DIALOG_H */
