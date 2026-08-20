#include "ui/imagecache.h"
#include "api/discord.h"
#include "net_common.h"
#include "console.h"
#include "tls/https_get.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/rtc.h>

#include <webp/decode.h>
#include <webp/demux.h>

#include "stb_image.h"

// Cache slots: sized to hold every avatar/icon visible at once.
#define CACHE_N     192
#define MAX_FRAMES  48  // animation frames kept per GIF
#define GRAVE_MAX   256  // deferred-free texture slots

#define IMG_MAX_BYTES (6 * 1024 * 1024)

// E_DECODED: the worker has decoded the bytes to RGBA; the render pump uploads
// that RGBA to GPU textures.
typedef enum { E_EMPTY, E_QUEUED, E_FETCHING, E_DECODED, E_READY, E_FAILED } estate;

// Max pending fetches at once; excess requests are not enqueued.
#define QUEUE_MAX 16

typedef struct {
    char            url[256];
    estate          state;
    unsigned char  *rgba;  // worker-decoded frames, contiguous rgba_n*w*h*4
    int             rgba_w, rgba_h, rgba_n;
    int             rgba_delay[MAX_FRAMES];  // per-frame delay ms (animation)
    vita2d_texture *frames[MAX_FRAMES];
    int             cum_ms[MAX_FRAMES];  // cumulative end time of each frame
    int             nframes;  // 1 for a static image
    int             total_ms;  // animation loop length
    unsigned        used;  // LRU tick
    unsigned        seen_frame;  // render frame this was last requested
} entry;

static entry     s_cache[CACHE_N];
static SceUID    s_mutex;
static SceUID    s_thread = -1;
static volatile int s_run;
static char      s_cacert[256];
static unsigned  s_tick;
static unsigned  s_frame = 2;  // render-frame counter (bumped in pump)

// Evicted textures queue here and are freed in pump() after the GPU is idle.
static vita2d_texture *s_graveyard[GRAVE_MAX];
static int s_ngrave;
static int s_evicts;  // diagnostics: READY entries evicted


// On-disk cache keyed by an FNV-1a hash of the full URL (query included).
#define IMGCACHE_DIR "ux0:data/cordless/imgcache"

static void disk_path(const char *url, char *out, size_t cap)
{
    unsigned long long h = 1469598103934665603ULL;  // FNV-1a 64 offset basis
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        h ^= *p; h *= 1099511628211ULL;
    }
    snprintf(out, cap, "%s/%016llx", IMGCACHE_DIR, h);
}

static unsigned char *disk_read(const char *path, int *outn)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return NULL;
    SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > IMG_MAX_BYTES) { sceIoClose(fd); return NULL; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { sceIoClose(fd); return NULL; }
    int rd = sceIoRead(fd, buf, (SceSize)sz);
    sceIoClose(fd);
    if (rd != (int)sz) { free(buf); return NULL; }
    *outn = (int)sz;
    return buf;
}

static void disk_write(const char *path, const unsigned char *bytes, int n)
{
    // Write to a temp file, then rename over the target.
    char tmp[336]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return;
    int wr = sceIoWrite(fd, bytes, n);
    sceIoClose(fd);
    if (wr == n) { sceIoRemove(path); sceIoRename(tmp, path); }
    else         sceIoRemove(tmp);
}

// If the disk cache exceeds the cap, delete oldest files (by mtime) until it
// is under the low-water mark. Runs once on the worker thread.
#define IMGCACHE_CAP_BYTES (256LL * 1024 * 1024)
#define IMGCACHE_LOW_BYTES (192LL * 1024 * 1024)
#define PRUNE_MAX_ENTRIES  20000

typedef struct { unsigned long long mtime; long long size; char name[64]; } prune_ent;

static int prune_cmp(const void *a, const void *b)
{
    unsigned long long ma = ((const prune_ent *)a)->mtime;
    unsigned long long mb = ((const prune_ent *)b)->mtime;
    return ma < mb ? -1 : (ma > mb ? 1 : 0);  // oldest first
}

