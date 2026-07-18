#include "VstPluginChain.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

using Steinberg::FUnknown;
using Steinberg::IPlugFrame;
using Steinberg::IPlugView;
using Steinberg::IPtr;
using Steinberg::kInvalidArgument;
using Steinberg::kNoInterface;
using Steinberg::kResultFalse;
using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::owned;
using Steinberg::tresult;
using Steinberg::TUID;
using Steinberg::uint32;
using Steinberg::ViewRect;
namespace Vst = Steinberg::Vst;
namespace Hosting = VST3::Hosting;

namespace {

// ---- Dedicated plugin UI thread -------------------------------------------
// Plugin editors are Win32 windows; their creation thread must pump their
// messages. AWT threads are off limits, so all editor work runs on this one
// internal thread via posted jobs. Leaked on purpose: it lives until process
// exit, exactly like the editor-window map on macOS.

constexpr UINT kRunJobMessage = WM_APP + 1;

struct UiJob {
    std::function<void()> fn;
    HANDLE doneEvent;  // null for fire-and-forget jobs
};

class PluginUiThread {
public:
    static PluginUiThread& instance() {
        static auto* thread = new PluginUiThread();
        return *thread;
    }

    // Runs [fn] on the UI thread. wait=true blocks until it finished — never
    // call that variant FROM the UI thread (self-deadlock); jobs themselves
    // always run there, so nested run(false, …) is fine.
    void run(bool wait, std::function<void()> fn) {
        WaitForSingleObject(mReady, INFINITE);
        if (GetCurrentThreadId() == mThreadId) {
            fn();
            return;
        }
        auto* job = new UiJob{std::move(fn),
                              wait ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr};
        HANDLE doneEvent = job->doneEvent;
        if (!PostMessageW(mMessageWindow, kRunJobMessage, reinterpret_cast<WPARAM>(job), 0)) {
            if (doneEvent != nullptr) {
                CloseHandle(doneEvent);
            }
            delete job;
            return;
        }
        if (doneEvent != nullptr) {
            WaitForSingleObject(doneEvent, INFINITE);
            CloseHandle(doneEvent);
        }
    }

private:
    PluginUiThread() {
        mReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID param) -> DWORD {
                static_cast<PluginUiThread*>(param)->threadMain();
                return 0;
            },
            this, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }

    static LRESULT CALLBACK messageProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == kRunJobMessage) {
            auto* job = reinterpret_cast<UiJob*>(wParam);
            job->fn();
            if (job->doneEvent != nullptr) {
                SetEvent(job->doneEvent);
            }
            delete job;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void threadMain() {
        mThreadId = GetCurrentThreadId();

        WNDCLASSW messageClass{};
        messageClass.lpfnWndProc = &PluginUiThread::messageProc;
        messageClass.hInstance = GetModuleHandleW(nullptr);
        messageClass.lpszClassName = L"NicheLooperUiJobs";
        RegisterClassW(&messageClass);
        mMessageWindow = CreateWindowExW(0, messageClass.lpszClassName, L"", 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, nullptr, messageClass.hInstance, nullptr);
        SetEvent(mReady);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    HANDLE mReady = nullptr;
    HWND mMessageWindow = nullptr;
    DWORD mThreadId = 0;
};

// ---- Editor windows -------------------------------------------------------

constexpr wchar_t kEditorClassName[] = L"NicheLooperPluginEditor";

class PlugFrame;

struct EditorWindow {
    HWND hwnd = nullptr;
    IPlugView* view = nullptr;   // owns one reference
    PlugFrame* frame = nullptr;  // owned
    bool resizable = false;
};

// Host side of editor resizing: the plugin asks for a new size, we resize
// the frame window; WM_SIZE then feeds the client size back via onSize.
class PlugFrame : public IPlugFrame {
public:
    explicit PlugFrame(HWND hwnd) : mHwnd(hwnd) {}
    virtual ~PlugFrame() = default;

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Owned by its EditorWindow, never by the plugin.
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (view == nullptr || newSize == nullptr || mHwnd == nullptr) {
            return kInvalidArgument;
        }
        RECT rect{0, 0, newSize->getWidth(), newSize->getHeight()};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(mHwnd, GWL_STYLE));
        AdjustWindowRectEx(&rect, style, FALSE, 0);
        SetWindowPos(mHwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        view->onSize(newSize);
        return kResultTrue;
    }

private:
    HWND mHwnd;
};

LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* window = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CLOSE:
            // The close button only hides — the window (and the view inside)
            // lives until the plugin is removed, exactly like on macOS.
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_SIZE:
            if (window != nullptr && window->view != nullptr) {
                RECT client{};
                GetClientRect(hwnd, &client);
                ViewRect rect(0, 0, client.right, client.bottom);
                window->view->onSize(&rect);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void registerEditorClassOnce() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSW editorClass{};
    editorClass.lpfnWndProc = &editorProc;
    editorClass.hInstance = GetModuleHandleW(nullptr);
    editorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    editorClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    editorClass.lpszClassName = kEditorClassName;
    RegisterClassW(&editorClass);
    registered = true;
}

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 1) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    }
    return wide;
}

// ---- Host context ---------------------------------------------------------

Vst::HostApplication& hostApplication() {
    static auto* app = new Vst::HostApplication();
    return *app;
}

void ensurePluginContext() {
    static bool done = false;
    if (!done) {
        Vst::PluginContextFactory::instance().setPluginContext(&hostApplication());
        done = true;
    }
}

// ---- Component handler ----------------------------------------------------
// The editor UI thread reports knob turns here; the audio thread drains them
// into the processor's input parameter changes at the next block.

class ComponentHandler : public Vst::IComponentHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Owned by its VstPlugin, never by the plugin.
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API beginEdit(Vst::ParamID /*id*/) override { return kResultOk; }

    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue valueNormalized) override {
        std::lock_guard<std::mutex> guard(mLock);
        mEdits.emplace_back(id, valueNormalized);
        return kResultOk;
    }

    tresult PLUGIN_API endEdit(Vst::ParamID /*id*/) override { return kResultOk; }

    tresult PLUGIN_API restartComponent(Steinberg::int32 /*flags*/) override {
        // Latency/IO changes are ignored: the chain runs fixed mono/stereo
        // at a fixed block size and re-prepares on every engine start.
        return kResultOk;
    }

    // Audio thread: appends pending edits into [changes] (one point each).
    // try_lock keeps the audio thread wait-free; a lost race just defers the
    // edits to the next block.
    void drainInto(Vst::ParameterChanges& changes) {
        std::unique_lock<std::mutex> guard(mLock, std::try_to_lock);
        if (!guard.owns_lock()) {
            return;
        }
        for (const auto& edit : mEdits) {
            Steinberg::int32 index = 0;
            if (Vst::IParamValueQueue* queue = changes.addParameterData(edit.first, index)) {
                Steinberg::int32 pointIndex = 0;
                queue->addPoint(0, edit.second, pointIndex);
            }
        }
        mEdits.clear();
    }

private:
    std::mutex mLock;
    std::vector<std::pair<Vst::ParamID, Vst::ParamValue>> mEdits;
};

// ---- One hosted VST3 ------------------------------------------------------

class VstPlugin {
public:
    VstPlugin(Hosting::Module::Ptr module, Hosting::ClassInfo classInfo, std::string name)
        : mModule(std::move(module)), mClassInfo(std::move(classInfo)), mName(std::move(name)) {}

    ~VstPlugin() {
        closeEditor();
        if (mProcessing && mProcessor != nullptr) {
            mProcessor->setProcessing(false);
            mProcessing = false;
        }
        if (mActive && mComponent != nullptr) {
            mComponent->setActive(false);
            mActive = false;
        }
        mProcessData.unprepare();
        if (mController != nullptr) {
            mController->setComponentHandler(nullptr);
        }
        mProcessor = nullptr;
        mComponent = nullptr;
        mController = nullptr;
        mProvider = nullptr;  // terminates + disconnects component/controller
        mModule = nullptr;
    }

    const std::string& name() const { return mName; }
    const std::string& modulePath() const { return mModule != nullptr ? mModule->getPath() : mEmptyPath; }
    const VST3::UID& uid() const { return mClassInfo.ID(); }

    bool instantiate() {
        ensurePluginContext();
        mProvider = owned(new Vst::PlugProvider(mModule->getFactory(), mClassInfo, true));
        if (!mProvider->initialize()) {
            std::fprintf(stderr, "NicheLooper: PlugProvider init failed for %s\n", mName.c_str());
            mProvider = nullptr;
            return false;
        }
        mComponent = mProvider->getComponentPtr();
        mController = mProvider->getControllerPtr();
        if (mComponent == nullptr) {
            return false;
        }
        mProcessor = Steinberg::FUnknownPtr<Vst::IAudioProcessor>(mComponent);
        if (mProcessor == nullptr) {
            std::fprintf(stderr, "NicheLooper: %s has no IAudioProcessor\n", mName.c_str());
            return false;
        }
        if (mController != nullptr) {
            mController->setComponentHandler(&mHandler);
            // Mirror the component state into the controller so the editor
            // opens showing the real values.
            Steinberg::MemoryStream stream;
            if (mComponent->getState(&stream) == kResultOk) {
                stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                mController->setComponentState(&stream);
            }
        }
        return true;
    }

