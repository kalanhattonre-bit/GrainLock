#pragma once

#include "Envelope.h"
#include "GrainBuffer.h"
#include "InputTracker.h"
#include "Lfo.h"
#include "../Parameters.h"

namespace grainlock
{
    /** One LFO as a voice sees it this sample. */
    struct LfoFeed
    {
        bool inVoice = false;     // the voice adds it itself; otherwise it is already in the context's sums
        bool perVoice = false;    // Voice and Once: the voice runs its own oscillator
        bool once = false;
        LfoShape shape = LfoShape::sine;
        double increment = 0.0;   // cycles per sample
        double offset = 0.0;      // the Phase control, 0..1
        float depth = 0.0f;       // 0..1, negative when inverted, 0 while the LFO is off
        float shared = 0.0f;      // the shared LFO's value x depth (smoothed for pitch and formant)
        float fadeStep = 1.0f;    // fade-in per sample; 1 = no fade
    };

    /** The modulation settings every voice shares for a block. */
    struct ModSettings
    {
        // Note envelope: rises 0..1 over Attack, falls back over Decay.
        float envAttackStep = 1.0f, envDecayStep = 1.0f;
        float envPitch = 0.0f, envFormant = 0.0f, envGrain = 0.0f;

        bool tapeStop = false;
        float tapeStep = 0.0f;        // semitones per sample while a released note slows down (negative)

        // Keyboard sources (wheel, aftertouch, expression pedal): what each moves and by how much (-1..1).
        std::array<ModDest, numModSources> dest { ModDest::off, ModDest::off, ModDest::off };
        std::array<float, numModSources> amount { 0.0f, 0.0f, 0.0f };
        bool anySource = false;
        float vibratoDepthSemitones = 0.0f;
        float pressureCoeff = 0.001f;   // smoothing of a voice's own (poly) aftertouch
        float lfoSmoothCoeff = 0.01f;
    };

    /** Per-sample values every voice shares, worked out once per sample by the engine. */
    struct VoiceContext
    {
        double sampleRate = 48000.0;
        float globalSemitones = 0.0f;   // tune + fine + pitch bend + the pitch LFO when every voice shares it
        float formantRatio = 1.0f;      // source read rate within each note period, before key tracking
        float formantSemitones = 0.0f;  // what formantRatio was made from (Formant + the shared formant LFO)
        float trackAmount = 0.0f;       // 0..1: how far each grab's formant follows its key (root C3)
        bool autoGain = false;          // keep a loop as loud as the slice it came from
        bool legacySeam = false;        // tests only: 0.2's plain equal-power seam and no loop-point nudge
        float smooth = 0.1f;            // seam crossfade, fraction of the loop (0..0.5)
        float sustain = 1.0f;           // envelope sustain level, smoothed
        int targetCycles = 2;           // grain cycles after the shared LFO
        float cyclesBase = 2.0f;        // what targetCycles was rounded from
        bool pitchLock = true;
        bool live = true;
        double refreshSamples = 1200.0;
        bool refreshSynced = false;     // Refresh is a note value: re-grabs keep their tempo from the note's start

        // Capture budget: the most the knobs and LFO can ask of a grain before it is replaced.
        float captureRatioMax = 1.0f;
        int captureCyclesMax = 2;
        bool captureBothLayouts = false;   // Hold keeps its grain, so it must also cover a Pitch Lock toggle

        // Per-voice modulation.
        std::array<LfoFeed, numLfos> lfo;
        ModSettings mods;
        float wheel = 0.0f, pressure = 0.0f, expression = 1.0f;   // smoothed, 0..1; these are their rest values
        float vibrato = 0.0f;                                     // the shared vibrato wave, -1..1

        // What a Live note will and will not re-grab.
        const InputTracker* tracker = nullptr;   // the dry input's level history
        float threshold = 0.0f;         // linear level a slice must reach throughout; 0 = off
        bool skipHiss = false;
        float skipChance = 0.0f;        // 0..1
        bool gridActive = false;        // re-grabs come from the song's grid lines, not from the Refresh clock
        bool stickyLive = false;        // a Live grain may outlive a refresh, so it is kept and extended like a Hold one
        bool feedbackOn = false;        // voices also hand over what goes back into the input memory
        float feedbackLag = 0.0f;       // how many samples late the feedback path's low pass hands audio back

        double beatsPerSample = 0.0;    // the song's tempo, for a note that ends after a number of beats

        // Tone and stereo, per voice. All 0..1; at 0 the voice does none of it.
        float hollow = 0.0f;            // takes its own loop, half a note period on, away from itself
        float width = 0.0f;             // adds the loop's "side" to one channel and takes it from the other
        float spread = 0.0f;            // how far from the centre notes are placed
        SpreadMode spreadMode = SpreadMode::alternate;
        float drift = 0.0f;             // slow wander of pitch and place
    };

