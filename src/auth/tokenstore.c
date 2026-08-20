#include "auth/tokenstore.h"
#include "console.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#define DATA_DIR      "ux0:data/cordless"
#define TOKEN_PATH    DATA_DIR "/session.bin"
#define ACCOUNTS_PATH DATA_DIR "/accounts.bin"

// Fixed obfuscation key. Not a security boundary.
static const unsigned char XKEY[16] = {
    0x9e, 0x3f, 0xa1, 0x77, 0x42, 0xdb, 0x10, 0x8c,
    0x55, 0x2a, 0xe4, 0x63, 0xbf, 0x19, 0x7d, 0xc0,
};

static void xor_buf(unsigned char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) buf[i] ^= XKEY[i & 15];
}

bool tokenstore_save(const char *token)
{
    sceIoMkdir("ux0:data", 0777);  // ignore EEXIST
    sceIoMkdir(DATA_DIR, 0777);

    size_t n = strlen(token);
    unsigned char buf[256];
    if (n >= sizeof(buf)) return false;
    memcpy(buf, token, n);
    xor_buf(buf, n);

    // Mode 0666: Vita hardware sceIoOpen rejects 0600 with EINVAL (0x80010016).
    SceUID fd = sceIoOpen(TOKEN_PATH,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { console_logf("[token] save: open failed 0x%08x", (unsigned)fd); return false; }
    int w = sceIoWrite(fd, buf, n);
    sceIoSyncByFd(fd, 0);
    sceIoClose(fd);
    // Flush the whole ux0 cache (data + dir entries); a fresh file's write+close
    // otherwise sits in the card cache and is lost on force-close.
    sceIoSync("ux0:", 0);

    // Verify it persisted via readback.
    SceIoStat st; int ok = (sceIoGetstat(TOKEN_PATH, &st) >= 0);
    if (ok && w == (int)n)
        console_dbgf("[token] save w=%d/%d persisted=yes (%lld bytes)", w, (int)n,
                     (long long)st.st_size);
    else
        console_logf("[token] save w=%d/%d persisted=%s (%lld bytes)", w, (int)n,
                     ok ? "yes" : "NO", ok ? (long long)st.st_size : -1);
    return w == (int)n;
}

bool tokenstore_load(char *out, size_t out_cap)
{
    SceUID fd = sceIoOpen(TOKEN_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return false;

    unsigned char buf[256];
    int r = sceIoRead(fd, buf, sizeof(buf));
    sceIoClose(fd);
    if (r <= 0 || (size_t)r >= out_cap) return false;

    xor_buf(buf, r);
    memcpy(out, buf, r);
    out[r] = '\0';
    return true;
}

void tokenstore_clear(void)
{
    sceIoRemove(TOKEN_PATH);
}


int accounts_load(account_t *out, int max)
{
    SceUID fd = sceIoOpen(ACCOUNTS_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    unsigned char buf[4096];
    int r = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (r <= 0) return 0;
    xor_buf(buf, r);
    buf[r] = '\0';
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r((char *)buf, "\n", &save);
         line && n < max; line = strtok_r(NULL, "\n", &save)) {
        char *f[4]; int fi = 0; char *s2 = NULL;
        for (char *t = strtok_r(line, "\t", &s2); t && fi < 4; t = strtok_r(NULL, "\t", &s2))
            f[fi++] = t;
        if (fi == 4) {
            snprintf(out[n].id, sizeof(out[0].id), "%s", f[0]);
            snprintf(out[n].name, sizeof(out[0].name), "%s", f[1]);
            snprintf(out[n].avatar, sizeof(out[0].avatar), "%s", f[2]);
            snprintf(out[n].token, sizeof(out[0].token), "%s", f[3]);
            n++;
        }
    }
    return n;
}

static bool accounts_save_all(account_t *acc, int n)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    unsigned char buf[4096]; int len = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf((char *)buf + len, sizeof(buf) - len, "%s\t%s\t%s\t%s\n",
                         acc[i].id, acc[i].name[0] ? acc[i].name : "?",
                         acc[i].avatar, acc[i].token);
        if (w < 0 || len + w >= (int)sizeof(buf)) break;
        len += w;
    }
    xor_buf(buf, len);
    SceUID fd = sceIoOpen(ACCOUNTS_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return false;
    int wr = sceIoWrite(fd, buf, len);
    sceIoSyncByFd(fd, 0);
    sceIoClose(fd);
    sceIoSync("ux0:", 0);  // commit the whole card, incl. dir entry
    return wr == len;
}

bool accounts_add(const char *token, const char *name, const char *id, const char *avatar)
{
    if (!token || !token[0] || !id) return false;
    account_t acc[ACCOUNTS_MAX];
    int n = accounts_load(acc, ACCOUNTS_MAX);
    for (int i = 0; i < n; i++)
        if (strcmp(acc[i].id, id) == 0) {  // update existing
            snprintf(acc[i].token, sizeof(acc[0].token), "%s", token);
            snprintf(acc[i].name, sizeof(acc[0].name), "%s", name ? name : "");
            snprintf(acc[i].avatar, sizeof(acc[0].avatar), "%s", avatar ? avatar : "");
            return accounts_save_all(acc, n);
        }
    if (n >= ACCOUNTS_MAX) return accounts_save_all(acc, n);  // roster full
    snprintf(acc[n].id, sizeof(acc[0].id), "%s", id);
    snprintf(acc[n].name, sizeof(acc[0].name), "%s", name ? name : "");
    snprintf(acc[n].avatar, sizeof(acc[0].avatar), "%s", avatar ? avatar : "");
    snprintf(acc[n].token, sizeof(acc[0].token), "%s", token);
    return accounts_save_all(acc, n + 1);
}

bool accounts_remove(const char *id)
{
    if (!id) return false;
    account_t acc[ACCOUNTS_MAX];
    int n = accounts_load(acc, ACCOUNTS_MAX);
    int w = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(acc[i].id, id) != 0) { if (w != i) acc[w] = acc[i]; w++; }
    return accounts_save_all(acc, w);
}
