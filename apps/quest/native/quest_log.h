// apps/quest/native — the Quest app (Quest port, phase 3): logs.
#pragma once
#include <android/log.h>
#define QLOG(...) __android_log_print(ANDROID_LOG_INFO, "Diorama64", __VA_ARGS__)
#define QERR(...) __android_log_print(ANDROID_LOG_ERROR, "Diorama64", __VA_ARGS__)
namespace quest {
// stdout and stderr (the game's printf, the runtime's AR_ lines) go to logcat, tag "Diorama64-out", one line per entry.
void redirect_stdio_to_logcat();
}
