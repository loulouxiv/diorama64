// apps/linux_headless — the game without a window or a GPU (Quest port, phase 1). The runtime steps SM64 one tick per
// frame at 30 fps, the common renderer draws every captured frame into the null device (counted, not rasterized), and
// the stereo mix goes to a WAV. A sarec recording replays with its state hashes checked tick by tick; a scripted pad
// (--pad) makes new recordings without a controller.
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "ar_audio_settings.h"
#include "ar_game_args.h"
#include "free_fly.h"
#include "headless_platform.h"
#include "headless_session.h"
#include "look.h"
#include "null_device.h"
#include "png_write.h"
#ifdef SM64AR_HAVE_VULKAN
#include "vk_device.h"
#endif
#include "room_light.h"
#include "wav_io.h"
extern "C" {
#include "ar_adapter_sm64.h"
#include "ar_sarec.h"
#include "sm64_ar_setup.h"
#include "sm64_host.h"
#include "sm64_story.h"
}
using namespace ar;

namespace {
struct OwnedSarec {
    sarec r{};
    OwnedSarec() = default;
    OwnedSarec(const OwnedSarec &) = delete;
    OwnedSarec &operator=(const OwnedSarec &) = delete;
    ~OwnedSarec() { sarec_free(&r); }
};
struct PadStep { uint32_t tick; ar_pad pad; };
struct Options {
    std::string rom, save, sarec, record, audio_out, camera = "near", level, start;
    std::string device = "null", out = "local/headless-out", look = "original", pipeline_cache, pipelines;
    std::vector<uint32_t> png_at;
    uint32_t msaa = 1;
    bool validation = false;
    std::vector<std::string> game_args;
    std::vector<PadStep> pad;
    uint32_t frames = 300, fps = 30, w = 1280, h = 720, warmup = 240, stats_every = 0;
    bool stereo = false, log_hash = false, log_frame_hash = false, story_events = false;
    bool pad_connected = true;
};

const char *kUsage =
    "usage: sm64ar_headless [options]\n"
    "  --rom PATH            US ROM (.z64; default $SM64_ROM or local/baserom.us.z64)\n"
    "  --save DIR            save folder (default local/headless-save)\n"
    "  --frames N            output frames after warm-up (default 300 = 10 s)\n"
    "  --fps N               output frames per second (default 30: one game tick per frame)\n"
    "  --warmup-ticks N      game ticks stepped before the first frame (default 240)\n"
    "  --level KEY | --start story|level | --game-arg KEY=VALUE   the game setup (as the macOS harness)\n"
    "  --sarec FILE          replay a recording (setup and pads from the file; state hashes checked)\n"
    "  --record FILE         record this run (pads, poses, state hashes)\n"
    "  --pad TICK:BUTTONS[:LX,LY]   from game tick TICK on, hold BUTTONS (A+B+START..., or none) and the left stick\n"
    "  --pad-file FILE       one --pad value per line (# comments)\n"
    "  --no-pad              no controller connected\n"
    "  --audio-out FILE.wav  the stereo mix of the frames after warm-up\n"
    "  --camera near|far|top|low|orbit   viewer head pose (default near)\n"
    "  --stereo  --size WxH  views drawn (default one 1280x720 view)\n"
    "  --device null|vulkan  null: counts draws only; vulkan: draws them (lavapipe or a GPU)\n"
    "  --png-at F1,F2,...    with --device vulkan, write frame_NNNN.png of these frames to --out DIR\n"
    "  --out DIR             PNG folder (default local/headless-out)\n"
    "  --look original|roomlit   Original N64 (default) or the room-lit look (lighting, shadows, toy)\n"
    "  --msaa 1|2|4          multisampling of the AR layer (default 1, as the macOS harness)\n"
    "  --validation          Vulkan validation layer; its error count is in AR_VK_SUMMARY\n"
    "  --pipeline-cache FILE VkPipelineCache loaded and saved there\n"
    "  --pipelines FILE      pipeline list: warmed before the first frame when it exists, written at the end\n"
    "  --log-hash            AR_TICK after warm-up;  --log-frame-hash: after every frame\n"
    "  --stats-every N       AR_FRAME line (draw counts) every N frames\n"
    "  --story-events        AR_LEVEL / AR_MENU / AR_DIALOG lines when they change\n";

bool parse_u32(const char *v, uint32_t *out) {
    if (!v || !*v || *v == '-' || *v == '+' || *v == ' ') return false;
    char *e = nullptr; errno = 0;
    const unsigned long long x = std::strtoull(v, &e, 10);
    if (*e || errno || x > 0xFFFFFFFFull) return false;
    *out = (uint32_t)x; return true;
}
bool parse_float(const std::string &v, float *out) {
    if (v.empty()) return false;
    char *e = nullptr;
    const float x = std::strtof(v.c_str(), &e);
    if (*e || !std::isfinite(x)) return false;
    *out = x; return true;
}
bool parse_buttons(const std::string &s, uint32_t *out) {
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"A", AR_BTN_A}, {"B", AR_BTN_B}, {"X", AR_BTN_X}, {"Y", AR_BTN_Y}, {"LB", AR_BTN_LB}, {"RB", AR_BTN_RB},
        {"START", AR_BTN_START}, {"SELECT", AR_BTN_SELECT}, {"UP", AR_BTN_DPAD_UP}, {"DOWN", AR_BTN_DPAD_DOWN},
        {"LEFT", AR_BTN_DPAD_LEFT}, {"RIGHT", AR_BTN_DPAD_RIGHT}, {"LSTICK", AR_BTN_LSTICK}, {"RSTICK", AR_BTN_RSTICK}};
    *out = 0;
    if (s == "none") return true;
    size_t at = 0;
    for (;;) {
        const size_t end = s.find('+', at);
        const std::string n = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
        bool found = false;
        for (const auto &b : names) if (n == b.name) { *out |= b.bit; found = true; }
        if (!found) return false;
        if (end == std::string::npos) return true;
        at = end + 1;
    }
}
// TICK:BUTTONS[:LX,LY]
bool parse_pad(const std::string &v, PadStep *out) {
    *out = PadStep{};
    const size_t c1 = v.find(':');
    if (c1 == std::string::npos || !parse_u32(v.substr(0, c1).c_str(), &out->tick)) return false;
    const size_t c2 = v.find(':', c1 + 1);
    if (!parse_buttons(v.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1), &out->pad.buttons)) return false;
    if (c2 == std::string::npos) return true;
    const std::string st = v.substr(c2 + 1);
    const size_t comma = st.find(',');
    return comma != std::string::npos && parse_float(st.substr(0, comma), &out->pad.lx) && parse_float(st.substr(comma + 1), &out->pad.ly) &&
           std::fabs(out->pad.lx) <= 1.f && std::fabs(out->pad.ly) <= 1.f;
}
bool parse(int argc, char **argv, Options &o, std::string &err) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * { if (i + 1 >= argc) { err = "missing value for " + a; return nullptr; } return argv[++i]; };
        auto u32 = [&](uint32_t *out) { const char *v = val(); if (!v) return false; if (!parse_u32(v, out)) { err = "bad " + a + " value '" + v + "'"; return false; } return true; };
        const char *v = nullptr;
        if (a == "--help" || a == "-h") { std::fputs(kUsage, stdout); std::exit(0); }
        else if (a == "--rom") { if (!(v = val())) return false; o.rom = v; }
        else if (a == "--save") { if (!(v = val())) return false; o.save = v; }
        else if (a == "--sarec") { if (!(v = val())) return false; o.sarec = v; }
        else if (a == "--record") { if (!(v = val())) return false; o.record = v; }
        else if (a == "--audio-out") { if (!(v = val())) return false; o.audio_out = v; }
        else if (a == "--camera") { if (!(v = val())) return false; o.camera = v; }
        else if (a == "--frames") { if (!u32(&o.frames)) return false; }
        else if (a == "--fps") { if (!u32(&o.fps)) return false; if (!o.fps || o.fps > 240) { err = "bad --fps (1..240)"; return false; } }
        else if (a == "--warmup-ticks") { if (!u32(&o.warmup)) return false; }
        else if (a == "--stats-every") { if (!u32(&o.stats_every)) return false; }
        else if (a == "--stereo") o.stereo = true;
        else if (a == "--log-hash") o.log_hash = true;
        else if (a == "--log-frame-hash") o.log_frame_hash = true;
        else if (a == "--story-events") o.story_events = true;
        else if (a == "--no-pad") o.pad_connected = false;
        else if (a == "--validation") o.validation = true;
        else if (a == "--device") {
            if (!(v = val())) return false;
            o.device = v;
            if (o.device != "null" && o.device != "vulkan") { err = "bad --device (null or vulkan)"; return false; }
        }
        else if (a == "--out") { if (!(v = val())) return false; o.out = v; }
        else if (a == "--pipeline-cache") { if (!(v = val())) return false; o.pipeline_cache = v; }
        else if (a == "--pipelines") { if (!(v = val())) return false; o.pipelines = v; }
        else if (a == "--look") {
            if (!(v = val())) return false;
            o.look = v;
            if (o.look != "original" && o.look != "roomlit") { err = "bad --look (original or roomlit)"; return false; }
        }
        else if (a == "--msaa") { if (!u32(&o.msaa)) return false; if (o.msaa != 1 && o.msaa != 2 && o.msaa != 4) { err = "bad --msaa (1, 2 or 4)"; return false; } }
        else if (a == "--png-at") {
            if (!(v = val())) return false;
            std::string list = v;
            for (size_t at = 0;;) {
                const size_t end = list.find(',', at);
                uint32_t f = 0;
                if (!parse_u32(list.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str(), &f)) { err = "bad --png-at"; return false; }
                o.png_at.push_back(f);
                if (end == std::string::npos) break;
                at = end + 1;
            }
            std::sort(o.png_at.begin(), o.png_at.end());
        }
        else if (a == "--level") {
            if (!(v = val())) return false;
            o.level = v;
            if (o.level.empty() || o.level.find(',') != std::string::npos) { err = std::string("bad --level '") + v + "' (one level key)"; return false; }
        } else if (a == "--start") {
            if (!(v = val())) return false;
            if (std::strcmp(v, "story") != 0 && std::strcmp(v, "level") != 0) { err = std::string("bad --start '") + v + "' (story or level)"; return false; }
            o.start = v;
        } else if (a == "--game-arg") {
            if (!(v = val())) return false;
            if (!valid_game_arg(v)) { err = std::string("bad --game-arg '") + v + "' (use KEY=VALUE)"; return false; }
            o.game_args.push_back(v);
        } else if (a == "--pad") {
            if (!(v = val())) return false;
            PadStep p;
            if (!parse_pad(v, &p)) { err = std::string("bad --pad '") + v + "' (TICK:BUTTONS[:LX,LY])"; return false; }
            o.pad.push_back(p);
        } else if (a == "--pad-file") {
            if (!(v = val())) return false;
            std::ifstream f(v);
            if (!f) { err = std::string("cannot read --pad-file ") + v; return false; }
            std::string line;
            for (int n = 1; std::getline(f, line); n++) {
                line = line.substr(0, line.find('#'));
                line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r'; }), line.end());
                if (line.empty()) continue;
                PadStep p;
                if (!parse_pad(line, &p)) { err = std::string(v) + ":" + std::to_string(n) + ": bad pad step '" + line + "'"; return false; }
                o.pad.push_back(p);
            }
        } else if (a == "--size") {
            if (!(v = val())) return false;
            const std::string sz = v;
            const size_t x = sz.find('x');
            if (x == std::string::npos || !parse_u32(sz.substr(0, x).c_str(), &o.w) || !parse_u32(sz.substr(x + 1).c_str(), &o.h) || !o.w || !o.h) { err = "bad --size (use WxH)"; return false; }
        } else { err = "unknown option " + a + " (--help lists them)"; return false; }
    }
    if (o.rom.empty()) { const char *e = std::getenv("SM64_ROM"); o.rom = e ? e : "local/baserom.us.z64"; }
    if (o.save.empty()) o.save = "local/headless-save";
    if (!o.level.empty()) {
        for (const std::string &g : o.game_args)
            if (g.rfind("level=", 0) == 0) { err = "--level and --game-arg level=... go not together"; return false; }
        o.game_args.push_back("level=" + o.level);
    }
    if (o.start == "story" && !o.level.empty()) { err = "--start story and --level go not together"; return false; }
    if (!o.start.empty() && !o.sarec.empty()) { err = "--start is taken from the recording (--sarec)"; return false; }
    if (!o.start.empty()) o.game_args.push_back("start=" + o.start);
    if (!o.pad.empty() && !o.sarec.empty()) { err = "--pad scripts new input: a replay (--sarec) plays the recorded pads"; return false; }
    std::stable_sort(o.pad.begin(), o.pad.end(), [](const PadStep &x, const PadStep &y) { return x.tick < y.tick; });
    CameraPreset p;
    if (o.camera != "orbit" && !parse_preset(o.camera.c_str(), &p)) { err = "unknown --camera " + o.camera; return false; }
    return true;
}

