// apps/quest/native — the Quest app's entry point (Quest port, phase 3). A NativeActivity: the game boots from the ROM
// pushed to the app's external files folder, runs on the runtime with the portable platform and the null renderer (no
// picture yet: OpenXR comes in phase 4), plays its stereo mix through AAudio, follows the Android lifecycle (the game
// clock runs only while the app is resumed and focused) and logs to logcat. On start it also reports the OpenXR runtime
// and its extensions, the first check for phase 4.
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include "headless_platform.h"
#include "headless_session.h"
#include "look.h"
#include "null_device.h"
#include "quest_assets.h"
#include "quest_audio.h"
#include "quest_log.h"
#include "sm64_launch.h"
extern "C" {
#include "ar_adapter_sm64.h"
#include "ar_audio_types.h"
}
#ifdef SM64AR_HAVE_OPENXR
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

using namespace ar;

namespace {
const uint32_t kDisplayHz = 72;   // the pace of the frame loop until OpenXR's xrWaitFrame takes over (phase 4)

struct App {
    android_app *android = nullptr;
    bool resumed = false, focused = false, failed = false, started = false;
    std::string files, external, res_dir, rom, save;
    std::unique_ptr<HeadlessPlatform> plat;
    std::unique_ptr<IRenderDevice> dev;
    std::unique_ptr<HeadlessSession> session;
    quest::AudioOut audio;
    int64_t t0 = 0, paused_at = 0, paused_total = 0;
    uint64_t frames = 0;
};

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

#ifdef SM64AR_HAVE_OPENXR
// The OpenXR loader and runtime as the app will meet them in phase 4: the loader initialised with the activity, the
// runtime's extensions listed (the ones phase 4/5 need are flagged).
void probe_openxr(android_app *a) {
    PFN_xrInitializeLoaderKHR init = nullptr;
    if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init) != XR_SUCCESS || !init) {
        QERR("OpenXR: the loader has no xrInitializeLoaderKHR");
        return;
    }
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = a->activity->vm;
    li.applicationContext = a->activity->clazz;
    if (init((const XrLoaderInitInfoBaseHeaderKHR *)&li) != XR_SUCCESS) { QERR("OpenXR: xrInitializeLoaderKHR failed"); return; }
    uint32_t n = 0;
    if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr) != XR_SUCCESS) { QERR("OpenXR: no runtime answered"); return; }
    std::vector<XrExtensionProperties> ext(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data());
    static const char *wanted[] = {"XR_KHR_vulkan_enable2", "XR_KHR_composition_layer_depth", "XR_FB_passthrough", "XR_FB_scene",
                                   "XR_FB_spatial_entity", "XR_FB_spatial_entity_storage", "XR_FB_display_refresh_rate",
                                   "XR_FB_foveation", "XR_FB_swapchain_update_state", "XR_EXT_performance_settings",
                                   "XR_META_environment_depth", "XR_EXT_hand_tracking"};
    QLOG("OpenXR runtime: %u extensions", n);
    for (const char *w : wanted) {
        bool have = false;
        for (const auto &e : ext) have = have || std::string(e.extensionName) == w;
        QLOG("  %s %s", have ? "has" : "LACKS", w);
    }
}
#endif

void set_state(App &app) {
    if (!app.plat) return;
    app.plat->set_session_state(app.resumed && app.focused ? AR_SESSION_FOCUSED : app.resumed ? AR_SESSION_VISIBLE : AR_SESSION_IDLE);
}

// Boots the game once, the first time the app is resumed: resources, ROM, save folder, runtime, audio.
bool start_game(App &app) {
    app.started = true;
    std::string err;
    if (!quest::extract_resources(app.android->activity->assetManager, app.files, &app.res_dir, &err)) { QERR("%s", err.c_str()); return false; }
    ar_adapter_sm64_set_resource_dir(app.res_dir.c_str());
    app.rom = app.external + "/baserom.us.z64";
    app.save = app.files + "/save";
    mkdir(app.save.c_str(), 0755);
    if (!exists(app.rom)) {
        QERR("no ROM at %s: push your Super Mario 64 (US) ROM there (python3 scripts/quest_install.py --rom ROM)", app.rom.c_str());
        return false;
    }
    OwnedSarec none;
    LaunchSetup l;
    l.save = app.save;
    std::string setup;
    if (!sm64_apply_setup(l, none.r, &setup)) { QERR("the game setup failed (see Diorama64-out)"); return false; }
    app.plat.reset(new HeadlessPlatform());
    app.plat->set_size(1832, 1920);   // one Quest 2 eye: the null renderer only counts
    set_state(app);
    app.dev = create_null_device();
    app.session.reset(new HeadlessSession(ar_adapter_sm64(), *app.plat, *app.dev));
    LookSettings look;
    look.roomlit = false;
    look.msaa = 1;
    app.session->set_look(look);
    if (!app.session->init(app.rom, app.save, &err)) { QERR("the game did not start: %s", err.c_str()); return false; }
    QLOG("game started: ROM %s, save %s", app.rom.c_str(), app.save.c_str());
    HeadlessSession *s = app.session.get();
    app.audio.start(AR_AUDIO_RATE_HZ, [s](int16_t *stereo, uint32_t frames) -> uint32_t {
        ar_runtime *rt = s->runtime();
        return rt ? ar_runtime_pull_audio(rt, stereo, frames) : 0;
    });
    app.t0 = now_ns();
    return true;
}

