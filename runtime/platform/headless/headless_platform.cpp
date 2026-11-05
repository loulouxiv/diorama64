#include "headless_platform.h"
#include <cstring>
extern "C" {
#include "ar_math.h"
}
namespace ar {
HeadlessPlatform::HeadlessPlatform() {
    api_.abi_version = AR_PLATFORM_ABI_VERSION;
    api_.user = this;
    api_.wait_frame = wait_frame; api_.get_table = get_table; api_.get_pad = get_pad; api_.session_state = state;
    api_.get_environment = nullptr;   // no light probe: reflections stay off
    api_.get_preview = get_preview;
}
void HeadlessPlatform::set_preview_test(const std::string &mode) {
    std::lock_guard<std::mutex> g(m_);
    preview_mode_ = mode == "seeking" ? AR_PREVIEW_SEEKING : mode == "dragging" ? AR_PREVIEW_DRAGGING : AR_PREVIEW_NONE;
}
int HeadlessPlatform::get_preview(void *u, ar_preview *out) {
    auto *p = static_cast<HeadlessPlatform *>(u);
    std::lock_guard<std::mutex> g(p->m_);
    *out = ar_preview{};
    out->state = (uint8_t)p->preview_mode_;
    out->tracking_from_preview = p->table_;
    if (p->preview_mode_ == AR_PREVIEW_SEEKING) {
        out->on_plane = 1;
        out->tracking_from_preview.pos[0] += 0.3f; out->tracking_from_preview.pos[2] += 0.2f;
        out->outline_count = 1;
        const float *o = p->table_.pos;
        const float c[4][2] = {{-0.5f, -0.3f}, {0.5f, -0.3f}, {0.5f, 0.3f}, {-0.5f, 0.3f}};
        for (int k = 0; k < 4; k++) {
            out->outlines[0][k][0] = o[0] + c[k][0]; out->outlines[0][k][1] = o[1]; out->outlines[0][k][2] = o[2] + c[k][1];
        }
    } else if (p->preview_mode_ == AR_PREVIEW_DRAGGING) {
        out->tracking_from_preview.pos[0] += 0.1f;
    }
    return 1;
}
void HeadlessPlatform::set_size(uint32_t w, uint32_t h) { std::lock_guard<std::mutex> g(m_); w_ = w ? w : 1; h_ = h ? h : 1; }
void HeadlessPlatform::set_stereo(bool on) { std::lock_guard<std::mutex> g(m_); stereo_ = on; }
void HeadlessPlatform::set_head(const ar_pose &p) { std::lock_guard<std::mutex> g(m_); head_ = p; }
void HeadlessPlatform::set_fov_up(float t) { std::lock_guard<std::mutex> g(m_); fov_up_ = t > 0.f && t < 100.f ? t : 0.5f; }
void HeadlessPlatform::set_near(float n) { std::lock_guard<std::mutex> g(m_); near_m_ = n > 0.f && n < 10.f ? n : 0.f; }
void HeadlessPlatform::set_time_ns(int64_t t) { std::lock_guard<std::mutex> g(m_); time_ = t; }
void HeadlessPlatform::set_pad(const ar_pad &pad, bool connected) { std::lock_guard<std::mutex> g(m_); pad_ = pad; pad_connected_ = connected; }
int HeadlessPlatform::wait_frame(void *u, ar_frame_info *out) {
    auto *p = static_cast<HeadlessPlatform *>(u);
    std::lock_guard<std::mutex> g(p->m_);
    std::memset(out, 0, sizeof *out);
    out->display_time_ns = p->time_;
    out->should_render = 1;
    out->tracking_from_head = p->head_;
    out->head_tracked = 1;
    const uint32_t n = p->stereo_ ? 2 : 1, vw0 = p->w_ / n;
    const float up = p->fov_up_;
    out->view_count = n;
    for (uint32_t i = 0; i < n; i++) {
        ar_pose off = ar_pose_identity();
        off.pos[0] = n == 2 ? (i == 0 ? -0.0315f : 0.0315f) : 0.f;
        out->views[i].tracking_from_eye = ar_pose_mul(p->head_, off);
        const uint32_t vw = i + 1 == n ? p->w_ - vw0 * (n - 1) : vw0;
        const float side = up * (float)vw / (float)p->h_;
        out->views[i].fov = ar_fov{-side, side, up, -up};
        out->views[i].near_m = p->near_m_;
        out->views[i].width = vw;
        out->views[i].height = p->h_;
    }
    return 1;
}
int HeadlessPlatform::get_table(void *u, ar_table *out) {
    auto *p = static_cast<HeadlessPlatform *>(u);
    std::lock_guard<std::mutex> g(p->m_);
    out->tracking_from_table = p->table_;
    out->half_extent[0] = 0.85f; out->half_extent[1] = 0.85f;
    return 1;
}
int HeadlessPlatform::get_pad(void *u, uint32_t player, ar_pad *out) {
    auto *p = static_cast<HeadlessPlatform *>(u);
    std::lock_guard<std::mutex> g(p->m_);
    *out = ar_pad{};
    if (player != 0) return 0;
    *out = p->pad_;
    return p->pad_connected_ ? 1 : 0;
}
ar_session_state HeadlessPlatform::state(void *) { return AR_SESSION_FOCUSED; }
}