    /** What the voices send back into the input memory (Feedback), summed over one sample. */
    struct FeedbackSend
    {
        float left = 0.0f, right = 0.0f;
        float weight = 0.0f;   // the voices' own levels added up, so a chord sends no more than one note does
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
        Lock) crossfade between two play states.

        A voice has two pitches. The base pitch (glide, tune, bend, pitch LFO) is the note it is
        playing: grabs are sized for it. The sounding pitch adds the note envelope, vibrato and
        tape stop, and only sets how fast the loop runs, so those move pitch and tone together,
        like a sampler. */
    class GrainVoice
    {
    public:
        void prepare (double sampleRate, int grainCapacity);

        /** Which of the engine's voices this is: gives each its own slow Drift. */
        void setIndex (int index) noexcept;
        void setEnvelopeTimes (float attackMs, float decayMs, float releaseMs) noexcept
        {
            envelope.setTimes (attackMs, decayMs, releaseMs);
        }

        /** A key was pressed: the voice is taken (note, level, order) but silent, with nothing grabbed. */
        void arm (int midiNote, float velocityLevel, juce::uint64 order) noexcept;

        /** The armed voice makes its grab and starts to sound. Every per-note clock (the envelopes, a
            per-voice LFO's phase, an LFO's fade-in) starts here, not at the key. source says where the
            slice ends; placed means that spot was chosen on purpose (At Key), so it is not nudged. */
        void beginSounding (const VoiceContext& ctx, const CaptureSource& source, bool placed = false, int atKeyPart = 0) noexcept;

        /** Source samples the loop region of midiNote would cover on this voice (its cycles x period x
            formant): how far after the key an At Key grab has to end. */
        double plannedRegion (const VoiceContext& ctx, int midiNote) noexcept;

        /** arm() and beginSounding() at once. */
        void start (int midiNote, float velocityLevel, juce::uint64 order,
                    const VoiceContext& ctx, const CaptureSource& source);

        /** Mono mode: move to a new note (gliding over glideSamples) and grab fresh audio for it.
            A negative velocityLevel keeps the current level. */
        void retarget (int midiNote, float velocityLevel, int glideSamples, bool retriggerEnvelope, bool perOctave = false);

        /** A new note that glides in from another pitch (Poly Glide). Call it on an armed voice. With
            perOctave, glideSamples is the time for one octave. */
        void glideFrom (double fromNote, int glideSamples, bool perOctave) noexcept;

        /** The note releases itself when this many quarter-note beats have gone by (an On Grid key-up,
            Full's length). It counts only while the voice is held and sounding; a release, a steal, a
            kill or a new key ends the count. */
        void releaseInBeats (double beats) noexcept;
        void cancelHoldTimer() noexcept { holdBeats = -1.0; }
        bool hasHoldTimer() const noexcept { return holdBeats >= 0.0; }

        /** How much of a steal's fade is left (0 when the voice is not being stolen). */
        int stealSamplesLeft() const noexcept { return stealing ? stealRemaining : 0; }

        /** The same, for a key that waited: the new grab ends grabDelay samples before now (counted on
            until the voice is free to take it) instead of at the usual Offset. */
        void retargetPlaced (int midiNote, float velocityLevel, int glideSamples, bool retriggerEnvelope,
                             int grabDelay, bool placed, int atKeyPart, bool perOctave = false);

        /** Mono: the key that just took this voice over had already come up (a tap made while it
            waited its turn). The note plays for as long as that key was down, then releases. */
        void releaseAfter (int samples) noexcept;

        /** Mono: a voice that is still waiting to grab is handed to another key. Its level and its
            planned grab stay as they are. */
        void renote (int midiNote) noexcept;

        /** A voice that is still waiting to grab remembers how long its key was down and plays that
            long once it has grabbed. Releasing a voice that is playing out such a length ends it now
            ("all notes off"); otherwise a voice that is already released is left alone. */
        void release() noexcept;
        void steal (int fadeSamples) noexcept;
        void kill() noexcept;

        /** This voice's own (polyphonic) aftertouch, 0..1. */
        void setPressure (float zeroToOne) noexcept { polyPressure = juce::jlimit (0.0f, 1.0f, zeroToOne); }

        /** Adds this voice's output for one sample, and (while Feedback is up) what it sends back. */
        void render (const VoiceContext& ctx, const CaptureSource& source, float& outLeft, float& outRight,
                     FeedbackSend& send) noexcept;

        /** The song has reached a grid line: a Live voice re-grabs the audio of the line (unless Skip,
            Threshold or Skip Hiss says no), fading over at most fadeCapSamples. */
        void gridLine (const VoiceContext& ctx, const CaptureSource& source, juce::int64 lineIndex, int fadeCapSamples) noexcept;

