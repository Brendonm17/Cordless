/* stb_image implementation unit. We only use it to decode animated GIFs
 * (vita2d handles PNG/JPEG); PNG/JPEG are enabled too as a harmless fallback. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_GIF
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
