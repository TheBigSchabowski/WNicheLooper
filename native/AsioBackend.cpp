#include "AsioBackend.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "iasiodrv.h"

// One backend can run at a time (ASIO callbacks carry no user pointer). The
// engine owns exactly one AsioBackend, so this is not a real restriction.
namespace {
AsioBackend* gActive = nullptr;

struct DriverEntry {
    std::string name;
    std::string clsid;
};

// HKLM\SOFTWARE\ASIO\<subkey>: CLSID (+ optional Description). Order is the
// registry enumeration order; driver indices refer to this list.
std::vector<DriverEntry> enumerateDrivers() {
    std::vector<DriverEntry> drivers;
    HKEY asioKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ,
                      &asioKey) != ERROR_SUCCESS) {
        return drivers;
    }
    for (DWORD i = 0;; ++i) {
        char subName[256];
        DWORD subLen = sizeof(subName);
        if (RegEnumKeyExA(asioKey, i, subName, &subLen, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        HKEY driverKey = nullptr;
        if (RegOpenKeyExA(asioKey, subName, 0, KEY_READ, &driverKey) != ERROR_SUCCESS) {
            continue;
        }
        char clsid[256] = {0};
        DWORD clsidLen = sizeof(clsid);
        DWORD type = 0;
        if (RegQueryValueExA(driverKey, "CLSID", nullptr, &type,
                             reinterpret_cast<LPBYTE>(clsid), &clsidLen) == ERROR_SUCCESS &&
            type == REG_SZ) {
            char description[256] = {0};
            DWORD descLen = sizeof(description);
            if (RegQueryValueExA(driverKey, "Description", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(description),
                                 &descLen) != ERROR_SUCCESS ||
                type != REG_SZ || description[0] == '\0') {
                std::snprintf(description, sizeof(description), "%s", subName);
            }
            drivers.push_back(DriverEntry{description, clsid});
        }
        RegCloseKey(driverKey);
    }
    RegCloseKey(asioKey);
    return drivers;
}

// ---- Sample conversion (driver format <-> float, per channel) -------------

void toFloat(ASIOSampleType type, const void* src, float* dest, int32_t frames,
             int32_t destStride) {
    switch (type) {
        case ASIOSTInt16LSB: {
            const int16_t* s = static_cast<const int16_t*>(src);
            for (int32_t i = 0; i < frames; ++i) {
                dest[i * destStride] = static_cast<float>(s[i]) / 32768.0f;
            }
            break;
        }
        case ASIOSTInt24LSB: {
            const uint8_t* s = static_cast<const uint8_t*>(src);
            for (int32_t i = 0; i < frames; ++i) {
                const int32_t v = static_cast<int32_t>(
                    (static_cast<uint32_t>(s[i * 3]) << 8) |
                    (static_cast<uint32_t>(s[i * 3 + 1]) << 16) |
                    (static_cast<uint32_t>(s[i * 3 + 2]) << 24));
                dest[i * destStride] = static_cast<float>(v >> 8) / 8388608.0f;
            }
            break;
        }
        case ASIOSTInt32LSB: {
            const int32_t* s = static_cast<const int32_t*>(src);
            for (int32_t i = 0; i < frames; ++i) {
                dest[i * destStride] = static_cast<float>(s[i]) / 2147483648.0f;
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            const float* s = static_cast<const float*>(src);
            for (int32_t i = 0; i < frames; ++i) {
                dest[i * destStride] = s[i];
            }
            break;
        }
        case ASIOSTFloat64LSB: {
            const double* s = static_cast<const double*>(src);
            for (int32_t i = 0; i < frames; ++i) {
                dest[i * destStride] = static_cast<float>(s[i]);
            }
            break;
        }
        default:
            for (int32_t i = 0; i < frames; ++i) {
                dest[i * destStride] = 0.0f;
            }
            break;
    }
}

void fromFloat(ASIOSampleType type, const float* src, int32_t srcStride,
               void* dest, int32_t frames) {
    switch (type) {
        case ASIOSTInt16LSB: {
            int16_t* d = static_cast<int16_t*>(dest);
            for (int32_t i = 0; i < frames; ++i) {
                const float v = std::clamp(src[i * srcStride], -1.0f, 1.0f);
                d[i] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
            }
            break;
        }
        case ASIOSTInt24LSB: {
            uint8_t* d = static_cast<uint8_t*>(dest);
            for (int32_t i = 0; i < frames; ++i) {
                const float v = std::clamp(src[i * srcStride], -1.0f, 1.0f);
                const int32_t s = static_cast<int32_t>(std::lrintf(v * 8388607.0f));
                d[i * 3] = static_cast<uint8_t>(s & 0xFF);
                d[i * 3 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
                d[i * 3 + 2] = static_cast<uint8_t>((s >> 16) & 0xFF);
            }
            break;
        }
        case ASIOSTInt32LSB: {
            int32_t* d = static_cast<int32_t*>(dest);
            for (int32_t i = 0; i < frames; ++i) {
                const double v = std::clamp(static_cast<double>(src[i * srcStride]), -1.0, 1.0);
                d[i] = static_cast<int32_t>(std::llrint(v * 2147483647.0));
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            float* d = static_cast<float*>(dest);
            for (int32_t i = 0; i < frames; ++i) {
                d[i] = src[i * srcStride];
            }
            break;
        }
        case ASIOSTFloat64LSB: {
            double* d = static_cast<double*>(dest);
            for (int32_t i = 0; i < frames; ++i) {
                d[i] = static_cast<double>(src[i * srcStride]);
            }
            break;
        }
        default:
            break;
    }
}

size_t sampleSizeBytes(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:
            return 2;
        case ASIOSTInt24LSB:
            return 3;
        case ASIOSTFloat64LSB:
            return 8;
        default:
            return 4;
    }
}

bool isSupportedType(ASIOSampleType type) {
    switch (type) {
        case ASIOSTInt16LSB:
        case ASIOSTInt24LSB:
        case ASIOSTInt32LSB:
        case ASIOSTFloat32LSB:
        case ASIOSTFloat64LSB:
            return true;
        default:
            return false;
    }
}

// ---- ASIO callback trampolines (no user pointer -> gActive) ---------------

void cbBufferSwitch(long index, ASIOBool /*directProcess*/) {
    if (gActive != nullptr) {
        gActive->onBufferSwitch(index);
    }
}

ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* /*params*/, long index,
                                 ASIOBool /*directProcess*/) {
    if (gActive != nullptr) {
        gActive->onBufferSwitch(index);
    }
    return nullptr;
}

void cbSampleRateDidChange(ASIOSampleRate /*rate*/) {
    if (gActive != nullptr) {
        gActive->requestReset();
    }
}

long cbAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case kAsioSelectorSupported:
            return (value == kAsioEngineVersion || value == kAsioResetRequest ||
                    value == kAsioResyncRequest)
                       ? 1
                       : 0;
        case kAsioEngineVersion:
            return 2;
        case kAsioResetRequest:
        case kAsioResyncRequest:
            if (gActive != nullptr) {
                gActive->requestReset();
            }
            return 1;
        default:
            return 0;
    }
}

}  // namespace

