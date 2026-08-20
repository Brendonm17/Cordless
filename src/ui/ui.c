#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/emoji_atlas.h"
#include "ui/imagecache.h"

#include <stdio.h>
#include <string.h>

#include <vita2d.h>

static vita2d_texture *s_emoji_atlas;

static char s_reg_path[256], s_semi_path[256];
static vita2d_font *s_base_reg, *s_base_semi;
static vita2d_pvf  *s_pad_pvf;

#define FONT_MAX_SLOTS 24
static struct { int size; bool bold; vita2d_font *f; } s_fslots[FONT_MAX_SLOTS];
static int s_nfslots;

// theme.h FS_* sizes
static const int s_ui_sizes[] = { 12, 14, 16, 17, 18, 20 };

static vita2d_font *font_for(int size, bool bold)
{
    if (size < 1) size = 1;
    for (int i = 0; i < s_nfslots; i++)
        if (s_fslots[i].size == size && s_fslots[i].bold == bold) return s_fslots[i].f;
    if (s_nfslots < FONT_MAX_SLOTS) {
        vita2d_font *f = vita2d_load_font_file(bold ? s_semi_path : s_reg_path);
        if (f) {
            s_fslots[s_nfslots].size = size;
            s_fslots[s_nfslots].bold = bold;
            s_fslots[s_nfslots].f    = f;
            s_nfslots++;
            return f;
        }
    }
    return bold ? s_base_semi : s_base_reg;
}
static inline vita2d_font *freg(int size)  { return font_for(size, false); }
static inline vita2d_font *fbold(int size) { return font_for(size, true);  }

bool ui_init(const char *regular_ttf, const char *semibold_ttf,
             const char *emoji_atlas_png)
{
    snprintf(s_reg_path,  sizeof(s_reg_path),  "%s", regular_ttf);
    snprintf(s_semi_path, sizeof(s_semi_path), "%s",
             semibold_ttf ? semibold_ttf : regular_ttf);
    s_base_reg  = vita2d_load_font_file(s_reg_path);
    s_base_semi = vita2d_load_font_file(s_semi_path);
    if (!s_base_semi) s_base_semi = s_base_reg;
    if (emoji_atlas_png)
        s_emoji_atlas = vita2d_load_PNG_file(emoji_atlas_png);
    // System font with the PS button glyphs.
    s_pad_pvf = vita2d_load_custom_pvf("sa0:data/font/pvf/psexchar.pvf");
    // Pre-create a handle per UI text size; rasterization happens in ui_prime_glyphs.
    for (unsigned i = 0; i < sizeof(s_ui_sizes) / sizeof(s_ui_sizes[0]); i++) {
        font_for(s_ui_sizes[i], false);
        font_for(s_ui_sizes[i], true);
    }
    return s_base_reg != NULL;
}

// Rasterizes a few glyphs per call; call once per frame inside a draw frame.
void ui_prime_glyphs(void)
{
    static int s0 = 0;
    static const char HOT[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@";
    if (s_base_semi && HOT[s0]) {
        for (int b = 0; b < 6 && HOT[s0]; b++) {
            char one[2] = { HOT[s0++], '\0' };
            vita2d_font_draw_text(fbold(18), 0, -100, 0, 18, one);
        }
        return;
    }
    static int slot = 0, ch = 32;
    if (!s_base_reg || slot >= s_nfslots) return;
    for (int b = 0; b < 6 && slot < s_nfslots; b++) {
        char one[2] = { (char)ch, '\0' };
        vita2d_font_draw_text(s_fslots[slot].f, 0, -100, 0, s_fslots[slot].size, one);
        if (++ch >= 127) { ch = 32; slot++; }
    }
}

void ui_fini(void)
{
    for (int i = 0; i < s_nfslots; i++)
        if (s_fslots[i].f) vita2d_free_font(s_fslots[i].f);
    s_nfslots = 0;
    if (s_base_semi && s_base_semi != s_base_reg) vita2d_free_font(s_base_semi);
    if (s_base_reg) vita2d_free_font(s_base_reg);
    if (s_emoji_atlas) vita2d_free_texture(s_emoji_atlas);
    if (s_pad_pvf) vita2d_free_pvf(s_pad_pvf);
    s_base_reg = s_base_semi = NULL; s_emoji_atlas = NULL; s_pad_pvf = NULL;
}

// Emoji draw from a bundled Twemoji atlas texture (assets/emoji_atlas.png).

static unsigned utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    unsigned cp; int n;
    if (s[0] < 0x80)        { cp = s[0]; n = 1; }
    else if ((s[0]&0xE0)==0xC0) { cp = s[0]&0x1F; n = 2; }
    else if ((s[0]&0xF0)==0xE0) { cp = s[0]&0x0F; n = 3; }
    else if ((s[0]&0xF8)==0xF0) { cp = s[0]&0x07; n = 4; }
    else { *pp += 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((s[i]&0xC0)!=0x80) { *pp += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i]&0x3F);
    }
    *pp += n;
    return cp;
}

