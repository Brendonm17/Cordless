#include "ui/inlinekbd.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "console.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <vita2d.h>
#include <psp2/libime.h>
#include <psp2/sysmodule.h>

// <= SCE_IME_MAX_TEXT_LENGTH (2048); Discord's message cap is 2000.
#define IME_MAXLEN 2000

// Kept alive for the whole session; libime writes into them asynchronously.
// The initial and input buffers are distinct.
static uint8_t    s_work[SCE_IME_WORK_BUFFER_SIZE] __attribute__((aligned(4)));
static SceWChar16 s_initial[IME_MAXLEN + 1];
static SceWChar16 s_text16[IME_MAXLEN + 1];
static char       s_text8[(IME_MAXLEN + 1) * 4];

static bool s_open;
static bool s_done;
static bool s_committed;
static int  s_caret_tick;  // drives the blinking caret in inlinekbd_draw
static int  s_caret_byte;  // caret position as a byte offset into s_text8
static int  s_kbd_top;  // keyboard panel top (screen y) from CHANGE_SIZE; 0=unknown

// UTF-16 -> UTF-8 (surrogate-pair aware).
static void utf16_to_utf8(const SceWChar16 *src, uint8_t *dst)
{
    for (int i = 0; src[i]; i++) {
        if ((src[i] & 0xFF80) == 0) {
            *dst++ = src[i] & 0xFF;
        } else if ((src[i] & 0xF800) == 0) {
            *dst++ = ((src[i] >> 6) & 0xFF) | 0xC0;
            *dst++ = (src[i] & 0x3F) | 0x80;
        } else if ((src[i] & 0xFC00) == 0xD800 && (src[i + 1] & 0xFC00) == 0xDC00) {
            *dst++ = (((src[i] + 64) >> 8) & 0x3) | 0xF0;
            *dst++ = (((src[i] >> 2) + 16) & 0x3F) | 0x80;
            *dst++ = ((src[i] >> 4) & 0x30) | 0x80 | ((src[i + 1] << 2) & 0xF);
            *dst++ = (src[i + 1] & 0x3F) | 0x80;
            i += 1;
        } else {
            *dst++ = ((src[i] >> 12) & 0xF) | 0xE0;
            *dst++ = ((src[i] >> 6) & 0x3F) | 0x80;
            *dst++ = (src[i] & 0x3F) | 0x80;
        }
    }
    *dst = '\0';
}

static void utf8_to_utf16(const uint8_t *src, SceWChar16 *dst)
{
    for (int i = 0; src[i];) {
        if ((src[i] & 0xE0) == 0xE0) {
            *dst++ = ((src[i] & 0x0F) << 12) | ((src[i + 1] & 0x3F) << 6) | (src[i + 2] & 0x3F);
            i += 3;
        } else if ((src[i] & 0xC0) == 0xC0) {
            *dst++ = ((src[i] & 0x1F) << 6) | (src[i + 1] & 0x3F);
            i += 2;
        } else {
            *dst++ = src[i];
            i += 1;
        }
    }
    *dst = '\0';
}

// UTF-8 byte length of the first `units` UTF-16 code units of `s`. Maps the
// caret index (UTF-16 units) to a byte offset in the UTF-8 text.
static int u16_prefix_bytes(const SceWChar16 *s, int units)
{
    int bytes = 0;
    for (int i = 0; i < units && s[i]; i++) {
        SceWChar16 c = s[i];
        if ((c & 0xFC00) == 0xD800 && i + 1 < units && (s[i + 1] & 0xFC00) == 0xDC00) {
            bytes += 4; i++;  // surrogate pair: 2 units, 4 bytes
        } else if (c < 0x80)  bytes += 1;
        else if (c < 0x800)   bytes += 2;
        else                  bytes += 3;
    }
    return bytes;
}

