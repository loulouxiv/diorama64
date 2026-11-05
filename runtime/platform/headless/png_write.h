#pragma once
#include <cstdint>
#include <string>
namespace ar {
// RGBA8 rows top to bottom (stb_image_write, from the pinned upstream tree's include/stb).
bool png_write_rgba(const std::string &path, const uint8_t *rgba, uint32_t w, uint32_t h);
}