static void disk_cache_prune(void)
{
    SceUID d = sceIoDopen(IMGCACHE_DIR);
    if (d < 0) return;
    prune_ent *ents = malloc(sizeof(prune_ent) * PRUNE_MAX_ENTRIES);
    if (!ents) { sceIoDclose(d); return; }

    int n = 0; long long total = 0;
    SceIoDirent de; memset(&de, 0, sizeof(de));
    while (sceIoDread(d, &de) > 0) {
        if (de.d_name[0] != '.') {
            total += (long long)de.d_stat.st_size;  // sum every file
            if (n < PRUNE_MAX_ENTRIES) {  // store a bounded set to sort
                SceRtcTick t; t.tick = 0;
                sceRtcGetTick(&de.d_stat.st_mtime, &t);
                ents[n].mtime = t.tick;
                ents[n].size  = (long long)de.d_stat.st_size;
                snprintf(ents[n].name, sizeof(ents[n].name), "%s", de.d_name);
                n++;
            }
        }
        memset(&de, 0, sizeof(de));
    }
    sceIoDclose(d);

    if (total > IMGCACHE_CAP_BYTES) {
        console_dbgf("[img] disk cache %lldMB over cap; evicting oldest", total >> 20);
        qsort(ents, n, sizeof(prune_ent), prune_cmp);
        for (int i = 0; i < n && total > IMGCACHE_LOW_BYTES; i++) {
            char p[336]; snprintf(p, sizeof(p), "%s/%s", IMGCACHE_DIR, ents[i].name);
            if (sceIoRemove(p) >= 0) total -= ents[i].size;
        }
    }
    free(ents);
}

// Fetch image bytes: disk cache first, else HTTPS via mbedTLS; successful
// downloads are written to the disk cache.
static int fetch(const char *url, unsigned char **out, int *outn)
{
    char path[320]; disk_path(url, path, sizeof(path));
    unsigned char *cached = disk_read(path, outn);
    if (cached) { *out = cached; return 0; }

    long code = 0;
    net_lock();
    int rc = https_get_mbedtls(url, s_cacert, out, outn, &code, IMG_MAX_BYTES);
    net_unlock();
    if (rc == 0 && *out && *outn > 0) disk_write(path, *out, *outn);
    else console_logf("[img] http=%ld %.70s", code, url);
    return rc;
}

static int decode_to_rgba(entry *e, const unsigned char *bytes, int nb);  // fwd

static int worker(SceSize a, void *ap)
{
    (void)a; (void)ap;
    disk_cache_prune();  // prune the on-disk cache once
    while (s_run) {
        int idx = -1;
        char url[256];
        sceKernelLockMutex(s_mutex, 1, NULL);
        // Fetch the most-recently-requested queued image first.
        unsigned best = 0;
        for (int i = 0; i < CACHE_N; i++)
            if (s_cache[i].state == E_QUEUED && s_cache[i].used >= best) {
                best = s_cache[i].used; idx = i;
            }
        if (idx >= 0) {
            s_cache[idx].state = E_FETCHING;
            snprintf(url, sizeof(url), "%s", s_cache[idx].url);
        }
        sceKernelUnlockMutex(s_mutex, 1);

        if (idx < 0) { sceKernelDelayThread(60 * 1000); continue; }

        unsigned char *bytes = NULL; int n = 0;
        int ok = fetch(url, &bytes, &n);

        // Decode on the worker. Wait until fewer than 3 decoded buffers are
        // pending before decoding another.
        int made = 0;
        if (ok == 0) {
            while (s_run) {
                int pend = 0;
                sceKernelLockMutex(s_mutex, 1, NULL);
                for (int i = 0; i < CACHE_N; i++) if (s_cache[i].state == E_DECODED) pend++;
                sceKernelUnlockMutex(s_mutex, 1);
                if (pend < 3) break;
                sceKernelDelayThread(8 * 1000);
            }
            made = decode_to_rgba(&s_cache[idx], bytes, n);
        }
        free(bytes);

        sceKernelLockMutex(s_mutex, 1, NULL);
        if (strcmp(s_cache[idx].url, url) == 0 && s_cache[idx].state == E_FETCHING) {
            s_cache[idx].state = (made > 0) ? E_DECODED : E_FAILED;
        } else if (made > 0) {  // slot reused mid-decode: drop it
            free(s_cache[idx].rgba); s_cache[idx].rgba = NULL; s_cache[idx].rgba_n = 0;
        }
        sceKernelUnlockMutex(s_mutex, 1);
    }
    return 0;
}

