#ifndef CORDLESS_UI_THEME_H
#define CORDLESS_UI_THEME_H

#include <vita2d.h>

// Discord "Dark" theme palette (approximated from the desktop client).
#define COL_GUILDBAR   RGBA8(0x1e, 0x1f, 0x22, 0xFF)
#define COL_SIDEBAR    RGBA8(0x2b, 0x2d, 0x31, 0xFF)
#define COL_CHAT       RGBA8(0x31, 0x33, 0x38, 0xFF)
#define COL_TOPBAR     RGBA8(0x31, 0x33, 0x38, 0xFF)
#define COL_ELEVATED   RGBA8(0x38, 0x3a, 0x40, 0xFF)  // inputs / cards
#define COL_HOVER      RGBA8(0x35, 0x37, 0x3c, 0xFF)
#define COL_SELECTED   RGBA8(0x40, 0x42, 0x49, 0xFF)
#define COL_DIVIDER    RGBA8(0x3f, 0x41, 0x47, 0xFF)

#define COL_TEXT       RGBA8(0xdb, 0xde, 0xe1, 0xFF)
#define COL_TEXT_BRIGHT RGBA8(0xf2, 0xf3, 0xf5, 0xFF)
#define COL_TEXT_MUTED RGBA8(0x94, 0x9b, 0xa4, 0xFF)
#define COL_TEXT_LINK  RGBA8(0x00, 0xa8, 0xfc, 0xFF)

#define COL_BLURPLE    RGBA8(0x58, 0x65, 0xf2, 0xFF)
#define COL_GREEN      RGBA8(0x23, 0xa5, 0x5a, 0xFF)  // online
#define COL_RED        RGBA8(0xf2, 0x3f, 0x43, 0xFF)  // mention / error
#define COL_WHITE      RGBA8(0xff, 0xff, 0xff, 0xFF)

// Type scale (pixel sizes).
#define FS_TITLE   20
#define FS_HEADER  18
#define FS_BODY    17
#define FS_CHANNEL 16
#define FS_SMALL   14
#define FS_TINY    12

#define SCREEN_W 960
#define SCREEN_H 544

#endif  // CORDLESS_UI_THEME_H
