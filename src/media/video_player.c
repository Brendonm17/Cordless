#include "media/video_player.h"
#include "media/mp4.h"
#include "tls/https_get.h"
#include "net_common.h"
#include "console.h"
#include "ui/ui.h"
#include "ui/theme.h"

#include <stdlib.h>
#include <string.h>

#include <vita2d.h>
#include <psp2/videodec.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/touch.h>

#define VID_MAX_BYTES (32 * 1024 * 1024)
#define AUD_GRAIN     1024  // one AAC frame
#define AUD_MAX_PCM   (24 * 1024 * 1024)  // cap decoded audio

// Decode every AAC sample with one (ch,rate,sbr) config; the caller inits the
// library. Returns frame count and fills *ppcm (caller owns), or 0 on failure.
static int try_aac(const mp4_video *mv, const unsigned char *data, int n,
                   int ch, int rate, int sbr, short **ppcm)
{
    SceAudiodecInfo info; memset(&info, 0, sizeof(info));
    info.aac.size = sizeof(SceAudiodecInfoAac);
    info.aac.isAdts = 0; info.aac.ch = ch; info.aac.samplingRate = rate; info.aac.isSbr = sbr;
    SceAudiodecCtrl ctrl; memset(&ctrl, 0, sizeof(ctrl));
    ctrl.size = sizeof(ctrl); ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    ctrl.maxEsSize = 8192; ctrl.maxPcmSize = 4096 * ch; ctrl.pInfo = &info;

    short *tmp = malloc(ctrl.maxPcmSize);
    if (!tmp || sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AAC) < 0) {
        free(tmp); return 0;
    }
    size_t cap = 4 * 1024 * 1024, len = 0;
    unsigned char *pcm = malloc(cap);
    if (!pcm) { sceAudiodecDeleteDecoder(&ctrl); free(tmp); return 0; }

    for (int i = 0; i < mv->num_audio_samples; i++) {
        const mp4_sample *s = &mv->audio_samples[i];
        if (s->offset + s->size > (unsigned)n || s->size == 0 || s->size > ctrl.maxEsSize) continue;
        ctrl.pEs = (SceUInt8 *)(data + s->offset); ctrl.inputEsSize = s->size;
        ctrl.pPcm = tmp; ctrl.outputPcmSize = 0;
        if (sceAudiodecDecode(&ctrl) < 0) continue;
        unsigned got = ctrl.outputPcmSize;
        if (got == 0) continue;
        if (len + got > cap) {
            if (cap >= AUD_MAX_PCM) break;
            size_t ncp = cap * 2; if (ncp > AUD_MAX_PCM) ncp = AUD_MAX_PCM;
            unsigned char *np = realloc(pcm, ncp);
            if (!np) break;
            pcm = np; cap = ncp;
            if (len + got > cap) continue;
        }
        memcpy(pcm + len, tmp, got); len += got;
    }
    sceAudiodecDeleteDecoder(&ctrl);
    free(tmp);

    int frames = (int)(len / (ch * 2));
    console_dbgf("[vid] audio: try ch=%d rate=%d sbr=%d -> %d frames", ch, rate, sbr, frames);
    if (frames < 1) { free(pcm); return 0; }
    *ppcm = (short *)pcm;
    return frames;
}

