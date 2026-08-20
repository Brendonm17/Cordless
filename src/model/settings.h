#ifndef CORDLESS_MODEL_SETTINGS_H
#define CORDLESS_MODEL_SETTINGS_H

// App-local settings, saved to ux0:data/cordless/settings.bin.
typedef struct {
    int  theme;        // 0 dark, 1 light
    int  accent;       // accent preset index
    int  font_size;    // 0 small, 1 medium, 2 large
    int  density;      // 0 cozy, 1 compact
    int  clock_24h;    // 0 = 12-hour, 1 = 24-hour
    int  open_last;    // 0 open Home, 1 open last channel
    int  low_battery;  // 1 idle frame-skip, 0 always 60fps
} app_settings;

extern app_settings g_settings;

// Load from disk (or defaults), then apply theme + font.
void settings_load(void);
// Persist g_settings to disk.
void settings_save(void);

#endif  // CORDLESS_MODEL_SETTINGS_H
