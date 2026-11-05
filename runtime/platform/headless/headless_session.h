// runtime/platform/headless — the harness session without Metal: the runtime, the common renderer and the AR layer
// drawn into any IRenderDevice (the null device in phase 1, Vulkan later). Mirrors platform/macos's HarnessSession.
#pragma once
#include <memory>
#include <string>
#include <vector>
extern "C" {
#include "ar_runtime.h"
#include "fc_builder.h"
}
#include "headless_platform.h"
#include "look.h"
#include "render_device.h"
#include "renderer.h"
#include "room_light_driver.h"
namespace ar {
class HeadlessSession {
public:
    HeadlessSession(const ar_adapter *adapter, HeadlessPlatform &platform, IRenderDevice &dev);
    ~HeadlessSession();
    bool init(const std::string &rom, const std::string &save_dir, std::string *error);
    void warmup(uint32_t ticks);
    // One frame: begin, step every due tick on this thread (sync), draw, submit, end.
    bool frame();
    void set_audio_mode(uint32_t m) { audio_mode_ = m; }   // before init
    uint32_t pull_audio_scene(ar_audio_pull *p) { return rt_ ? ar_runtime_pull_audio_scene(rt_, p) : 0; }
    bool read_final(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h);
    void set_look(const LookSettings &s);
    void set_key_light_callback(void (*fn)(void *ctx, int on, const float dir[3]), void *ctx) { light_.set_key_light_callback(fn, ctx); }
    RoomLightStatus room_light_status() const { return light_.status(); }
    uint64_t ticks_done() const;
    ar_runtime *runtime() const { return rt_; }
    uint64_t frames_with_snapshot() const { return snapshots_; }
    // The last snapshot drawn: its captured world and overlay draws and vertices; and the draws of every snapshot drawn.
    uint32_t last_world_draws() const { return last_world_; }
    uint32_t last_overlay_draws() const { return last_overlay_; }
    uint32_t last_vertices() const { return last_verts_; }
    uint64_t capture_draws_total() const { return draws_total_; }
    uint64_t ao_bakes() const { return renderer_ ? renderer_->ao_bakes() : 0; }
    uint64_t ao_failures() const { return renderer_ ? renderer_->ao_failures() : 0; }
private:
    const ar_adapter *adapter_;
    HeadlessPlatform &platform_;
    IRenderDevice &dev_;
    std::unique_ptr<Renderer> renderer_;
    ar_runtime *rt_ = nullptr;
    uint32_t audio_mode_ = AR_AUDIO_MODE_STEREO;
    fc_builder *room_ = nullptr;
    const fc_frame *room_frame_ = nullptr;
    uint32_t env_ = 0, ar_ = 0, final_ = 0, w_ = 0, h_ = 0, samples_ = 0;
    LookSettings look_ = [] { LookSettings s; s.roomlit = false; s.msaa = 1; return s; }();
    RoomLightDriver light_;
    int64_t last_time_ = 0;
    bool have_time_ = false;
    uint64_t snapshots_ = 0, draws_total_ = 0;
    uint32_t last_world_ = 0, last_overlay_ = 0, last_verts_ = 0;
};
}
