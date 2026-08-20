#ifndef CORDLESS_UI_THEME_H
#define CORDLESS_UI_THEME_H

#include <vita2d.h>

// Active color palette; the COL_* macros read these. See theme.c.
typedef struct {
    unsigned guildbar, sidebar, chat, topbar, elevated, hover, selected, divider;
    unsigned text, text_bright, text_muted, text_link;
    unsigned blurple, green, red, white;
} cl_palette;
extern cl_palette g_pal;

#define COL_GUILDBAR    (g_pal.guildbar)
#define COL_SIDEBAR     (g_pal.sidebar)
#define COL_CHAT        (g_pal.chat)
#define COL_TOPBAR      (g_pal.topbar)
#define COL_ELEVATED    (g_pal.elevated)
#define COL_HOVER       (g_pal.hover)
#define COL_SELECTED    (g_pal.selected)
#define COL_DIVIDER     (g_pal.divider)
#define COL_TEXT        (g_pal.text)
#define COL_TEXT_BRIGHT (g_pal.text_bright)
#define COL_TEXT_MUTED  (g_pal.text_muted)
#define COL_TEXT_LINK   (g_pal.text_link)
#define COL_BLURPLE     (g_pal.blurple)
#define COL_GREEN       (g_pal.green)
#define COL_RED         (g_pal.red)
#define COL_WHITE       (g_pal.white)

// Active type scale; the FS_* macros read these. Native px per size. See theme.c.
enum { R_TINY, R_SMALL, R_CHANNEL, R_BODY, R_HEADER, R_TITLE, R_COUNT };
extern int g_fs[R_COUNT];
#define FS_TINY    (g_fs[R_TINY])
#define FS_SMALL   (g_fs[R_SMALL])
#define FS_CHANNEL (g_fs[R_CHANNEL])
#define FS_BODY    (g_fs[R_BODY])
#define FS_HEADER  (g_fs[R_HEADER])
#define FS_TITLE   (g_fs[R_TITLE])

void theme_set(int which);        // 0 dark, 1 light (reapplies accent)
void theme_set_accent(int idx);
void font_set(int profile);       // 0 small, 1 medium, 2 large
void theme_apply_all(void);       // apply theme + accent + font from g_settings
int  theme_accent_count(void);

#define SCREEN_W 960
#define SCREEN_H 544

#endif  // CORDLESS_UI_THEME_H
