#ifndef CORDLESS_UI_QR_H
#define CORDLESS_UI_QR_H

#include <stdbool.h>
#include <stdint.h>

// Encode `text` into a QR code (ECC LOW). Writes into a caller buffer sized
// qrcodegen_BUFFER_LEN_MAX. Returns true on success.
bool qr_encode(const char *text, uint8_t qrcode_out[]);

// Draw a QR (from qr_encode) centered at (cx, cy) on a white panel with a quiet
// zone, `scale` pixels per module. Call between vita2d start/end drawing.
void qr_draw(const uint8_t *qrcode, int cx, int cy, int scale);

// Dump the QR to the debug log as ASCII.
void qr_dump_ascii(const uint8_t *qrcode);

#endif  // CORDLESS_UI_QR_H
