// apps/linux_window (sm64ar_window) — the Linux development harness with a window (Quest port, phase 2): the diorama on
// the table of a virtual room, drawn by the Vulkan backend, a free camera, and the game played with a gamepad or the
// keyboard. The final image is read back and shown through SDL2 (no swapchain: it runs on lavapipe or any GPU, and under
// Xvfb for tests).
//
// Camera: W/A/S/D move, Q/E down/up, right mouse button drag to look, O orbit, V stereo, P screenshot, Esc quit.
// Game (keyboard): arrows stick, X = A, C = B, Z = Z, Enter = Start, I/J/K/L = C buttons, R = R. A gamepad wins.
#include <SDL2/SDL.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "free_fly.h"
#include "headless_platform.h"
#include "headless_session.h"
#include "look.h"
#include "png_write.h"
#include "sm64_launch.h"
#include "vk_device.h"
extern "C" {
#include "ar_adapter_sm64.h"
}
using namespace ar;

namespace {
struct Options {
    std::string rom, save = "local/window-save", look = "original", pipelines, screenshot, record;
    std::vector<std::string> game_args;
    uint32_t w = 1280, h = 720, msaa = 1, warmup = 0, screenshot_after = 0, quit_after = 0;
    bool stereo = false, validation = false, orbit = false;
};
const char *kUsage =
    "usage: sm64ar_window [options]\n"
    "  --rom PATH  --save DIR  --level KEY  --start story|level  --game-arg KEY=VALUE\n"
    "  --size WxH  --look original|roomlit  --msaa 1|2|4  --stereo  --orbit  --validation\n"
    "  --pipelines FILE        pipeline list warmed at start, written at exit\n"
    "  --record FILE           record the session (sarec)\n"
    "  --warmup-ticks N        game ticks before the window opens (default 0)\n"
    "  --screenshot-after N PATH   PNG of frame N;  --quit-after N: quit after N frames\n"
    "keys: W/A/S/D/Q/E camera, right mouse drag look, O orbit, V stereo, P screenshot, Esc quit\n"
    "      arrows stick, X = A, C = B, Z = Z, R = R, Enter = Start, I/J/K/L = C buttons (a gamepad wins)\n";

bool parse_u32(const char *v, uint32_t *out) {
    if (!v || !*v || *v == '-') return false;
    char *e = nullptr;
    const unsigned long x = std::strtoul(v, &e, 10);
    if (*e) return false;
    *out = (uint32_t)x; return true;
}
bool parse(int argc, char **argv, Options &o, std::string &err) {
    std::string level, start;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() -> const char * { if (i + 1 >= argc) { err = "missing value for " + a; return nullptr; } return argv[++i]; };
        auto u32 = [&](uint32_t *out) { const char *v = val(); if (!v || !parse_u32(v, out)) { if (v) err = "bad " + a; return false; } return true; };
        const char *v = nullptr;
        if (a == "--help" || a == "-h") { std::fputs(kUsage, stdout); std::exit(0); }
        else if (a == "--rom") { if (!(v = val())) return false; o.rom = v; }
        else if (a == "--save") { if (!(v = val())) return false; o.save = v; }
        else if (a == "--level") { if (!(v = val())) return false; level = v; }
        else if (a == "--start") { if (!(v = val())) return false; start = v; }
        else if (a == "--game-arg") { if (!(v = val())) return false; o.game_args.push_back(v); }
        else if (a == "--look") { if (!(v = val())) return false; o.look = v; if (o.look != "original" && o.look != "roomlit") { err = "bad --look"; return false; } }
        else if (a == "--msaa") { if (!u32(&o.msaa)) return false; }
        else if (a == "--stereo") o.stereo = true;
        else if (a == "--orbit") o.orbit = true;
        else if (a == "--validation") o.validation = true;
        else if (a == "--pipelines") { if (!(v = val())) return false; o.pipelines = v; }
        else if (a == "--record") { if (!(v = val())) return false; o.record = v; }
        else if (a == "--warmup-ticks") { if (!u32(&o.warmup)) return false; }
        else if (a == "--quit-after") { if (!u32(&o.quit_after)) return false; }
        else if (a == "--screenshot-after") { if (!u32(&o.screenshot_after)) return false; if (!(v = val())) return false; o.screenshot = v; }
        else if (a == "--size") {
            if (!(v = val())) return false;
            const std::string sz = v;
            const size_t x = sz.find('x');
            if (x == std::string::npos || !parse_u32(sz.substr(0, x).c_str(), &o.w) || !parse_u32(sz.substr(x + 1).c_str(), &o.h) || !o.w || !o.h) { err = "bad --size"; return false; }
        } else { err = "unknown option " + a + " (--help lists them)"; return false; }
    }
    if (o.rom.empty()) { const char *e = std::getenv("SM64_ROM"); o.rom = e ? e : "local/baserom.us.z64"; }
    if (!level.empty()) o.game_args.push_back("level=" + level);
    if (!start.empty()) o.game_args.push_back("start=" + start);
    return true;
}

