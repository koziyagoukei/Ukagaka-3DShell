// stb_image implementation unit (public domain / MIT, https://github.com/nothings/stb).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO      // files are read via std::ifstream with UTF-16 paths
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
