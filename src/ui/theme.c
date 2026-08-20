#include "ui/theme.h"
#include "model/settings.h"

#include <vita2d.h>

cl_palette g_pal;
int        g_fs[R_COUNT];

// Discord "Dark" palette (the current look).
static const cl_palette PAL_DARK = {
    .guildbar = RGBA8(0x1e, 0x1f, 0x22, 0xFF),
    .sidebar  = RGBA8(0x2b, 0x2d, 0x31, 0xFF),
    .chat     = RGBA8(0x31, 0x33, 0x38, 0xFF),
    .topbar   = RGBA8(0x31, 0x33, 0x38, 0xFF),
    .elevated = RGBA8(0x38, 0x3a, 0x40, 0xFF),
    .hover    = RGBA8(0x35, 0x37, 0x3c, 0xFF),
    .selected = RGBA8(0x40, 0x42, 0x49, 0xFF),
    .divider  = RGBA8(0x3f, 0x41, 0x47, 0xFF),
    .text     = RGBA8(0xdb, 0xde, 0xe1, 0xFF),
    .text_bright = RGBA8(0xf2, 0xf3, 0xf5, 0xFF),
    .text_muted  = RGBA8(0x94, 0x9b, 0xa4, 0xFF),
    .text_link   = RGBA8(0x00, 0xa8, 0xfc, 0xFF),
    .blurple  = RGBA8(0x58, 0x65, 0xf2, 0xFF),
    .green    = RGBA8(0x23, 0xa5, 0x5a, 0xFF),
    .red      = RGBA8(0xf2, 0x3f, 0x43, 0xFF),
    .white    = RGBA8(0xff, 0xff, 0xff, 0xFF),
};

// Discord "Light" palette.
static const cl_palette PAL_LIGHT = {
    .guildbar = RGBA8(0xe3, 0xe5, 0xe8, 0xFF),
    .sidebar  = RGBA8(0xf2, 0xf3, 0xf5, 0xFF),
    .chat     = RGBA8(0xff, 0xff, 0xff, 0xFF),
    .topbar   = RGBA8(0xff, 0xff, 0xff, 0xFF),
    .elevated = RGBA8(0xe3, 0xe5, 0xe8, 0xFF),
    .hover    = RGBA8(0xe7, 0xe9, 0xec, 0xFF),
    .selected = RGBA8(0xd7, 0xd9, 0xdd, 0xFF),
    .divider  = RGBA8(0xdd, 0xdf, 0xe3, 0xFF),
    .text     = RGBA8(0x31, 0x33, 0x38, 0xFF),
    .text_bright = RGBA8(0x06, 0x06, 0x07, 0xFF),
    .text_muted  = RGBA8(0x5c, 0x5e, 0x66, 0xFF),
    .text_link   = RGBA8(0x00, 0x68, 0xe0, 0xFF),
    .blurple  = RGBA8(0x58, 0x65, 0xf2, 0xFF),
    .green    = RGBA8(0x1a, 0x93, 0x4f, 0xFF),
    .red      = RGBA8(0xd8, 0x3c, 0x3e, 0xFF),
    .white    = RGBA8(0xff, 0xff, 0xff, 0xFF),
};

// Accent presets (override the blurple accent).
static const unsigned ACCENTS[] = {
    RGBA8(0x58, 0x65, 0xf2, 0xFF),  // blurple
    RGBA8(0x23, 0xa5, 0x5a, 0xFF),  // green
    RGBA8(0xeb, 0x45, 0x9e, 0xFF),  // fuchsia
    RGBA8(0xf2, 0x3f, 0x43, 0xFF),  // red
    RGBA8(0x1a, 0xbc, 0x9c, 0xFF),  // teal
    RGBA8(0xe6, 0x7e, 0x22, 0xFF),  // orange
};

// Native rasterized px per role, one row per size profile.
static const int FS_TABLE[3][R_COUNT] = {
    // TINY SMALL CHANNEL BODY HEADER TITLE
    {  11,  13,   14,     15,  16,    18 },  // small
    {  12,  14,   16,     17,  18,    20 },  // medium
    {  14,  16,   18,     20,  22,    24 },  // large
};

void theme_set_accent(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof ACCENTS / sizeof ACCENTS[0])) idx = 0;
    g_pal.blurple = ACCENTS[idx];
}

void theme_set(int which)
{
    g_pal = (which == 1) ? PAL_LIGHT : PAL_DARK;
    theme_set_accent(g_settings.accent);
}

void font_set(int profile)
{
    if (profile < 0 || profile > 2) profile = 1;
    for (int r = 0; r < R_COUNT; r++) g_fs[r] = FS_TABLE[profile][r];
}

void theme_apply_all(void)
{
    theme_set(g_settings.theme);
    font_set(g_settings.font_size);
}

int theme_accent_count(void) { return (int)(sizeof ACCENTS / sizeof ACCENTS[0]); }
