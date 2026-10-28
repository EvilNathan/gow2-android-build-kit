// AAudio output: PCM s16, stereo, low latency, 768-frame buffer.
#include "ps2x_host.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace ps2x::host {
namespace {

using FillFn = void (*)(void* dst, unsigned frames);

constexpr int kChannelCount = 2;
constexpr int32_t kBufferCapacityFrames = 768;
constexpr int kDefaultSampleRate = 48000;

std::mutex g_audioMutex;
AAudioStream* g_stream = nullptr;
bool g_broken = false;
int g_sampleRate = kDefaultSampleRate;

std::atomic<FillFn> g_fill{nullptr};
std::atomic<uint64_t> g_framesWritten{0}; // device-consumed frames, monotonic
std::atomic<uint64_t> g_callbackCount{0};

aaudio_data_callback_result_t OnAudioData(AAudioStream* /*stream*/, void* /*userData*/, void* audioData, int32_t numFrames)
{
    if (audioData == nullptr || numFrames <= 0)
        return AAUDIO_CALLBACK_RESULT_CONTINUE;

    const auto samples = static_cast<size_t>(numFrames) * kChannelCount;
    auto* out = static_cast<int16_t*>(audioData);
    const FillFn fill = g_fill.load(std::memory_order_acquire);

    if (fill == nullptr || IsPaused())
        std::memset(out, 0, samples * sizeof(int16_t));
    else
        fill(audioData, static_cast<unsigned>(numFrames));

    // Frames handed to the device are the best cheap approximation of playback
    // position; AudioFramesPlayed() refines it with AAudio timestamps.
    g_framesWritten.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);
    g_callbackCount.fetch_add(1, std::memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void OnAudioError(AAudioStream* /*stream*/, void* /*userData*/, aaudio_result_t error)
{
    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_broken = true;
    }
    Log("E/host/audio", "AAudio error: %s", AAudio_convertResultToText(error));
}

void CloseStreamLocked()
{
    if (g_stream == nullptr)
        return;
    AAudioStream_requestStop(g_stream);
    AAudioStream_close(g_stream);
    g_stream = nullptr;
    g_broken = false;
}

} // namespace

bool StartAudio(int sampleRate, void (*fill)(void* dst, unsigned frames))
{
    std::lock_guard<std::mutex> lock(g_audioMutex);

    if (sampleRate <= 0)
        sampleRate = kDefaultSampleRate;

    // Rebuild on every start: the previous stream may be disconnected or gone.
    CloseStreamLocked();
    g_fill.store(fill, std::memory_order_release);
    g_sampleRate = sampleRate;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == nullptr) {
        Log("E/host/audio", "AAudio_createStreamBuilder failed: %s", AAudio_convertResultToText(result));
        g_fill.store(nullptr, std::memory_order_release);
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, kChannelCount);
    AAudioStreamBuilder_setSampleRate(builder, sampleRate);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, kBufferCapacityFrames);
    AAudioStreamBuilder_setDataCallback(builder, &OnAudioData, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, &OnAudioError, nullptr);

    AAudioStream* stream = nullptr;
    result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream == nullptr) {
        Log("E/host/audio", "AAudioStreamBuilder_openStream failed: %s (rate=%d)", AAudio_convertResultToText(result), sampleRate);
        g_fill.store(nullptr, std::memory_order_release);
        return false;
    }

    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) {
        Log("E/host/audio", "AAudioStream_requestStart failed: %s", AAudio_convertResultToText(result));
        AAudioStream_close(stream);
        g_fill.store(nullptr, std::memory_order_release);
        return false;
    }

    g_stream = stream;
    g_broken = false;

    Log("host/audio", "AAudio started: rate=%d actual=%d channels=%d cap=%d", sampleRate,
        AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream),
        AAudioStream_getBufferCapacityInFrames(stream));
    return true;
}

void StopAudio()
{
    {
        std::lock_guard<std::mutex> lock(g_audioMutex);
        g_fill.store(nullptr, std::memory_order_release);
        if (g_stream != nullptr)
            Log("host/audio", "AAudio stopping after %llu frames", static_cast<unsigned long long>(g_framesWritten.load()));
        CloseStreamLocked();
    }
}

bool IsAudioReady()
{
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_stream == nullptr || g_broken)
        return false;
    return AAudioStream_getState(g_stream) == AAUDIO_STREAM_STATE_STARTED;
}

uint64_t AudioFramesPlayed()
{
    std::lock_guard<std::mutex> lock(g_audioMutex);
    uint64_t best = g_framesWritten.load(std::memory_order_relaxed);
    if (g_stream != nullptr) {
        int64_t framePosition = 0;
        int64_t timeNanos = 0;
        if (AAudioStream_getTimestamp(g_stream, CLOCK_MONOTONIC, &framePosition, &timeNanos) == AAUDIO_OK && framePosition > 0) {
            const auto presented = static_cast<uint64_t>(framePosition);
            if (presented > best)
                best = presented;
        }
    }
    return best;
}

} // namespace ps2x::host