static bool bmp_maybe_emoji(unsigned cp)
{
    return (cp >= 0x2600 && cp <= 0x27BF) ||  // misc symbols + dingbats
           (cp >= 0x2B00 && cp <= 0x2BFF) ||
           (cp >= 0x2190 && cp <= 0x21FF) ||  // arrows
           (cp >= 0x2300 && cp <= 0x23FF) ||  // misc technical
           (cp >= 0x25A0 && cp <= 0x25FF) ||
           cp == 0x2122 || cp == 0x2139 || cp == 0x24C2 ||
           cp == 0x00A9 || cp == 0x00AE || cp == 0x303D ||
           (cp >= 0x2934 && cp <= 0x2935) || (cp >= 0x3297 && cp <= 0x3299);
}
static bool bmp_always_emoji(unsigned cp)
{
    switch (cp) {
    case 0x2764: case 0x2B50: case 0x2705: case 0x274C: case 0x2728:
    case 0x26A1: case 0x2757: case 0x2753: case 0x2755: case 0x2754:
    case 0x2734: case 0x2733: case 0x2795: case 0x2796: case 0x2797:
    case 0x2b55: case 0x274E: case 0x2049: case 0x203C:
        return true;
    default: return false;
    }
}

static bool is_emoji_start(unsigned cp, unsigned next)
{
    if (cp >= 0x1F000) return true;  // astral: always emoji
    if (bmp_always_emoji(cp)) return true;
    if (bmp_maybe_emoji(cp) && next == 0xFE0F) return true;
    return false;
}

static unsigned utf8_peek(const char *p)
{
    if (!*p) return 0;
    return utf8_next(&p);
}

// Consume the rest of an emoji cluster after `first`, filling cps/ncp/has_zwj.
static void emoji_cluster(const char **pp, unsigned first,
                          unsigned *cps, int cap, int *ncp, bool *has_zwj)
{
    int n = 0; *has_zwj = false;
    if (n < cap) cps[n++] = first;
    for (;;) {
        unsigned c = utf8_peek(*pp);
        if (c == 0xFE0F || (c >= 0x1F3FB && c <= 0x1F3FF) || c == 0x20E3) {
            const char *q = *pp; utf8_next(&q); *pp = q;
            if (n < cap) cps[n++] = c;
            continue;
        }
        if (c == 0x200D) {
            *has_zwj = true;
            const char *q = *pp; utf8_next(&q); *pp = q;
            if (n < cap) cps[n++] = c;
            unsigned j = utf8_peek(*pp);
            if (j) { const char *r = *pp; utf8_next(&r); *pp = r;
                     if (n < cap) cps[n++] = j; }
            continue;
        }
        break;
    }
    *ncp = n;
}

// Build the Twemoji atlas key for a cluster: strip U+FE0F unless the cluster
// has a ZWJ. Produces e.g. "1f525", "2764".
static void twemoji_key(const unsigned *cps, int n, bool has_zwj,
                        char *out, size_t cap)
{
    int p = 0; bool first = true;
    out[0] = '\0';
    for (int i = 0; i < n && p < (int)cap - 8; i++) {
        if (cps[i] == 0xFE0F && !has_zwj) continue;
        p += snprintf(out + p, cap - p, "%s%x", first ? "" : "-", cps[i]);
        first = false;
    }
}

static int emoji_cell(const char *key)
{
    for (int i = 0; i < EMOJI_COUNT; i++)
        if (strcmp(EMOJI_TABLE[i].key, key) == 0) return EMOJI_TABLE[i].cell;
    return -1;
}

