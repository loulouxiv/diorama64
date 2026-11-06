#include "quest_audio.h"
#include <aaudio/AAudio.h>
#include <cstring>
#include "quest_log.h"
namespace quest {
namespace {
aaudio_data_callback_result_t cb(AAudioStream *, void *user, void *data, int32_t frames) {
    static_cast<AudioOut *>(user)->on_data(data, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
}
int32_t AudioOut::on_data(void *data, int32_t frames) {
    auto *out = static_cast<int16_t *>(data);
    const uint32_t got = pull_ ? pull_(out, (uint32_t)frames) : 0;
    if (got < (uint32_t)frames) {
        std::memset(out + 2 * got, 0, (size_t)(frames - got) * 2 * sizeof(int16_t));
        if (got > 0) underruns_++;
    }
    played_ += (uint64_t)frames;
    return frames;
}
bool AudioOut::start(uint32_t rate_hz, Pull pull) {
    pull_ = std::move(pull);
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { QERR("AAudio: no stream builder"); return false; }
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)rate_hz);   // the game's 32 kHz: AAudio converts to the device rate
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, cb, this);
    AAudioStream *s = nullptr;
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { QERR("AAudio: cannot open the stream: %s", AAudio_convertResultToText(r)); return false; }
    stream_ = s;
    rate_ = AAudioStream_getSampleRate(s);
    QLOG("audio: AAudio %d Hz, %d channels, burst %d frames", rate_, AAudioStream_getChannelCount(s), AAudioStream_getFramesPerBurst(s));
    if (rate_ != (int32_t)rate_hz) QERR("audio: the stream runs at %d Hz, not %u (pitch will be off until phase 7's resampler)", rate_, rate_hz);
    return AAudioStream_requestStart(s) == AAUDIO_OK;
}
void AudioOut::pause() { if (stream_) AAudioStream_requestPause((AAudioStream *)stream_); }
void AudioOut::resume() { if (stream_) AAudioStream_requestStart((AAudioStream *)stream_); }
void AudioOut::stop() {
    if (!stream_) return;
    AAudioStream_requestStop((AAudioStream *)stream_);
    AAudioStream_close((AAudioStream *)stream_);
    stream_ = nullptr;
}
}
