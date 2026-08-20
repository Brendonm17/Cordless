#ifndef CORDLESS_CONSOLE_H
#define CORDLESS_CONSOLE_H

#include <vita2d.h>

// Tiny scrolling on-screen log. Not thread-safe: main thread only.

void console_init(vita2d_pgf *font);

// printf-style; appends one line. The Vita printf has no %z; cast sizes to int.
void console_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Like console_logf, but silent unless verbose mode is on (off by default).
void console_dbgf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void console_set_verbose(int on);

// Draw all visible lines. Call between vita2d_start_drawing / end_drawing.
void console_draw(void);

#endif  // CORDLESS_CONSOLE_H