static int decode_audio(const mp4_video *mv, const unsigned char *data, int n, short **ppcm, int *pch)
{
    if (!mv->has_audio || mv->num_audio_samples < 1 || !mv->asc) {
        console_logf("[vid] audio: none (has=%d nsamp=%d asc=%d)",
                     mv->has_audio, mv->num_audio_samples, mv->asc != NULL);
        return 0;
    }
    int ch = mv->audio_channels ? mv->audio_channels : 2;
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    int rate = mv->audio_rate > 0 ? mv->audio_rate : 44100;
    int sbr = (mv->asc[0] >> 3) == 5 ? 1 : 0;  // objType 5 = HE-AAC (SBR)

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC) < 0) {
        console_logf("[vid] audio: AUDIOCODEC module load fail"); return 0;
    }
    SceAudiodecInitParam ip; memset(&ip, 0, sizeof(ip));
    ip.aac.size = sizeof(SceAudiodecInitStreamParam); ip.aac.totalStreams = 1;
    int alr = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ip);
    if (alr < 0) { console_logf("[vid] audio: initLibrary fail 0x%08X", (unsigned)alr); return 0; }

    // Try the stream's config first, then fallbacks: flip SBR (HE-AAC vs LC), swap
    // channel count, last-ditch stereo/44100/LC. First success wins.
    int cch[5], crate[5], csbr[5], nc = 0;
    cch[nc] = ch; crate[nc] = rate; csbr[nc] = sbr;  nc++;
    cch[nc] = ch; crate[nc] = rate; csbr[nc] = !sbr; nc++;
    if (ch != 2) { cch[nc] = 2; crate[nc] = rate; csbr[nc] = sbr; nc++; }
    if (ch != 1) { cch[nc] = 1; crate[nc] = rate; csbr[nc] = sbr; nc++; }
    cch[nc] = 2; crate[nc] = 44100; csbr[nc] = 0; nc++;

    int frames = 0, gotch = ch;
    for (int i = 0; i < nc && frames < 1; i++) {
        frames = try_aac(mv, data, n, cch[i], crate[i], csbr[i], ppcm);
        if (frames > 0) gotch = cch[i];
    }
    sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
    if (frames < 1) return 0;
    *pch = gotch;
    return frames;
}

typedef struct {
    short *pcm; int frames; int rate; int ch;
    volatile int *stop;
} audio_ctx;

// Feed pre-decoded PCM to sceAudioOut; each Output call blocks one grain, so
// playback self-paces in real time.
static int audio_thread(SceSize argsz, void *argp)
{
    (void)argsz;
    audio_ctx *a = *(audio_ctx **)argp;
    int mode = a->ch == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUD_GRAIN, a->rate, mode);
    if (port < 0) return 0;
    short tail[AUD_GRAIN * 2];
    int pos = 0;
    while (!*a->stop && pos < a->frames) {
        int rem = a->frames - pos;
        const short *chunk;
        if (rem >= AUD_GRAIN) { chunk = a->pcm + (size_t)pos * a->ch; pos += AUD_GRAIN; }
        else {
            memcpy(tail, a->pcm + (size_t)pos * a->ch, (size_t)rem * a->ch * 2);
            memset(tail + (size_t)rem * a->ch, 0, (size_t)(AUD_GRAIN - rem) * a->ch * 2);
            chunk = tail; pos += rem;
        }
        sceAudioOutOutput(port, chunk);
    }
    sceAudioOutReleasePort(port);
    return 0;
}

// Allocate phys-contiguous memory the codec engine can DMA.
static void *phys_alloc(int size, SceUID *out_id)
{
    int a = (size + 0xFFFFF) & ~0xFFFFF;  // 1 MiB granularity
    SceUID id = sceKernelAllocMemBlock("cl_vid",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, a, NULL);
    if (id < 0) { *out_id = -1; return NULL; }
    void *p = NULL; sceKernelGetMemBlockBase(id, &p);
    *out_id = id; return p;
}

// Upload a decoded RGBA frame (source stride = pitch px) to a vita2d texture.
static void upload_frame(vita2d_texture *t, const unsigned char *src, int w, int h, int pitch_px)
{
    unsigned char *dst = vita2d_texture_get_datap(t);
    unsigned int dstride = vita2d_texture_get_stride(t);
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * dstride, src + (size_t)y * pitch_px * 4, (size_t)w * 4);
}

static int tapped(void)
{
    SceTouchData td;
    return sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) >= 0 && td.reportNum > 0;
}

