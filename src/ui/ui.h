#ifndef CORDLESS_UI_UI_H
#define CORDLESS_UI_UI_H

#include <stdbool.h>

// Thin UI layer over vita2d: bundled Inter font + reusable Discord-style
// drawing primitives. Load fonts once at startup.

// Load Regular + SemiBold TTFs (packed in the vpk). Returns true on success;
// on failure the text helpers draw nothing.
bool ui_init(const char *regular_ttf, const char *semibold_ttf,
             const char *emoji_atlas_png);
void ui_fini(void);

// Prime the glyph atlas a few glyphs per call; call inside a draw frame. Idempotent.
void ui_prime_glyphs(void);

// Text. `size` is pixel height. Returns the advance width in px.
int  ui_text(int x, int y, int size, unsigned color, const char *s);
int  ui_text_bold(int x, int y, int size, unsigned color, const char *s);
int  ui_text_w(int size, const char *s);
int  ui_text_bold_w(int size, const char *s);

// PlayStation button glyphs from the system font (psexchar.pvf), drawn inline
// with text. Pass a UI_PAD_* as `glyph`; `alt` draws as plain text if missing.
int  ui_pad_glyph(int x, int y, int size, unsigned color, const char *glyph, const char *alt);
int  ui_pad_glyph_w(int size, const char *glyph, const char *alt);
#define UI_PAD_TRIANGLE "$"
#define UI_PAD_CIRCLE   "!"
#define UI_PAD_CROSS    "\""
#define UI_PAD_SQUARE   "#"
#define UI_PAD_ENTER    "C"

// Draw text truncated with an ellipsis to fit max_w px.
void ui_text_clip(int x, int y, int size, unsigned color, const char *s, int max_w);
void ui_text_bold_clip(int x, int y, int size, unsigned color, const char *s, int max_w);

// If `s` begins with an emoji, draw it as a `size`-px square at (x,y) and
// return true; otherwise draw nothing and return false.
bool ui_draw_emoji(const char *s, int x, int y, int size);

void ui_rect(int x, int y, int w, int h, unsigned color);
void ui_rounded_rect(int x, int y, int w, int h, int r, unsigned color);

// Avatar placeholder: filled circle in `bg` with the first letter of `name`
// centered in white.
void ui_avatar_placeholder(int cx, int cy, int r, const char *name, unsigned bg);

// Word-wrap `text` into the given width, drawing lines from (x,y) downward at
// `line_h` spacing. Returns the total height consumed (px).
int  ui_text_wrapped(int x, int y, int size, unsigned color, const char *text,
                     int max_w, int line_h);

// Height (px) that ui_text_wrapped would consume, without drawing.
int  ui_wrap_height(int size, const char *text, int max_w, int line_h);

// Rich text: renders a subset of Discord markdown (**bold**, `code`,
// ~~strike~~, *italic*) with word-wrapping. Height variant measures only.
void ui_richtext_draw(int x, int y, int size, unsigned color, const char *text,
                      int max_w, int line_h);
int  ui_richtext_height(int size, const char *text, int max_w, int line_h);

// Toggle whether ||spoiler|| runs render hidden (solid bar) or shown. Set before
// drawing a message's content; reset to true afterwards.
void ui_set_spoiler_reveal(bool on);

#endif  // CORDLESS_UI_UI_H
