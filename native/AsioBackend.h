#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * Receives audio from the ASIO driver thread. Buffers are interleaved
 * float32 at the channel counts reported by the backend after start().
 */
class AsioClient {
public:
    virtual ~AsioClient() = default;

    // Driver audio thread. input may be null when the driver has no inputs.
    virtual void onAsioAudio(float* interleavedOutput, const float* interleavedInput,
                             int32_t numFrames) = 0;

    // Driver requested a reset (device change, sample-rate change, panel
    // apply). The stream is dead; the app layer stops + restarts the engine.
    virtual void onAsioStopped() = 0;
};

/**
 * Minimal ASIO host against the Steinberg ASIO SDK (third_party/asiosdk,
 * GPLv3 option). Drivers are enumerated from HKLM\SOFTWARE\ASIO; one driver
 * carries input AND output (unlike WASAPI there is no separate in/out pick).
 *
 * COM/threading: ASIO drivers are STA COM objects that expect creation,
 * init, start, stop and disposal to happen on ONE thread. All driver calls
 * are therefore marshalled to an internal control thread; the public methods
 * block until the control thread has executed them. Audio callbacks arrive
 * on the driver's own realtime thread and are forwarded to the AsioClient
 * after sample-type conversion (Int16/Int24/Int32/Float32/Float64 LSB).
 *
 * Up to 2 input and 2 output channels are used (guitar interfaces); extra
 * driver channels stay untouched/zeroed.
 */
class AsioBackend {
public:
    static constexpr int32_t kMaxChannels = 2;

    AsioBackend();
    ~AsioBackend();

    AsioBackend(const AsioBackend&) = delete;
    AsioBackend& operator=(const AsioBackend&) = delete;

    // App thread; registry only, loads no driver.
    static std::vector<std::string> driverNames();

    // App thread. Blocks until the driver runs (or failed). The client must
    // outlive the backend or the next stop().
    bool start(int32_t driverIndex, int32_t preferredSampleRate, AsioClient* client);

    // App thread. Blocks until the driver is stopped + disposed. Safe to
    // call repeatedly.
    void stop();

    // App thread. Opens the driver's own settings panel (running driver only).
    void openControlPanel();

    bool isRunning() const { return mRunning.load(std::memory_order_relaxed); }
    int32_t sampleRate() const { return mSampleRate; }
    int32_t bufferFrames() const { return mBufferFrames; }
    int32_t inputChannels() const { return mNumInputs; }
    int32_t outputChannels() const { return mNumOutputs; }

private:
    struct Impl;
    friend struct Impl;

    void controlThreadMain();
    // Runs [job] on the control thread and waits for completion.
    void runOnControlThread(std::function<void()> job);

    bool startOnControlThread(int32_t driverIndex, int32_t preferredSampleRate);
    void stopOnControlThread();

    // Driver audio thread.
    void onBufferSwitch(long bufferIndex);
    void requestReset();

    Impl* mImpl = nullptr;  // driver handles + buffer infos (Windows types)
    AsioClient* mClient = nullptr;

    std::thread mControlThread;
    std::mutex mJobLock;
    std::condition_variable mJobSignal;
    std::deque<std::function<void()>> mJobs;
    bool mShutdown = false;

    std::vector<float> mInterleavedIn;
    std::vector<float> mInterleavedOut;

    int32_t mSampleRate = 0;
    int32_t mBufferFrames = 0;
    int32_t mNumInputs = 0;
    int32_t mNumOutputs = 0;

    std::atomic<bool> mRunning{false};
};
