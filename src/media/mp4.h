#ifndef CORDLESS_MEDIA_MP4_H
#define CORDLESS_MEDIA_MP4_H

// Minimal MP4 (ISO-BMFF) demuxer for H.264: extracts the first video track's
// SPS/PPS and a per-sample table. Pure C, no Vita deps (offline-testable).

typedef struct {
    unsigned offset;  // absolute byte offset in the file
    unsigned size;  // bytes
    int      keyframe;  // 1 if a sync sample (IDR)
    unsigned duration;  // in media timescale ticks
} mp4_sample;

typedef struct {
    unsigned char *sps; int sps_len;  // SPS NAL payload (no start code / length)
    unsigned char *pps; int pps_len;
    int nal_length_size;  // avcC length prefix size (1/2/4)
    int width, height;
    unsigned timescale;  // video timescale (ticks/sec)
    mp4_sample *samples;
    int num_samples;
    int has_audio;
    unsigned char *asc; int asc_len;  // AudioSpecificConfig
    int audio_rate;  // Hz
    int audio_channels;
    unsigned audio_timescale;
    mp4_sample *audio_samples;
    int num_audio_samples;
} mp4_video;

// Returns 0 on success (fills `out`, caller frees with mp4_free), <0 on error.
int  mp4_parse(const unsigned char *data, int len, mp4_video *out);
void mp4_free(mp4_video *out);

#endif  // CORDLESS_MEDIA_MP4_H