static void draw_emoji_cell(int cell, int x, int y, int box)
{
    if (!s_emoji_atlas || cell < 0) return;
    int sx = (cell % EMOJI_COLS) * EMOJI_CELL;
    int sy = (cell / EMOJI_COLS) * EMOJI_CELL;
    vita2d_draw_texture_part_scale(s_emoji_atlas, x, y, sx, sy,
                                   EMOJI_CELL, EMOJI_CELL,
                                   (float)box / EMOJI_CELL,
                                   (float)box / EMOJI_CELL);
}

static void draw_emoji_cdn(const char *key, int x, int y, int box)
{
    char url[160];
    snprintf(url, sizeof(url),
             "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/%s.png",
             key);
    vita2d_texture *t = imagecache_get(url);
    if (!t) return;
    float tw = vita2d_texture_get_width(t), th = vita2d_texture_get_height(t);
    if (tw < 1) tw = 1; if (th < 1) th = 1;
    vita2d_draw_texture_part_scale(t, x, y, 0, 0, tw, th,
                                   (float)box / tw, (float)box / th);
}

bool ui_draw_emoji(const char *s, int x, int y, int size)
{
    if (!s || !s[0]) return false;
    const char *p = s;
    unsigned cp = utf8_next(&p);
    unsigned next = utf8_peek(p);
    if (!is_emoji_start(cp, next)) return false;
    unsigned cps[16]; int ncp; bool zwj;
    emoji_cluster(&p, cp, cps, 16, &ncp, &zwj);
    char key[128]; twemoji_key(cps, ncp, zwj, key, sizeof key);
    int cell = emoji_cell(key);
    if (cell >= 0) draw_emoji_cell(cell, x, y, size);
    else           draw_emoji_cdn(key, x, y, size);
    return true;
}

// draw=0 measures without drawing; y is the text baseline.
static int draw_mixed(vita2d_font *f, int x, int y, int size, unsigned color,
                      const char *s, int draw)
{
    if (!f || !s) return 0;
    int cx = x;
    const char *p = s;
    char run[512]; int rl = 0;
    int box = size;
    int adv = box + box / 8;

    while (*p) {
        // Custom server emoji: <:name:id> or <a:name:id>.
        if (p[0] == '<' && (p[1] == ':' || (p[1] == 'a' && p[2] == ':'))) {
            const char *gt = strchr(p, '>');
            if (gt && gt - p < 64) {
                const char *colon = gt;
                while (colon > p && *colon != ':') colon--;
                int idn = (int)(gt - (colon + 1));
                int alldig = idn > 0 && idn < 24;
                for (const char *d = colon + 1; alldig && d < gt; d++)
                    if (*d < '0' || *d > '9') alldig = 0;
                if (alldig) {
                    char id[24]; memcpy(id, colon + 1, idn); id[idn] = '\0';
                    if (rl) { run[rl] = '\0';
                              if (draw) vita2d_font_draw_text(f, cx, y, color, size, run);
                              cx += vita2d_font_text_width(f, size, run); rl = 0; }
                    if (draw) {
                        char url[96];
                        snprintf(url, sizeof(url),
                                 "https://cdn.discordapp.com/emojis/%s.png?size=48", id);
                        vita2d_texture *t = imagecache_get(url);
                        if (t) { float tw = vita2d_texture_get_width(t);
                                 float th = vita2d_texture_get_height(t);
                                 if (tw < 1) tw = 1; if (th < 1) th = 1;
                                 int ex = cx + (adv - box) / 2, ey = y - (int)(size * 0.84f);
                                 vita2d_draw_texture_part_scale(t, ex, ey, 0, 0, tw, th,
                                                                (float)box / tw, (float)box / th); }
                    }
                    cx += adv; p = gt + 1; continue;
                }
            }
        }
        const char *start = p;
        unsigned cp = utf8_next(&p);
        unsigned next = utf8_peek(p);
        if (is_emoji_start(cp, next)) {
            unsigned cps[16]; int ncp; bool zwj;
            emoji_cluster(&p, cp, cps, 16, &ncp, &zwj);
            char key[128]; twemoji_key(cps, ncp, zwj, key, sizeof key);
            int cell = emoji_cell(key);
            if (rl) { run[rl] = '\0';
                      if (draw) vita2d_font_draw_text(f, cx, y, color, size, run);
                      cx += vita2d_font_text_width(f, size, run); rl = 0; }
            if (draw) {
                int ex = cx + (adv - box) / 2, ey = y - (int)(size * 0.84f);
                if (cell >= 0) draw_emoji_cell(cell, ex, ey, box);
                else           draw_emoji_cdn(key, ex, ey, box);
            }
            cx += adv;
            continue;
        }
        int l = (int)(p - start);
        if (rl + l < (int)sizeof(run) - 1) { memcpy(run + rl, start, l); rl += l; }
    }
    if (rl) { run[rl] = '\0';
              if (draw) vita2d_font_draw_text(f, cx, y, color, size, run);
              cx += vita2d_font_text_width(f, size, run); }
    return cx - x;
}

