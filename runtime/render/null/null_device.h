// runtime/render/null — an IRenderDevice that draws nothing. It keeps targets' sizes and counts what the renderer
// sends (passes, draws, vertices, texture uploads), so the game, the capture and the common renderer run headless
// (Linux harness, tests) with no GPU. read_target returns a cleared image of the target's size.
#pragma once
#include <cstdint>
#include <memory>
#include "render_device.h"
namespace ar {
struct NullDeviceStats {
    uint64_t frames = 0;
    uint64_t passes = 0, draws = 0, vertices = 0;           // totals since creation
    uint64_t texture_uploads = 0, texture_bytes = 0;
    uint32_t textures = 0;                                  // distinct ids uploaded
    uint32_t last_passes = 0, last_draws = 0, last_vertices = 0;   // the last completed frame
    uint64_t shadow_maps = 0, underside_maps = 0, composites = 0, resolves = 0;
};
std::unique_ptr<IRenderDevice> create_null_device();
// dev must come from create_null_device.
NullDeviceStats null_device_stats(const IRenderDevice &dev);
}