// Event handler; fires synchronously from inside sceImeUpdate() on the same
// thread. Reads state and sets flags.
static void ime_handler(void *arg, const SceImeEventData *e)
{
    (void)arg;
    switch (e->id) {
    case SCE_IME_EVENT_UPDATE_TEXT:
        utf16_to_utf8(s_text16, (uint8_t *)s_text8);  // whole buffer -> UTF-8 view
        s_caret_byte = u16_prefix_bytes(s_text16, (int)e->param.text.caretIndex);
        break;
    case SCE_IME_EVENT_UPDATE_CARET:  // arrow keys move the caret
        s_caret_byte = u16_prefix_bytes(s_text16, (int)e->param.caretIndex);
        break;
    case SCE_IME_EVENT_CHANGE_SIZE:  // keyboard panel geometry
        s_kbd_top = (int)e->param.rect.y;
        break;
    case SCE_IME_EVENT_PRESS_ENTER:
        s_committed = true;  s_done = true;
        break;
    case SCE_IME_EVENT_PRESS_CLOSE:
        s_committed = false; s_done = true;
        break;
    default:
        break;
    }
}

bool inlinekbd_open(const char *initial_utf8)
{
    if (s_open) return true;
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

    s_done = s_committed = false;
    s_kbd_top = 0;  // re-learned via CHANGE_SIZE
    s_initial[0] = 0;
    if (initial_utf8 && initial_utf8[0]) utf8_to_utf16((const uint8_t *)initial_utf8, s_initial);
    s_text16[0] = 0;
    if (s_initial[0]) utf16_to_utf8(s_initial, (uint8_t *)s_text8);  // seed the UTF-8 view with the draft
    else s_text8[0] = 0;
    s_caret_byte = (int)strlen(s_text8);  // caret starts at the end of the draft

    SceImeParam p;
    sceImeParamInit(&p);  // sets sdkVersion
    p.supportedLanguages = 0;  // all languages
    p.languagesForced    = SCE_FALSE;
    p.type               = SCE_IME_TYPE_DEFAULT;
    p.option             = SCE_IME_OPTION_NO_ASSISTANCE;  // compact keyboard, no predictive bar
    p.work               = s_work;
    p.arg                = NULL;
    p.handler            = ime_handler;
    p.filter             = NULL;
    p.initialText        = s_initial;  // distinct from inputTextBuffer
    p.maxTextLength      = IME_MAXLEN;
    p.inputTextBuffer    = s_text16;
    p.enterLabel         = SCE_IME_ENTER_LABEL_SEND;

    int r = sceImeOpen(&p);
    if (r < 0) { console_logf("[ime] inline open 0x%08X", (unsigned)r); return false; }
    s_open = true;
    return true;
}

int inlinekbd_pump(void)
{
    if (!s_open) return -1;
    if (sceImeUpdate() < 0) s_done = true;
    if (s_done) {
        int r = s_committed ? 1 : -1;
        inlinekbd_close();  // close exactly once
        return r;
    }
    return 0;
}

const char *inlinekbd_text(void)   { return s_text8; }

void inlinekbd_close(void)
{
    if (!s_open) return;  // idempotent
    sceImeClose();
    s_open = false;
}


#define KB_MAXLINES 128  // cap on total wrapped lines