void video_play(const char *url, const char *cacert_path)
{
    // Dim the chat behind a "Loading video" overlay during the blocking download.
    for (int f = 0; f < 2; f++) {  // both display buffers
        vita2d_start_drawing();
        ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xE6));
        const char *lm = "Loading video\xE2\x80\xA6";
        int lw = ui_text_w(FS_BODY, lm);
        ui_text(SCREEN_W / 2 - lw / 2, SCREEN_H / 2 - 12, FS_BODY, COL_TEXT_BRIGHT, lm);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    // 1. Fetch the whole file (attachments <= 25 MB).
    unsigned char *data = NULL; int n = 0; long http = 0;
    net_lock();
    int rc = https_get_mbedtls(url, cacert_path, &data, &n, &http, VID_MAX_BYTES);
    net_unlock();
    if (rc != 0 || !data) { console_logf("[vid] fetch fail rc=%d http=%ld", rc, http); return; }

    // 2. Demux.
    mp4_video mv;
    if (mp4_parse(data, n, &mv) != 0 || mv.num_samples < 1 || !mv.sps || !mv.pps) {
        console_logf("[vid] demux fail"); free(data); return;
    }
    int W = mv.width, H = mv.height;
    console_dbgf("[vid] %dx%d samples=%d nal=%d", W, H, mv.num_samples, mv.nal_length_size);

    // 3. Decoder setup. The HW AVC decoder needs multiple-of-16 dimensions;
    // a raw 480x854 fails initLibrary.
    int aW = (W + 15) & ~15, aH = (H + 15) & ~15;
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);  // ok if already loaded
    SceVideodecQueryInitInfoHwAvcdec init; memset(&init, 0, sizeof(init));
    init.size = sizeof(init); init.horizontal = aW; init.vertical = aH;
    init.numOfRefFrames = 3; init.numOfStreams = 1;
    SceUID fb_id = -1, ob_id = -1; void *fb = NULL, *ob = NULL;
    SceAvcdecCtrl dec; memset(&dec, 0, sizeof(dec));
    int lib_ok = 0, dec_ok = 0;
    SceUID athread = -1; short *apcm = NULL; int ach = 0, aframes = 0;
    volatile int astop = 0;

    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);  // clear a stale init
    int ilr = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (ilr < 0) {
        console_logf("[vid] initLibrary fail 0x%08X (%dx%d aligned)", (unsigned)ilr, aW, aH);
        goto cleanup;
    }
    lib_ok = 1;
    SceAvcdecQueryDecoderInfo q; memset(&q, 0, sizeof(q));
    q.horizontal = aW; q.vertical = aH; q.numOfRefFrames = 3;
    SceAvcdecDecoderInfo di; memset(&di, 0, sizeof(di));
    if (sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di) < 0) {
        console_logf("[vid] queryMemSize fail"); goto cleanup;
    }
    fb = phys_alloc((int)di.frameMemSize, &fb_id);
    if (!fb) { console_logf("[vid] frameBuf alloc fail"); goto cleanup; }
    dec.frameBuf.pBuf = fb; dec.frameBuf.size = di.frameMemSize;
    if (sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &dec, &q) < 0) {
        console_logf("[vid] createDecoder fail"); goto cleanup;
    }
    dec_ok = 1;
    ob = phys_alloc(aW * aH * 4, &ob_id);  // RGBA output (aligned, pitch==aW)
    if (!ob) { console_logf("[vid] outBuf alloc fail"); goto cleanup; }

    // Scratch AU buffer: largest sample + SPS/PPS + start codes.
    unsigned maxs = 0;
    for (int i = 0; i < mv.num_samples; i++) if (mv.samples[i].size > maxs) maxs = mv.samples[i].size;
    unsigned char *au = malloc(maxs + mv.sps_len + mv.pps_len + 64);
    vita2d_texture *tex = vita2d_create_empty_texture(W, H);
    if (!au || !tex) { console_logf("[vid] scratch/tex alloc fail"); free(au); if (tex) vita2d_free_texture(tex); goto cleanup; }

    // 3b. Decode audio up front, then start a real-time playback thread.
    aframes = decode_audio(&mv, data, n, &apcm, &ach);
    audio_ctx actx = { apcm, aframes, mv.audio_rate > 0 ? mv.audio_rate : 44100, ach, &astop };
    audio_ctx *actxp = &actx;
    if (aframes > 0) {
        athread = sceKernelCreateThread("cl_aud", audio_thread, 0x10000100, 0x4000, 0, 0, NULL);
        if (athread >= 0) sceKernelStartThread(athread, sizeof(actxp), &actxp);
        else { astop = 1; }
        console_dbgf("[vid] audio %dch %dHz frames=%d", ach, actx.rate, aframes);
    }

    // 4. Play loop (once through). Tap closes.
    long long start = (long long)(sceKernelGetProcessTimeWide() / 1000);
    long long play_ms = 0;
    int closed = 0;
    for (int i = 0; i < mv.num_samples && !closed; i++) {
        mp4_sample *s = &mv.samples[i];
        if (s->offset + s->size > (unsigned)n) continue;
        // Build an Annex-B access unit (SPS/PPS before each keyframe).
        int p = 0;
        if (s->keyframe) {
            au[p++]=0;au[p++]=0;au[p++]=0;au[p++]=1; memcpy(au+p, mv.sps, mv.sps_len); p += mv.sps_len;
            au[p++]=0;au[p++]=0;au[p++]=0;au[p++]=1; memcpy(au+p, mv.pps, mv.pps_len); p += mv.pps_len;
        }
        const unsigned char *sp = data + s->offset; int rem = (int)s->size;
        while (rem > mv.nal_length_size) {
            unsigned nl = 0;
            for (int k = 0; k < mv.nal_length_size; k++) nl = (nl << 8) | sp[k];
            sp += mv.nal_length_size; rem -= mv.nal_length_size;
            if (nl > (unsigned)rem) break;
            au[p++]=0;au[p++]=0;au[p++]=0;au[p++]=1;
            memcpy(au+p, sp, nl); p += nl; sp += nl; rem -= nl;
        }

        SceAvcdecAu adu; memset(&adu, 0, sizeof(adu));
        adu.es.pBuf = au; adu.es.size = p;
        // The HW decoder needs the "no timestamp" sentinel (0xFFFFFFFF) per videodec.h;
        // with pts/dts left 0 it swallows every frame (numOfOutput stays 0).
        adu.pts.upper = adu.pts.lower = 0xFFFFFFFF;
        adu.dts.upper = adu.dts.lower = 0xFFFFFFFF;
        SceAvcdecFrame fr; memset(&fr, 0, sizeof(fr));
        fr.pixelType   = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        fr.framePitch  = aW; fr.frameWidth = aW; fr.frameHeight = aH;  // aligned; pitch==width
        fr.opt.rgba.alpha = 0xFF;  // opaque (0 -> transparent -> invisible)
        fr.pPicture[0] = ob;
        SceAvcdecPicture pic; memset(&pic, 0, sizeof(pic));
        pic.size = sizeof(pic); pic.frame = fr;
        SceAvcdecPicture *picp = &pic;
        SceAvcdecArrayPicture arr; memset(&arr, 0, sizeof(arr));
        arr.numOfElm = 1; arr.pPicture = &picp;

        int dr = sceAvcdecDecode(&dec, &adu, &arr);
        if (i < 4) console_dbgf("[vid] frame%d dr=0x%08X out=%d au=%d",
                                i, (unsigned)dr, (int)arr.numOfOutput, p);
        if (dr >= 0 && arr.numOfOutput > 0) {
            vita2d_wait_rendering_done();
            upload_frame(tex, ob, W, H, aW);  // src stride = aligned width
        }

        // Present, scaled to fit the screen, and pace to the frame's duration.
        float sx = 960.0f / W, sy = 544.0f / H, sc = sx < sy ? sx : sy;
        int dw = (int)(W * sc), dh = (int)(H * sc);
        vita2d_start_drawing();
        vita2d_clear_screen();
        ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 0xE6));  // shadowbox-black letterbox
        vita2d_draw_texture_scale(tex, (960 - dw) / 2, (544 - dh) / 2, sc, sc);
        vita2d_end_drawing();
        vita2d_swap_buffers();

        play_ms += mv.timescale ? (long long)s->duration * 1000 / mv.timescale : 40;
        long long now = (long long)(sceKernelGetProcessTimeWide() / 1000) - start;
        long long wait = play_ms - now;
        if (wait > 0 && wait < 2000) sceKernelDelayThread((int)wait * 1000);
        if (tapped()) closed = 1;
    }
    // Stop audio (signal + wake any blocked Output by waiting it out).
    astop = 1;
    if (athread >= 0) { sceKernelWaitThreadEnd(athread, NULL, NULL); sceKernelDeleteThread(athread); athread = -1; }
    free(apcm); apcm = NULL;

    // Drain the closing tap.
    for (int i = 0; i < 150 && tapped(); i++) sceKernelDelayThread(8000);
    vita2d_free_texture(tex); free(au);

cleanup:
    astop = 1;
    if (athread >= 0) { sceKernelWaitThreadEnd(athread, NULL, NULL); sceKernelDeleteThread(athread); }
    free(apcm);
    if (dec_ok) sceAvcdecDeleteDecoder(&dec);
    if (ob_id >= 0) sceKernelFreeMemBlock(ob_id);
    if (fb_id >= 0) sceKernelFreeMemBlock(fb_id);
    if (lib_ok) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    mp4_free(&mv);
    free(data);
    console_dbgf("[vid] done");
}
