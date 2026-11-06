// apps/quest/native — stereo output through AAudio (low latency), fed by the runtime's audio ring. The spatial mix comes
// in phase 7; this is the plain stereo bed, so the game is audible from the first build.
#pragma once
#include <cstdint>
#include <functional>
namespace quest {
class AudioOut {
public:
    using Pull = std::function<uint32_t(int16_t *stereo, uint32_t frames)>;   // the frames it wrote (silence for the rest)
    bool start(uint32_t rate_hz, Pull pull);
    void pause();
    void resume();
    void stop();
    uint64_t frames_played() const { return played_; }
    uint64_t underruns() const { return underruns_; }
    int32_t actual_rate() const { return rate_; }
    ~AudioOut() { stop(); }
    // internal
    int32_t on_data(void *data, int32_t frames);
private:
    void *stream_ = nullptr;   // AAudioStream
    Pull pull_;
    uint64_t played_ = 0, underruns_ = 0;
    int32_t rate_ = 0;
};
}