// Char-wrap s_text8 to `maxw` px and draw a `maxlines`-tall window bottom-
// anchored at `y_bottom`. The window follows the caret; placeholder when empty.
static void draw_live_text(int x, int y_bottom, int maxw, int size,
                           int line_h, int maxlines)
{
    if (maxlines < 1) maxlines = 1;

    const char *t = s_text8;
    int total = (int)strlen(t);

    // Wrap into line spans [start,len) over the original text.
    static struct { int start, len; } L[KB_MAXLINES];
    int nL = 0, ls = 0, last_space = -1;
    for (int i = 0; t[i] && nL < KB_MAXLINES - 1; ) {
        unsigned char c = (unsigned char)t[i];
        int cl = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;

        if (c == '\n') { L[nL].start = ls; L[nL].len = i - ls; nL++; i++; ls = i; last_space = -1; continue; }

        int seglen = (i + cl) - ls;
        int over = 0;
        if (i > ls) {
            char tmp[384];
            if (seglen >= (int)sizeof(tmp)) over = 1;
            else { memcpy(tmp, t + ls, seglen); tmp[seglen] = 0; over = ui_text_w(size, tmp) > maxw; }
        }
        if (over) {  // break before this codepoint
            if (last_space > ls) {  // at the last word boundary
                L[nL].start = ls; L[nL].len = last_space - ls; nL++;
                ls = last_space + 1;  // skip the space
            } else {  // one long token: hard char break
                L[nL].start = ls; L[nL].len = i - ls; nL++;
                ls = i;
            }
            last_space = -1;
            continue;  // re-measure this codepoint on the new line
        }
        if (c == ' ') last_space = i;
        i += cl;
    }
    L[nL].start = ls; L[nL].len = total - ls; nL++;  // trailing (caret) line

    int caret = s_caret_byte;
    if (caret < 0) caret = 0;
    if (caret > total) caret = total;

    int caret_line = nL - 1;
    for (int k = 0; k < nL; k++)
        if (caret >= L[k].start && caret <= L[k].start + L[k].len) { caret_line = k; break; }

    // Visible window: tail by default, scrolled up if the caret is above it.
    int vis = nL < maxlines ? nL : maxlines;
    int first = nL - vis;
    if (caret_line < first) first = caret_line;
    if (first < 0) first = 0;

    bool empty = (total == 0);
    for (int k = 0; k < vis; k++) {
        int li = first + k;
        int ty = y_bottom - (vis - k) * line_h;

        char buf[384];
        int len = L[li].len; if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
        memcpy(buf, t + L[li].start, len); buf[len] = 0;

        if (empty && li == caret_line) ui_text(x, ty, size, COL_TEXT_MUTED, "Type here\xE2\x80\xA6");
        else                           ui_text(x, ty, size, COL_TEXT, buf);

        if (li == caret_line && ((s_caret_tick / 28) & 1) == 0) {
            int off = caret - L[li].start;
            if (off < 0) off = 0;
            if (off > len) off = len;
            char pre[384]; memcpy(pre, t + L[li].start, off); pre[off] = 0;
            int cxp = x + (off > 0 ? ui_text_w(size, pre) : 0);
            ui_rect(cxp + 1, ty + 2, 2, size, COL_TEXT_BRIGHT);
        }
    }
}

void inlinekbd_draw(const char *header)
{
    if (!s_open) return;
    ui_prime_glyphs();
    s_caret_tick++;

    // Dim behind the panel.
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xB2));

    // Keyboard top from CHANGE_SIZE; assume ~300 until it arrives.
    int kbd_top = (s_kbd_top > 120 && s_kbd_top < SCREEN_H) ? s_kbd_top : 300;

    const int bx = 24, bw = SCREEN_W - 48;  // near full width
    const int pad = 16, head_h = 34, foot_h = 10, line_h = 24, size = FS_BODY;
    int bh = 204;  // ~6 visible lines
    if (bh > kbd_top - 28) bh = kbd_top - 28;  // fit the space above the keyboard
    int by = (kbd_top - bh) / 2;  // centered: equal gap top and bottom
    if (by < 14) by = 14;

    ui_rounded_rect(bx, by, bw, bh, 12, RGBA8(0x2b, 0x2d, 0x31, 0xFF));
    ui_rect(bx, by + head_h - 1, bw, 1, COL_DIVIDER);

    if (header && header[0])
        ui_text_bold_clip(bx + pad, by + 9, FS_SMALL, COL_TEXT_BRIGHT, header, bw - 2 * pad - 74);
    // Live char count, top-right in the header row (only while typing).
    const char *t = inlinekbd_text();
    if (t && t[0]) {
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d/2000", (int)strlen(t));
        ui_text(bx + bw - pad - ui_text_w(FS_TINY, cnt), by + 12, FS_TINY, COL_TEXT_MUTED, cnt);
    }

    int content_bottom = by + bh - foot_h;
    int maxlines = (content_bottom - (by + head_h)) / line_h;
    draw_live_text(bx + pad, content_bottom - 3, bw - 2 * pad - 6, size, line_h, maxlines);
}