float axis(SDL_GameController *c, SDL_GameControllerAxis a) {
    const float v = SDL_GameControllerGetAxis(c, a) / 32767.f;
    return std::fabs(v) < 0.15f ? 0.f : std::fmax(-1.f, std::fmin(1.f, v));
}
// The pad of the first gamepad, else of the keyboard.
ar_pad read_pad(SDL_GameController *c) {
    ar_pad p{};
    if (c) {
        static const struct { SDL_GameControllerButton b; uint32_t bit; } map[] = {
            {SDL_CONTROLLER_BUTTON_A, AR_BTN_A}, {SDL_CONTROLLER_BUTTON_B, AR_BTN_B}, {SDL_CONTROLLER_BUTTON_X, AR_BTN_X},
            {SDL_CONTROLLER_BUTTON_Y, AR_BTN_Y}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, AR_BTN_LB},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, AR_BTN_RB}, {SDL_CONTROLLER_BUTTON_START, AR_BTN_START},
            {SDL_CONTROLLER_BUTTON_BACK, AR_BTN_SELECT}, {SDL_CONTROLLER_BUTTON_DPAD_UP, AR_BTN_DPAD_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, AR_BTN_DPAD_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, AR_BTN_DPAD_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, AR_BTN_DPAD_RIGHT}};
        for (const auto &m : map) if (SDL_GameControllerGetButton(c, m.b)) p.buttons |= m.bit;
        p.lx = axis(c, SDL_CONTROLLER_AXIS_LEFTX); p.ly = -axis(c, SDL_CONTROLLER_AXIS_LEFTY);
        p.rx = axis(c, SDL_CONTROLLER_AXIS_RIGHTX); p.ry = -axis(c, SDL_CONTROLLER_AXIS_RIGHTY);
        p.lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f;
        p.rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f;
        return p;
    }
    const Uint8 *k = SDL_GetKeyboardState(nullptr);
    p.lx = (k[SDL_SCANCODE_RIGHT] ? 1.f : 0.f) - (k[SDL_SCANCODE_LEFT] ? 1.f : 0.f);
    p.ly = (k[SDL_SCANCODE_UP] ? 1.f : 0.f) - (k[SDL_SCANCODE_DOWN] ? 1.f : 0.f);
    p.rx = (k[SDL_SCANCODE_L] ? 1.f : 0.f) - (k[SDL_SCANCODE_J] ? 1.f : 0.f);
    p.ry = (k[SDL_SCANCODE_I] ? 1.f : 0.f) - (k[SDL_SCANCODE_K] ? 1.f : 0.f);
    if (k[SDL_SCANCODE_X]) p.buttons |= AR_BTN_A;
    if (k[SDL_SCANCODE_C]) p.buttons |= AR_BTN_B;
    if (k[SDL_SCANCODE_Z]) p.buttons |= AR_BTN_LB;
    if (k[SDL_SCANCODE_R]) p.buttons |= AR_BTN_RB;
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= AR_BTN_START;
    return p;
}
int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

