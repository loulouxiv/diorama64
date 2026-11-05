#include "png_write.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
namespace ar {
bool png_write_rgba(const std::string &path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    return stbi_write_png(path.c_str(), (int)w, (int)h, 4, rgba, (int)w * 4) != 0;
}
}
