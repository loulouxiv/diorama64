// apps/quest/native — the game's resources (lang/, mods/, palettes/, dynos/) travel in the APK under assets/res, with an
// index (assets/res/index.txt: "SIZE PATH" per file, written by the Gradle build); they are copied once to the app's
// internal storage, where the game reads them as files, and again when the index changes (a new APK).
#pragma once
#include <string>
#include <android/asset_manager.h>
namespace quest {
// dest/res ready (dest: the app's internal files folder). false with *err when the APK has no index or a copy fails.
bool extract_resources(AAssetManager *am, const std::string &dest, std::string *res_dir, std::string *err);
}
