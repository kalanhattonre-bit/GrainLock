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
            LfoTrig trig = LfoTrig::free;
            float fadeMs = 0.0f;
            float phaseDegrees = 0.0f;
            bool invert = false;
        };
        std::array<LfoParams, numLfos> lfos {};

        float mixPercent = 100.0f;
        bool dryWhenIdle = true;
        float outGainDb = 0.0f;
        bool bypass = false;         // the host's bypass switch: fade to the untouched input

        bool formantTrack = false;   // each voice's formant follows its own pitch (root C3)
        bool autoGain = true;        // a loop stays as loud as the slice it came from

        // Note envelope and tape stop
        float envAttackMs = 0.0f;
        float envDecayMs = 300.0f;
        float envPitch = 0.0f;       // semitones at the envelope's peak
        float envFormant = 0.0f;     // semitones
        int envGrain = 0;            // cycles
        bool tapeStop = false;

        // When a note grabs
        bool grabAtKey = false;      // the loop region starts at the key instead of ending before it
        float waitMs = 0.0f;
        int waitSync = 0;            // index into grabSyncChoices(); 0 = the ms knob
        int offsetSync = 0;
        int refreshSync = 0;         // index into lfoSyncChoices(); 0 = the ms knob

        // Keyboard
        int bendUp = 2, bendDown = 2;    // semitones
        float vibRateHz = 5.5f;
        float vibDepthCents = 50.0f;

        /** One keyboard source (index = ModSource): what it moves and how far. */
        struct SourceParams
        {
            ModDest dest = ModDest::off;
            float amountPercent = 100.0f;
        };
        std::array<SourceParams, numModSources> sources { SourceParams { ModDest::vibrato, 100.0f }, SourceParams {}, SourceParams {} };
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

        /** Tests only: play with 0.2's seam (no correlation gain, no loop-point nudge), so a build can
            be compared with the 0.2 reference sound. Call it between blocks. */
        void setLegacySeamForTests (bool shouldBeLegacy) noexcept { legacySeam = shouldBeLegacy; }

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
        /** Dry When Idle: a voice is held (by anything) and has made its grab. Release tails and
            voices still waiting to grab do not count. */
        bool anyVoiceEngaged() const noexcept;
        void restartNoteLfos() noexcept;
        void updateBendTarget() noexcept;

        /** The only place input reaches the memory: also advances the sample clock. */
        void pushInput (float left, float right) noexcept;

        /** A grab that has been asked for and may have to wait (Wait, At Key). */
        struct PendingGrab
        {
            bool active = false;
            juce::int64 planEnd = 0;   // where the slice ends, on the sample clock (may lie before the key)
            juce::int64 dueTime = 0;   // the first moment the grab can happen: max(key, planEnd)
            bool placed = false;       // the spot was chosen on purpose (At Key): no loop-point nudge
        };

        /** Works out when a key pressed now grabs. Returns true when that is right away. */
        bool planGrab (int slot, int note, const VoiceContext& ctx, PendingGrab& plan) noexcept;
        void startArmedVoice (int slot, const VoiceContext& ctx) noexcept;
        void fireGrab (int slot, const VoiceContext& ctx, const PendingGrab& plan) noexcept;
        void fireMonoKey (const VoiceContext& ctx) noexcept;
        void servePendingGrabs (const VoiceContext& ctx) noexcept;
        void clearPendingGrabs() noexcept;
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
        juce::SmoothedValue<float> mix, sustain, bypassFade, trackAmount;
        juce::SmoothedValue<float> wheel, pressure, expression;     // keyboard sources, 0..1
        float bendNorm = 0.0f;                                      // the pitch wheel, -1..1
        double vibratoPhase = 0.0;
        bool vibratoInUse = false;
        ModSettings mods;
        std::array<double, numLfos> lfoOffset {};
        std::array<float, numLfos> lfoFadeStep { 1.0f, 1.0f, 1.0f };
        std::array<juce::SmoothedValue<float>, numLfos> lfoDepth;   // 0 when that LFO is off, so switching fades
        std::array<juce::SmoothedValue<float>, numLfos> lfoRate;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain { 1.0f };

        // Values fixed for the current block.
        EngineParams block;
        int offsetSamples = 0;
        int waitSamples = 0;
        int liveLagSamples = 0;         // where a Live re-grab ends: Offset less the Wait already served
        double refreshSamples = 1200.0;
        bool refreshSynced = false;

        juce::int64 sampleClock = 0;    // input samples pushed since prepare / reset
        std::array<PendingGrab, numVoiceSlots> pendingGrabs {};

        // Mono: a key whose grab lies in the future is acted on at that time, not at the key, so the
        // sounding note carries on unchanged until then.
        struct MonoKey
        {
            bool active = false;
            int note = 60;
            float level = 1.0f;
            PendingGrab grab;
        } monoKey;
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
        bool legacySeam = false;

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
