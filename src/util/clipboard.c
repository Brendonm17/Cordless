#include "util/clipboard.h"

#include <psp2/sysmodule.h>

#define SCE_CLIPBOARD_MAX_STRING_LENGTH		8189

#define SCE_CLIPBOARD_ERROR_INTERNAL				-2146417152	/* 0x80104600 */
#define SCE_CLIPBOARD_ERROR_NULL_POINTER			-2146417151	/* 0x80104601 */
#define SCE_CLIPBOARD_ERROR_TEXT_TOO_LONG			-2146417150	/* 0x80104602 */
#define SCE_CLIPBOARD_ERROR_UNSUPPORTED_CHARACTER	-2146417149	/* 0x80104603 */

SceInt32 sceClipboardSetText(const SceWChar16* newContents, SceUInt length);

SceInt32 sceClipboardGetText(SceWChar16* buffer, SceUInt maxLength, SceUInt* pLength);

static void strWChar16ncpy(SceWChar16* out, const char* str2, int len)
{
    char* str1 = (char*) out;

    while (*str2 && len--)
    {
        *str1++ = *str2++;
        *str1++ = '\0';
    }
}

int clipboard_set_utf8(const char *utf8)
{
    if (!utf8) return -1;
    sceSysmoduleLoadModule(SCE_SYSMODULE_CLIPBOARD);  // ok if already loaded
    char str[SCE_CLIPBOARD_MAX_STRING_LENGTH];
    snprintf(str, sizeof(str), utf8);
    static SceWChar16 buf[SCE_CLIPBOARD_MAX_STRING_LENGTH];
    strWChar16ncpy(buf, str, SCE_CLIPBOARD_MAX_STRING_LENGTH);
    return sceClipboardSetText(buf, SCE_CLIPBOARD_MAX_STRING_LENGTH);
}