        /** Fills dest with one loop of the current play state, for the display. Does not advance anything. */
        int renderLoopShape (const VoiceContext& ctx, float* dest, int numPoints) const noexcept;

        bool isActive() const noexcept    { return active; }
        bool isHeld() const noexcept      { return active && held && ! stealing; }
        bool isStealing() const noexcept  { return active && stealing; }
        bool isReleasing() const noexcept { return active && ! held && ! stealing; }
        /** Armed but not yet sounding. */
        bool isWaiting() const noexcept   { return active && waiting; }
        /** Sounding, and still on (held, or playing out the length of a key that came up while it
            waited): what holds the dry signal down for Dry When Idle. */
        bool isEngaged() const noexcept   { return active && ! stealing && ! waiting && (held || gateRemaining > 0); }
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
            float gainTarget = 1.0f;      // Auto Gain as measured
            float gain = 1.0f;            // Auto Gain as applied: glides to the target, so an update never clicks
            float sendGain = 1.0f;        // the same measurement, unsmoothed and always on: what Feedback sends
            double measuredCycle = 0.0;   // the settings the measurement was made for
            float measuredSeam = -1.0f;
            bool measuredAuto = false;
            bool measuredSend = false;
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

        enum class EnvStage { idle, attack, decay };

        double frequencyFor (double semitones) const noexcept;

        /** Works out this voice's pitches, formant ratio, cycle count and level for one sample, from
            the shared context plus its own LFOs, note envelope, pressure and tape stop. With nothing
            of its own in play it hands back the context's numbers untouched. advance false: look,
            without moving any clock (used when the voice starts). */
        void evaluate (const VoiceContext& ctx, bool advance) noexcept;
        void restartNoteEnvelope() noexcept;
        /** Everything that starts with the note: this voice's own LFOs, their fade-in, the note envelope. */
        void restartNoteClocks (const VoiceContext& ctx) noexcept;