int main(int argc, char **argv) {
    Options o;
    std::string err;
    if (!parse(argc, argv, o, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    std::error_code ec;
    std::filesystem::create_directories(o.save, ec);
    OwnedSarec rec, out;
    LaunchSetup l;
    l.game_args = o.game_args; l.record = !o.record.empty(); l.save = o.save;
    std::string setup;
    if (!sm64_apply_setup(l, rec.r, &setup)) return 1;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { std::fprintf(stderr, "error: SDL: %s\n", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("Diorama64 (Linux harness)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)o.w, (int)o.h,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
    if (!ren) { std::fprintf(stderr, "error: SDL window: %s\n", SDL_GetError()); return 1; }

    VulkanDeviceOptions vo;
    vo.validation = o.validation;
    std::string why;
    std::unique_ptr<IRenderDevice> dev = create_vulkan_device(vo, &why);
    if (!dev) { std::fprintf(stderr, "error: Vulkan: %s\n", why.c_str()); return 1; }
    HeadlessPlatform plat;
    plat.set_stereo(o.stereo);
    int dw = 0, dh = 0;
    SDL_GetRendererOutputSize(ren, &dw, &dh);
    plat.set_size((uint32_t)dw, (uint32_t)dh);
    {
        HeadlessSession s(ar_adapter_sm64(), plat, *dev);
        s.set_key_light_callback([](void *, int on, const float d[3]) { ar_adapter_sm64_set_key_light(on, d); }, nullptr);
        LookSettings look;
        look.roomlit = o.look == "roomlit";
        look.msaa = (uint8_t)o.msaa;
        s.set_look(look);
        ar_adapter_sm64_set_normal_flags(look_normal_flags(look));
        if (!s.init(o.rom, o.save, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        if (!o.record.empty()) { sarec_init(&out.r, ar_adapter_sm64()->name, setup.c_str()); ar_runtime_set_recorder(s.runtime(), &out.r); }
        if (!o.pipelines.empty()) {
            std::ifstream in(o.pipelines, std::ios::binary);
            if (in) {
                const std::string list((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::printf("AR_WARM built=%zu\n", dev->warm_pipelines(list, 0));
            }
        }
        s.warmup(o.warmup);
        SDL_GameController *pad = nullptr;
        FreeFly fly;
        SDL_Texture *tex = nullptr;
        uint32_t tw = 0, th = 0;
        const int64_t t0 = now_ns();
        int64_t last = t0, fps_t = t0;
        uint64_t frame = 0, fps_frames = 0;
        bool running = true, orbit = o.orbit, stereo = o.stereo, look_drag = false;
        auto shot = [&](const std::string &path) {
            std::vector<uint8_t> px; uint32_t w = 0, h = 0;
            if (s.read_final(px, w, h) && png_write_rgba(path, px.data(), w, h)) std::printf("screenshot %s (%ux%u)\n", path.c_str(), w, h);
            else std::fprintf(stderr, "screenshot failed: %s\n", path.c_str());
            std::fflush(stdout);
        };
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) running = false;
                else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
                else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad &&
                         e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) { SDL_GameControllerClose(pad); pad = nullptr; }
                else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) look_drag = true;
                else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) look_drag = false;
                else if (e.type == SDL_MOUSEMOTION && look_drag) fly.look(-e.motion.xrel * 0.005f, -e.motion.yrel * 0.005f);
                else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    switch (e.key.keysym.sym) {
                        case SDLK_ESCAPE: running = false; break;
                        case SDLK_o: orbit = !orbit; break;
                        case SDLK_v: stereo = !stereo; plat.set_stereo(stereo); break;
                        case SDLK_p: std::filesystem::create_directories("local/screenshots", ec); shot("local/screenshots/screenshot.png"); break;
                        default: break;
                    }
                }
            }
            const int64_t now = now_ns();
            const float dt = (float)(now - last) * 1e-9f;
            last = now;
            const Uint8 *k = SDL_GetKeyboardState(nullptr);
            fly.move((k[SDL_SCANCODE_W] ? 1.f : 0.f) - (k[SDL_SCANCODE_S] ? 1.f : 0.f), (k[SDL_SCANCODE_D] ? 1.f : 0.f) - (k[SDL_SCANCODE_A] ? 1.f : 0.f),
                     (k[SDL_SCANCODE_E] ? 1.f : 0.f) - (k[SDL_SCANCODE_Q] ? 1.f : 0.f), dt);
            plat.set_pad(read_pad(pad), true);
            SDL_GetRendererOutputSize(ren, &dw, &dh);
            plat.set_size((uint32_t)std::max(dw, 1), (uint32_t)std::max(dh, 1));
            plat.set_time_ns(now - t0);
            plat.set_head(orbit ? orbit_pose((double)(now - t0) * 1e-9, 1.9f, 1.5f, 12.0) : fly.pose());
            if (!s.frame()) break;
            dev->wait_idle();
            std::vector<uint8_t> px; uint32_t w = 0, h = 0;
            if (s.read_final(px, w, h) && w && h) {
                if (!tex || w != tw || h != th) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
                    tw = w; th = h;
                }
                SDL_UpdateTexture(tex, nullptr, px.data(), (int)w * 4);
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, tex, nullptr, nullptr);
                SDL_RenderPresent(ren);
            }
            frame++; fps_frames++;
            if (now - fps_t > 2000000000LL) {
                char title[128];
                std::snprintf(title, sizeof title, "Diorama64 (Linux harness) - %.1f fps", fps_frames * 1e9 / (double)(now - fps_t));
                SDL_SetWindowTitle(win, title);
                fps_t = now; fps_frames = 0;
            }
            if (o.screenshot_after && frame == o.screenshot_after) shot(o.screenshot);
            if (o.quit_after && frame >= o.quit_after) running = false;
        }
        if (!o.pipelines.empty()) { std::ofstream po(o.pipelines, std::ios::binary | std::ios::trunc); po << dev->pipeline_list(); }
        const VulkanDeviceInfo vi = vulkan_device_info(*dev);
        std::printf("AR_WINDOW frames=%llu ticks=%llu validation_errors=%llu\n", (unsigned long long)frame, (unsigned long long)s.ticks_done(),
                    (unsigned long long)vi.validation_errors);
        if (tex) SDL_DestroyTexture(tex);
        if (pad) SDL_GameControllerClose(pad);
    }
    if (!o.record.empty()) {
        if (!sarec_save(&out.r, o.record.c_str())) std::fprintf(stderr, "error: cannot write %s\n", o.record.c_str());
        else std::printf("AR_RECORD saved=1 path=%s ticks=%u\n", o.record.c_str(), out.r.count);
    }
    dev.reset();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
