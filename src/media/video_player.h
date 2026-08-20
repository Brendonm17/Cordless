#ifndef CORDLESS_MEDIA_VIDEO_PLAYER_H
#define CORDLESS_MEDIA_VIDEO_PLAYER_H

// Fetch, demux, and play an H.264 MP4 fullscreen via sceAvcdec. Blocking; returns
// on a tap, end of stream, or error. Hardware-only (no HW video decode in Vita3K).
void video_play(const char *url, const char *cacert_path);

#endif  // CORDLESS_MEDIA_VIDEO_PLAYER_H