    // App thread, audio callback not touching this plugin. Tries mono I/O
    // first (NAM & friends), falls back to stereo (dual mono feed).
    bool prepare(int sampleRate, int maxFrames) {
        if (mProcessor == nullptr || mComponent == nullptr) {
            return false;
        }
        if (mProcessing) {
            mProcessor->setProcessing(false);
            mProcessing = false;
        }
        if (mActive) {
            mComponent->setActive(false);
            mActive = false;
        }
        mInitialized = false;

        const Steinberg::int32 numInBuses = mComponent->getBusCount(Vst::kAudio, Vst::kInput);
        const Steinberg::int32 numOutBuses = mComponent->getBusCount(Vst::kAudio, Vst::kOutput);
        if (numInBuses < 1 || numOutBuses < 1) {
            std::fprintf(stderr, "NicheLooper: %s has no main audio bus (in=%d out=%d)\n",
                         mName.c_str(), numInBuses, numOutBuses);
            return false;
        }

        mChannels = 0;
        for (int channels : {1, 2}) {
            const Vst::SpeakerArrangement arr =
                channels == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
            std::vector<Vst::SpeakerArrangement> inArrs(static_cast<size_t>(numInBuses),
                                                        Vst::SpeakerArr::kEmpty);
            std::vector<Vst::SpeakerArrangement> outArrs(static_cast<size_t>(numOutBuses),
                                                         Vst::SpeakerArr::kEmpty);
            inArrs[0] = arr;
            outArrs[0] = arr;
            if (mProcessor->setBusArrangements(inArrs.data(), numInBuses, outArrs.data(),
                                               numOutBuses) == kResultTrue) {
                mChannels = channels;
                break;
            }
        }
        if (mChannels == 0) {
            // Plugin insists on its own layout; accept it if the main output
            // is mono or stereo.
            Vst::SpeakerArrangement actual = 0;
            if (mProcessor->getBusArrangement(Vst::kOutput, 0, actual) == kResultTrue) {
                const int channels = Vst::SpeakerArr::getChannelCount(actual);
                if (channels == 1 || channels == 2) {
                    mChannels = channels;
                }
            }
        }
        if (mChannels == 0) {
            std::fprintf(stderr, "NicheLooper: %s accepts neither mono nor stereo f32\n",
                         mName.c_str());
            return false;
        }

        // Main buses on, aux/sidechain and event buses off.
        for (Steinberg::int32 i = 0; i < numInBuses; ++i) {
            mComponent->activateBus(Vst::kAudio, Vst::kInput, i, i == 0);
        }
        for (Steinberg::int32 i = 0; i < numOutBuses; ++i) {
            mComponent->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0);
        }
        const Steinberg::int32 numEventIn = mComponent->getBusCount(Vst::kEvent, Vst::kInput);
        for (Steinberg::int32 i = 0; i < numEventIn; ++i) {
            mComponent->activateBus(Vst::kEvent, Vst::kInput, i, false);
        }
        const Steinberg::int32 numEventOut = mComponent->getBusCount(Vst::kEvent, Vst::kOutput);
        for (Steinberg::int32 i = 0; i < numEventOut; ++i) {
            mComponent->activateBus(Vst::kEvent, Vst::kOutput, i, false);
        }

        Vst::ProcessSetup setup{};
        setup.processMode = Vst::kRealtime;
        setup.symbolicSampleSize = Vst::kSample32;
        setup.maxSamplesPerBlock = maxFrames;
        setup.sampleRate = static_cast<Vst::SampleRate>(sampleRate);
        if (mProcessor->setupProcessing(setup) != kResultOk) {
            std::fprintf(stderr, "NicheLooper: setupProcessing failed for %s\n", mName.c_str());
            return false;
        }

        // HostProcessData allocates sample memory for every bus; process()
        // just copies mono in/out of bus 0.
        mProcessData.unprepare();
        if (!mProcessData.prepare(*mComponent, maxFrames, Vst::kSample32)) {
            std::fprintf(stderr, "NicheLooper: process buffers failed for %s\n", mName.c_str());
            return false;
        }
        mInputChanges.setMaxParameters(kMaxParameterEdits);
        mOutputChanges.setMaxParameters(kMaxParameterEdits);
        mProcessData.inputParameterChanges = &mInputChanges;
        mProcessData.outputParameterChanges = &mOutputChanges;

