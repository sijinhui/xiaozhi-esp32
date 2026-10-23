#ifndef METERED_BOX_AUDIO_CODEC_H
#define METERED_BOX_AUDIO_CODEC_H

#include "box_audio_codec.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

// Measures loudness on the way in and out, for the voice bars beside the emoji
class MeteredBoxAudioCodec : public BoxAudioCodec {
public:
    using BoxAudioCodec::BoxAudioCodec;

    // 0..100, from the microphone and towards the speaker
    int input_level() const { return input_level_; }
    int output_level() const { return output_level_; }

    virtual bool InputData(std::vector<int16_t>& data) override {
        bool ok = BoxAudioCodec::InputData(data);
        if (ok) {
            input_level_ = Level(data, input_channels());
        }
        return ok;
    }

    virtual void OutputData(std::vector<int16_t>& data) override {
        output_level_ = Level(data, output_channels());
        BoxAudioCodec::OutputData(data);
    }

private:
    std::atomic<int> input_level_{0};
    std::atomic<int> output_level_{0};

    // RMS of the first channel (the microphone when a reference rides along),
    // mapped from -50..-10 dBFS onto 0..100
    static int Level(const std::vector<int16_t>& data, int channels) {
        channels = std::max(channels, 1);
        if (data.size() < static_cast<size_t>(channels)) {
            return 0;
        }
        int64_t sum = 0;
        size_t n = 0;
        for (size_t i = 0; i < data.size(); i += channels, n++) {
            sum += static_cast<int32_t>(data[i]) * data[i];
        }
        float rms = sqrtf(static_cast<float>(sum) / n);
        if (rms < 1) {
            return 0;
        }
        float db = 20 * log10f(rms / 32768);
        return std::clamp(static_cast<int>((db + 50) * 100 / 40), 0, 100);
    }
};

#endif  // METERED_BOX_AUDIO_CODEC_H
