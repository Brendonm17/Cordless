#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/clib.h>  // sceClibPrintf
#include <psp2/kernel/processmgr.h>  // sceKernelGetProcessTimeWide
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

// Persistent log file on the memory card.
#define LOG_PATH "ux0:data/cordless/log.txt"

static void logfile_open(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/cordless", 0777);
    // Keep the previous boot's log as log.prev.txt.
    sceIoRemove("ux0:data/cordless/log.prev.txt");
    sceIoRename(LOG_PATH, "ux0:data/cordless/log.prev.txt");
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
}

#define CON_MAX_LINES 256
#define CON_LINE_LEN  128
#define CON_VISIBLE   26  // lines that fit on screen at this font size
#define CON_FONT_SCALE 1.0f
#define CON_LINE_H    20
#define CON_MARGIN_X  12
#define CON_MARGIN_Y  24

static char       s_lines[CON_MAX_LINES][CON_LINE_LEN];
static int        s_count;  // total lines written
static vita2d_pgf *s_font;

void console_init(vita2d_pgf *font)
{
    s_font  = font;
    s_count = 0;
    memset(s_lines, 0, sizeof(s_lines));
    logfile_open();
}

static void push_line(const char *text)
{
    int slot = s_count % CON_MAX_LINES;
    snprintf(s_lines[slot], CON_LINE_LEN, "%s", text);
    s_count++;
}

static void console_vlogf(const char *fmt, va_list ap)
{
    char buf[640];  // fits full URLs
    vsnprintf(buf, sizeof(buf), fmt, ap);

    // Mirror to the debug channel (Vita3K stdout).
    sceClibPrintf("[cordless] %s\n", buf);

    // Append with a seconds.mmm-since-boot prefix; the fd stays open and is
    // flushed (close+reopen) every 16 lines.
    static SceUID s_fd = -1;
    static int    s_n  = 0;
    if (s_fd < 0) s_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (s_fd >= 0) {
        unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000ull);
        char line[700];
        int n = snprintf(line, sizeof(line), "%u.%03u %s\n", ms / 1000u, ms % 1000u, buf);
        if (n > 0) sceIoWrite(s_fd, line, n < (int)sizeof(line) ? (size_t)n : sizeof(line));
        if (++s_n >= 16) { sceIoClose(s_fd); s_fd = -1; s_n = 0; }  // flush to card
    }

    // Split on embedded newlines.
    char *start = buf;
    for (char *p = buf; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            push_line(start);
            if (saved == '\0') break;
            start = p + 1;
        }
    }
}

void console_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    console_vlogf(fmt, ap);
    va_end(ap);
}

// Verbose logging, off by default.
static int s_verbose = 0;
void console_set_verbose(int on) { s_verbose = on; }

void console_dbgf(const char *fmt, ...)
{
    if (!s_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    console_vlogf(fmt, ap);
    va_end(ap);
}

void console_draw(void)
{
    if (!s_font) return;

    int first = s_count > CON_VISIBLE ? s_count - CON_VISIBLE : 0;
    int row = 0;
    for (int i = first; i < s_count; i++, row++) {
        const char *line = s_lines[i % CON_MAX_LINES];
        vita2d_pgf_draw_text(s_font, CON_MARGIN_X,
                             CON_MARGIN_Y + row * CON_LINE_H,
                             0xFFFFFFFF, CON_FONT_SCALE, line);
    }
}