        std::memset(&mProcessContext, 0, sizeof(mProcessContext));
        mProcessContext.state = Vst::ProcessContext::kPlaying | Vst::ProcessContext::kTempoValid |
                                Vst::ProcessContext::kTimeSigValid;
        mProcessContext.sampleRate = sampleRate;
        mProcessContext.tempo = 120.0;
        mProcessContext.timeSigNumerator = 4;
        mProcessContext.timeSigDenominator = 4;
        mProcessData.processContext = &mProcessContext;

        mMaxFrames = maxFrames;
        mSampleTime = 0;

        if (mComponent->setActive(true) != kResultOk) {
            std::fprintf(stderr, "NicheLooper: setActive failed for %s\n", mName.c_str());
            return false;
        }
        mActive = true;
        if (mProcessor->setProcessing(true) != kResultOk) {
            // Optional per spec — many effects return kNotImplemented.
        }
        mProcessing = true;
        mInitialized = true;
        return true;
    }

    // Audio thread. In-place on mono; bypasses itself on any process error.
    void process(float* mono, int frames) {
        if (!mInitialized || frames > mMaxFrames || mProcessData.numOutputs < 1 ||
            mProcessData.outputs[0].numChannels < 1) {
            return;
        }
        const size_t bytes = static_cast<size_t>(frames) * sizeof(float);

        mInputChanges.clearQueue();
        mOutputChanges.clearQueue();
        mHandler.drainInto(mInputChanges);

        if (mProcessData.numInputs > 0) {
            Vst::AudioBusBuffers& in = mProcessData.inputs[0];
            for (Steinberg::int32 c = 0; c < in.numChannels && c < 2; ++c) {
                if (in.channelBuffers32[c] != nullptr) {
                    std::memcpy(in.channelBuffers32[c], mono, bytes);
                }
            }
            in.silenceFlags = 0;
        }

        mProcessData.numSamples = frames;
        mProcessContext.projectTimeSamples = mSampleTime;

        const tresult result = mProcessor->process(mProcessData);
        mSampleTime += frames;
        if (result != kResultOk) {
            mLastError.store(static_cast<int32_t>(result), std::memory_order_relaxed);
            if (mErrorCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                std::fprintf(stderr, "NicheLooper: process(%s) failed: %d\n", mName.c_str(),
                             static_cast<int>(result));
            }
            return;  // dry passthrough
        }
        mRenderCount.fetch_add(1, std::memory_order_relaxed);

        Vst::AudioBusBuffers& out = mProcessData.outputs[0];
        if (mChannels >= 2 && out.numChannels >= 2 && out.channelBuffers32[0] != nullptr &&
            out.channelBuffers32[1] != nullptr) {
            const float* left = out.channelBuffers32[0];
            const float* right = out.channelBuffers32[1];
            for (int i = 0; i < frames; ++i) {
                mono[i] = 0.5f * (left[i] + right[i]);
            }
        } else if (out.channelBuffers32[0] != nullptr) {
            std::memcpy(mono, out.channelBuffers32[0], bytes);
        }
    }

    void openEditor() {
        if (mController == nullptr) {
            std::fprintf(stderr, "NicheLooper: %s has no edit controller\n", mName.c_str());
            return;
        }
        PluginUiThread::instance().run(false, [this] {
            if (mEditor != nullptr && mEditor->hwnd != nullptr) {
                ShowWindow(mEditor->hwnd, SW_SHOW);
                SetForegroundWindow(mEditor->hwnd);
                return;
            }
            IPlugView* view = mController->createView(Vst::ViewType::kEditor);
            if (view == nullptr) {
                std::fprintf(stderr, "NicheLooper: %s has no editor view\n", mName.c_str());
                return;
            }

            registerEditorClassOnce();
            ViewRect size{};
            if (view->getSize(&size) != kResultOk || size.getWidth() < 50 ||
                size.getHeight() < 50) {
                size = ViewRect(0, 0, 900, 600);
            }
            const bool resizable = view->canResize() == kResultTrue;
            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            if (resizable) {
                style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
            }
            RECT rect{0, 0, size.getWidth(), size.getHeight()};
            AdjustWindowRectEx(&rect, style, FALSE, 0);
            HWND hwnd = CreateWindowExW(
                0, kEditorClassName, toWide(mName).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (hwnd == nullptr) {
                view->release();
                return;
            }

            auto* editor = new EditorWindow();
            editor->hwnd = hwnd;
            editor->view = view;
            editor->frame = new PlugFrame(hwnd);
            editor->resizable = resizable;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));

            view->setFrame(editor->frame);
            if (view->attached(hwnd, Steinberg::kPlatformTypeHWND) != kResultOk) {
                std::fprintf(stderr, "NicheLooper: attach editor failed for %s\n",
                             mName.c_str());
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                DestroyWindow(hwnd);
                view->setFrame(nullptr);
                view->release();
                delete editor->frame;
                delete editor;
                return;
            }
            mEditor = editor;
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            std::fprintf(stderr, "NicheLooper: editor window open for %s\n", mName.c_str());
        });
    }

    // --- Full state capture/restore (component + controller streams) ---
    // The component state carries every parameter plus the plugin's custom
    // data (e.g. the NAM model), so a captured/restored plugin comes back
    // identical without reopening its editor.

    bool captureState(std::vector<uint8_t>& componentState,
                      std::vector<uint8_t>& controllerState) const {
        componentState.clear();
        controllerState.clear();
        if (mComponent == nullptr) {
            return false;
        }
        Steinberg::MemoryStream compStream;
        if (mComponent->getState(&compStream) == kResultOk && compStream.getSize() > 0) {
            const auto* data = reinterpret_cast<const uint8_t*>(compStream.getData());
            componentState.assign(data, data + compStream.getSize());
        }
        if (mController != nullptr) {
            Steinberg::MemoryStream ctrlStream;
            if (mController->getState(&ctrlStream) == kResultOk && ctrlStream.getSize() > 0) {
                const auto* data = reinterpret_cast<const uint8_t*>(ctrlStream.getData());
                controllerState.assign(data, data + ctrlStream.getSize());
            }
        }
        return true;
    }

    bool restoreState(const std::vector<uint8_t>& componentState,
                      const std::vector<uint8_t>& controllerState) {
        if (mComponent == nullptr || componentState.empty()) {
            return false;
        }
        Steinberg::MemoryStream compStream(
            const_cast<uint8_t*>(componentState.data()),
            static_cast<Steinberg::TSize>(componentState.size()));
        if (mComponent->setState(&compStream) != kResultOk) {
            // Some plugins return kResultFalse yet still applied the state.
        }
        if (mController != nullptr) {
            compStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            mController->setComponentState(&compStream);
            if (!controllerState.empty()) {
                Steinberg::MemoryStream ctrlStream(
                    const_cast<uint8_t*>(controllerState.data()),
                    static_cast<Steinberg::TSize>(controllerState.size()));
                mController->setState(&ctrlStream);
            }
        }
        return true;
    }

    uint32_t renderCount() const { return mRenderCount.load(std::memory_order_relaxed); }
    uint32_t errorCount() const { return mErrorCount.load(std::memory_order_relaxed); }
    int32_t lastError() const { return mLastError.load(std::memory_order_relaxed); }
    int channels() const { return mChannels; }
    bool initialized() const { return mInitialized; }