        /** Formant Track: how much longer a cycle a grab made at capturePeriod reads than its own
            period, so that every key reads the same length of source (the C3 period x Formant). The
            loop still runs at the sounding pitch, which is what makes the formant follow the key. */
        double trackFactor (double capturePeriod) const noexcept;
        /** The same, at its largest while the switch is fading: what a new grab must be sized for. */
        double trackReach (double capturePeriod) const noexcept;
        int tapsThatFit (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept;
        bool grainHolds (const GrainBuffer& grain, int cycles, bool lockOn, float formantRatio) const noexcept;
        double cycleLengthOf (const PlayState& state) const noexcept;
        /** Measures the seam correlation and the Auto Gain of a play state on its grain. blend 1
            replaces the previous values; less than 1 moves towards the new ones (Live re-grabs). */
        void measure (PlayState& state, const VoiceContext& ctx, int points, float blend) const noexcept;
        int nudgedEndDelay (const CaptureSource& source, const VoiceContext& ctx, double period) const noexcept;
        void capture (GrainBuffer& grain, double frequency, const VoiceContext& ctx, const CaptureSource& source,
                      bool nudge = true) noexcept;
        void renderState (const PlayState& state, const VoiceContext& ctx, float& left, float& right) const noexcept;
        /** Hollow and Width, on the voice's finished loop sound. Both work from a short memory of what
            the voice has just played (a tenth of a second), so they cost a few reads, not a second and
            third rendering of every cycle. Hollow is done to left / right here. Width is handed back:
            what to add to the left and take from the right once the voice has been placed.
            repeats: the share of the sound that comes from a loop repeating every note period
            (1 with Pitch Lock on), for which the memory holds what Width needs; ahead: the rest of
            it a quarter of a note period from now, from the loop itself (loopAhead).
            Call it once per sample, whatever the two are set to. */
        float applyShape (const VoiceContext& ctx, double frequency, float& left, float& right,
                          float repeats, float ahead) noexcept;
        /** A play state's loop a quarter of a note period ahead of where it is, left and right as one. */
        float loopAhead (const PlayState& state, const VoiceContext& ctx) const noexcept;
        bool advance (PlayState& state, double frequency) const noexcept;
        /** firstRenderDelay: 0 when the new state is heard in this same sample, 1 when from the next. */
        void beginRecapture (double loopFrequency, const VoiceContext& ctx, const CaptureSource& source, double firstRenderDelay) noexcept;
        void beginReshape (const VoiceContext& ctx) noexcept;
        bool beginExtend (int cycles, bool lockOn, const VoiceContext& ctx, const CaptureSource& source) noexcept;
        void startTransition (int lengthSamples, float correlation) noexcept;
        double loopLengthSamples (const PlayState& state, double frequency) const noexcept;
        static float correlationBetween (const PlayState& a, const PlayState& b) noexcept;

        /** Source samples everything the loop reads covers for a grab at period: cycles and seam. */
        double regionFor (const VoiceContext& ctx, double period) const noexcept;
        /** Where the next Live re-grab ends: the usual lag, but never behind the grain it replaces. */
        int nextEndDelay (const CaptureSource& source) const noexcept;
        /** Threshold and Skip Hiss: may a Live re-grab ending endDelay ago be taken? */
        bool liveGrabAllowed (const VoiceContext& ctx, int endDelay) const noexcept;
        /** Skip: the same turn and key always decide the same way. */
        bool skipsGrab (const VoiceContext& ctx, juce::uint64 turn) const noexcept;
        float sendScale (const PlayState& state) const noexcept;
        /** How much quieter the summed cycles of a state must be played to be as loud as one of them:
            1 when they are unrelated, down to 1 / sqrt (taps) when they are alike. */
        float coherenceOf (const PlayState& state, int points) const noexcept;
        /** How alike two play states sound, measured over one loop (0..1). */
        float correlationOf (const PlayState& a, const PlayState& b, const VoiceContext& ctx) const noexcept;

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

        // This sample's result of evaluate().
        double basePitch = 60.0, baseTargetPitch = 60.0;   // semitones; the target is where a glide ends
        double soundingFrequency = 261.6;                  // Hz: how fast the loop runs
        float ratio = 1.0f;                 // this voice's formant ratio right now, before key tracking
        float captureRatio = 1.0f;          // the most formant this voice's next grab must hold
        int cyclesNow = 2;                  // this voice's cycle count right now
        float levelMod = 1.0f;              // level from a keyboard source
        float trackAmount = 0.0f;           // the Formant Track switch, as it fades

        // Per-voice modulation state.
        std::array<Lfo, numLfos> ownLfos;

        // Spread and Drift.
        std::array<Lfo, 2> driftLfos;       // pitch, place
        double driftStep = 0.0;             // cycles per sample of the pitch one
        float driftSemitones = 0.0f;
        float place = 0.0f, placeLeft = 1.0f, placeRight = 1.0f;   // -1 = left, 1 = right, and the gains for it
        float placeStep = 0.002f;            // the furthest a sounding voice's place moves in one sample
        bool offCentre = false;              // a place other than the centre is in use

        // Hollow and Width: the last tenth of a second of this voice's own loop sound.
        std::array<std::vector<float>, 2> shapeLine;
        int shapeMask = 0, shapeWrite = 0;
        int shapeAge = 0;                   // samples of this note in the line
        float shapeFade = 144.0f;           // a delayed read fades in over this many samples once the note has reached it
        std::array<float, numLfos> lfoFade {};
        std::array<float, numLfos> lfoSmoothed {};
        EnvStage noteEnvStage = EnvStage::idle;
        float noteEnv = 0.0f;
        bool noteEnvInstant = true;         // Attack is 0: the envelope starts at its peak
        // A key released during the wait: the note still plays for as long as the key was down.
        int waitedSamples = 0, gateSamples = 0, gateRemaining = 0;

        // A mono re-grab that waited: where its slice ends, and whether that spot was chosen on purpose.
        int placedDelay = -1;
        bool placedGrab = false;

        // At Key: the note's first slice ended one loop region later than the Offset says, and every
        // later grab of the note keeps that distance from the input.
        int atKeySamples = 0;
        bool keyGrabPlaced = false;         // the note's own grab was put on a spot (At Key, Snap)
        int sinceSend = 0;                  // samples since the Feedback send gain was last looked at
        bool refreshClockStale = false;     // a key took the voice over: the synced Refresh clock restarts
        double holdBeats = -1.0;            // beats until the note releases itself; negative = no such count
        int nudgeHistory = 131072;          // the loop-point nudge looks no further back than 0.3 stage 1 could

        // Synced Refresh: re-grabs fall due on a clock that starts with the note.
        double noteAge = 0.0, refreshDueAt = 0.0;

        // Skip: a skipped re-grab uses up its turn, and the next one is a Refresh later.
        double refreshSkippedAt = 0.0;
        juce::uint64 refreshTurn = 0;
        int gridFadeCap = 0;                // a grid grab's crossfade is at most this long; 0 = not a grid grab

        bool tapeStopEnabled = false;
        bool tapeStopping = false;
        double tapeSemitones = 0.0;
        float polyPressure = 0.0f, polyPressureSmoothed = 0.0f;

        float gainSlew = 0.002f;
        int sinceMeasure = 0;
        double samplesSinceCapture = 0.0;   // how long ago the current grain's frozen instant was
        int lastCaptureOffset = 0;          // the Offset (in samples) that instant was taken with
        bool pendingRecapture = false;

        int note = 60;
        bool active = false;
        bool held = false;
        bool waiting = false;
        bool stealing = false;
        int stealRemaining = 0;
        int stealLength = 1;
        juce::uint64 startOrder = 0;
    };
}
