#include "quest_log.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
namespace quest {
void redirect_stdio_to_logcat() {
    static bool done = false;
    if (done) return;
    done = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([rd = fds[0]]() {
        char buf[1024];
        std::string line;
        for (;;) {
            const ssize_t n = read(rd, buf, sizeof buf);
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n') { __android_log_write(ANDROID_LOG_INFO, "Diorama64-out", line.c_str()); line.clear(); }
                else line += buf[i];
                if (line.size() > 4000) { __android_log_write(ANDROID_LOG_INFO, "Diorama64-out", line.c_str()); line.clear(); }
            }
        }
    }).detach();
}
}