private:
    static constexpr Steinberg::int32 kMaxParameterEdits = 64;

    void closeEditor() {
        // ALWAYS post (even when mEditor still looks null): a pending
        // openEditor job may be queued ahead of us; FIFO order guarantees it
        // runs first (while this object is still alive, because we block
        // here), and this job then tears its window down again.
        // Synchronous: after this returns the view no longer references the
        // controller, so the caller may tear the plugin down.
        PluginUiThread::instance().run(true, [this] {
            EditorWindow* editor = mEditor;
            mEditor = nullptr;
            if (editor == nullptr) {
                return;
            }
            SetWindowLongPtrW(editor->hwnd, GWLP_USERDATA, 0);
            if (editor->view != nullptr) {
                editor->view->setFrame(nullptr);
                editor->view->removed();
            }
            DestroyWindow(editor->hwnd);
            if (editor->view != nullptr) {
                editor->view->release();
            }
            delete editor->frame;
            delete editor;
        });
    }

    Hosting::Module::Ptr mModule;
    Hosting::ClassInfo mClassInfo;
    std::string mName;
    std::string mEmptyPath;

    IPtr<Vst::PlugProvider> mProvider;
    IPtr<Vst::IComponent> mComponent;
    IPtr<Vst::IAudioProcessor> mProcessor;
    IPtr<Vst::IEditController> mController;
    ComponentHandler mHandler;

    Vst::HostProcessData mProcessData;
    Vst::ParameterChanges mInputChanges;
    Vst::ParameterChanges mOutputChanges;
    Vst::ProcessContext mProcessContext{};

    EditorWindow* mEditor = nullptr;  // touched on the UI thread only

    int mChannels = 0;
    int mMaxFrames = 0;
    Steinberg::int64 mSampleTime = 0;
    bool mActive = false;
    bool mProcessing = false;
    bool mInitialized = false;
    std::atomic<uint32_t> mRenderCount{0};
    std::atomic<uint32_t> mErrorCount{0};
    std::atomic<int32_t> mLastError{0};
};