// Each decoder fills e->rgba (contiguous nframes*w*h*4) plus per-frame delays.

// Upload one w*h RGBA frame into a fresh texture (render thread).
static vita2d_texture *tex_from_rgba(const unsigned char *rgba, int w, int h)
{
    vita2d_texture *t = vita2d_create_empty_texture(w, h);
    if (!t) return NULL;
    unsigned char *dst = vita2d_texture_get_datap(t);
    unsigned int stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * stride, rgba + (size_t)y * w * 4, (size_t)w * 4);
    return t;
}

// PNG / JPEG still -> a single RGBA frame (stbi). Oversized stills are
// integer-subsampled down to MAXD.
static int static_to_rgba(entry *e, const unsigned char *bytes, int nb)
{
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(bytes, nb, &w, &h, &comp, 4);
    if (!px || w < 1 || h < 1) { free(px); return 0; }
    const int MAXD = 1600;
    if (w > MAXD || h > MAXD) {
        int sx = (w + MAXD - 1) / MAXD, sy = (h + MAXD - 1) / MAXD;
        int s = sx > sy ? sx : sy;
        int nw = w / s, nh = h / s;
        if (nw < 1) nw = 1; if (nh < 1) nh = 1;
        unsigned char *dn = malloc((size_t)nw * nh * 4);
        if (dn) {
            for (int y = 0; y < nh; y++)
                for (int x = 0; x < nw; x++)
                    memcpy(dn + ((size_t)y * nw + x) * 4,
                           px + ((size_t)(y * s) * w + (x * s)) * 4, 4);
            free(px); px = dn; w = nw; h = nh;
        }
    }
    e->rgba = px; e->rgba_w = w; e->rgba_h = h; e->rgba_n = 1; e->rgba_delay[0] = 0;
    return 1;
}

// Animated GIF -> contiguous RGBA frames (stbi returns one buffer for all frames).
static int gif_to_rgba(entry *e, const unsigned char *bytes, int nb)
{
    int *delays = NULL, w = 0, h = 0, frames = 0, comp = 0;
    unsigned char *data = stbi_load_gif_from_memory(bytes, nb, &delays,
                                                    &w, &h, &frames, &comp, 4);
    if (!data || frames < 1 || w < 1 || h < 1 || w > 1024 || h > 1024) {
        free(data); free(delays); return 0;
    }
    int nf = frames;
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;
    long long budget = 8LL * 1024 * 1024;  // cap held pixels to ~8M
    if ((long long)w * h > 0) { int by = (int)(budget / ((long long)w * h)); if (nf > by) nf = by; }
    if (nf < 1) nf = 1;
    for (int f = 0; f < nf; f++) {
        int d = delays ? delays[f] : 100;
        e->rgba_delay[f] = d < 20 ? 100 : d;  // default for 0-delay frames
    }
    free(delays);
    e->rgba = data;  // render uploads the first nf frames
    e->rgba_w = w; e->rgba_h = h; e->rgba_n = nf;
    return nf;
}

