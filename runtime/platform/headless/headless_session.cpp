#include "headless_session.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "ar_layer.h"
#include "room.h"
namespace ar {
namespace {
const float kRoomClear[4] = {0.62f, 0.66f, 0.70f, 1.f};
struct Done { ar_runtime *rt; ar_render_frame f; };
void on_done(void *ctx) {   // the device's completion: release the snapshot the frame was reading
    auto *d = static_cast<Done *>(ctx);
    ar_runtime_end_frame(d->rt, &d->f);
    delete d;
}
void upload(void *user, uint32_t id, uint32_t w, uint32_t h, const uint8_t *rgba) { static_cast<IRenderDevice *>(user)->upload_texture(id, w, h, rgba); }
}
HeadlessSession::HeadlessSession(const ar_adapter *a, HeadlessPlatform &p, IRenderDevice &dev)
    : adapter_(a), platform_(p), dev_(dev) {
    renderer_.reset(new Renderer(dev_));
    room_ = fc_builder_create();
    fc_builder_reset(room_, 0, 320, 240);
    build_room(room_);
    room_frame_ = fc_builder_finish(room_);
    env_ = dev_.create_target(1, 1);
    ar_ = dev_.create_target(1, 1);
    final_ = dev_.create_target(1, 1);
}
HeadlessSession::~HeadlessSession() {
    dev_.wait_idle();
    if (rt_) { ar_runtime_stop_game_thread(rt_); ar_runtime_destroy(rt_); }
    fc_builder_destroy(room_);
}
bool HeadlessSession::init(const std::string &rom, const std::string &save, std::string *error) {
    rt_ = ar_runtime_create(adapter_, platform_.api());
    if (!rt_) { *error = "adapter or platform ABI mismatch"; return false; }
    ar_runtime_set_texture_sink(rt_, upload, &dev_);
    if (!ar_runtime_set_audio_mode(rt_, audio_mode_)) { *error = "the adapter cannot produce this audio mode"; return false; }
    if (!ar_runtime_init_game(rt_, rom.c_str(), save.c_str())) { *error = adapter_->last_error(); return false; }
    return true;
}
void HeadlessSession::warmup(uint32_t ticks) { for (uint32_t i = 0; i < ticks; i++) ar_runtime_step_game(rt_); }
uint64_t HeadlessSession::ticks_done() const { return rt_ ? ar_runtime_ticks_done(rt_) : 0; }
bool HeadlessSession::read_final(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) { return dev_.read_target(final_, rgba, w, h); }
bool HeadlessSession::frame() {
    if (!rt_) return false;
    ar_render_frame rf{};
    if (!ar_runtime_begin_frame(rt_, &rf)) return false;
    const LightingFrame lf = light_.begin(dev_, look_, rf);   // before the ticks: they already use this frame's steering
    while (ar_runtime_ticks_due(rt_)) ar_runtime_step_game(rt_);
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i < rf.view_count; i++) { w += rf.views[i].width; h = std::max(h, rf.views[i].height); }
    if (w == 0 || h == 0) {
        ar_render_frame none = rf;
        none.view_count = 0;
        ar_runtime_submit_frame(rt_, &none);
        ar_runtime_end_frame(rt_, &rf);
        return true;
    }
    const uint32_t samples = dev_.supported_samples(look_.msaa);
    if (w != w_ || h != h_ || samples != samples_) {
        dev_.wait_idle();
        dev_.resize_target(env_, w, h); dev_.resize_target(ar_, w, h, 1, samples); dev_.resize_target(final_, w, h);
        w_ = w; h_ = h; samples_ = samples;
    }
    if (rf.snapshot) {
        snapshots_++;
        last_world_ = rf.snapshot->world_count; last_overlay_ = rf.snapshot->overlay_count; last_verts_ = rf.snapshot->vert_count;
        draws_total_ += (uint64_t)rf.snapshot->world_count + rf.snapshot->overlay_count;
    }
    FrameView room[AR_MAX_VIEWS];
    for (uint32_t i = 0; i < rf.view_count; i++) {
        const ar_view_out &v = rf.views[i];
        room[i] = FrameView{v.clip_from_table, {0.f, 0.f, 0.f}, Viewport{float(v.x), 0.f, float(v.width), float(v.height)}};
    }
    ArLayerOptions lo;
    lo.lighting = &lf;
    lo.decal_fix = look_.decal_fix;
    const float dt = have_time_ ? std::fmax(0.f, float(rf.display_time_ns - last_time_) * 1e-9f) : 1.f / 30.f;
    dev_.begin_frame();
    light_.encode(dev_, rf);
    renderer_->draw_world(env_, *room_frame_, room, rf.view_count, 0.f, kRoomClear);
    draw_ar_layer(*renderer_, ar_, rf, lo, dt);
    dev_.encode_composite(final_, nullptr, env_, ar_);
    ar_runtime_submit_frame(rt_, &rf);
    dev_.end_frame(on_done, new Done{rt_, rf}, nullptr);
    last_time_ = rf.display_time_ns; have_time_ = true;
    return true;
}
void HeadlessSession::set_look(const LookSettings &s) {
    look_ = s;
    dev_.set_edge_coverage(s.edge_aa);
    dev_.set_texture_filter(s.tex_smooth ? TextureFilter::smooth : TextureFilter::vanilla);
    light_.begin(dev_, look_, ar_render_frame{});
}
}
