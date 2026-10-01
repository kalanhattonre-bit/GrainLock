#pragma once

#include "Envelope.h"
#include "GrainBuffer.h"
#include "../Parameters.h"

namespace grainlock
{
    /** Per-sample values every voice shares, worked out once per sample by the engine. */
    struct VoiceContext
    {
        double sampleRate = 48000.0;
        float globalSemitones = 0.0f;   // tune + fine + pitch bend + LFO pitch
        float formantRatio = 1.0f;      // source read rate within each note period, before key tracking
        float trackAmount = 0.0f;       // 0..1: how far each voice's formant follows its own pitch (root C3)
        bool autoGain = false;          // keep a loop as loud as the slice it came from
        float smooth = 0.1f;            // seam crossfade, fraction of the loop (0..0.5)
        float sustain = 1.0f;           // envelope sustain level, smoothed
        int targetCycles = 2;           // grain cycles after LFO modulation
        bool pitchLock = true;
        bool live = true;
        double refreshSamples = 1200.0;

        // Capture budget: the most the knobs and LFO can ask of a grain before it is replaced.
        float captureRatioMax = 1.0f;
        int captureCyclesMax = 2;
        bool captureBothLayouts = false;   // Hold keeps its grain, so it must also cover a Pitch Lock toggle
    };

    /** Where a voice grabs audio from: the input ring, ending offsetSamples before now. */
    struct CaptureSource
    {
        const InputRing* ring = nullptr;
        int offsetSamples = 0;
    };

    /** One note: a captured grain looped at the note's period.

        Pitch Lock on: the grain holds N note periods; every output period sums all N of them
        (tap k reads period k), so the loop repeats every single note period and the pitch is
        exactly the note. Pitch Lock off: the whole N-period grain is one loop, so the loop
        repeats every N periods (the pitch drops by N).

        Changes that would jump the waveform (a fresh capture, a new cycle count, toggling Pitch
        Lock) crossfade between two play states. */
    class GrainVoice
    {
    public:
        void prepare (double sampleRate, int grainCapacity);
        void setEnvelopeTimes (float attackMs, float decayMs, float releaseMs) noexcept
        {
            envelope.setTimes (attackMs, decayMs, releaseMs);
        }

        void start (int midiNote, float velocityLevel, juce::uint64 order,
                    const VoiceContext& ctx, const CaptureSource& source);

        /** Mono mode: move to a new note (gliding over glideSamples) and grab fresh audio for it.
            A negative velocityLevel keeps the current level. */
        void retarget (int midiNote, float velocityLevel, int glideSamples, bool retriggerEnvelope);

        void release() noexcept;
        void steal (int fadeSamples) noexcept;
        void kill() noexcept;

        /** Adds this voice's output for one sample. */
        void render (const VoiceContext& ctx, const CaptureSource& source, float& outLeft, float& outRight) noexcept;

        /** Fills dest with one loop of the current play state, for the display. Does not advance anything. */
        int renderLoopShape (const VoiceContext& ctx, float* dest, int numPoints) const noexcept;

        bool isActive() const noexcept    { return active; }
        bool isHeld() const noexcept      { return active && held && ! stealing; }
        bool isStealing() const noexcept  { return active && stealing; }
        bool isReleasing() const noexcept { return active && ! held && ! stealing; }
        int getNote() const noexcept      { return note; }
        juce::uint64 getStartOrder() const noexcept { return startOrder; }

    private:
        struct PlayState
        {
            int grain = 0;          // which of the two grain buffers this state reads
            int cycles = 2;         // cycles asked for
            int taps = 2;           // cycles the grain actually holds (fixed for the life of the state)
            bool lockOn = true;
            double theta = 0.0;     // position through the loop, 0..1

            // Measured on the grain (see measure()): how alike the two sides of the seam are, and how
            // much louder the summed cycles are than one of them.
            float seamRho = 0.0f;
            float gain = 1.0f;
            double measuredCycle = 0.0;   // the settings the measurement was made for
            float measuredSeam = -1.0f;
            bool measuredAuto = false;
        };

        /** Linear glide in semitones, in double so long glides land exactly on the note. */
        struct Glide
        {
            double value = 60.0, target = 60.0, step = 0.0;
            int remaining = 0;

            void jumpTo (double v) noexcept { value = target = v; remaining = 0; }

            void moveTo (double v, int samples) noexcept
            {
                target = v;
                if (samples <= 0) { value = v; remaining = 0; return; }
                step = (v - value) / (double) samples;
                remaining = samples;
            }

            double next() noexcept
            {
                if (remaining > 0)
                {
                    --remaining;
                    value = remaining == 0 ? target : value + step;
                }
                return value;
            }
        };

        double frequencyFor (double semitones) const noexcept;
        /** Works out this voice's own formant ratio and capture budget for the pitch it sounds at
            (and the pitch it is gliding to). */
        void updateShape (const VoiceContext& ctx, double frequency, double targetFrequency) noexcept;
        int tapsThatFit (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept;
        bool grainHolds (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept;
        double cycleLengthOf (const PlayState& state) const noexcept;
        /** Measures the seam correlation and the Auto Gain of a play state on its grain. blend 1
            replaces the previous values; less than 1 moves towards the new ones (Live re-grabs). */
        void measure (PlayState& state, const VoiceContext& ctx, int points, float blend) const noexcept;
        int nudgedEndDelay (const CaptureSource& source, double period, int cycles, bool lockOn) const noexcept;
        void capture (GrainBuffer& grain, double frequency, const VoiceContext& ctx, const CaptureSource& source,
                      bool nudge = true) noexcept;
        void renderState (const PlayState& state, const VoiceContext& ctx, float& left, float& right) const noexcept;
        bool advance (PlayState& state, double frequency) const noexcept;
        void beginRecapture (double soundingFrequency, const VoiceContext& ctx, const CaptureSource& source) noexcept;
        void beginReshape (const VoiceContext& ctx) noexcept;
        bool beginExtend (int cycles, bool lockOn, const VoiceContext& ctx, const CaptureSource& source) noexcept;
        void startTransition (int lengthSamples, float correlation) noexcept;
        double loopLengthSamples (const PlayState& state, double frequency) const noexcept;
        static float correlationBetween (const PlayState& a, const PlayState& b) noexcept;

        std::array<GrainBuffer, 2> grains;
        std::array<PlayState, 2> states;
        int current = 0;   // states[current] plays; states[1 - current] is the one fading out

        struct Transition
        {
            bool active = false;
            float correlation = 0.0f;   // expected correlation of the two states, for a flat-power fade
            int position = 0;
            int length = 1;
        } transition;

        Envelope envelope;
        juce::SmoothedValue<float> level { 1.0f };
        Glide pitch;

        double sampleRate = 48000.0;
        float ratio = 1.0f;                 // this voice's formant ratio right now (global x key tracking)
        float captureRatio = 1.0f;          // the most formant this voice's next grab must hold
        int sinceMeasure = 0;
        double samplesSinceCapture = 0.0;   // how long ago the current grain's frozen instant was
        int lastCaptureOffset = 0;          // the Offset (in samples) that instant was taken with
        bool pendingRecapture = false;

        int note = 60;
        bool active = false;
        bool held = false;
        bool stealing = false;
        int stealRemaining = 0;
        int stealLength = 1;
        juce::uint64 startOrder = 0;
    };
}
