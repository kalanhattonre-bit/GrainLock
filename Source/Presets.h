#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace grainlock
{
    struct PresetValue
    {
        const char* id;
        float value;   // plain (not normalised) parameter value
    };

    /** A factory preset: every parameter starts at its default, then these values apply. */
    struct Preset
    {
        const char* name;
        std::vector<PresetValue> values;
    };

    /** Index 0 is "Init" (all defaults). */
    const std::vector<Preset>& factoryPresets();

    /** Message thread only. Sets every parameter as one host-visible gesture per parameter. */
    void applyPreset (juce::AudioProcessorValueTreeState& state, const Preset& preset);
}