struct AsioBackend::Impl {
    IASIO* driver = nullptr;
    bool buffersCreated = false;
    bool started = false;
    bool comInitialized = false;
    bool supportsOutputReady = false;
    ASIOCallbacks callbacks{};
    std::vector<ASIOBufferInfo> bufferInfos;   // inputs first, then outputs
    std::vector<ASIOSampleType> inputTypes;
    std::vector<ASIOSampleType> outputTypes;
};

AsioBackend::AsioBackend() : mImpl(new Impl()) {
    mControlThread = std::thread([this] { controlThreadMain(); });
}

AsioBackend::~AsioBackend() {
    stop();
    {
        std::lock_guard<std::mutex> lock(mJobLock);
        mShutdown = true;
    }
    mJobSignal.notify_all();
    if (mControlThread.joinable()) {
        mControlThread.join();
    }
    delete mImpl;
}

std::vector<std::string> AsioBackend::driverNames() {
    std::vector<std::string> names;
    for (const auto& driver : enumerateDrivers()) {
        names.push_back(driver.name);
    }
    return names;
}

void AsioBackend::controlThreadMain() {
    // STA apartment for the driver COM object; every driver call happens on
    // this thread (see class comment).
    mImpl->comInitialized = SUCCEEDED(CoInitialize(nullptr));
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mJobLock);
            mJobSignal.wait(lock, [this] { return mShutdown || !mJobs.empty(); });
            if (mJobs.empty() && mShutdown) {
                break;
            }
            job = std::move(mJobs.front());
            mJobs.pop_front();
        }
        job();
    }
    if (mImpl->comInitialized) {
        CoUninitialize();
    }
}