int ui_text(int x, int y, int size, unsigned color, const char *s)
{
    if (!s_base_reg || !s) return 0;
    // vita2d_font draws with y as the baseline; add size to the caller's top y.
    return draw_mixed(freg(size), x, y + size, size, color, s, 1);
}

int ui_text_bold(int x, int y, int size, unsigned color, const char *s)
{
    if (!s_base_semi || !s) return 0;
    return draw_mixed(fbold(size), x, y + size, size, color, s, 1);
}

int ui_text_w(int size, const char *s)
{
    if (!s_base_reg || !s) return 0;
    return draw_mixed(freg(size), 0, 0, size, 0, s, 0);
}

int ui_text_bold_w(int size, const char *s)
{
    if (!s_base_semi || !s) return 0;
    return draw_mixed(fbold(size), 0, 0, size, 0, s, 0);
}

static float pad_scale(int size, const char *glyph)
{
    int h1 = vita2d_pvf_text_height(s_pad_pvf, 1.0f, glyph);
    return (h1 > 0 ? (float)size / (float)h1 : 1.0f) * 0.9f;  // sit slightly under the text height
}

int ui_pad_glyph(int x, int y, int size, unsigned color, const char *glyph, const char *alt)
{
    if (!s_pad_pvf) return alt ? ui_text(x, y, size, color, alt) : 0;
    float scale = pad_scale(size, glyph);
    // nudge up off the baseline
    vita2d_pvf_draw_text(s_pad_pvf, x, y + size - (int)(size * 0.06f), color, scale, glyph);
    return vita2d_pvf_text_width(s_pad_pvf, scale, glyph);
}

int ui_pad_glyph_w(int size, const char *glyph, const char *alt)
{
    if (!s_pad_pvf) return alt ? ui_text_w(size, alt) : 0;
    return vita2d_pvf_text_width(s_pad_pvf, pad_scale(size, glyph), glyph);
}

static void clip_to_width(vita2d_font *f, int size, const char *s, int max_w,
                          char *out, size_t out_cap)
{
    if (!f) { out[0] = '\0'; return; }
    if (vita2d_font_text_width(f, size, s) <= max_w) {
        snprintf(out, out_cap, "%s", s);
        return;
    }
    size_t n = strlen(s);
    while (n > 0) {
        char tmp[256];
        if (n + 2 >= sizeof(tmp)) { n--; continue; }
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        strcat(tmp, "...");
        if (vita2d_font_text_width(f, size, tmp) <= max_w) {
            snprintf(out, out_cap, "%s", tmp);
            return;
        }
        n--;
    }
    snprintf(out, out_cap, "...");
}

static unsigned ui_strhash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}
#define CLIP_CACHE_N 192
static struct {
    const void *font; int size, max_w; unsigned hash, age; char out[96];
} s_clipc[CLIP_CACHE_N];
static unsigned s_clip_clock;
static void clip_cached(vita2d_font *f, int size, const char *s, int max_w,
                        char *out, size_t out_cap)
{
    if (!f || !s) { out[0] = '\0'; return; }
    unsigned h = ui_strhash(s);
    if (++s_clip_clock == 0) s_clip_clock = 1;
    for (int i = 0; i < CLIP_CACHE_N; i++)
        if (s_clipc[i].hash == h && s_clipc[i].font == f &&
            s_clipc[i].size == size && s_clipc[i].max_w == max_w) {
            s_clipc[i].age = s_clip_clock;
            snprintf(out, out_cap, "%s", s_clipc[i].out);
            return;
        }
    char tmp[256];
    clip_to_width(f, size, s, max_w, tmp, sizeof(tmp));
    snprintf(out, out_cap, "%s", tmp);
    if (strlen(tmp) >= sizeof(s_clipc[0].out)) return;
    int lru = 0; unsigned best = 0xFFFFFFFFu;
    for (int i = 0; i < CLIP_CACHE_N; i++) if (s_clipc[i].age < best) { best = s_clipc[i].age; lru = i; }
    s_clipc[lru].font = f; s_clipc[lru].size = size; s_clipc[lru].max_w = max_w;
    s_clipc[lru].hash = h; s_clipc[lru].age = s_clip_clock;
    snprintf(s_clipc[lru].out, sizeof(s_clipc[lru].out), "%s", tmp);
}

