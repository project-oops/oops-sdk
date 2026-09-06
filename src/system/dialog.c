#include "oops/dialog.h"
#include "oops/sysmodule.h"
#include "oops/system.h"

/* Platform weak symbols for libSceCommonDialog */
__attribute__((weak)) int sceCommonDialogInitialize(void);
__attribute__((weak)) bool sceCommonDialogIsUsed(void);

/* Platform weak symbols for libSceImeDialog */
__attribute__((weak)) int sceImeDialogInit(void *param, void *extendedParam);
__attribute__((weak)) int sceImeDialogGetStatus(void);
__attribute__((weak)) int sceImeDialogGetResult(void *result);
__attribute__((weak)) int sceImeDialogAbort(void);
__attribute__((weak)) int sceImeDialogTerm(void);

/* Platform weak symbols for libSceMsgDialog */
__attribute__((weak)) int sceMsgDialogInitialize(void);
__attribute__((weak)) int sceMsgDialogOpen(const void *param);
__attribute__((weak)) int sceMsgDialogGetStatus(void);
__attribute__((weak)) int sceMsgDialogUpdateStatus(void);
__attribute__((weak)) int sceMsgDialogGetResult(void *result);
__attribute__((weak)) int sceMsgDialogClose(void);
__attribute__((weak)) int sceMsgDialogTerminate(void);