struct AvailablePlugin {
    std::string displayName;  // "Vendor: Name" (the UI strips the prefix)
    std::string path;
    VST3::UID uid;
};

std::vector<AvailablePlugin> scanInstalledPlugins() {
    std::vector<AvailablePlugin> plugins;
    for (const auto& modulePath : Hosting::Module::getModulePaths()) {
        std::string error;
        auto module = Hosting::Module::create(modulePath, error);
        if (module == nullptr) {
            std::fprintf(stderr, "NicheLooper: skipping %s: %s\n", modulePath.c_str(),
                         error.c_str());
            continue;
        }
        for (const auto& classInfo : module->getFactory().classInfos()) {
            if (classInfo.category() != kVstAudioEffectClass) {
                continue;
            }
            // Instruments make no sense in front of the looper.
            const std::string sub = classInfo.subCategoriesString();
            if (sub.find("Instrument") != std::string::npos) {
                continue;
            }
            std::string display = classInfo.vendor().empty()
                                      ? classInfo.name()
                                      : classInfo.vendor() + ": " + classInfo.name();
            plugins.push_back(AvailablePlugin{std::move(display), modulePath, classInfo.ID()});
        }
    }
    std::sort(plugins.begin(), plugins.end(),
              [](const AvailablePlugin& a, const AvailablePlugin& b) {
                  return a.displayName < b.displayName;
              });
    return plugins;
}

// Loads the module at [path] and builds the plugin for [uid]. Returns null
// (with a log line) if the module or class vanished.
std::unique_ptr<VstPlugin> buildPlugin(const std::string& path, const VST3::UID& uid,
                                       const std::string& displayName) {
    std::string error;
    auto module = Hosting::Module::create(path, error);
    if (module == nullptr) {
        std::fprintf(stderr, "NicheLooper: module load failed (%s): %s\n", path.c_str(),
                     error.c_str());
        return nullptr;
    }
    for (const auto& classInfo : module->getFactory().classInfos()) {
        if (classInfo.category() == kVstAudioEffectClass && classInfo.ID() == uid) {
            auto plugin = std::make_unique<VstPlugin>(module, classInfo, displayName);
            if (!plugin->instantiate()) {
                return nullptr;
            }
            return plugin;
        }
    }
    std::fprintf(stderr, "NicheLooper: class not found in %s\n", path.c_str());
    return nullptr;
}

// ---- Bank serialization helpers -------------------------------------------

constexpr uint32_t kBankMagic = 0x314B574Eu;  // "NWK1" little-endian

void putInt(std::vector<uint8_t>& out, int32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void putBlob(std::vector<uint8_t>& out, const void* data, size_t size) {
    putInt(out, static_cast<int32_t>(size));
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

struct BankReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    bool readInt(int32_t* value) {
        if (pos + 4 > size) {
            return false;
        }
        *value = static_cast<int32_t>(static_cast<uint32_t>(data[pos]) |
                                      (static_cast<uint32_t>(data[pos + 1]) << 8) |
                                      (static_cast<uint32_t>(data[pos + 2]) << 16) |
                                      (static_cast<uint32_t>(data[pos + 3]) << 24));
        pos += 4;
        return true;
    }

    bool readBlob(std::vector<uint8_t>* out) {
        int32_t length = 0;
        if (!readInt(&length) || length < 0 || pos + static_cast<size_t>(length) > size) {
            return false;
        }
        out->assign(data + pos, data + pos + length);
        pos += static_cast<size_t>(length);
        return true;
    }

    bool readString(std::string* out) {
        std::vector<uint8_t> blob;
        if (!readBlob(&blob)) {
            return false;
        }
        out->assign(blob.begin(), blob.end());
        return true;
    }
};

}  // namespace

// ---- Manager --------------------------------------------------------------

struct PluginChainManager::Impl {
    std::mutex lock;
    std::vector<AvailablePlugin> available;
    std::vector<std::unique_ptr<VstPlugin>> chains[kNumChains];
    std::atomic<int> active{0};
    int sampleRate = 48000;
    int maxFrames = 8192;
    // Diagnostics (audio thread writes, app thread reads).
    std::atomic<uint32_t> blocksProcessed{0};    // blocks with non-empty chain
    std::atomic<uint32_t> blocksSkippedLock{0};  // try_lock lost against an edit
};

PluginChainManager::PluginChainManager() : mImpl(std::make_unique<Impl>()) {}
PluginChainManager::~PluginChainManager() = default;

