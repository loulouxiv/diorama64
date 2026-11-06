#include "quest_assets.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <vector>
#include "quest_log.h"
namespace quest {
namespace {
std::string read_asset(AAssetManager *am, const char *path) {
    AAsset *a = AAssetManager_open(am, path, AASSET_MODE_BUFFER);
    if (!a) return std::string();
    const void *p = AAsset_getBuffer(a);
    std::string s(p ? (const char *)p : "", p ? (size_t)AAsset_getLength(a) : 0);
    AAsset_close(a);
    return s;
}
bool read_file(const std::string &path, std::string *out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}
bool mkdirs(const std::string &dir) {
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            const std::string d = dir.substr(0, i);
            if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}
bool copy_asset(AAssetManager *am, const std::string &src, const std::string &dst) {
    AAsset *a = AAssetManager_open(am, src.c_str(), AASSET_MODE_STREAMING);
    if (!a) return false;
    const size_t slash = dst.rfind('/');
    if (slash != std::string::npos && !mkdirs(dst.substr(0, slash))) { AAsset_close(a); return false; }
    FILE *f = std::fopen(dst.c_str(), "wb");
    if (!f) { AAsset_close(a); return false; }
    char buf[65536];
    int n;
    bool ok = true;
    while ((n = AAsset_read(a, buf, sizeof buf)) > 0) ok = ok && std::fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    ok = ok && n == 0;
    ok = std::fclose(f) == 0 && ok;
    AAsset_close(a);
    return ok;
}
}
bool extract_resources(AAssetManager *am, const std::string &dest, std::string *res_dir, std::string *err) {
    const std::string index = read_asset(am, "res/index.txt");
    if (index.empty()) { *err = "the APK has no assets/res/index.txt (built without the game resources?)"; return false; }
    *res_dir = dest + "/res";
    const std::string stamp = *res_dir + "/.index";
    std::string have;
    if (read_file(stamp, &have) && have == index) { QLOG("resources up to date in %s", res_dir->c_str()); return true; }
    QLOG("copying the game resources to %s", res_dir->c_str());
    std::istringstream in(index);
    std::string line;
    size_t files = 0, bytes = 0;
    while (std::getline(in, line)) {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string rel = line.substr(sp + 1);
        if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) continue;
        if (!copy_asset(am, "res/" + rel, *res_dir + "/" + rel)) { *err = "cannot copy res/" + rel; return false; }
        files++;
        bytes += std::strtoul(line.c_str(), nullptr, 10);
    }
    FILE *f = std::fopen(stamp.c_str(), "wb");
    if (!f || std::fwrite(index.data(), 1, index.size(), f) != index.size()) { if (f) std::fclose(f); *err = "cannot write " + stamp; return false; }
    std::fclose(f);
    QLOG("copied %zu files (%.1f MB)", files, bytes / 1048576.0);
    return true;
}
}