/* Internal UTF-8 <-> UTF-16 conversion utilities */
static void utf8_to_utf16(const char *src, uint16_t *dst, size_t max_dst_chars) {
    if (!dst || max_dst_chars == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max_dst_chars) {
            dst[i] = (uint16_t)(uint8_t)src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static void utf16_to_utf8(const uint16_t *src, char *dst, size_t max_dst_bytes) {
    if (!dst || max_dst_bytes == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max_dst_bytes) {
            dst[i] = (char)(src[i] & 0xFF);
            i++;
        }
    }
    dst[i] = '\0';
}

/* IME Dialog State */
#define IME_BUFFER_CHARS 1024
static uint16_t s_ime_text_buffer[IME_BUFFER_CHARS];
static uint16_t s_ime_title_buffer[256];
static uint16_t s_ime_placeholder_buffer[256];
static bool s_ime_active = false;
static bool s_ime_module_loaded = false;

/* SceImeDialogParam layout (96 bytes) */
typedef struct {
    int32_t   userId;
    int32_t   type;
    uint64_t  supportedLanguages;
    int32_t   enterLabel;
    int32_t   inputMethod;
    void     *filter;
    uint32_t  option;
    uint32_t  maxTextLength;
    uint16_t *inputTextBuffer;
    float     posX;
    float     posY;
    int32_t   horizontalAlignment;
    int32_t   verticalAlignment;
    uint16_t *placeholder;
    uint16_t *title;
    uint8_t   reserved[16];
} sce_ime_dialog_param_t;

typedef struct {
    int32_t endStatus;
    uint8_t reserved[12];
} sce_ime_dialog_result_t;

int oops_dialog_ime_open(const oops_ime_param_t *param) {
    if (!param) return -1;

    /* Ensure module is loaded */
    if (oops_sysmodule_load(OOPS_SYSMODULE_IME_DIALOG) == 0) {
        s_ime_module_loaded = true;
    }

    if (!sceImeDialogInit) {
        return -1;
    }

    int user_id = param->user_id;
    if (user_id < 0) {
        user_id = oops_user_get_initial_user_id();
        if (user_id < 0) user_id = 0;
    }

    /* Initialize buffers */
    for (size_t i = 0; i < IME_BUFFER_CHARS; i++) s_ime_text_buffer[i] = 0;
    if (param->initial_text) {
        utf8_to_utf16(param->initial_text, s_ime_text_buffer, IME_BUFFER_CHARS);
    }

    utf8_to_utf16(param->title, s_ime_title_buffer, 256);
    utf8_to_utf16(param->placeholder, s_ime_placeholder_buffer, 256);

    sce_ime_dialog_param_t sce_param;
    for (size_t i = 0; i < sizeof(sce_param); i++) ((uint8_t *)&sce_param)[i] = 0;

    sce_param.userId = user_id;
    sce_param.type = (int32_t)param->type;
    sce_param.maxTextLength = param->max_text_len > 0 ? param->max_text_len : (IME_BUFFER_CHARS - 1);
    sce_param.inputTextBuffer = s_ime_text_buffer;
    sce_param.title = param->title ? s_ime_title_buffer : NULL;
    sce_param.placeholder = param->placeholder ? s_ime_placeholder_buffer : NULL;
    sce_param.posX = param->pos_x;
    sce_param.posY = param->pos_y;

    int rc = sceImeDialogInit(&sce_param, NULL);
    if (rc == 0) {
        s_ime_active = true;
    }
    return rc;
}

oops_ime_status_t oops_dialog_ime_poll(void) {
    if (!s_ime_active || !sceImeDialogGetStatus) {
        return OOPS_IME_STATUS_NONE;
    }

    int status = sceImeDialogGetStatus();
    if (status == 1) return OOPS_IME_STATUS_RUNNING;
    if (status == 2) return OOPS_IME_STATUS_FINISHED;
    return OOPS_IME_STATUS_NONE;
}

int oops_dialog_ime_get_result(char *out_text, size_t max_len, oops_ime_result_t *out_result) {
    if (!out_text || max_len == 0) return -1;
    out_text[0] = '\0';

    if (!s_ime_active || !sceImeDialogGetResult) {
        return -1;
    }

    sce_ime_dialog_result_t res;
    for (size_t i = 0; i < sizeof(res); i++) ((uint8_t *)&res)[i] = 0;

    int rc = sceImeDialogGetResult(&res);
    if (rc == 0) {
        utf16_to_utf8(s_ime_text_buffer, out_text, max_len);
        if (out_result) {
            if (res.endStatus == 0) *out_result = OOPS_IME_RESULT_OK;
            else if (res.endStatus == 1) *out_result = OOPS_IME_RESULT_CANCELED;
            else *out_result = OOPS_IME_RESULT_ABORTED;
        }
    }
    return rc;
}

void oops_dialog_ime_abort(void) {
    if (s_ime_active && sceImeDialogAbort) {
        sceImeDialogAbort();
    }
}

void oops_dialog_ime_close(void) {
    if (s_ime_active && sceImeDialogTerm) {
        sceImeDialogTerm();
        s_ime_active = false;
    }
    if (s_ime_module_loaded) {
        (void)oops_sysmodule_unload(OOPS_SYSMODULE_IME_DIALOG);
        s_ime_module_loaded = false;
    }
}

/* Common Dialog Magic Calculation */
#define OOPS_COMMON_DIALOG_MAGIC_NUMBER 0xC0D1A109u

typedef struct {
    size_t   size;
    uint8_t  reserved[36];
    uint32_t magic;
} __attribute__((aligned(8))) sce_common_dialog_base_param_t;

static inline void set_common_dialog_magic(sce_common_dialog_base_param_t *param) {
    for (size_t i = 0; i < sizeof(*param); i++) ((uint8_t *)param)[i] = 0;
    param->size = sizeof(sce_common_dialog_base_param_t);
    param->magic = (uint32_t)(OOPS_COMMON_DIALOG_MAGIC_NUMBER + (uintptr_t)param);
}

/* Message Dialog State */
static bool s_msg_active = false;
static bool s_msg_module_loaded = false;

typedef struct {
    int32_t     buttonType;
    int32_t     _pad;
    const char *msg;
    void       *buttonsParam;
    uint8_t     reserved[24];
} sce_msg_dialog_user_message_param_t;

typedef struct {
    sce_common_dialog_base_param_t baseParam;
    size_t      size;
    int32_t     mode;
    int32_t     _pad;
    sce_msg_dialog_user_message_param_t *userMsgParam;
    void       *progBarParam;
    void       *sysMsgParam;
    int32_t     userId;
    uint8_t     reserved[40];
    int32_t     _pad2;
} sce_msg_dialog_param_t;

typedef struct {
    int32_t mode;
    int32_t result;
    int32_t buttonId;
    char    reserved[32];
} sce_msg_dialog_result_t;

static sce_msg_dialog_user_message_param_t s_user_msg_param;
static sce_msg_dialog_param_t              s_msg_dialog_param;

int oops_dialog_message_show(const char *message, oops_msg_dialog_button_t buttons) {
    if (!message) return -1;

    if (oops_sysmodule_load(OOPS_SYSMODULE_MESSAGE_DIALOG) == 0) {
        s_msg_module_loaded = true;
    }

    if (!sceMsgDialogOpen) {
        return -1;
    }

    if (sceCommonDialogInitialize) {
        sceCommonDialogInitialize();
    }
    if (sceMsgDialogInitialize) {
        sceMsgDialogInitialize();
    }

    for (size_t i = 0; i < sizeof(s_user_msg_param); i++) ((uint8_t *)&s_user_msg_param)[i] = 0;
    s_user_msg_param.buttonType = (int32_t)buttons;
    s_user_msg_param.msg = message;

    for (size_t i = 0; i < sizeof(s_msg_dialog_param); i++) ((uint8_t *)&s_msg_dialog_param)[i] = 0;
    set_common_dialog_magic(&s_msg_dialog_param.baseParam);
    s_msg_dialog_param.size = sizeof(s_msg_dialog_param);
    s_msg_dialog_param.mode = 1; /* ORBIS_MSG_DIALOG_MODE_USER_MSG */
    s_msg_dialog_param.userMsgParam = &s_user_msg_param;
    s_msg_dialog_param.userId = oops_user_get_initial_user_id();

    int rc = sceMsgDialogOpen(&s_msg_dialog_param);
    if (rc == 0) {
        s_msg_active = true;
    }
    return rc;
}

int oops_dialog_message_poll(oops_msg_dialog_result_t *out_result) {
    if (!s_msg_active) {
        return -1;
    }

    int status = -1;
    if (sceMsgDialogUpdateStatus) {
        status = sceMsgDialogUpdateStatus();
    } else if (sceMsgDialogGetStatus) {
        status = sceMsgDialogGetStatus();
    }

    if (status == 3 /* ORBIS_COMMON_DIALOG_STATUS_FINISHED */ || status == 2) {
        if (sceMsgDialogGetResult) {
            sce_msg_dialog_result_t res;
            for (size_t i = 0; i < sizeof(res); i++) ((uint8_t *)&res)[i] = 0;
            if (sceMsgDialogGetResult(&res) == 0) {
                if (out_result) {
                    if (res.buttonId == 1) *out_result = OOPS_MSG_DIALOG_RES_OK;
                    else if (res.buttonId == 2) *out_result = OOPS_MSG_DIALOG_RES_NO;
                    else *out_result = OOPS_MSG_DIALOG_RES_INVALID;
                }
                return 1; /* Completed */
            }
        }
    }
    return (status == 2 /* RUNNING */ || status == 1 /* INITIALIZED */) ? 0 : -1;
}

void oops_dialog_message_close(void) {
    if (s_msg_active && sceMsgDialogClose) {
        sceMsgDialogClose();
    }
    if (s_msg_active && sceMsgDialogTerminate) {
        sceMsgDialogTerminate();
        s_msg_active = false;
    }
    if (s_msg_module_loaded) {
        (void)oops_sysmodule_unload(OOPS_SYSMODULE_MESSAGE_DIALOG);
        s_msg_module_loaded = false;
    }
}

