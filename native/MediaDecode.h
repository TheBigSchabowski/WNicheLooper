#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Decodes an audio file (m4a/aac, mp3, wma, …) to mono float PCM via Windows
 * Media Foundation — the Windows replacement for macOS' afconvert. Channels
 * are averaged; the sample rate is the file's own (the Kotlin layer
 * resamples to the engine rate). Runs off the audio path.
 */
bool decodeMediaFileToMono(const std::wstring& path, std::vector<float>* samples,
                           int32_t* sampleRate);
