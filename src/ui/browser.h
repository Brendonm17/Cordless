#ifndef CORDLESS_UI_BROWSER_H
#define CORDLESS_UI_BROWSER_H

#include <stdbool.h>
#include <stddef.h>

// Main Discord browsing screen: server rail + channel sidebar + message view.
// Reads the shared store and fetches channel history on demand.

void browser_init(const char *token, const char *cacert_path);

// `pressed` = rising-edge buttons, `held` = current buttons, `ly` = left-stick
// Y (-128..127).
void browser_input(unsigned pressed, unsigned held, int ly);

void browser_draw(void);

// True while on-screen content is still animating (gates idle redraws).
bool browser_wants_redraw(void);

// Pump the inline message keyboard once per frame, after swap (outside any GXM
// scene). No-op when the keyboard isn't open.
void browser_ime_pump(void);

// Front-panel touch, screen-space (x,y). On a tap `tap` is true and `dy` is 0;
// during a drag `tap` is false and `dy` is the vertical movement since last frame.
void browser_touch(int x, int y, int dy, bool tap);

// True once when the user tapped Log Out in Settings (clears the request).
bool browser_wants_logout(void);

// pending_switch fills out with a saved account token, true once on switch;
// wants_add_account is true once on add-account.
bool browser_pending_switch(char *out, size_t out_cap);
bool browser_wants_add_account(void);

#endif  // CORDLESS_UI_BROWSER_H
