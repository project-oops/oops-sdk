/*
 * Loading platform system modules by identifier through libSceSysmodule.
 */
#ifndef OOPS_SYSMODULE_H
#define OOPS_SYSMODULE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * System module identifiers for dynamic loading via libSceSysmodule.
 * Several high-level libraries on Orbis and Prospero are not resident
 * by default and must be loaded by identifier before use.
 */
#define OOPS_SYSMODULE_NET 0x0001
#define OOPS_SYSMODULE_HTTP 0x0002
#define OOPS_SYSMODULE_SSL 0x0003
#define OOPS_SYSMODULE_PERF 0x0005
#define OOPS_SYSMODULE_FIBER 0x0006
#define OOPS_SYSMODULE_ULT 0x0007
#define OOPS_SYSMODULE_NGS2 0x000B
#define OOPS_SYSMODULE_NET_CTL 0x0011
#define OOPS_SYSMODULE_XML 0x0017
#define OOPS_SYSMODULE_VOICE 0x001A
#define OOPS_SYSMODULE_JSON 0x0080
#define OOPS_SYSMODULE_GAME_LIVE_STREAMING 0x0081
#define OOPS_SYSMODULE_PLAYGO 0x0083
#define OOPS_SYSMODULE_FONT 0x0084
#define OOPS_SYSMODULE_AUDIO_DEC 0x0088
#define OOPS_SYSMODULE_JPEG_DEC 0x008A
#define OOPS_SYSMODULE_JPEG_ENC 0x008B
#define OOPS_SYSMODULE_PNG_DEC 0x008C
#define OOPS_SYSMODULE_PNG_ENC 0x008D
#define OOPS_SYSMODULE_VIDEODEC 0x008E
#define OOPS_SYSMODULE_MOVE 0x008F
#define OOPS_SYSMODULE_IME_DIALOG 0x0096
#define OOPS_SYSMODULE_NP_PARTY 0x0097
#define OOPS_SYSMODULE_FONT_FT 0x0098
#define OOPS_SYSMODULE_FREETYPE_OT 0x0099
#define OOPS_SYSMODULE_FREETYPE_OL 0x009A
#define OOPS_SYSMODULE_SCREEN_SHOT 0x009C
#define OOPS_SYSMODULE_NP_AUTH 0x009D
#define OOPS_SYSMODULE_SAVE_DATA_DIALOG 0x00A0
#define OOPS_SYSMODULE_INVITATION_DIALOG 0x00A2
#define OOPS_SYSMODULE_MESSAGE_DIALOG 0x00A4
#define OOPS_SYSMODULE_AV_PLAYER 0x00A5
#define OOPS_SYSMODULE_AUDIO_3D 0x00A7
#define OOPS_SYSMODULE_NP_COMMERCE 0x00A8
#define OOPS_SYSMODULE_MOUSE 0x00A9
#define OOPS_SYSMODULE_WEB_BROWSER_DIALOG 0x00AB
#define OOPS_SYSMODULE_ERROR_DIALOG 0x00AC
#define OOPS_SYSMODULE_NP_TROPHY 0x00AD
#define OOPS_SYSMODULE_APP_CONTENT 0x00B4
#define OOPS_SYSMODULE_SAVE_DATA 0x00B6
#define OOPS_SYSMODULE_USBD 0x00B7
#define OOPS_SYSMODULE_RANDOM 0x00BA
#define OOPS_SYSMODULE_ZLIB 0x00C5
#define OOPS_SYSMODULE_VIDEODEC2 0x00CF
#define OOPS_SYSMODULE_KEYBOARD 0x0106
#define OOPS_SYSMODULE_SHARE 0x010F

/**
 * Load a system module by identifier.
 * Returns 0 on success, or a negative error code.
 */
int oops_sysmodule_load(uint16_t id);

/**
 * Unload a previously loaded system module.
 * Returns 0 on success, or a negative error code.
 */
int oops_sysmodule_unload(uint16_t id);

/**
 * Check if a system module is currently loaded.
 * Returns 1 if loaded, 0 if not, or -1 if unsupported.
 */
int oops_sysmodule_is_loaded(uint16_t id);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSMODULE_H */
