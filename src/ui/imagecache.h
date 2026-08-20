#ifndef CORDLESS_UI_IMAGECACHE_H
#define CORDLESS_UI_IMAGECACHE_H

#include <vita2d.h>
#include <stdbool.h>

// Async image cache for Discord CDN avatars/icons. A worker thread fetches and
// decodes; imagecache_get() returns the texture once ready, NULL meanwhile.

void imagecache_init(const char *cacert_path);
void imagecache_fini(void);

// Request the image at `url`. Returns the texture if ready, else NULL and
// queues a background load.
vita2d_texture *imagecache_get(const char *url);

// Like imagecache_get but, for animated GIFs, returns the frame for `now_ms`
// (a monotonic millisecond clock). Static images ignore now_ms.
vita2d_texture *imagecache_get_anim(const char *url, long long now_ms);

// Upload a few decoded images to GPU textures. Call once per frame on the main
// (render) thread.
void imagecache_pump(void);

// Render-loop dirty signals. frame_begin() resets; animating() reports whether
// an animated frame was drawn; uploaded() returns and clears the upload flag.
void imagecache_frame_begin(void);
bool imagecache_animating(void);
bool imagecache_uploaded(void);

#endif  // CORDLESS_UI_IMAGECACHE_H
