// apps/common — what the hosts without Apple frameworks (Linux harnesses, Quest app) share: the SM64 game setup, the same order and
// meaning as apps/macos_harness's apply_setup.
#pragma once
#include <string>
#include <vector>
extern "C" {
#include "ar_sarec.h"
}
namespace ar {
struct OwnedSarec {
    sarec r{};
    OwnedSarec() = default;
    OwnedSarec(const OwnedSarec &) = delete;
    OwnedSarec &operator=(const OwnedSarec &) = delete;
    ~OwnedSarec() { sarec_free(&r); }
};
struct LaunchSetup {
    std::vector<std::string> game_args;   // KEY=VALUE (level=..., start=...)
    bool replay = false;                  // rec holds the recording being replayed
    bool record = false;                  // this run is recorded: the setup gets the game settings' items
    std::string save;
};
// A replay takes the recording's setup; a recording gets an item for each game setting not at its default. Prints
// AR_SETUP / AR_STORY; false (with a message on stderr) when the setup cannot be used.
bool sm64_apply_setup(const LaunchSetup &l, const sarec &rec, std::string *setup);
}
