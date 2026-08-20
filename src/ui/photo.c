#include "ui/photo.h"
#include "console.h"

#include <stdio.h>
#include <string.h>

#include <vita2d.h>
#include <psp2/common_dialog.h>  // SceCommonDialogParam, magic number, PSP2_SDK_VERSION
#include <psp2/display.h>  // sceDisplayWaitVblankStart
#include <psp2/touch.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>  // sceClibMemset

// Structs adapted from LifeLua (github.com/HarommelRabbid/LifeLua).
#define CL_PHOTO_MAX_FS_PATH        1024
#define CL_PHOTO_MODE_DEFAULT       0
#define CL_PHOTO_CATEGORY_DEFAULT   0x00000007U
#define CL_PHOTO_MAX_ITEM_NUM       1

typedef struct {
    SceUInt32 width, height;
    SceInt32  format, orientation;
    SceChar8  reserved[32];
} ClPhotoFileDataSub;

typedef struct {
    SceChar8 fileName[CL_PHOTO_MAX_FS_PATH];
    SceChar8 photoTitle[256];
    SceChar8 reserved[32];
} ClPhotoFileData;

typedef struct {
    ClPhotoFileData    fileData;
    ClPhotoFileDataSub dataSub;
    SceChar8           reserved[32];
} ClPhotoItemData;

typedef struct {
    SceInt32  result;
    SceUInt32 importedItemNum;
    SceChar8  reserved[32];
} ClPhotoResult;

typedef struct {
    SceUInt32            sdkVersion;
    SceCommonDialogParam commonParam;
    SceInt32             mode;
    SceUInt32            visibleCategory;
    SceUInt32            itemCount;
    ClPhotoItemData     *itemData;
    SceChar8             reserved[32];
} ClPhotoParam;

extern int scePhotoImportDialogInit(ClPhotoParam *param);
extern int scePhotoImportDialogGetStatus(void);
extern int scePhotoImportDialogGetResult(ClPhotoResult *result);
extern int scePhotoImportDialogTerm(void);

static ClPhotoItemData s_item[CL_PHOTO_MAX_ITEM_NUM];

bool photo_pick(char *out, size_t out_cap)
{
    ClPhotoParam p;
    sceClibMemset(&p, 0, sizeof(p));
    _sceCommonDialogSetMagicNumber(&p.commonParam);
    p.sdkVersion      = PSP2_SDK_VERSION;
    p.mode            = CL_PHOTO_MODE_DEFAULT;
    p.visibleCategory = CL_PHOTO_CATEGORY_DEFAULT;
    p.itemCount       = 1;
    p.itemData        = s_item;
    sceClibMemset(s_item, 0, sizeof(s_item));

    int ir = scePhotoImportDialogInit(&p);
    if (ir < 0) { console_logf("[photo] init 0x%08X", (unsigned)ir); return false; }

    // Dialog pump: end_drawing, update, swap, vblank each frame.
    vita2d_start_drawing();
    vita2d_clear_screen();
    while (scePhotoImportDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_wait_rendering_done();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
        vita2d_start_drawing();
        vita2d_clear_screen();
    }
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_wait_rendering_done();
    vita2d_swap_buffers();
    sceDisplayWaitVblankStart();

    bool ok = false;
    ClPhotoResult r;
    sceClibMemset(&r, 0, sizeof(r));
    scePhotoImportDialogGetResult(&r);
    if (r.result == SCE_COMMON_DIALOG_RESULT_OK && r.importedItemNum > 0) {
        snprintf(out, out_cap, "%s", (char *)s_item[0].fileData.fileName);
        ok = out[0] != '\0';
    } else {
        console_logf("[photo] result 0x%08X n=%u", (unsigned)r.result, (unsigned)r.importedItemNum);
    }
    scePhotoImportDialogTerm();

    // Drain the dismiss touch; bounded.
    for (int i = 0; i < 150; i++) {
        SceTouchData td;
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0 || td.reportNum == 0) break;
        sceKernelDelayThread(8000);
    }
    return ok;
}
