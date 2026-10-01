#pragma once

#include "GrainVoice.h"
#include "Lfo.h"
#include "ScopeFrame.h"
#include "SoftLimiter.h"

namespace grainlock
{
    /** Every parameter's value for one block, read once from the atomics. */
    struct EngineParams
    {
        int grainCycles = 2;
        float smoothPercent = 10.0f;
        float offsetMs = 0.0f;
        CaptureMode captureMode = CaptureMode::live;
        float refreshMs = 25.0f;
        bool pitchLock = true;

        float formantSemitones = 0.0f;
        int tuneSemitones = 0;
        float fineCents = 0.0f;
        float glideMs = 0.0f;
        bool mono = false;
        float velSensPercent = 100.0f;

        float attackMs = 5.0f;
        float decayMs = 250.0f;
        float sustainPercent = 100.0f;
        float releaseMs = 150.0f;

        /** One of the three LFOs (index = LfoTarget). */
        struct LfoParams
        {
            bool on = false;
            float rateHz = 1.0f;
            int sync = 0;
            LfoShape shape = LfoShape::sine;
            float depthPercent = 50.0f;
        };
        std::array<LfoParams, numLfos> lfos {};

        float mixPercent = 100.0f;
        bool dryWhenIdle = true;
        float outGainDb = 0.0f;
        bool bypass = false;         // the host's bypass switch: fade to the untouched input
    };

    /** What the host said about tempo and position for this block. */
    struct HostTiming
    {
        double bpm = 120.0;
        bool hasPpq = false;
        double ppq = 0.0;
        bool playing = false;
    };

    class GrainEngine
    {
    public:
        static constexpr int maxPolyphony = 8;
        static constexpr int numVoiceSlots = 12;   // 8 playing plus room for stolen voices to fade out

        void prepare (double sampleRate, int maxBlockSize);
        void reset();

        /** Processes in place. Channel 0 (and 1 when numInputChannels > 1) hold the input. */
        void process (float* const* channels, int numChannels, int numInputChannels, int numSamples,
                      const juce::MidiBuffer& midi, const EngineParams& params, const HostTiming& timing) noexcept;

        /** The host bypassed the plugin: keep the input history current and silence every note,
            so nothing is left stuck when the bypass is lifted. Audio passes through untouched. */
        void processBypassed (const float* const* channels, int numChannels, int numInputChannels, int numSamples) noexcept;

        ScopeFifo& getScopeFifo() noexcept { return scopeFifo; }

        const LimiterStats& getLimiterStats() const noexcept { return limiter.getStats(); }
        void resetLimiterStats() noexcept { limiter.resetStats(); }

    private:
        void applyBlockParams (const EngineParams& params, const HostTiming& timing, int numSamples) noexcept;
        VoiceContext nextContext() noexcept;
        void handleMidiEvent (const juce::uint8* data, int numBytes, const VoiceContext& ctx) noexcept;

        void noteOn (int note, int velocity, const VoiceContext& ctx) noexcept;
        void noteOff (int note, const VoiceContext& ctx) noexcept;
        void monoNoteOn (int note, int velocity, const VoiceContext& ctx) noexcept;
        void monoNoteOff (int note, const VoiceContext& ctx) noexcept;
        void releaseAll() noexcept;
        void killAll() noexcept;
        int findFreeSlot() noexcept;
        float velocityLevel (int velocity) const noexcept;
        bool anyNoteHeld() const noexcept;
        void publishScope (const VoiceContext& ctx) noexcept;

        void stackRemove (int note) noexcept;
        void stackPush (int note) noexcept;
        int stackTop() const noexcept { return stackSize > 0 ? noteStack[(size_t) (stackSize - 1)] : -1; }

        double sampleRate = 48000.0;
        bool prepared = false;

        InputRing ring;
        std::array<GrainVoice, numVoiceSlots> voices;
        std::array<Lfo, numLfos> lfos;
        SoftLimiter limiter;
        ScopeFifo scopeFifo;

        // Smoothed continuous controls.
        juce::SmoothedValue<float> tuneSemis, bendSemis, formantSemis, smoothFraction;
        juce::SmoothedValue<float> mix, sustain, bypassFade;
        std::array<juce::SmoothedValue<float>, numLfos> lfoDepth;   // 0 when that LFO is off, so switching fades
        std::array<juce::SmoothedValue<float>, numLfos> lfoRate;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain { 1.0f };

        // Values fixed for the current block.
        EngineParams block;
        int offsetSamples = 0;
        double refreshSamples = 1200.0;
        std::array<double, numLfos> lfoIncrement {};
        std::array<bool, numLfos> lfoSynced {};
        float captureRatioMax = 1.0f;
        int captureCyclesMax = 2;
        bool captureBothLayouts = false;
        CaptureSource captureSource;
        VoiceContext lastContext;
        int stealFadeSamples = 240;
        bool monoMode = false;
        bool firstBlock = true;
        bool wasBypassed = false;

        std::array<float, numLfos> lfoSmoothed {};
        std::array<float, numLfos> lfoLastScaled {};
        float lfoSmoothCoeff = 0.01f;
        float activity = 0.0f;
        float activityUp = 0.01f, activityDown = 0.003f;
        float limiterBlend = 0.0f;      // 0 = limiter out of the path (pure dry at rest), 1 = fully in
        float limiterFadeStep = 0.002f;

        std::array<int, 128> noteStack {};
        int stackSize = 0;
        int monoVoice = -1;

        juce::uint64 voiceCounter = 0;
        int lastStartedVoice = -1;
        int scopeInterval = 1600;
        int scopeCountdown = 0;
    };
}
