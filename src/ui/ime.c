#include "ui/ime.h"
#include "ui/inlinekbd.h"
#include "console.h"

#include <stdio.h>

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>

// Blocking text entry on the inline libime keyboard (inlinekbd.c): render a
// frame, swap, pump the keyboard outside the GXM scene each loop. START cancels.
bool ime_get_text(const char *title, const char *initial,
                  char *out, size_t out_cap)
{
    console_dbgf("[ime] get_text '%s'", title ? title : "");
    if (!inlinekbd_open(initial ? initial : "")) {
        console_logf("[ime] open FAILED");
        return false;
    }

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned prevb = pad.buttons;

    int r = 0, frames = 0;
    for (;;) {
        frames++;
        SceCtrlData cur;
        sceCtrlPeekBufferPositive(0, &cur, 1);
        unsigned pressed = cur.buttons & ~prevb;
        prevb = cur.buttons;
        SceTouchData td;
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);

        vita2d_start_drawing();
        vita2d_clear_screen();
        inlinekbd_draw(title);
        vita2d_end_drawing();
        vita2d_swap_buffers();

        r = inlinekbd_pump();  // sceImeUpdate, outside the GXM scene
        if (r != 0) break;  // 1 = Enter/Send, -1 = Close

        if (pressed & SCE_CTRL_START) {
            console_logf("[ime] START cancel (%d frames)", frames);
            inlinekbd_close();
            r = -1;
            break;
        }
    }
    console_dbgf("[ime] get_text done r=%d frames=%d", r, frames);

    bool ok = false;
    if (r == 1) {
        snprintf(out, out_cap, "%s", inlinekbd_text());
        ok = out[0] != '\0';
    } else if (out_cap) {
        out[0] = '\0';
    }

    // Wait for the dismiss finger to lift; bounded.
    for (int i = 0; i < 150; i++) {
        SceTouchData t2;
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t2, 1) < 0 || t2.reportNum == 0) break;
        sceKernelDelayThread(8000);
    }
    return ok;
}