// WebP (animated or still) -> contiguous RGBA; each frame is copied into one
// buffer.
static int webp_to_rgba(entry *e, const unsigned char *bytes, int nb)
{
    WebPData wd = { bytes, (size_t)nb };
    WebPAnimDecoderOptions opts;
    if (!WebPAnimDecoderOptionsInit(&opts)) return 0;
    opts.color_mode = MODE_RGBA;
    opts.use_threads = 0;
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opts);
    if (!dec) return 0;
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info) ||
        info.canvas_width < 1 || info.canvas_height < 1 ||
        info.canvas_width > 1024 || info.canvas_height > 1024) {
        WebPAnimDecoderDelete(dec); return 0;
    }
    int w = (int)info.canvas_width, h = (int)info.canvas_height;
    int nf = (int)info.frame_count;
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;
    long long budget = 8LL * 1024 * 1024;
    if ((long long)w * h > 0) { int by = (int)(budget / ((long long)w * h)); if (nf > by) nf = by; }
    if (nf < 1) nf = 1;
    unsigned char *out = malloc((size_t)nf * w * h * 4);
    if (!out) { WebPAnimDecoderDelete(dec); return 0; }
    int made = 0, prev = 0;
    for (int f = 0; f < nf; f++) {
        uint8_t *buf = NULL; int ts = 0;  // ts = cumulative end time (ms)
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
        memcpy(out + (size_t)made * w * h * 4, buf, (size_t)w * h * 4);
        int d = ts - prev; prev = ts;
        e->rgba_delay[made] = d < 20 ? 100 : d;
        made++;
    }
    WebPAnimDecoderDelete(dec);
    if (made < 1) { free(out); return 0; }
    e->rgba = out; e->rgba_w = w; e->rgba_h = h; e->rgba_n = made;
    return made;
}

