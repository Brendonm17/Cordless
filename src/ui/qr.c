#include "ui/qr.h"
#include "console.h"

#include <string.h>

#include <vita2d.h>
#include "qrcodegen.h"

bool qr_encode(const char *text, uint8_t qrcode_out[])
{
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    return qrcodegen_encodeText(text, tmp, qrcode_out,
                                qrcodegen_Ecc_LOW,
                                qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                qrcodegen_Mask_AUTO, true);
}

void qr_draw(const uint8_t *qrcode, int cx, int cy, int scale)
{
    int n = qrcodegen_getSize(qrcode);
    const int quiet = 4;  // quiet zone (modules)
    int panel = (n + 2 * quiet) * scale;
    int x0 = cx - panel / 2;
    int y0 = cy - panel / 2;

    vita2d_draw_rectangle(x0, y0, panel, panel, RGBA8(0xFF, 0xFF, 0xFF, 0xFF));

    int origin = quiet * scale;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                vita2d_draw_rectangle(x0 + origin + x * scale,
                                      y0 + origin + y * scale,
                                      scale, scale,
                                      RGBA8(0x00, 0x00, 0x00, 0xFF));
            }
        }
    }
}

void qr_dump_ascii(const uint8_t *qrcode)
{
    int n = qrcodegen_getSize(qrcode);
    console_dbgf("[qr] size=%d modules", n);
    // Two chars per module (roughly square in the log).
    char line[128];
    for (int y = 0; y < n; y++) {
        int p = 0;
        for (int x = 0; x < n && p < (int)sizeof(line) - 3; x++) {
            char c = qrcodegen_getModule(qrcode, x, y) ? '#' : ' ';
            line[p++] = c;
            line[p++] = c;
        }
        line[p] = '\0';
        console_dbgf("%s", line);
    }
}
