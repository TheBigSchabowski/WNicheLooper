#include "MediaDecode.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdio>

namespace {

template <typename T>
void safeRelease(T** object) {
    if (*object != nullptr) {
        (*object)->Release();
        *object = nullptr;
    }
}

}  // namespace

bool decodeMediaFileToMono(const std::wstring& path, std::vector<float>* samples,
                           int32_t* sampleRate) {
    samples->clear();
    *sampleRate = 0;

    // The JVM may have initialized this thread as STA; RPC_E_CHANGED_MODE
    // then just means COM is already usable.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitCom = SUCCEEDED(comInit);

    bool mfStarted = false;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* requestedType = nullptr;
    IMFMediaType* actualType = nullptr;
    WAVEFORMATEX* format = nullptr;
    bool ok = false;
    int32_t channels = 0;

    do {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            break;
        }
        mfStarted = true;

        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
            std::fprintf(stderr, "NicheLooper: MF cannot open file\n");
            break;
        }
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                   TRUE);

        // Ask for float32 PCM; the reader inserts the matching decoder.
        if (FAILED(MFCreateMediaType(&requestedType)) ||
            FAILED(requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(requestedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) ||
            FAILED(reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                requestedType))) {
            std::fprintf(stderr, "NicheLooper: MF cannot decode to float PCM\n");
            break;
        }

        if (FAILED(reader->GetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actualType))) {
            break;
        }
        UINT32 formatSize = 0;
        if (FAILED(MFCreateWaveFormatExFromMFMediaType(actualType, &format, &formatSize)) ||
            format->nChannels == 0 || format->nSamplesPerSec == 0) {
            break;
        }
        channels = format->nChannels;
        *sampleRate = static_cast<int32_t>(format->nSamplesPerSec);

        for (;;) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                    &flags, nullptr, &sample))) {
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                safeRelease(&sample);
                ok = true;
                break;
            }
            if (sample == nullptr) {
                continue;
            }
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                BYTE* bytes = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&bytes, nullptr, &length))) {
                    const float* floats = reinterpret_cast<const float*>(bytes);
                    const size_t frames = length / (sizeof(float) * channels);
                    samples->reserve(samples->size() + frames);
                    for (size_t i = 0; i < frames; ++i) {
                        float sum = 0.0f;
                        for (int32_t c = 0; c < channels; ++c) {
                            sum += floats[i * channels + c];
                        }
                        samples->push_back(sum / static_cast<float>(channels));
                    }
                    buffer->Unlock();
                }
                safeRelease(&buffer);
            }
            safeRelease(&sample);
        }
    } while (false);

    if (format != nullptr) {
        CoTaskMemFree(format);
    }
    safeRelease(&actualType);
    safeRelease(&requestedType);
    safeRelease(&reader);
    if (mfStarted) {
        MFShutdown();
    }
    if (uninitCom) {
        CoUninitialize();
    }
    return ok && !samples->empty() && *sampleRate > 0;
}
