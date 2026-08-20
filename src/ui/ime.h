#ifndef CORDLESS_UI_IME_H
#define CORDLESS_UI_IME_H

#include <stddef.h>
#include <stdbool.h>

// Show the on-screen keyboard and block until the user confirms or cancels.
// UTF-8 result written to `out`; returns true on confirm with non-empty text.
bool ime_get_text(const char *title, const char *initial,
                  char *out, size_t out_cap);

#endif  // CORDLESS_UI_IME_H
