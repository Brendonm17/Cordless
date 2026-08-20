#include "util/clipboard.h"

#include <psp2/sysmodule.h>

// vitasdk ships no SceClipboard header, so declare the ABI here. Text is UTF-16LE.
typedef unsigned short cl_wchar16;
extern int sceClipboardSetText(const cl_wchar16 *text);

// UTF-8 -> UTF-16 (with surrogate pairs for astral chars, e.g. emoji).
static int utf8_to_utf16(const char *s, cl_wchar16 *out, int cap)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap - 2) {
        unsigned cp; int len;
        unsigned char c = *p;
        if (c < 0x80)            { cp = c;        len = 1; }
        else if ((c >> 5) == 6)  { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { p++; continue; }
        int ok = 1;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!ok) { p++; continue; }
        p += len;
        if (cp <= 0xFFFF) {
            out[n++] = (cl_wchar16)cp;
        } else {
            cp -= 0x10000;
            out[n++] = (cl_wchar16)(0xD800 | (cp >> 10));
            out[n++] = (cl_wchar16)(0xDC00 | (cp & 0x3FF));
        }
    }
    out[n] = 0;
    return n;
}

int clipboard_set_utf8(const char *utf8)
{
    if (!utf8) return -1;
    sceSysmoduleLoadModule(SCE_SYSMODULE_CLIPBOARD);  // ok if already loaded
    static cl_wchar16 buf[4096];
    utf8_to_utf16(utf8, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    return sceClipboardSetText(buf);
}
