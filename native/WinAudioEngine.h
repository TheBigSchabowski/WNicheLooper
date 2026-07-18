#pragma once

#include "miniaudio.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AsioBackend.h"
#include "LooperEngine.h"
#include "VstPluginChain.h"

// Mirrored in Kotlin: audio/AudioEngine.kt (BACKEND_WASAPI / BACKEND_ASIO).
enum class AudioBackendType : int32_t {
    Wasapi = 0,
    Asio = 1,
};

/**
 * Windows counterpart of MacAudioEngine: the same realtime pipeline
 * (mono downmix → active VST3 chain → looper → monitor mix → limiter)
 * fed by one of two backends:
 *
 *  - WASAPI (default): full-duplex via miniaudio, separate input/output
 *    device pick — works everywhere, shared-mode latency.
 *  - ASIO: one driver for input AND output via AsioBackend — the low-latency
 *    path for guitar through NAM & friends. In ASIO mode both name lists
 *    return the driver list and start() uses only inputIndex.
 *
 * The audio callback is allocation-free, lock-free and log-free. All buffers
 * are sized in start(). Control values cross threads via atomics only.
 */
class WinAudioEngine : public AsioClient {
public:
    static constexpr int32_t kMaxLoopSeconds = 120;

    // The engine is a static global in the JNI bridge: stop() uninits the
    // backend synchronously, so the callback is provably finished before any
    // member dies at process exit.
    ~WinAudioEngine() override { stop(); }

    void setBackendType(AudioBackendType type);
    AudioBackendType backendType() const {
        return static_cast<AudioBackendType>(mBackendType.load(std::memory_order_relaxed));
    }

    // App thread: re-enumerates devices (WASAPI) or drivers (ASIO). Returns
    // false if the audio context could not be created.
    bool refreshDevices();

    std::vector<std::string> inputDeviceNames();
    std::vector<std::string> outputDeviceNames();
    int32_t defaultInputIndex();
    int32_t defaultOutputIndex();

    // App thread. Opens + starts the backend on the given indices (into the
    // last enumeration; < 0 = default). ASIO: inputIndex = driver index,
    // outputIndex ignored. Returns false (and leaves everything closed) on
    // any failure.
    bool start(int32_t inputIndex, int32_t outputIndex);

    // App thread. Stops and closes the backend. Safe to call repeatedly.
    void stop();

    // App thread; ASIO only (no-op otherwise, needs a running driver).
    void openAsioControlPanel();

    bool isRunning() const { return mRunning.load(std::memory_order_relaxed); }
    bool isDisconnected() const { return mDisconnected.load(std::memory_order_relaxed); }

    LooperEngine& looper() { return mLooper; }
    PluginChainManager& plugins() { return mPlugins; }

    // App thread: snapshot the current loop (mono samples). Serialized with
    // start()/stop() so the buffer cannot be reallocated mid-copy.
    int32_t copyLoop(float* dest, int32_t maxSamples) {
        std::lock_guard<std::mutex> lock(mLock);
        return mLooper.copyLoop(dest, maxSamples);
    }

    // App thread: replace the loop with mono samples and start playback.
    bool loadLoop(const float* data, int32_t numSamples) {
        std::lock_guard<std::mutex> lock(mLock);
        if (!isRunning()) {
            return false;
        }
        return mLooper.loadLoop(data, numSamples);
    }

    // App thread: stage real drum one-shots. Call before start(); while the
    // engine is running they only take effect on the next (re)start.
    void setDrumSamples(std::vector<float> kick, std::vector<float> snare,
                        std::vector<float> hat, int32_t sourceRate) {
        std::lock_guard<std::mutex> lock(mLock);
        mLooper.rhythm().setDrumSamples(std::move(kick), std::move(snare),
                                        std::move(hat), sourceRate);
    }

    void setMonitorEnabled(bool enabled) { mMonitorEnabled.store(enabled, std::memory_order_relaxed); }
    void setInputGain(float gain) { mInputGain.store(gain, std::memory_order_relaxed); }
    void setOutputGain(float gain) { mOutputGain.store(gain, std::memory_order_relaxed); }

    int32_t sampleRate() const { return mSampleRate; }
    int32_t framesPerBurst() const { return mFramesPerBurst; }

    // Peak meters: max |sample| since the last read (read resets to 0).
    float readInputPeak() { return mInputPeak.exchange(0.0f, std::memory_order_relaxed); }
    float readFxPeak() { return mFxPeak.exchange(0.0f, std::memory_order_relaxed); }
    float readOutputPeak() { return mOutputPeak.exchange(0.0f, std::memory_order_relaxed); }

    // ---- AsioClient (driver audio thread) ----
    void onAsioAudio(float* interleavedOutput, const float* interleavedInput,
                     int32_t numFrames) override;
    void onAsioStopped() override;

private:
    static void dataTrampoline(ma_device* device, void* output,
                               const void* input, ma_uint32 frameCount);
    static void notificationTrampoline(const ma_device_notification* notification);

    void onAudio(float* output, const float* input, int32_t numFrames);
    void processChunk(float* output, const float* input, int32_t numFrames);

    bool ensureContextLocked();
    bool startWasapiLocked(int32_t inputIndex, int32_t outputIndex);
    bool startAsioLocked(int32_t driverIndex);
    void stopLocked();

    std::mutex mLock;  // guards start/stop/enumeration from app threads (never the callback)

    ma_context mContext{};
    bool mContextReady = false;

    // Last enumeration snapshot (guarded by mLock). In ASIO mode the input
    // list carries the driver names and the output list mirrors it.
    std::vector<ma_device_id> mInputIds;
    std::vector<ma_device_id> mOutputIds;
    std::vector<std::string> mInputNames;
    std::vector<std::string> mOutputNames;
    int32_t mDefaultInput = -1;
    int32_t mDefaultOutput = -1;

    std::unique_ptr<ma_device> mDevice;
    AsioBackend mAsio;

    LooperEngine mLooper;
    PluginChainManager mPlugins;

    // Pre-allocated conversion buffers (sized in start()).
    std::vector<float> mMonoIn;
    std::vector<float> mMonoOut;
    int32_t mMaxBlockFrames = 0;

    int32_t mSampleRate = 0;
    int32_t mFramesPerBurst = 0;
    int32_t mInputChannels = 0;
    int32_t mOutputChannels = 0;

    std::atomic<int32_t> mBackendType{static_cast<int32_t>(AudioBackendType::Wasapi)};
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mDisconnected{false};
    std::atomic<bool> mMonitorEnabled{true};
    std::atomic<float> mInputGain{1.0f};
    std::atomic<float> mOutputGain{1.0f};
    std::atomic<float> mInputPeak{0.0f};
    std::atomic<float> mFxPeak{0.0f};
    std::atomic<float> mOutputPeak{0.0f};
};
