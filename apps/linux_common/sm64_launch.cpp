#include "sm64_launch.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "ar_audio_settings.h"
#include "ar_game_args.h"
extern "C" {
#include "ar_adapter_sm64.h"
#include "sm64_ar_setup.h"
#include "sm64_story.h"
}
namespace ar {
bool sm64_apply_setup(const LaunchSetup &l, const sarec &rec, std::string *setup) {
    std::string serr;
    if (!resolve_setup(join_game_args(l.game_args), l.replay ? rec.setup : "", setup, &serr, l.replay)) {
        std::fprintf(stderr, "error: %s\n", serr.c_str());
        return false;
    }
    if (l.replay && rec.game[0] && std::strcmp(rec.game, ar_adapter_sm64()->name) != 0) {
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
    if (l.record) {
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
    std::printf("AR_STORY start=%s source=%s\n", story ? "story" : "level", l.replay ? "recording" : "launch");
    if (story && (l.replay || l.record)) {
        std::error_code ec;
        const bool empty = !std::filesystem::exists(l.save, ec) || std::filesystem::is_empty(l.save, ec);
        if (!empty) { std::fprintf(stderr, "error: a story recording or replay needs an empty --save folder (%s)\n", l.save.c_str()); return false; }
    }
    const AudioSettings audio = audio_settings_off();
    ar_adapter_sm64_set_player_focus(audio.player_focus);
    ar_adapter_sm64_set_object_focus(audio.object_focus);
    ar_adapter_sm64_set_music_ring(audio.music_ring, audio.ring_radius, audio.ring_rotate);
    ar_adapter_sm64_set_ring_listener(audio.ring_at_head, audio.ring_focus);
    return true;
}
}
