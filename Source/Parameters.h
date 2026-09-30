#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are part of saved Cubase projects. Never rename or remove one once shipped.
// (v0.2 replaced the single shared LFO's IDs before any release; setStateInformation migrates them.)
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

    // Three LFOs, one per destination, each with its own switch, speed, sync, shape and depth.
    inline constexpr const char* pitchLfoOn      = "pitchLfoOn";
    inline constexpr const char* pitchLfoRate    = "pitchLfoRate";
    inline constexpr const char* pitchLfoSync    = "pitchLfoSync";
    inline constexpr const char* pitchLfoShape   = "pitchLfoShape";
    inline constexpr const char* pitchLfoDepth   = "pitchLfoDepth";
    inline constexpr const char* formantLfoOn    = "formantLfoOn";
    inline constexpr const char* formantLfoRate  = "formantLfoRate";
    inline constexpr const char* formantLfoSync  = "formantLfoSync";
    inline constexpr const char* formantLfoShape = "formantLfoShape";
    inline constexpr const char* formantLfoDepth = "formantLfoDepth";
    inline constexpr const char* grainLfoOn      = "grainLfoOn";
    inline constexpr const char* grainLfoRate    = "grainLfoRate";
    inline constexpr const char* grainLfoSync    = "grainLfoSync";
    inline constexpr const char* grainLfoShape   = "grainLfoShape";
    inline constexpr const char* grainLfoDepth   = "grainLfoDepth";

    inline constexpr const char* mix         = "mix";
    inline constexpr const char* dryWhenIdle = "dryWhenIdle";
    inline constexpr const char* outGain     = "outGain";

    inline constexpr const char* all[] = {
        grainCycles, smooth, offset, captureMode, refresh, pitchLock,
        formant, tune, fine, glide, mono, velSens,
        attack, decay, sustain, release,
        pitchLfoOn, pitchLfoRate, pitchLfoSync, pitchLfoShape, pitchLfoDepth,
        formantLfoOn, formantLfoRate, formantLfoSync, formantLfoShape, formantLfoDepth,
        grainLfoOn, grainLfoRate, grainLfoSync, grainLfoShape, grainLfoDepth,
        mix, dryWhenIdle, outGain
    };

    /** The five parameter IDs of one LFO. */
    struct LfoIds
    {
        const char* on;
        const char* rate;
        const char* sync;
        const char* shape;
        const char* depth;
    };

    /** Indexed by LfoTarget: pitch, formant, grain cycles. */
    inline constexpr LfoIds lfo[] = {
        { pitchLfoOn,   pitchLfoRate,   pitchLfoSync,   pitchLfoShape,   pitchLfoDepth },
        { formantLfoOn, formantLfoRate, formantLfoSync, formantLfoShape, formantLfoDepth },
        { grainLfoOn,   grainLfoRate,   grainLfoSync,   grainLfoShape,   grainLfoDepth },
    };
}

namespace grainlock
{
    enum class CaptureMode { hold = 0, live = 1 };
    enum class LfoShape    { sine = 0, triangle, square, sampleHold };

    /** What each LFO moves. Also the index of that LFO everywhere (ParamID::lfo, EngineParams::lfos, ...). */
    enum class LfoTarget   { pitch = 0, formant, grainCycles };
    inline constexpr int numLfos = 3;

    inline constexpr int minCycles = 1;
    inline constexpr int maxCycles = 16;

    // Full-depth LFO swing per target.
    inline constexpr float lfoPitchRangeSemitones   = 1.0f;   // +/-100 cents
    inline constexpr float lfoFormantRangeSemitones = 12.0f;
    inline constexpr float lfoCyclesRange           = 8.0f;

    /** Choice labels for the LFO sync parameters. Index 0 is free-running (Hz); the rest are note divisions. */
    const juce::StringArray& lfoSyncChoices();

    /** Length of one LFO cycle in quarter-note beats for a sync index, or 0 for free-running. */
    double lfoSyncBeats (int syncIndex) noexcept;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Rewrites a state saved before v0.2 (one shared LFO with a Target choice) so the old LFO's
        settings land on the LFO for the target it pointed at. States without the old IDs pass through. */
    void migrateLegacyLfoState (juce::XmlElement& state);

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

        struct Lfo
        {
            std::atomic<float>* on;
            std::atomic<float>* rate;
            std::atomic<float>* sync;
            std::atomic<float>* shape;
            std::atomic<float>* depth;
        };
        std::array<Lfo, numLfos> lfo;

        std::atomic<float>* mix;
        std::atomic<float>* dryWhenIdle;
        std::atomic<float>* outGain;
    };
}