void ui_text_clip(int x, int y, int size, unsigned color, const char *s, int max_w)
{
    char buf[256];
    clip_cached(freg(size), size, s, max_w, buf, sizeof(buf));
    ui_text(x, y, size, color, buf);
}

void ui_text_bold_clip(int x, int y, int size, unsigned color, const char *s, int max_w)
{
    char buf[256];
    clip_cached(fbold(size), size, s, max_w, buf, sizeof(buf));
    ui_text_bold(x, y, size, color, buf);
}

void ui_rect(int x, int y, int w, int h, unsigned color)
{
    vita2d_draw_rectangle(x, y, w, h, color);
}

void ui_rounded_rect(int x, int y, int w, int h, int r, unsigned color)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, color);
    vita2d_draw_rectangle(x, y + r, r, h - 2 * r, color);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, color);
    vita2d_draw_fill_circle(x + r,     y + r,     r, color);
    vita2d_draw_fill_circle(x + w - r, y + r,     r, color);
    vita2d_draw_fill_circle(x + r,     y + h - r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, color);
}

void ui_avatar_placeholder(int cx, int cy, int r, const char *name, unsigned bg)
{
    vita2d_draw_fill_circle(cx, cy, r, bg);
    if (name && name[0] && s_base_semi) {
        char letter[2] = { name[0], '\0' };
        if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
        int sz = r;  // letter roughly half the diameter
        vita2d_font *f = fbold(sz);
        int w = vita2d_font_text_width(f, sz, letter);
        vita2d_font_draw_text(f, cx - w / 2, cy + sz / 2 - 1,
                              COL_WHITE, sz, letter);
    }
}

int ui_text_wrapped(int x, int y, int size, unsigned color, const char *text,
                    int max_w, int line_h)
{
    if (!s_base_reg || !text) return 0;
    char line[512];
    int len = 0, drawn_y = y;

    const char *word = text;
    line[0] = '\0'; len = 0;
    while (1) {
        const char *sp = word;
        while (*sp && *sp != ' ' && *sp != '\n') sp++;
        int wlen = (int)(sp - word);
        char cand[512];
        int clen = len;
        if (len > 0) { memcpy(cand, line, len); cand[clen++] = ' '; }
        if (clen + wlen >= (int)sizeof(cand)) wlen = sizeof(cand) - clen - 1;
        memcpy(cand + clen, word, wlen);
        cand[clen + wlen] = '\0';

        if (vita2d_font_text_width(freg(size), size, cand) > max_w && len > 0) {
            ui_text(x, drawn_y, size, color, line);
            drawn_y += line_h;
            snprintf(line, sizeof(line), "%.*s", wlen, word);
            len = (int)strlen(line);
        } else {
            memcpy(line, cand, strlen(cand) + 1);
            len = (int)strlen(line);
        }

        if (*sp == '\n') {
            ui_text(x, drawn_y, size, color, line);
            drawn_y += line_h;
            line[0] = '\0'; len = 0;
        }
        if (!*sp) break;
        word = sp + 1;
    }
    if (len > 0) { ui_text(x, drawn_y, size, color, line); drawn_y += line_h; }
    return drawn_y - y;
}

#define MD_BOLD      1
#define MD_ITALIC    2
#define MD_CODE      4
#define MD_STRIKE    8
#define MD_UNDERLINE 16
#define MD_SPOILER   32
#define MD_MENTION   64  // @user / @role / #channel pill (wrapped in \x01..\x02)
#define MD_CODEBLOCK 128  // one line of a ``` fenced block (implies MD_CODE)

static bool s_reveal_spoilers = true;
void ui_set_spoiler_reveal(bool on) { s_reveal_spoilers = on; }

// block: 0 none, 1 blockquote, 2 bullet list item
typedef struct { const char *s; int len; int style; int nl; int sp; int block; } md_word;