std::vector<std::string> PluginChainManager::availablePluginNames() {
    auto scanned = scanInstalledPlugins();
    std::vector<std::string> names;
    names.reserve(scanned.size());
    for (const auto& plugin : scanned) {
        names.push_back(plugin.displayName);
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->available = std::move(scanned);
    return names;
}

bool PluginChainManager::addPlugin(int chain, int pluginIndex) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    AvailablePlugin spec;
    int sampleRate;
    int maxFrames;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(mImpl->available.size())) {
            return false;
        }
        spec = mImpl->available[static_cast<size_t>(pluginIndex)];
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }

    // Instantiate + initialize OUTSIDE the lock (can take hundreds of ms for
    // big plugins) so the audio thread keeps processing meanwhile.
    auto plugin = buildPlugin(spec.path, spec.uid, spec.displayName);
    if (plugin == nullptr || !plugin->prepare(sampleRate, maxFrames)) {
        return false;
    }

    std::lock_guard<std::mutex> guard(mImpl->lock);
    mImpl->chains[chain].push_back(std::move(plugin));
    return true;
}

bool PluginChainManager::removePlugin(int chain, int slot) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    std::unique_ptr<VstPlugin> removed;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        auto& plugins = mImpl->chains[chain];
        if (slot < 0 || slot >= static_cast<int>(plugins.size())) {
            return false;
        }
        removed = std::move(plugins[static_cast<size_t>(slot)]);
        plugins.erase(plugins.begin() + slot);
    }
    // Destroyed outside the lock: closes the editor window (sync on the UI
    // thread) and terminates the plugin after the audio thread can no longer
    // see it.
    removed.reset();
    return true;
}

bool PluginChainManager::movePlugin(int chain, int from, int to) {
    if (chain < 0 || chain >= kNumChains) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    auto& plugins = mImpl->chains[chain];
    const int count = static_cast<int>(plugins.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return false;
    }
    std::swap(plugins[static_cast<size_t>(from)], plugins[static_cast<size_t>(to)]);
    return true;
}

