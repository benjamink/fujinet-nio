#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
// Bodies come from the network, so bound each side (stb_image's default is
// 2^24). 8192 is above any platform's pixel cap (4096x4096 on POSIX) yet keeps
// a corrupt or hostile header from asking for a huge allocation on a decode
// path that the stbi_info() pre-check does not cover. It also bounds the
// net.image.set range (8192x8192).
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"