// draw=0 measures only; returns the height consumed.
static int md_run(int x, int y, int size, unsigned base, const char *text,
                  int max_w, int lh, int draw)
{
    if (!s_base_reg || !text) return lh;

    static md_word W[640];
    int nw = 0, style = 0, sp = 0, cur_block = 0;
    bool at_ls = true, cur_hdr = false;
    const char *p = text;
    while (*p && nw < 636) {
        if (*p == '\n') { W[nw++] = (md_word){0,0,0,1,0,0}; p++; sp = 0;
                          cur_block = 0; cur_hdr = false; at_ls = true; continue; }
        if (at_ls) {  // line-level markers (column 0)
            if (p[0]=='>' && p[1]==' ')                { cur_block = 1; p += 2; at_ls = false; continue; }
            if ((p[0]=='-'||p[0]=='*') && p[1]==' ')   { cur_block = 2; p += 2; at_ls = false; continue; }
            if (p[0]=='#') { int h = 0; const char *q = p;
                             while (*q=='#') { h++; q++; }
                             if (h>=1 && h<=3 && *q==' ') { cur_hdr = true; p = q+1; at_ls = false; continue; } }
            if (p[0]>='0' && p[0]<='9') {  // "1. " numbered list: indent, keep the number
                const char *q = p; while (*q>='0' && *q<='9') q++;
                if (q[0]=='.' && q[1]==' ') cur_block = 3; }  // 3 = indent, no bullet dot
            at_ls = false;
        }
        if (*p == '\x01') { style |= MD_MENTION;  p++; continue; }
        if (*p == '\x02') { style &= ~MD_MENTION; p++; continue; }
        if (p[0]=='`' && p[1]=='`' && p[2]=='`') {
            const char *q = p + 3;
            const char *close = strstr(q, "```");
            if (close) {
                // Skip an opening language label: a spaceless token on the
                // first line, only when a newline precedes the closing fence.
                const char *nlp = memchr(q, '\n', close - q);
                if (nlp) {
                    int lang = 1;
                    for (const char *s = q; s < nlp; s++)
                        if (*s == ' ' || *s == '`') { lang = 0; break; }
                    if (lang) q = nlp + 1;
                }
                while (q < close && nw < 634) {  // one word per code line
                    const char *ls = q;
                    while (q < close && *q != '\n') q++;
                    if (q > ls) { W[nw].s = ls; W[nw].len = (int)(q - ls);
                                  W[nw].style = MD_CODE | MD_CODEBLOCK;
                                  W[nw].nl = 0; W[nw].sp = 0; W[nw].block = 3; nw++; }
                    if (q < close && *q == '\n') { W[nw++] = (md_word){0,0,0,1,0,0}; q++; }
                }
                p = close + 3;
                at_ls = true;
                continue;
            }
        }
        if (*p == '`')  { style ^= MD_CODE; p++; continue; }
        if (!(style & MD_CODE)) {
            if (p[0]=='*'&&p[1]=='*'){ style^=MD_BOLD;      p+=2; continue; }
            if (p[0]=='_'&&p[1]=='_'){ style^=MD_UNDERLINE; p+=2; continue; }
            if (p[0]=='~'&&p[1]=='~'){ style^=MD_STRIKE;    p+=2; continue; }
            if (p[0]=='|'&&p[1]=='|'){ style^=MD_SPOILER;   p+=2; continue; }
            if (p[0]=='*'||p[0]=='_'){ style^=MD_ITALIC;    p++;  continue; }
        }
        if (*p == ' ') { sp = 1; p++; continue; }
        const char *ws = p;
        while (*p && *p != ' ' && *p != '\n') {
            if (*p == '`' || *p == '\x01' || *p == '\x02') break;
            if (!(style & MD_CODE) &&
                ((p[0]=='*'&&p[1]=='*')||(p[0]=='_'&&p[1]=='_')||
                 (p[0]=='~'&&p[1]=='~')||(p[0]=='|'&&p[1]=='|')||
                 p[0]=='*'||p[0]=='_'))
                break;
            p++;
        }
        if (p > ws) { W[nw].s = ws; W[nw].len = (int)(p - ws);
                      W[nw].style = style | (cur_hdr ? MD_BOLD : 0);
                      W[nw].nl = 0; W[nw].sp = sp; W[nw].block = cur_block;
                      nw++; sp = 0; }
    }

    int cx = x, cy = y;
    int space_w = vita2d_font_text_width(freg(size), size, " ");
    char buf[512];
    bool line_start = true;
    for (int i = 0; i < nw; i++) {
        if (W[i].nl) { cx = x; cy += lh; line_start = true; continue; }
        vita2d_font *f = (W[i].style & (MD_BOLD | MD_MENTION)) ? fbold(size) : freg(size);
        unsigned col = (W[i].style & MD_MENTION) ? RGBA8(0xc9,0xcd,0xfb,0xFF)
                     : (W[i].style & MD_CODE)    ? 0xFFD0D0D0 : base;
        int len = W[i].len < 511 ? W[i].len : 511;
        memcpy(buf, W[i].s, len); buf[len] = '\0';
        int wwd = draw_mixed(f, 0, 0, size, 0, buf, 0);
        int lx = W[i].block ? x + 20 : x;
        int spw = W[i].sp ? space_w : 0;
        if (cx + spw + wwd > x + max_w && cx > lx) { cx = lx; cy += lh; spw = 0; line_start = true; }
        else if (line_start && cx < lx) cx = lx;
        else cx += spw;
        if (draw && line_start && W[i].block == 1)
            vita2d_draw_rectangle(x + 4, cy + 1, 3, lh - 2, RGBA8(0x4f,0x54,0x5c,0xFF));
        if (draw && line_start && W[i].block == 2)
            vita2d_draw_fill_circle(x + 10, cy + size / 2 + 1, 2, base);
        if (draw) {
            if (W[i].style & MD_MENTION)
                vita2d_draw_rectangle(cx - 3, cy + 1, wwd + 6, size + 4,
                                      RGBA8(0x3c,0x40,0x6b,0xFF));
            if (W[i].style & MD_CODEBLOCK)
                vita2d_draw_rectangle(x + 4, cy + 1, max_w - 8, lh,
                                      RGBA8(0x1e,0x1f,0x22,0xFF));
            else if (W[i].style & MD_CODE)
                vita2d_draw_rectangle(cx - 3, cy + 2, wwd + 6, size + 2,
                                      RGBA8(0x2b,0x2d,0x31,0xFF));
            bool hide_spoiler = (W[i].style & MD_SPOILER) && !s_reveal_spoilers;
            if (W[i].style & MD_SPOILER)
                vita2d_draw_rectangle(cx - 2, cy + 1, wwd + 4, size + 3,
                                      hide_spoiler ? RGBA8(0x53,0x57,0x62,0xFF)
                                                   : RGBA8(0x14,0x15,0x18,0xFF));
            if (!hide_spoiler)
                draw_mixed(f, cx, cy + size, size, col, buf, 1);
            if (W[i].style & MD_STRIKE)
                vita2d_draw_rectangle(cx, cy + size / 2 + 1, wwd, 1, base);
            if (W[i].style & MD_UNDERLINE)
                vita2d_draw_rectangle(cx, cy + size, wwd, 1, base);
        }
        line_start = false;
        cx += wwd;
    }
    return (cy - y) + lh;
}