std::vector<std::string> PluginChainManager::chainPluginNames(int chain) {
    std::vector<std::string> names;
    if (chain < 0 || chain >= kNumChains) {
        return names;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    for (const auto& plugin : mImpl->chains[chain]) {
        names.push_back(plugin->name());
    }
    return names;
}

void PluginChainManager::openEditor(int chain, int slot) {
    if (chain < 0 || chain >= kNumChains) {
        return;
    }
    std::lock_guard<std::mutex> guard(mImpl->lock);
    auto& plugins = mImpl->chains[chain];
    if (slot >= 0 && slot < static_cast<int>(plugins.size())) {
        plugins[static_cast<size_t>(slot)]->openEditor();
    }
}

void PluginChainManager::setActiveChain(int index) {
    if (index >= 0 && index < kNumChains) {
        mImpl->active.store(index, std::memory_order_relaxed);
    }
}

int PluginChainManager::activeChain() const {
    return mImpl->active.load(std::memory_order_relaxed);
}

void PluginChainManager::prepare(int sampleRate, int maxFrames) {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    const bool changed = sampleRate != mImpl->sampleRate || maxFrames != mImpl->maxFrames;
    mImpl->sampleRate = sampleRate;
    mImpl->maxFrames = maxFrames;
    if (!changed) {
        return;
    }
    for (auto& chain : mImpl->chains) {
        for (auto& plugin : chain) {
            plugin->prepare(sampleRate, maxFrames);
        }
    }
}

void PluginChainManager::processBlock(float* mono, int frames) {
    std::unique_lock<std::mutex> guard(mImpl->lock, std::try_to_lock);
    if (!guard.owns_lock()) {
        mImpl->blocksSkippedLock.fetch_add(1, std::memory_order_relaxed);
        return;  // edit in progress → this block stays dry
    }
    auto& chain = mImpl->chains[mImpl->active.load(std::memory_order_relaxed)];
    if (!chain.empty()) {
        mImpl->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& plugin : chain) {
        plugin->process(mono, frames);
    }
}

std::string PluginChainManager::debugReport() {
    std::lock_guard<std::mutex> guard(mImpl->lock);
    char line[512];
    std::snprintf(line, sizeof(line), "active=%d rate=%d blocks=%u lockSkips=%u\n",
                  mImpl->active.load(std::memory_order_relaxed), mImpl->sampleRate,
                  mImpl->blocksProcessed.load(std::memory_order_relaxed),
                  mImpl->blocksSkippedLock.load(std::memory_order_relaxed));
    std::string report(line);
    for (int c = 0; c < kNumChains; ++c) {
        for (size_t s = 0; s < mImpl->chains[c].size(); ++s) {
            const auto& plugin = mImpl->chains[c][s];
            std::snprintf(line, sizeof(line),
                          "chain%d[%zu] %s ch=%d init=%d renders=%u errors=%u lastErr=%d\n", c, s,
                          plugin->name().c_str(), plugin->channels(),
                          plugin->initialized() ? 1 : 0, plugin->renderCount(),
                          plugin->errorCount(), plugin->lastError());
            report += line;
        }
    }
    return report;
}

// ---- Preset bank (all 3 chains: identity + full plugin state) -------------

std::vector<uint8_t> PluginChainManager::saveBank() {
    std::vector<uint8_t> out;
    std::lock_guard<std::mutex> guard(mImpl->lock);

    putInt(out, static_cast<int32_t>(kBankMagic));
    putInt(out, 1);  // version
    putInt(out, mImpl->active.load(std::memory_order_relaxed));
    for (int c = 0; c < kNumChains; ++c) {
        putInt(out, static_cast<int32_t>(mImpl->chains[c].size()));
        for (const auto& plugin : mImpl->chains[c]) {
            std::vector<uint8_t> componentState;
            std::vector<uint8_t> controllerState;
            plugin->captureState(componentState, controllerState);
            const std::string uid = plugin->uid().toString();
            putBlob(out, plugin->name().data(), plugin->name().size());
            putBlob(out, plugin->modulePath().data(), plugin->modulePath().size());
            putBlob(out, uid.data(), uid.size());
            putBlob(out, componentState.data(), componentState.size());
            putBlob(out, controllerState.data(), controllerState.size());
        }
    }
    return out;
}

bool PluginChainManager::loadBank(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 12) {
        return false;
    }

    struct PluginSpec {
        std::string name;
        std::string path;
        std::string uid;
        std::vector<uint8_t> componentState;
        std::vector<uint8_t> controllerState;
    };
    std::array<std::vector<PluginSpec>, kNumChains> chainsSpec;
    int32_t active = 0;

    BankReader reader{data, size};
    int32_t magic = 0;
    int32_t version = 0;
    if (!reader.readInt(&magic) || static_cast<uint32_t>(magic) != kBankMagic ||
        !reader.readInt(&version) || version != 1 || !reader.readInt(&active)) {
        return false;
    }
    for (int c = 0; c < kNumChains; ++c) {
        int32_t count = 0;
        if (!reader.readInt(&count) || count < 0 || count > 64) {
            return false;
        }
        for (int32_t i = 0; i < count; ++i) {
            PluginSpec spec;
            if (!reader.readString(&spec.name) || !reader.readString(&spec.path) ||
                !reader.readString(&spec.uid) || !reader.readBlob(&spec.componentState) ||
                !reader.readBlob(&spec.controllerState)) {
                return false;
            }
            chainsSpec[c].push_back(std::move(spec));
        }
    }

    int sampleRate = 0;
    int maxFrames = 0;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        sampleRate = mImpl->sampleRate;
        maxFrames = mImpl->maxFrames;
    }
    if (sampleRate <= 0 || maxFrames <= 0) {
        std::fprintf(stderr, "NicheLooper: loadBank without a running engine (rate=%d)\n",
                     sampleRate);
        return false;
    }

    // Instantiate everything OUTSIDE the lock, then swap the chains in.
    std::array<std::vector<std::unique_ptr<VstPlugin>>, kNumChains> built;
    for (int c = 0; c < kNumChains; ++c) {
        for (const auto& spec : chainsSpec[c]) {
            auto uid = VST3::UID::fromString(spec.uid);
            if (!uid) {
                std::fprintf(stderr, "NicheLooper: bad plugin id for %s\n", spec.name.c_str());
                continue;
            }
            auto plugin = buildPlugin(spec.path, *uid, spec.name);
            if (plugin == nullptr || !plugin->prepare(sampleRate, maxFrames)) {
                std::fprintf(stderr, "NicheLooper: preset plugin failed: %s\n",
                             spec.name.c_str());
                continue;
            }
            if (!spec.componentState.empty()) {
                plugin->restoreState(spec.componentState, spec.controllerState);
            }
            built[c].push_back(std::move(plugin));
        }
    }

    std::array<std::vector<std::unique_ptr<VstPlugin>>, kNumChains> old;
    {
        std::lock_guard<std::mutex> guard(mImpl->lock);
        for (int c = 0; c < kNumChains; ++c) {
            old[c] = std::move(mImpl->chains[c]);
            mImpl->chains[c] = std::move(built[c]);
        }
        if (active >= 0 && active < kNumChains) {
            mImpl->active.store(active, std::memory_order_relaxed);
        }
    }
    for (int c = 0; c < kNumChains; ++c) {
        old[c].clear();  // editor windows + plugins torn down outside the lock
    }
    return true;
}
