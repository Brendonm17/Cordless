#ifndef CORDLESS_UI_PHOTO_H
#define CORDLESS_UI_PHOTO_H

#include <stdbool.h>
#include <stddef.h>

// Open the system photo-import dialog to pick an image. Blocks. On success
// writes the imported file's device path into `out` and returns true.
bool photo_pick(char *out, size_t out_cap);

#endif  // CORDLESS_UI_PHOTO_H
