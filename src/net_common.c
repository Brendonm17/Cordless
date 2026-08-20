#include "net_common.h"
#include "console.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/appmgr.h>

// sceNet memory pool shared by all sockets (gateway WSS + image + REST).
#define NET_MEMPOOL_SIZE (6 * 1024 * 1024)

static char s_net_mempool[NET_MEMPOOL_SIZE];
static int  s_net_up;

// Serializes all blocking network operations across threads (see header).
static SceUID s_net_mutex = -1;
void net_lock(void)   { if (s_net_mutex >= 0) sceKernelLockMutex(s_net_mutex, 1, NULL); }
void net_unlock(void) { if (s_net_mutex >= 0) sceKernelUnlockMutex(s_net_mutex, 1); }

int net_init(void)
{
    if (s_net_mutex < 0)
        s_net_mutex = sceKernelCreateMutex("net_serialize", 0, 0, NULL);

    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (ret < 0) return ret;

    SceNetInitParam param;
    param.memory = s_net_mempool;
    param.size   = NET_MEMPOOL_SIZE;
    param.flags  = 0;

    ret = sceNetInit(&param);
    // SCE_NET_ERROR_EBUSY (already initialised) is ok.
    if (ret < 0 && ret != (int)0x80410102) return ret;

    ret = sceNetCtlInit();
    if (ret < 0 && ret != (int)0x80412102) return ret;

    s_net_up = 1;
    return 0;
}

int net_wait_for_connection(int timeout_ms)
{
    const int step_ms = 100;
    int waited = 0;
    while (waited < timeout_ms) {
        if (net_is_connected()) return 1;
        sceKernelDelayThread(step_ms * 1000);
        waited += step_ms;
    }
    return 0;
}

int net_is_connected(void)
{
    int state = 0;
    return (sceNetCtlInetGetState(&state) == 0 &&
            state == SCE_NETCTL_STATE_CONNECTED) ? 1 : 0;
}

void net_finish(void)
{
    if (!s_net_up) return;
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    s_net_up = 0;
}

#ifndef APP_TITLEID
#define APP_TITLEID "CRDL00001"
#endif

void net_app_path(char *out, unsigned out_len, const char *rel)
{
    // Files packed in the vpk land under ux0:app/<TITLEID>/.
    snprintf(out, out_len, "ux0:app/%s/%s", APP_TITLEID, rel);
}