void ui_richtext_draw(int x, int y, int size, unsigned color, const char *text,
                      int max_w, int line_h)
{
    md_run(x, y, size, color, text, max_w, line_h, 1);
}

int ui_richtext_height(int size, const char *text, int max_w, int line_h)
{
    return md_run(0, 0, size, 0, text, max_w, line_h, 0);
}

int ui_wrap_height(int size, const char *text, int max_w, int line_h)
{
    if (!s_base_reg || !text) return 0;
    int lines = 0, len = 0;
    char line[512];
    const char *word = text;
    line[0] = '\0';
    while (1) {
        const char *sp = word;
        while (*sp && *sp != ' ' && *sp != '\n') sp++;
        int wlen = (int)(sp - word);
        char cand[512];
        int clen = len;
        if (len > 0) { memcpy(cand, line, len); cand[clen++] = ' '; }
        if (clen + wlen >= (int)sizeof(cand)) wlen = sizeof(cand) - clen - 1;
        memcpy(cand + clen, word, wlen);
        cand[clen + wlen] = '\0';
        if (vita2d_font_text_width(freg(size), size, cand) > max_w && len > 0) {
            lines++;
            snprintf(line, sizeof(line), "%.*s", wlen, word);
            len = (int)strlen(line);
        } else {
            memcpy(line, cand, strlen(cand) + 1);
            len = (int)strlen(line);
        }
        if (*sp == '\n') { lines++; line[0] = '\0'; len = 0; }
        if (!*sp) break;
        word = sp + 1;
    }
    if (len > 0) lines++;
    return lines * line_h;
}