// Same order and meaning as apps/macos_harness's apply_setup: a replay takes the recording's setup; a recording gets
// an item for each game setting not at its default.
bool apply_setup(const Options &o, const sarec &rec, std::string *setup) {
    std::string serr;
    const bool replay = !o.sarec.empty();
    if (!resolve_setup(join_game_args(o.game_args), replay ? rec.setup : "", setup, &serr, replay)) {
        std::fprintf(stderr, "error: %s\n", serr.c_str());
        return false;
    }
    if (replay && rec.game[0] && std::strcmp(rec.game, ar_adapter_sm64()->name) != 0) {
        std::fprintf(stderr, "error: the recording is for %s, not %s\n", rec.game, ar_adapter_sm64()->name);
        return false;
    }
    ar_adapter_sm64_reset_game_settings();
    ar_adapter_sm64_set_pip_sky(1);
    ar_adapter_sm64_set_title_eyes_active(0);
    char why[160];
    if (!ar_adapter_sm64_setup(setup->c_str(), why, sizeof why)) { std::fprintf(stderr, "error: setup: %s\n", why); return false; }
    ar_adapter_sm64_set_screen_log([](int screen, int story, uint32_t tick) {
        const char *name = sm64_screen_name(screen);
        std::printf("AR_SCREEN name=%s story=%d tick=%u\n", name ? name : "?", story, tick);
    });
    ar_adapter_sm64_set_dropped_log([](const char *what, const char *why2, uint32_t tick) {
        std::printf("AR_LEVEL dropped %s: %s tick=%u\n", what, why2, tick);
    });
    if (!o.record.empty()) {
        char items[96];
        if (sm64_game_setting_items(ar_adapter_sm64_mario_cm(), ar_adapter_sm64_cannon_visible(), ar_adapter_sm64_draw_all_rooms(),
                                    ar_adapter_sm64_other_room_objects(), items, sizeof items) < 0) {
            std::fprintf(stderr, "error: the game settings' setup items do not fit\n"); return false;
        }
        *setup = setup_with_items(*setup, items);
        if (setup->size() > SAREC_SETUP_MAX) { std::fprintf(stderr, "error: %s\n", setup_too_long(items).c_str()); return false; }
    }
    std::printf("AR_SETUP %s\n", setup->c_str());
    const bool story = sm64_setup_start(setup->c_str()) == 1;
    std::printf("AR_STORY start=%s source=%s\n", story ? "story" : "level", replay ? "recording" : "launch");
    if (story && (replay || !o.record.empty())) {
        std::error_code ec;
        const bool empty = !std::filesystem::exists(o.save, ec) || std::filesystem::is_empty(o.save, ec);
        if (!empty) { std::fprintf(stderr, "error: a story recording or replay needs an empty --save folder (%s)\n", o.save.c_str()); return false; }
    }
    const AudioSettings audio = audio_settings_off();
    ar_adapter_sm64_set_player_focus(audio.player_focus);
    ar_adapter_sm64_set_object_focus(audio.object_focus);
    ar_adapter_sm64_set_music_ring(audio.music_ring, audio.ring_radius, audio.ring_rotate);
    ar_adapter_sm64_set_ring_listener(audio.ring_at_head, audio.ring_focus);
    return true;
}