void on_cmd(android_app *a, int32_t cmd) {
    App &app = *static_cast<App *>(a->userData);
    switch (cmd) {
        case APP_CMD_RESUME:
            app.resumed = true;
            if (app.paused_at) { app.paused_total += now_ns() - app.paused_at; app.paused_at = 0; }
            app.audio.resume();
            QLOG("lifecycle: resume");
            break;
        case APP_CMD_PAUSE:
            app.resumed = false;
            app.paused_at = now_ns();
            app.audio.pause();
            QLOG("lifecycle: pause");
            break;
        case APP_CMD_GAINED_FOCUS: app.focused = true; QLOG("lifecycle: focus"); break;
        case APP_CMD_LOST_FOCUS: app.focused = false; QLOG("lifecycle: focus lost"); break;
        case APP_CMD_DESTROY: QLOG("lifecycle: destroy"); break;
        default: break;
    }
    set_state(app);
}

void log_stats(App &app, int64_t elapsed) {
    const HeadlessSession &s = *app.session;
    const NullDeviceStats ds = null_device_stats(*app.dev);
    QLOG("running: %.0f s, %llu frames, %llu game ticks, last frame %u world + %u overlay draws (%u passes), %u textures, "
         "audio %llu frames played (%d Hz), %llu underruns",
         elapsed * 1e-9, (unsigned long long)app.frames, (unsigned long long)s.ticks_done(), s.last_world_draws(), s.last_overlay_draws(),
         ds.last_passes, ds.textures, (unsigned long long)app.audio.frames_played(), app.audio.actual_rate(),
         (unsigned long long)app.audio.underruns());
}
}

void android_main(android_app *a) {
    quest::redirect_stdio_to_logcat();
    App app;
    app.android = a;
    a->userData = &app;
    a->onAppCmd = on_cmd;
    app.files = a->activity->internalDataPath ? a->activity->internalDataPath : "";
    app.external = a->activity->externalDataPath ? a->activity->externalDataPath : app.files;
    QLOG("Diorama64 for Quest (phase 3: game, audio and lifecycle; no picture yet). files=%s external=%s", app.files.c_str(),
         app.external.c_str());
#ifdef SM64AR_HAVE_OPENXR
    probe_openxr(a);
#endif
    const int64_t frame_ns = 1000000000LL / kDisplayHz;
    int64_t next = now_ns(), last_log = next;
    while (!a->destroyRequested) {
        int events = 0;
        android_poll_source *src = nullptr;
        // block while there is nothing to run (paused, or the game could not start)
        const bool idle = !app.resumed || app.failed;
        while (ALooper_pollOnce(idle && !a->destroyRequested ? -1 : 0, nullptr, &events, (void **)&src) >= 0) {
            if (src) src->process(a, src);
            if (a->destroyRequested || (!app.resumed || app.failed) != idle) break;
        }
        if (a->destroyRequested || !app.resumed || app.failed) continue;
        if (!app.started && !start_game(app)) { app.failed = true; continue; }
        const int64_t now = now_ns();
        app.plat->set_time_ns(now - app.t0 - app.paused_total);   // the game's time stands still while paused
        if (!app.session->frame()) { QERR("the platform stopped"); app.failed = true; continue; }
        app.frames++;
        if (now - last_log >= 5000000000LL) { log_stats(app, now - app.t0 - app.paused_total); last_log = now; }
        next += frame_ns;
        if (next < now) next = now;
        std::this_thread::sleep_for(std::chrono::nanoseconds(next - now));
    }
    QLOG("shutting down");
    app.audio.stop();
    app.session.reset();
    app.dev.reset();
}