void AsioBackend::runOnControlThread(std::function<void()> job) {
    std::mutex doneLock;
    std::condition_variable doneSignal;
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(mJobLock);
        if (mShutdown) {
            return;
        }
        mJobs.push_back([&, job = std::move(job)] {
            job();
            {
                std::lock_guard<std::mutex> dl(doneLock);
                done = true;
            }
            doneSignal.notify_all();
        });
    }
    mJobSignal.notify_all();
    std::unique_lock<std::mutex> lock(doneLock);
    doneSignal.wait(lock, [&] { return done; });
}

bool AsioBackend::start(int32_t driverIndex, int32_t preferredSampleRate,
                        AsioClient* client) {
    stop();
    mClient = client;
    bool ok = false;
    runOnControlThread([&] { ok = startOnControlThread(driverIndex, preferredSampleRate); });
    if (!ok) {
        mClient = nullptr;
    }
    return ok;
}

void AsioBackend::stop() {
    runOnControlThread([&] { stopOnControlThread(); });
    mClient = nullptr;
}

void AsioBackend::openControlPanel() {
    runOnControlThread([&] {
        if (mImpl->driver != nullptr) {
            mImpl->driver->controlPanel();
        }
    });
}

bool AsioBackend::startOnControlThread(int32_t driverIndex, int32_t preferredSampleRate) {
    stopOnControlThread();

    const auto drivers = enumerateDrivers();
    if (driverIndex < 0 || driverIndex >= static_cast<int32_t>(drivers.size())) {
        std::fprintf(stderr, "NicheLooper: ASIO driver index %d out of range (%zu drivers)\n",
                     driverIndex, drivers.size());
        return false;
    }

    CLSID clsid{};
    std::wstring wideClsid(drivers[driverIndex].clsid.begin(),
                           drivers[driverIndex].clsid.end());
    if (FAILED(CLSIDFromString(wideClsid.c_str(), &clsid))) {
        std::fprintf(stderr, "NicheLooper: bad ASIO CLSID for %s\n",
                     drivers[driverIndex].name.c_str());
        return false;
    }

    // ASIO convention: the class ID doubles as the interface ID.
    IASIO* driver = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                  reinterpret_cast<void**>(&driver));
    if (FAILED(hr) || driver == nullptr) {
        std::fprintf(stderr, "NicheLooper: CoCreateInstance failed for ASIO %s (0x%08lx)\n",
                     drivers[driverIndex].name.c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    mImpl->driver = driver;

    if (driver->init(GetDesktopWindow()) != ASIOTrue) {
        char message[256] = {0};
        driver->getErrorMessage(message);
        std::fprintf(stderr, "NicheLooper: ASIO init failed: %s\n", message);
        stopOnControlThread();
        return false;
    }

    long numInputs = 0;
    long numOutputs = 0;
    if (driver->getChannels(&numInputs, &numOutputs) != ASE_OK || numOutputs < 1) {
        std::fprintf(stderr, "NicheLooper: ASIO getChannels failed (in=%ld out=%ld)\n",
                     numInputs, numOutputs);
        stopOnControlThread();
        return false;
    }
    mNumInputs = std::min<int32_t>(static_cast<int32_t>(numInputs), kMaxChannels);
    mNumOutputs = std::min<int32_t>(static_cast<int32_t>(numOutputs), kMaxChannels);

    ASIOSampleRate currentRate = 0;
    const bool haveCurrentRate =
        driver->getSampleRate(&currentRate) == ASE_OK && currentRate > 0;

    // Only poke the driver if it isn't already running at the preferred rate; a
    // rejected change (e.g. Windows shared mode holding the device at 44.1 kHz)
    // is not fatal, we fall back to whatever rate the driver reports.
    if (preferredSampleRate > 0 &&
        (!haveCurrentRate || static_cast<int32_t>(currentRate) != preferredSampleRate) &&
        driver->canSampleRate(static_cast<ASIOSampleRate>(preferredSampleRate)) == ASE_OK) {
        const ASIOError setErr =
            driver->setSampleRate(static_cast<ASIOSampleRate>(preferredSampleRate));
        if (setErr != ASE_OK) {
            std::fprintf(stderr,
                         "NicheLooper: ASIO setSampleRate(%d) rejected (err %ld), "
                         "falling back to current rate\n",
                         preferredSampleRate, setErr);
        }
    }

    ASIOSampleRate rate = 0;
    if (driver->getSampleRate(&rate) != ASE_OK || rate <= 0) {
        if (!haveCurrentRate) {
            std::fprintf(stderr, "NicheLooper: ASIO getSampleRate failed\n");
            stopOnControlThread();
            return false;
        }
        rate = currentRate;
    }
    mSampleRate = static_cast<int32_t>(rate);

    long minSize = 0;
    long maxSize = 0;
    long preferredSize = 0;
    long granularity = 0;
    if (driver->getBufferSize(&minSize, &maxSize, &preferredSize, &granularity) != ASE_OK ||
        preferredSize <= 0) {
        std::fprintf(stderr, "NicheLooper: ASIO getBufferSize failed\n");
        stopOnControlThread();
        return false;
    }
    mBufferFrames = static_cast<int32_t>(preferredSize);

    mImpl->bufferInfos.clear();
    for (int32_t c = 0; c < mNumInputs; ++c) {
        ASIOBufferInfo info{};
        info.isInput = ASIOTrue;
        info.channelNum = c;
        mImpl->bufferInfos.push_back(info);
    }
    for (int32_t c = 0; c < mNumOutputs; ++c) {
        ASIOBufferInfo info{};
        info.isInput = ASIOFalse;
        info.channelNum = c;
        mImpl->bufferInfos.push_back(info);
    }

    mImpl->callbacks.bufferSwitch = &cbBufferSwitch;
    mImpl->callbacks.sampleRateDidChange = &cbSampleRateDidChange;
    mImpl->callbacks.asioMessage = &cbAsioMessage;
    mImpl->callbacks.bufferSwitchTimeInfo = &cbBufferSwitchTimeInfo;

    gActive = this;
    if (driver->createBuffers(mImpl->bufferInfos.data(),
                              static_cast<long>(mImpl->bufferInfos.size()),
                              preferredSize, &mImpl->callbacks) != ASE_OK) {
        std::fprintf(stderr, "NicheLooper: ASIO createBuffers failed (%ld frames)\n",
                     preferredSize);
        stopOnControlThread();
        return false;
    }
    mImpl->buffersCreated = true;

    mImpl->inputTypes.clear();
    mImpl->outputTypes.clear();
    for (size_t i = 0; i < mImpl->bufferInfos.size(); ++i) {
        ASIOChannelInfo info{};
        info.channel = mImpl->bufferInfos[i].channelNum;
        info.isInput = mImpl->bufferInfos[i].isInput;
        if (driver->getChannelInfo(&info) != ASE_OK || !isSupportedType(info.type)) {
            std::fprintf(stderr, "NicheLooper: unsupported ASIO sample type %ld on %s %ld\n",
                         info.type, info.isInput == ASIOTrue ? "input" : "output",
                         info.channel);
            stopOnControlThread();
            return false;
        }
        if (mImpl->bufferInfos[i].isInput == ASIOTrue) {
            mImpl->inputTypes.push_back(info.type);
        } else {
            mImpl->outputTypes.push_back(info.type);
        }
    }

    mImpl->supportsOutputReady = driver->outputReady() == ASE_OK;

    mInterleavedIn.assign(static_cast<size_t>(mBufferFrames) *
                              std::max<int32_t>(mNumInputs, 1),
                          0.0f);
    mInterleavedOut.assign(static_cast<size_t>(mBufferFrames) * mNumOutputs, 0.0f);

    if (driver->start() != ASE_OK) {
        std::fprintf(stderr, "NicheLooper: ASIO start failed\n");
        stopOnControlThread();
        return false;
    }
    mImpl->started = true;
    mRunning.store(true, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "NicheLooper: ASIO started (%s, rate=%d, buffer=%d, in=%d, out=%d)\n",
                 drivers[driverIndex].name.c_str(), mSampleRate, mBufferFrames,
                 mNumInputs, mNumOutputs);
    return true;
}

void AsioBackend::stopOnControlThread() {
    mRunning.store(false, std::memory_order_relaxed);
    if (mImpl->driver != nullptr) {
        if (mImpl->started) {
            mImpl->driver->stop();
            mImpl->started = false;
        }
        if (mImpl->buffersCreated) {
            mImpl->driver->disposeBuffers();
            mImpl->buffersCreated = false;
        }
        mImpl->driver->Release();
        mImpl->driver = nullptr;
    }
    gActive = nullptr;
}

void AsioBackend::onBufferSwitch(long bufferIndex) {
    if (!mRunning.load(std::memory_order_relaxed) || mClient == nullptr) {
        // Not (fully) started: write silence so undefined driver buffer
        // content never reaches the speakers.
        for (int32_t c = 0; c < mNumOutputs; ++c) {
            const size_t index = static_cast<size_t>(mNumInputs + c);
            if (index < mImpl->bufferInfos.size() &&
                index - static_cast<size_t>(mNumInputs) < mImpl->outputTypes.size()) {
                void* dest = mImpl->bufferInfos[index].buffers[bufferIndex];
                if (dest != nullptr) {
                    std::memset(dest, 0,
                                static_cast<size_t>(mBufferFrames) *
                                    sampleSizeBytes(mImpl->outputTypes[static_cast<size_t>(c)]));
                }
            }
        }
        return;
    }
    const int32_t frames = mBufferFrames;

    for (int32_t c = 0; c < mNumInputs; ++c) {
        const void* src = mImpl->bufferInfos[static_cast<size_t>(c)].buffers[bufferIndex];
        toFloat(mImpl->inputTypes[static_cast<size_t>(c)], src,
                mInterleavedIn.data() + c, frames, mNumInputs);
    }

    mClient->onAsioAudio(mInterleavedOut.data(),
                         mNumInputs > 0 ? mInterleavedIn.data() : nullptr, frames);

    for (int32_t c = 0; c < mNumOutputs; ++c) {
        void* dest =
            mImpl->bufferInfos[static_cast<size_t>(mNumInputs + c)].buffers[bufferIndex];
        fromFloat(mImpl->outputTypes[static_cast<size_t>(c)],
                  mInterleavedOut.data() + c, mNumOutputs, dest, frames);
    }

    if (mImpl->supportsOutputReady) {
        mImpl->driver->outputReady();
    }
}

void AsioBackend::requestReset() {
    // Called from driver threads; no driver calls allowed here. The client
    // marks itself disconnected, the app layer then stops + restarts.
    if (mClient != nullptr) {
        mClient->onAsioStopped();
    }
}