// Worker-thread dispatch: fetched bytes -> e->rgba. Returns frame count, 0 on fail.
static int decode_to_rgba(entry *e, const unsigned char *bytes, int nb)
{
    e->rgba = NULL; e->rgba_n = 0; e->rgba_w = e->rgba_h = 0;
    if (!bytes || nb < 4) return 0;
    if (nb >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F')
        return gif_to_rgba(e, bytes, nb);
    if (nb >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' &&
        bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' &&
        bytes[10] == 'B' && bytes[11] == 'P')
        return webp_to_rgba(e, bytes, nb);
    return static_to_rgba(e, bytes, nb);  // PNG / JPEG
}

// Render-thread upload: e->rgba frames -> GPU textures; frees e->rgba.
static int upload_rgba(entry *e)
{
    int w = e->rgba_w, h = e->rgba_h, n = e->rgba_n, made = 0, cum = 0;
    if (!e->rgba || w < 1 || h < 1) { free(e->rgba); e->rgba = NULL; return 0; }
    for (int f = 0; f < n && f < MAX_FRAMES; f++) {
        vita2d_texture *t = tex_from_rgba(e->rgba + (size_t)f * w * h * 4, w, h);
        if (!t) break;
        e->frames[made] = t;
        if (n > 1) { cum += e->rgba_delay[f]; e->cum_ms[made] = cum; }
        else         e->cum_ms[made] = 0;
        made++;
    }
    free(e->rgba); e->rgba = NULL; e->rgba_n = 0;
    e->nframes = made;
    e->total_ms = (n > 1 && made > 0) ? e->cum_ms[made - 1] : 0;
    return made;
}

// Move an entry's textures to the graveyard for deferred free. Returns false if
// the graveyard can't hold them all (caller retries next frame).
static bool entry_to_grave(entry *e)
{
    for (int f = 0; f < e->nframes; f++) {
        if (!e->frames[f]) continue;
        if (s_ngrave >= GRAVE_MAX) return false;
        s_graveyard[s_ngrave++] = e->frames[f];
        e->frames[f] = NULL;
    }
    e->nframes = 0;
    return true;
}


void imagecache_init(const char *cacert_path)
{
    memset(s_cache, 0, sizeof(s_cache));
    snprintf(s_cacert, sizeof(s_cacert), "%s", cacert_path);
    sceIoMkdir("ux0:data/cordless", 0777);
    sceIoMkdir(IMGCACHE_DIR, 0777);
    s_mutex = sceKernelCreateMutex("img_cache", 0, 0, NULL);
    s_run = 1;
    // Pin the decode worker to CPU core 1 (render=0, images=1, gateway=2).
    s_thread = sceKernelCreateThread("img_worker", worker, 0x40, 0x100000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (s_thread >= 0) sceKernelStartThread(s_thread, 0, NULL);
}

void imagecache_fini(void)
{
    s_run = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
}

// Find the entry for `url`. Returns it if READY; otherwise claims a slot and
// queues a background fetch.
static entry *touch(const char *url)
{
    if (!url || !url[0]) return NULL;
    entry *ready = NULL;
    sceKernelLockMutex(s_mutex, 1, NULL);

    int found = -1, empty = -1, lru = -1, pending = 0;
    unsigned lru_used = 0xFFFFFFFF;
    for (int i = 0; i < CACHE_N; i++) {
        if (s_cache[i].state != E_EMPTY && strcmp(s_cache[i].url, url) == 0) {
            found = i; break;
        }
        if (s_cache[i].state == E_QUEUED || s_cache[i].state == E_FETCHING ||
            s_cache[i].state == E_DECODED) pending++;
        if (s_cache[i].state == E_EMPTY) { if (empty < 0) empty = i; }
        else if (s_cache[i].state == E_READY || s_cache[i].state == E_FAILED) {
            // Skip entries requested within the last 2 frames.
            bool onscreen = s_cache[i].seen_frame + 2 >= s_frame;
            if (!onscreen && s_cache[i].used < lru_used) {
                lru = i; lru_used = s_cache[i].used;
            }
        }
    }

    if (found >= 0) {
        s_cache[found].used = ++s_tick;
        s_cache[found].seen_frame = s_frame;
        if (s_cache[found].state == E_READY) ready = &s_cache[found];
    } else if (pending < QUEUE_MAX) {  // bounded queue
        int slot = (empty >= 0) ? empty : lru;
        if (slot >= 0 && entry_to_grave(&s_cache[slot])) {
            entry *e = &s_cache[slot];
            if (empty < 0 && e->state == E_READY) s_evicts++;
            free(e->rgba); e->rgba = NULL; e->rgba_n = 0;
            e->total_ms = 0;
            snprintf(e->url, sizeof(e->url), "%s", url);
            e->state = E_QUEUED; e->used = ++s_tick;
            e->seen_frame = s_frame;
        }
    }
    sceKernelUnlockMutex(s_mutex, 1);
    return ready;
}

vita2d_texture *imagecache_get(const char *url)
{
    entry *e = touch(url);
    return e ? e->frames[0] : NULL;
}

// Render-loop dirty signals: an animated frame was drawn this frame, or a
// fetched image was uploaded.
static bool s_anim_seen, s_uploaded;
void imagecache_frame_begin(void) { s_anim_seen = false; }
bool imagecache_animating(void)   { return s_anim_seen; }
bool imagecache_uploaded(void)    { bool u = s_uploaded; s_uploaded = false; return u; }

vita2d_texture *imagecache_get_anim(const char *url, long long now_ms)
{
    entry *e = touch(url);
    if (!e) return NULL;
    if (e->nframes <= 1 || e->total_ms <= 0) return e->frames[0];
    s_anim_seen = true;  // a GIF is on screen; keep redrawing
    int m = (int)(now_ms % e->total_ms);
    for (int f = 0; f < e->nframes; f++)
        if (m < e->cum_ms[f]) return e->frames[f];
    return e->frames[e->nframes - 1];
}

void imagecache_pump(void)
{
    s_frame++;  // one render frame per pump

    // Free evicted textures once the GPU is done with the last frame.
    if (s_ngrave) {
        vita2d_wait_rendering_done();
        sceKernelLockMutex(s_mutex, 1, NULL);
        while (s_ngrave) vita2d_free_texture(s_graveyard[--s_ngrave]);
        sceKernelUnlockMutex(s_mutex, 1);
    }

    // Upload worker-decoded RGBA to GPU textures, up to 2 per frame.
    for (int pass = 0; pass < 2; pass++) {
        int idx = -1;
        sceKernelLockMutex(s_mutex, 1, NULL);
        for (int i = 0; i < CACHE_N; i++)
            if (s_cache[i].state == E_DECODED) { idx = i; break; }
        sceKernelUnlockMutex(s_mutex, 1);
        if (idx < 0) return;

        vita2d_wait_rendering_done();  // GPU idle before creating textures
        int made = upload_rgba(&s_cache[idx]);
        if (made > 0) s_uploaded = true;  // a new image appeared; force a redraw

        sceKernelLockMutex(s_mutex, 1, NULL);
        s_cache[idx].state = made > 0 ? E_READY : E_FAILED;
        sceKernelUnlockMutex(s_mutex, 1);
    }
}
