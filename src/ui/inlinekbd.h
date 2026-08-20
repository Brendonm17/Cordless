#ifndef CORDLESS_UI_INLINEKBD_H
#define CORDLESS_UI_INLINEKBD_H

#include <stdbool.h>

// Inline (bottom-half) software keyboard via libime: renders only the keyboard
// panel while the app keeps drawing its own UI. Open, pump each frame, close.

// Open with an initial UTF-8 string. Returns false on failure.
bool inlinekbd_open(const char *initial_utf8);

// Pump once per frame on the render thread, after end_drawing + swap. Returns
// 0 active, 1 Enter/Send pressed (read inlinekbd_text()), -1 closed/cancelled.
int  inlinekbd_pump(void);

const char *inlinekbd_text(void);  // current UTF-8 text
void        inlinekbd_close(void);  // force close (idempotent)

// Draw the floating composer box (dim backdrop, rounded panel, live text);
// `header` labels the box. Call inside a draw frame. No-op when not open.
void        inlinekbd_draw(const char *header);

#endif  // CORDLESS_UI_INLINEKBD_H
