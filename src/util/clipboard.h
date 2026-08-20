#ifndef CORDLESS_UTIL_CLIPBOARD_H
#define CORDLESS_UTIL_CLIPBOARD_H

// Copy a UTF-8 string to the Vita system clipboard. Returns 0 ok, <0 error.
// Hardware-only: Vita3K stubs SceClipboard, so this no-ops in the emulator.
int clipboard_set_utf8(const char *utf8);

#endif  // CORDLESS_UTIL_CLIPBOARD_H
