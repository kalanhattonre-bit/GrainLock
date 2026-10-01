#pragma once

#include "GrainVoice.h"
#include "Lfo.h"
#include "ScopeFrame.h"
#include "SoftLimiter.h"
#include "ToneChain.h"

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

        // What a note will and will not grab
        float snapMs = 0.0f;                  // 0 = off
        float thresholdDb = thresholdOffDb;   // at the bottom of its range the threshold is off
        float maxWaitMs = 500.0f;
        bool skipHiss = false;
        bool gate = false;
        bool gridGrabs = false;               // Live re-grabs land on the song's grid (needs Refresh Sync)
        float skipChancePercent = 0.0f;
        float feedbackPercent = 0.0f;

        // How notes are held, and how many play
        bool sustainPedal = true;             // CC64 holds notes (Normal and On Grid)
        HoldMode holdMode = HoldMode::normal;
        int holdTime = 6;                     // index into holdTimeChoices(): the grid of On Grid, the length of Full
        bool glideLegato = false;             // glide only when the new key overlaps another
        bool glidePerOctave = false;          // Glide is the time for one octave
        bool polyGlide = false;               // a new poly note glides in from the last key played
        int voices = 8;

        // Tone and stereo, on the frozen sound only. These defaults are all "off".
        float lowCutHz = 20.0f;
        float highCutHz = 20000.0f;
        float tiltDb = 0.0f;
        float driveDb = 0.0f;
        float hollowPercent = 0.0f;
        float diffusePercent = 0.0f;
        float spreadPercent = 0.0f;
        SpreadMode spreadMode = SpreadMode::alternate;
        float widthPercent = 0.0f;
        float driftPercent = 0.0f;

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

        /** overlap: another key was physically down when this one was pressed. */
        void noteOn (int note, int velocity, const VoiceContext& ctx, bool overlap) noexcept;
        void monoNoteOn (int note, int velocity, const VoiceContext& ctx, bool overlap) noexcept;
        /** deferBeats: the release happens after that many beats (On Grid); negative = now. */
        void noteOff (int note, double deferBeats) noexcept;
        void monoNoteOff (int note, double deferBeats) noexcept;
        /** Mono, after the stack of keys has changed: brings the voice (and a key waiting its turn)
            into line with the keys that are left. */
        void monoSettle (double deferBeats) noexcept;

        // Physical keys.
        bool keyIsDown (int note) const noexcept { return note >= 0 && note < 128 && ((keysDown[(size_t) (note >> 6)] >> (note & 63)) & 1u) != 0; }
        bool anyKeyDown() const noexcept         { return (keysDown[0] | keysDown[1]) != 0; }
        void setKey (int note, bool down) noexcept;
        bool stackHolds (int note) const noexcept;

        /** The sustain pedal counts in Normal and On Grid. */
        bool pedalHolds() const noexcept { return pedalDown && (block.holdMode == HoldMode::normal || block.holdMode == HoldMode::onGrid); }
        /** Releases every held note whose key is up (pedal up, Latch switched off, the song stopping). */
        void releaseKeysUp (double deferBeats) noexcept;
        void releaseVoice (GrainVoice& voice, double deferBeats) noexcept;
        /** On Grid: beats from the sample being worked on to the next Hold Time line; negative when the
            release should happen now (stopped, or the key came up on a line). */
        double beatsToNextLine() const noexcept;
        /** Full: starts a note's length. */
        void startHoldLength (GrainVoice& voice) noexcept;
        /** Fades or drops voices until no more than limit are playing: release tails first, then notes
            whose key is up, then the oldest key. */
        void trimVoices (int limit) noexcept;
        int nextVictim() const noexcept;
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

        /** A grab that has been asked for and may have to wait (Wait, At Key, Snap, Threshold). */
        struct PendingGrab
        {
            bool active = false;
            juce::int64 planEnd = 0;   // where the slice ends, on the sample clock (may lie before the key)
            juce::int64 dueTime = 0;   // the first moment the grab can happen: max(key, planEnd)
            bool placed = false;       // the spot was chosen on purpose (At Key, Snap): no loop-point nudge

            juce::int64 centre = 0;    // key + Wait - Offset: where Snap looks for a hit
            juce::int64 giveUp = 0;    // Threshold: by now the grab happens whatever the input does
            int region = 0;            // source samples the loop will read
            int atKeyPart = 0;         // At Key: how much later than the Offset says the slice ends (the region)
            juce::int64 dipHop = -1;   // Threshold: the dip that failed the last look; no pass while it is in the slice
            float dipThreshold = 0.0f; // ...at this Threshold
            bool anchored = false;     // Snap found a hit: the region starts there
            bool unsnapped = false;    // Snap's window passed with no hit
        };

        /** Works out when a key pressed now grabs. Returns true when that is right away, with nothing
            to wait for or to look at. */
        bool planGrab (int slot, int note, const VoiceContext& ctx, PendingGrab& plan) noexcept;
        /** A planned grab, asked every sample from its due time on: true = grab now (endDelay and
            placed say where); drop = forget the note instead. */
        bool grabReady (PendingGrab& plan, bool keyDown, int& endDelay, bool& placed, bool& drop) noexcept;
        int endDelayFor (juce::int64 sliceEnd) const noexcept;
        void startArmedVoice (int slot, const VoiceContext& ctx) noexcept;
        void fireGrab (int slot, const VoiceContext& ctx, int endDelay, bool placed, int atKeyPart) noexcept;
        void fireMonoKey (const VoiceContext& ctx, int endDelay, bool placed) noexcept;
        void servePendingGrabs (const VoiceContext& ctx) noexcept;
        void clearPendingGrabs() noexcept;

        /** The song's grid: tells the Live voices when a line is crossed. */
        void advanceGrid (const VoiceContext& ctx, int sample) noexcept;
        /** Works out what the next input sample has added to it in the memory. */
        void updateFeedback (const FeedbackSend& send) noexcept;
        void clearFeedback() noexcept;
        void publishScope (const VoiceContext& ctx) noexcept;

        void stackRemove (int note) noexcept;
        void stackPush (int note) noexcept;
        int stackTop() const noexcept { return stackSize > 0 ? noteStack[(size_t) (stackSize - 1)] : -1; }

        double sampleRate = 48000.0;
        bool prepared = false;

        InputRing ring;
        InputTracker tracker;           // the dry input's level, hiss and hits: never hears Feedback
        std::array<GrainVoice, numVoiceSlots> voices;
        std::array<Lfo, numLfos> lfos;
        SoftLimiter limiter;
        ToneChain tone;                 // Low Cut, Drive, Tilt, High Cut, Diffuse on the summed frozen sound
        int toneTail = 0;               // samples the Tone section may still ring after the last voice
        juce::SmoothedValue<float> hollow, width, spread, drift;
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

            // A tap: the key came up before its turn, with no other key down. It still plays its length.
            juce::int64 pressedAt = 0;
            bool released = false;
            int heldFor = 0;
            bool overlap = false;      // another key was down when it was pressed (legato)
        } monoKey;
        std::array<double, numLfos> lfoIncrement {};
        std::array<bool, numLfos> lfoSynced {};

        // What a note will and will not grab.
        int snapSamples = 0;
        int maxWaitSamples = 0;
        float thresholdLevel = 0.0f;    // linear; 0 = off
        bool stickyLive = false;        // a Live grain may outlive a refresh
        float gateGain = 1.0f, gateUp = 0.004f, gateDown = 0.0004f;

        // The grid.
        bool gridActive = false;
        bool gridHasSeen = false;
        double gridBeats = 1.0, gridPpq = 0.0, gridPpqPerSample = 0.0;
        juce::int64 gridLastSeen = 0;
        double gridPrevPpq = 0.0, gridBeatsSinceFire = 0.0;
        int gridHalfLine = 1;           // half a line in samples: the longest a grid grab may fade

        // Feedback: what the last sample's voices put back into the input memory.
        juce::SmoothedValue<float> feedback;
        bool feedbackActive = false;
        float feedbackLeft = 0.0f, feedbackRight = 0.0f;
        std::array<float, 2> feedbackDcIn {}, feedbackDcOut {}, feedbackLow {};
        float feedbackDcCoeff = 0.997f, feedbackLowCoeff = 0.5f, feedbackLag = 0.0f;
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

        // Hold: which keys are physically down, the pedal, and the song as this block sees it.
        std::array<juce::uint64, 2> keysDown {};
        std::array<juce::uint64, 2> keysAtStop {};   // Latch: keys that were down when the song stopped
        bool pedalDown = false;
        int lastNote = -1;              // the last key played, for Poly Glide; -1 = none yet
        bool songRunning = false;       // playing, with a position
        double songPpq = 0.0, beatsPerSample = 0.0, holdLineBeats = 1.0;
        int sampleInBlock = 0;          // the sample being worked on

        juce::uint64 voiceCounter = 0;
        int lastStartedVoice = -1;
        int scopeInterval = 1600;
        int scopeCountdown = 0;
    };
}
