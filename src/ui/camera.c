#include "ui/camera.h"
#include "console.h"

#include <stdlib.h>
#include <string.h>

#include <vita2d.h>
#include <psp2/common_dialog.h>  // SceCommonDialogParam, magic number, PSP2_SDK_VERSION
#include <psp2/gxm.h>  // texture formats, sceGxmMapMemory
#include <psp2/display.h>  // sceDisplayWaitVblankStart
#include <psp2/touch.h>
#include <psp2/kernel/sysmem.h>  // memblocks
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>  // sceClibMemset

// Structs adapted from LifeLua (github.com/HarommelRabbid/LifeLua).
#define CL_CAM_WORKING_BUFFER_SIZE   (2 * 1024 * 1024)
#define CL_CAM_OUT_CAP               (4 * 1024 * 1024)
#define CL_CAM_OUT_MODE_JPEG         0x00000001U
#define CL_CAM_RES_640X480           0x00000001U
#define CL_CAM_RES_640X360           0x00000002U
#define CL_CAM_RES_480X480           0x00000004U
#define CL_CAM_DEV_FRONT             0x00000001U
#define CL_CAM_DEV_REAR              0x00000002U
#define CL_CAM_ROT_ENABLE            0x00000001U
#define CL_CAM_OVERLAY_NOT_USE       0x00000000U

typedef struct {
    void     *buffer;
    SceSize   bufferSize;
    SceChar8  reserved[32];
} ClCamJpegParam;

typedef struct {
    void               *buffer;
    SceSize             bufferSize;
    SceGxmTextureType   texType;
    SceGxmTextureFormat texFormat;
    SceChar8            reserved[32];
} ClCamRawParam;

typedef struct {
    void               *buffer;
    SceSize             bufferSize;
    SceUInt32           width, height, stride;
    SceGxmTextureType   texType;
    SceGxmTextureFormat texFormat;
    SceChar8            reserved[32];
} ClCamOverlayImage;

typedef struct {
    SceUInt32            sdkVersion;
    SceCommonDialogParam commonParam;
    SceUInt32            outputMode;
    ClCamRawParam       *memRawParam;
    ClCamJpegParam      *memJpegParam;
    SceChar8             reserved1[8];
    void                *workingBuffer;
    SceInt32             workingBufferSize;
    SceUInt32            availableResolution;
    SceUInt32            initialResolution;
    SceUInt32            availableDevice;
    SceUInt32            initialDevice;
    SceUInt32            rotationMode;
    SceUInt32            overlayMode;
    ClCamOverlayImage   *overlayImage;
    SceChar8             reserved2[48];
} ClCamParam;

typedef struct {
    SceSize   size;
    SceUInt32 width, height, device, orientation;
    SceChar8  reserved[32];
} ClCamOutputParam;

typedef struct {
    SceInt32         result;
    ClCamOutputParam output;
    SceChar8         reserved[32];
} ClCamResult;

extern int sceCameraImportDialogInit(ClCamParam *param);
extern int sceCameraImportDialogGetStatus(void);
extern int sceCameraImportDialogGetResult(ClCamResult *result);
extern int sceCameraImportDialogTerm(void);

bool camera_capture(unsigned char **out, int *out_len)
{
    *out = NULL; *out_len = 0;

    // Camera pipeline scratch: phys-contiguous, GXM-mapped.
    SceUID workId = sceKernelAllocMemBlock("cl_cam_work",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
        CL_CAM_WORKING_BUFFER_SIZE, NULL);
    if (workId < 0) { console_logf("[cam] work alloc 0x%08X", (unsigned)workId); return false; }
    void *work = NULL;
    sceKernelGetMemBlockBase(workId, &work);
    sceGxmMapMemory(work, CL_CAM_WORKING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);

    // Output buffer for the encoded JPEG.
    SceUID outId = sceKernelAllocMemBlock("cl_cam_out",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, CL_CAM_OUT_CAP, NULL);
    if (outId < 0) {
        console_logf("[cam] out alloc 0x%08X", (unsigned)outId);
        sceGxmUnmapMemory(work); sceKernelFreeMemBlock(workId);
        return false;
    }
    void *outbuf = NULL;
    sceKernelGetMemBlockBase(outId, &outbuf);

    ClCamJpegParam jpeg;
    sceClibMemset(&jpeg, 0, sizeof(jpeg));
    jpeg.buffer     = outbuf;
    jpeg.bufferSize = CL_CAM_OUT_CAP;

    ClCamParam p;
    sceClibMemset(&p, 0, sizeof(p));
    _sceCommonDialogSetMagicNumber(&p.commonParam);
    p.sdkVersion          = PSP2_SDK_VERSION;
    p.outputMode          = CL_CAM_OUT_MODE_JPEG;
    p.memJpegParam        = &jpeg;
    p.memRawParam         = NULL;
    p.workingBuffer       = work;
    p.workingBufferSize   = CL_CAM_WORKING_BUFFER_SIZE;
    p.availableResolution = CL_CAM_RES_640X480 | CL_CAM_RES_640X360 | CL_CAM_RES_480X480;
    p.initialResolution   = CL_CAM_RES_640X480;
    p.availableDevice     = CL_CAM_DEV_FRONT | CL_CAM_DEV_REAR;
    p.initialDevice       = CL_CAM_DEV_REAR;
    p.rotationMode        = CL_CAM_ROT_ENABLE;
    p.overlayMode         = CL_CAM_OVERLAY_NOT_USE;
    p.overlayImage        = NULL;

    bool ok = false;
    int ir = sceCameraImportDialogInit(&p);
    if (ir >= 0) {
        // Dialog pump: prime a frame, then end_drawing, update, swap, vblank until FINISHED.
        vita2d_start_drawing();
        vita2d_clear_screen();
        while (sceCameraImportDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
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

        ClCamResult r;
        sceClibMemset(&r, 0, sizeof(r));
        sceCameraImportDialogGetResult(&r);
        if (r.result == SCE_COMMON_DIALOG_RESULT_OK) {
            unsigned char *jb = (unsigned char *)outbuf;
            int len = (int)r.output.size;
            // If the reported size is out of range, scan for the JPEG end-of-image marker.
            if (len <= 2 || len > CL_CAM_OUT_CAP) {
                len = 0;
                for (int i = 2; i < CL_CAM_OUT_CAP - 1; i++)
                    if (jb[i] == 0xFF && jb[i + 1] == 0xD9) { len = i + 2; break; }
            }
            if (len > 2 && jb[0] == 0xFF && jb[1] == 0xD8) {
                unsigned char *cp = malloc((size_t)len);
                if (cp) {
                    memcpy(cp, jb, (size_t)len);
                    *out = cp; *out_len = len; ok = true;
                }
            } else {
                console_logf("[cam] bad jpeg (len=%d b0=%02X b1=%02X)",
                             len, jb[0], jb[1]);
            }
        }
        sceCameraImportDialogTerm();
    } else {
        console_logf("[cam] init 0x%08X", (unsigned)ir);
    }

    sceGxmUnmapMemory(work);
    sceKernelFreeMemBlock(outId);
    sceKernelFreeMemBlock(workId);

    // Drain the dismiss touch; bounded.
    for (int i = 0; i < 150; i++) {
        SceTouchData td;
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0 || td.reportNum == 0) break;
        sceKernelDelayThread(8000);
    }
    return ok;
}
