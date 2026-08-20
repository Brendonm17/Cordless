#ifndef CORDLESS_UI_CAMERA_H
#define CORDLESS_UI_CAMERA_H

#include <stdbool.h>

// Open the system camera-capture dialog and take a photo. Blocks. On success
// returns true and a malloc'd JPEG (caller frees) in *out and *out_len.
bool camera_capture(unsigned char **out, int *out_len);

#endif  // CORDLESS_UI_CAMERA_H
