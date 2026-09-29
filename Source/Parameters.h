#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are part of saved Cubase projects. Never rename or remove one once shipped.
namespace grainlock::ParamID
{
    inline constexpr const char* grainCycles = "grainCycles";
    inline constexpr const char* smooth      = "smooth";
    inline constexpr const char* offset      = "offset";
    inline constexpr const char* captureMode = "captureMode";
    inline constexpr const char* refresh     = "refresh";
    inline constexpr const char* pitchLock   = "pitchLock";
    inline constexpr const char* formant     = "formant";
    inline constexpr const char* tune        = "tune";
    inline constexpr const char* fine        = "fine";
    inline constexpr const char* glide       = "glide";
    inline constexpr const char* mono        = "mono";
    inline constexpr const char* velSens     = "velSens";
    inline constexpr const char* attack      = "attack";
    inline constexpr const char* decay       = "decay";
    inline constexpr const char* sustain     = "sustain";
    inline constexpr const char* release     = "release";
    inline constexpr const char* lfoRate     = "lfoRate";
    inline constexpr const char* lfoSync     = "lfoSync";
    inline constexpr const char* lfoShape    = "lfoShape";
    inline constexpr const char* lfoDepth    = "lfoDepth";
    inline constexpr const char* lfoTarget   = "lfoTarget";
    inline constexpr const char* mix         = "mix";
    inline constexpr const char* dryWhenIdle = "dryWhenIdle";
    inline constexpr const char* outGain     = "outGain";

    inline constexpr const char* all[] = {
        grainCycles, smooth, offset, captureMode, refresh, pitchLock,
        formant, tune, fine, glide, mono, velSens,
        attack, decay, sustain, release,
        lfoRate, lfoSync, lfoShape, lfoDepth, lfoTarget,
        mix, dryWhenIdle, outGain
    };
}

namespace grainlock
{
    enum class CaptureMode { hold = 0, live = 1 };
    enum class LfoShape    { sine = 0, triangle, square, sampleHold };
    enum class LfoTarget   { pitch = 0, formant, grainCycles };

    inline constexpr int minCycles = 1;
    inline constexpr int maxCycles = 16;

    // Full-depth LFO swing per target.
    inline constexpr float lfoPitchRangeSemitones   = 1.0f;   // +/-100 cents
    inline constexpr float lfoFormantRangeSemitones = 12.0f;
    inline constexpr float lfoCyclesRange           = 8.0f;

    /** Choice labels for lfoSync. Index 0 is free-running (Hz); the rest are note divisions. */
    const juce::StringArray& lfoSyncChoices();

    /** Length of one LFO cycle in quarter-note beats for a lfoSync index, or 0 for free-running. */
    double lfoSyncBeats (int syncIndex) noexcept;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Note name as Cubase shows it by default (middle C, MIDI 60, is C3). */
    juce::String formatNoteName (int midiNote);

    /** Cached pointers to every parameter's live value, read on the audio thread without locks. */
    struct ParameterRefs
    {
        explicit ParameterRefs (juce::AudioProcessorValueTreeState& state);

        std::atomic<float>* grainCycles;
        std::atomic<float>* smooth;
        std::atomic<float>* offset;
        std::atomic<float>* captureMode;
        std::atomic<float>* refresh;
        std::atomic<float>* pitchLock;
        std::atomic<float>* formant;
        std::atomic<float>* tune;
        std::atomic<float>* fine;
        std::atomic<float>* glide;
        std::atomic<float>* mono;
        std::atomic<float>* velSens;
        std::atomic<float>* attack;
        std::atomic<float>* decay;
        std::atomic<float>* sustain;
        std::atomic<float>* release;
        std::atomic<float>* lfoRate;
        std::atomic<float>* lfoSync;
        std::atomic<float>* lfoShape;
        std::atomic<float>* lfoDepth;
        std::atomic<float>* lfoTarget;
        std::atomic<float>* mix;
        std::atomic<float>* dryWhenIdle;
        std::atomic<float>* outGain;
    };
}