int run(const Options &o) {
    OwnedSarec rec, out;   // outlive the session
    if (!o.sarec.empty()) {
        char why[200];
        if (!sarec_load_err(&rec.r, o.sarec.c_str(), why, sizeof why)) { std::fprintf(stderr, "error: cannot read %s: %s\n", o.sarec.c_str(), why); return 1; }
    }
    std::string setup;
    if (!apply_setup(o, rec.r, &setup)) return 1;
    std::error_code ec;
    std::filesystem::create_directories(o.save, ec);

    HeadlessPlatform plat;
    plat.set_size(o.w, o.h);
    plat.set_stereo(o.stereo);
    std::unique_ptr<IRenderDevice> dev;
    if (o.device == "vulkan") {
#ifdef SM64AR_HAVE_VULKAN
        VulkanDeviceOptions vo;
        vo.validation = o.validation;
        vo.pipeline_cache_path = o.pipeline_cache;
        std::string why;
        dev = create_vulkan_device(vo, &why);
        if (!dev) { std::fprintf(stderr, "error: Vulkan: %s\n", why.c_str()); return 1; }
#else
        std::fprintf(stderr, "error: built without the Vulkan backend (apt install libvulkan-dev libshaderc-dev, then rebuild)\n");
        return 1;
#endif
    } else {
        dev = create_null_device();
    }
    if (!o.png_at.empty() && o.device != "vulkan") { std::fprintf(stderr, "error: --png-at needs --device vulkan\n"); return 2; }
    if (!o.png_at.empty()) std::filesystem::create_directories(o.out, ec);
    HeadlessSession s(ar_adapter_sm64(), plat, *dev);
    s.set_audio_mode(AR_AUDIO_MODE_STEREO);
    s.set_key_light_callback([](void *, int on, const float d[3]) { ar_adapter_sm64_set_key_light(on, d); }, nullptr);
    LookSettings look;   // the app's room-lit defaults, or Original N64 (the harness default)
    look.roomlit = o.look == "roomlit";
    look.msaa = (uint8_t)o.msaa;
    s.set_look(look);
    ar_adapter_sm64_set_normal_flags(look_normal_flags(look));
    std::string err;
    if (!s.init(o.rom, o.save, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    if (!o.sarec.empty()) {
        ar_runtime_set_replay(s.runtime(), &rec.r);
        std::printf("AR_INPUT source=sarec ticks=%u version=%u hashes=%d input_frame=%d setup=%s\n", rec.r.count, rec.r.version,
                    (rec.r.flags & SAREC_FLAG_HASHES) ? 1 : 0, (rec.r.flags & SAREC_FLAG_INPUT_FRAME) ? 1 : 0, rec.r.setup);
    } else {
        std::printf("AR_INPUT source=%s steps=%zu\n", o.pad_connected ? "script" : "none", o.pad.size());
    }
    if (!o.record.empty()) { sarec_init(&out.r, ar_adapter_sm64()->name, setup.c_str()); ar_runtime_set_recorder(s.runtime(), &out.r); }

    size_t next_pad = 0;
    ar_pad pad{};
    auto feed_pad = [&]() {   // the pad of the tick about to run
        const uint64_t t = s.ticks_done();
        while (next_pad < o.pad.size() && o.pad[next_pad].tick <= t) pad = o.pad[next_pad++].pad;
        plat.set_pad(pad, o.pad_connected);
    };
    if (!o.pipelines.empty()) {
        std::ifstream in(o.pipelines, std::ios::binary);
        if (in) {
            const std::string list((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const size_t built = dev->warm_pipelines(list, 0);
            std::printf("AR_WARM built=%zu from=%s\n", built, o.pipelines.c_str());
        }
    }
    for (uint32_t i = 0; i < o.warmup; i++) { feed_pad(); s.warmup(1); }
    if (o.log_hash) std::printf("AR_TICK n=%llu hash=0x%016llx\n", (unsigned long long)s.ticks_done(), (unsigned long long)ar_adapter_sm64()->state_hash());

    std::vector<int16_t> audio, chunk(2 * 4096);
    auto pull_all = [&](bool keep) {
        for (;;) {
            ar_audio_pull p{chunk.data(), nullptr, 4096, 0, nullptr, 0, 0};
            const uint32_t n = s.pull_audio_scene(&p);
            if (keep) audio.insert(audio.end(), chunk.begin(), chunk.begin() + 2 * n);
            if (n == 0) return;
        }
    };
    pull_all(false);   // warm-up audio is not part of the output

    const bool orbit = o.camera == "orbit";
    CameraPreset preset = CameraPreset::Near;
    if (!orbit) parse_preset(o.camera.c_str(), &preset);
    int last_level = -2, last_menu = -2, last_dialog = -2;
    uint64_t snapshot_frames = 0;
    for (uint32_t f = 0; f < o.frames; f++) {
        plat.set_time_ns(headless_frame_time_ns(f, o.fps));
        plat.set_head(orbit ? orbit_pose((double)f / o.fps, 1.9f, 1.5f, 12.0) : preset_pose(preset));
        feed_pad();
        if (!s.frame()) { std::fprintf(stderr, "error: frame %u: the platform stopped\n", f); return 1; }
        if (std::binary_search(o.png_at.begin(), o.png_at.end(), f)) {
            dev->wait_idle();
            std::vector<uint8_t> px; uint32_t w = 0, h = 0;
            char name[64]; std::snprintf(name, sizeof name, "/frame_%04u.png", f);
            if (!s.read_final(px, w, h) || !png_write_rgba(o.out + name, px.data(), w, h)) {
                std::fprintf(stderr, "error: cannot write %s%s\n", o.out.c_str(), name);
                return 1;
            }
            std::printf("AR_PNG frame=%u size=%ux%u path=%s%s\n", f, w, h, o.out.c_str(), name);
        }
        const NullDeviceStats ds = o.device == "null" ? null_device_stats(*dev) : NullDeviceStats{};
        if (o.log_frame_hash)
            std::printf("AR_TICK n=%llu hash=0x%016llx\n", (unsigned long long)s.ticks_done(), (unsigned long long)ar_adapter_sm64()->state_hash());
        if (o.stats_every && f % o.stats_every == 0)
            std::printf("AR_FRAME f=%u tick=%llu world=%u overlay=%u verts=%u passes=%u draws=%u\n", f, (unsigned long long)s.ticks_done(),
                        s.last_world_draws(), s.last_overlay_draws(), s.last_vertices(), ds.last_passes, ds.last_draws);
        if (o.story_events) {
            const int lv = sm64_host_level(), menu = sm64_host_menu_mode(), dlg = sm64_host_dialog_id();
            if (lv != last_level) { std::printf("AR_LEVEL now=%d tick=%llu\n", lv, (unsigned long long)s.ticks_done()); last_level = lv; }
            if (menu != last_menu) { std::printf("AR_MENU mode=%d tick=%llu\n", menu, (unsigned long long)s.ticks_done()); last_menu = menu; }
            if (dlg != last_dialog) { std::printf("AR_DIALOG id=%d tick=%llu\n", dlg, (unsigned long long)s.ticks_done()); last_dialog = dlg; }
        }
        pull_all(!o.audio_out.empty());
    }
    snapshot_frames = s.frames_with_snapshot();
    const NullDeviceStats ds = o.device == "null" ? null_device_stats(*dev) : NullDeviceStats{};
#ifdef SM64AR_HAVE_VULKAN
    if (o.device == "vulkan") {
        dev->wait_idle();
        const VulkanDeviceInfo vi = vulkan_device_info(*dev);
        std::printf("AR_VK_SUMMARY device=\"%s\" pipelines=%llu shaders=%llu compile_ms=%.0f last_draws=%llu last_passes=%llu "
                    "draw_time_builds=%zu validation_errors=%llu validation_warnings=%llu\n",
                    vi.name.c_str(), (unsigned long long)vi.pipelines_built, (unsigned long long)vi.shader_modules, vi.compile_ms_total,
                    (unsigned long long)vi.draws_last_frame, (unsigned long long)vi.render_passes_last_frame, dev->draw_time_builds(),
                    (unsigned long long)vi.validation_errors, (unsigned long long)vi.validation_warnings);
    }
#endif
    if (!o.pipelines.empty()) {
        std::ofstream outp(o.pipelines, std::ios::binary | std::ios::trunc);
        outp << dev->pipeline_list();
    }
    std::printf("AR_LIGHT %s\n", room_light_line(s.room_light_status()).c_str());
    const uint64_t ticks = s.ticks_done() - o.warmup, want = headless_ticks_expected(o.frames, o.fps);
    std::printf("AR_SUMMARY frames=%u ticks=%llu snapshot_frames=%llu capture_draws=%llu device_frames=%llu passes=%llu draws=%llu "
                "vertices=%llu textures=%u uploads=%llu upload_mb=%.1f shadow_maps=%llu hash=0x%016llx\n",
                o.frames, (unsigned long long)ticks, (unsigned long long)snapshot_frames, (unsigned long long)s.capture_draws_total(),
                (unsigned long long)ds.frames, (unsigned long long)ds.passes, (unsigned long long)ds.draws, (unsigned long long)ds.vertices,
                ds.textures, (unsigned long long)ds.texture_uploads, ds.texture_bytes / 1048576.0, (unsigned long long)ds.shadow_maps,
                (unsigned long long)ar_adapter_sm64()->state_hash());
    if (ticks != want) {
        std::fprintf(stderr, "error: %u frame(s) at %u fps ran %llu game tick(s), expected %llu\n", o.frames, o.fps,
                     (unsigned long long)ticks, (unsigned long long)want);
        return 1;
    }
    bool diverged = false;
    if (!o.sarec.empty()) {
        const ar_replay_status st = ar_runtime_replay_status(s.runtime());
        std::printf("AR_REPLAY played=%llu checked=%llu diverged=%d", (unsigned long long)st.played, (unsigned long long)st.checked, st.diverged);
        if (!st.done) std::printf(" ended_early=1 done=%llu/%u", (unsigned long long)st.played, rec.r.count);
        if (st.diverged) std::printf(" tick=%llu expected=0x%016llx got=0x%016llx", (unsigned long long)st.first_bad_tick,
                                     (unsigned long long)st.expected, (unsigned long long)st.got);
        std::printf("\n");
        diverged = st.diverged != 0;
    }
    if (!o.record.empty()) {
        if (!sarec_save(&out.r, o.record.c_str())) { std::fprintf(stderr, "error: cannot write %s\n", o.record.c_str()); return 1; }
        std::printf("AR_RECORD saved=1 path=%s ticks=%u dropped=%u\n", o.record.c_str(), out.r.count, out.r.dropped);
        if (out.r.dropped) { std::fprintf(stderr, "error: the recording dropped %u tick(s)\n", out.r.dropped); return 1; }
    }
    if (!o.audio_out.empty()) {
        if (!wav_write_s16_stereo(o.audio_out, audio.data(), audio.size() / 2, AR_AUDIO_RATE_HZ)) {
            std::fprintf(stderr, "error: cannot write %s\n", o.audio_out.c_str());
            return 1;
        }
        int16_t peak = 0;
        double sq = 0;
        for (int16_t v : audio) { peak = std::max<int16_t>(peak, (int16_t)std::min(32767, std::abs((int)v))); sq += (double)v * v; }
        std::printf("AR_AUDIO frames=%zu seconds=%.2f peak=%d rms=%.1f path=%s\n", audio.size() / 2, (double)(audio.size() / 2) / AR_AUDIO_RATE_HZ,
                    peak, audio.empty() ? 0.0 : std::sqrt(sq / audio.size()), o.audio_out.c_str());
    }
    std::fflush(stdout);
    return diverged ? 3 : 0;
}
}

int main(int argc, char **argv) {
    Options o;
    std::string err;
    if (!parse(argc, argv, o, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    return run(o);
}
