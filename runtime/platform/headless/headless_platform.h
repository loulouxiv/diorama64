// runtime/platform/headless — a portable ar_platform for the Linux harness (and later tests): the clock, the head pose
// and the pad are set by the caller, the table is fixed (0.75 m high, 1.7 m square). Mirrors platform/macos's
// HarnessPlatform without Metal or GameController (no light probe, no live gamepad).
#pragma once
#include <mutex>
#include <string>
extern "C" {
#include "ar_platform.h"
}
namespace ar {
// Offscreen display time of output frame `frame` at `fps`: ceil(frame/fps s) in ns, so each frame lands on or just
// after its tick boundary (same rule as harness_platform.h).
inline int64_t headless_frame_time_ns(uint32_t frame, uint32_t fps) { return ((int64_t)frame * 1000000000LL + fps - 1) / fps; }
inline uint64_t headless_ticks_expected(uint32_t frames, uint32_t fps) { return frames ? (uint64_t)(frames - 1) * 30 / fps + 1 : 0; }
class HeadlessPlatform {
public:
    HeadlessPlatform();
    const ar_platform *api() const { return &api_; }
    void set_size(uint32_t w, uint32_t h);
    void set_stereo(bool on);
    void set_head(const ar_pose &tracking_from_head);
    void set_fov_up(float tan_half);
    void set_near(float near_m);
    void set_time_ns(int64_t t);
    void set_pad(const ar_pad &pad, bool connected);
    void set_preview_test(const std::string &mode);
    void set_session_state(ar_session_state s);   // default FOCUSED (the Quest app follows the Android lifecycle)
private:
    static int wait_frame(void *u, ar_frame_info *out);
    static int get_table(void *u, ar_table *out);
    static int get_pad(void *u, uint32_t player, ar_pad *out);
    static ar_session_state state(void *u);
    static int get_preview(void *u, ar_preview *out);
    ar_platform api_{};
    mutable std::mutex m_;
    ar_pose head_{{0.f, 1.55f, 1.9f}, {0.f, 0.f, 0.f, 1.f}};
    ar_pose table_{{0.f, 0.75f, 0.f}, {0.f, 0.f, 0.f, 1.f}};
    uint32_t w_ = 1280, h_ = 720;
    float fov_up_ = 0.5f;
    float near_m_ = 0.f;
    bool stereo_ = false;
    int64_t time_ = 0;
    ar_pad pad_{};
    bool pad_connected_ = true;
    int preview_mode_ = AR_PREVIEW_NONE;
    ar_session_state state_ = AR_SESSION_FOCUSED;
};
}
