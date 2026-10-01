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

    // 0.3
    inline constexpr const char* formantTrack = "formantTrack";
    inline constexpr const char* autoGain     = "autoGain";

    // 0.3: how each LFO starts (per LFO), the note envelope, and what the keyboard's controllers do.
    inline constexpr const char* pitchLfoTrig     = "pitchLfoTrig";
    inline constexpr const char* pitchLfoFade     = "pitchLfoFade";
    inline constexpr const char* pitchLfoPhase    = "pitchLfoPhase";
    inline constexpr const char* pitchLfoInvert   = "pitchLfoInvert";
    inline constexpr const char* formantLfoTrig   = "formantLfoTrig";
    inline constexpr const char* formantLfoFade   = "formantLfoFade";
    inline constexpr const char* formantLfoPhase  = "formantLfoPhase";
    inline constexpr const char* formantLfoInvert = "formantLfoInvert";
    inline constexpr const char* grainLfoTrig     = "grainLfoTrig";
    inline constexpr const char* grainLfoFade     = "grainLfoFade";
    inline constexpr const char* grainLfoPhase    = "grainLfoPhase";
    inline constexpr const char* grainLfoInvert   = "grainLfoInvert";

    inline constexpr const char* envAttack  = "envAttack";
    inline constexpr const char* envDecay   = "envDecay";
    inline constexpr const char* envPitch   = "envPitch";
    inline constexpr const char* envFormant = "envFormant";
    inline constexpr const char* envGrain   = "envGrain";
    inline constexpr const char* tapeStop   = "tapeStop";

    inline constexpr const char* bendUp    = "bendUp";
    inline constexpr const char* bendDown  = "bendDown";
    inline constexpr const char* vibRate   = "vibRate";
    inline constexpr const char* vibDepth  = "vibDepth";
    inline constexpr const char* wheelDest = "wheelDest";
    inline constexpr const char* wheelAmt  = "wheelAmt";
    inline constexpr const char* touchDest = "touchDest";
    inline constexpr const char* touchAmt  = "touchAmt";
    inline constexpr const char* exprDest  = "exprDest";
    inline constexpr const char* exprAmt   = "exprAmt";

    // 0.3: when a note grabs its audio.
    inline constexpr const char* grabAt      = "grabAt";
    inline constexpr const char* wait        = "wait";
    inline constexpr const char* waitSync    = "waitSync";
    inline constexpr const char* offsetSync  = "offsetSync";
    inline constexpr const char* refreshSync = "refreshSync";

    // 0.3: what a note will and will not grab, and feedback.
    inline constexpr const char* snap       = "snap";
    inline constexpr const char* threshold  = "threshold";
    inline constexpr const char* maxWait    = "maxWait";
    inline constexpr const char* skipHiss   = "skipHiss";
    inline constexpr const char* gate       = "gate";
    inline constexpr const char* gridGrabs  = "gridGrabs";
    inline constexpr const char* skipChance = "skipChance";
    inline constexpr const char* feedback   = "feedback";

    // 0.3: how notes are held and how many play.
    inline constexpr const char* sustainPedal = "sustainPedal";
    inline constexpr const char* holdMode     = "holdMode";
    inline constexpr const char* holdTime     = "holdTime";
    inline constexpr const char* glideLegato  = "glideLegato";
    inline constexpr const char* glideRate    = "glideRate";
    inline constexpr const char* polyGlide    = "polyGlide";
    inline constexpr const char* voices       = "voices";

    /** The host's bypass switch. Kept out of `all`: presets and resets must never touch it. */
    inline constexpr const char* bypass      = "bypass";

    inline constexpr const char* all[] = {
        grainCycles, smooth, offset, captureMode, refresh, pitchLock,
        formant, tune, fine, glide, mono, velSens,
        attack, decay, sustain, release,
        pitchLfoOn, pitchLfoRate, pitchLfoSync, pitchLfoShape, pitchLfoDepth,
        formantLfoOn, formantLfoRate, formantLfoSync, formantLfoShape, formantLfoDepth,
        grainLfoOn, grainLfoRate, grainLfoSync, grainLfoShape, grainLfoDepth,
        mix, dryWhenIdle, outGain,
        formantTrack, autoGain,
        pitchLfoTrig, pitchLfoFade, pitchLfoPhase, pitchLfoInvert,
        formantLfoTrig, formantLfoFade, formantLfoPhase, formantLfoInvert,
        grainLfoTrig, grainLfoFade, grainLfoPhase, grainLfoInvert,
        envAttack, envDecay, envPitch, envFormant, envGrain, tapeStop,
        bendUp, bendDown, vibRate, vibDepth,
        wheelDest, wheelAmt, touchDest, touchAmt, exprDest, exprAmt,
        grabAt, wait, waitSync, offsetSync, refreshSync,
        snap, threshold, maxWait, skipHiss, gate, gridGrabs, skipChance, feedback,
        sustainPedal, holdMode, holdTime, glideLegato, glideRate, polyGlide, voices
    };

    /** The parameter IDs of one LFO. */
    struct LfoIds
    {
        const char* on;
        const char* rate;
        const char* sync;
        const char* shape;
        const char* depth;
        const char* trig;
        const char* fade;
        const char* phase;
        const char* invert;
    };

    /** Indexed by LfoTarget: pitch, formant, grain cycles. */
    inline constexpr LfoIds lfo[] = {
        { pitchLfoOn,   pitchLfoRate,   pitchLfoSync,   pitchLfoShape,   pitchLfoDepth,   pitchLfoTrig,   pitchLfoFade,   pitchLfoPhase,   pitchLfoInvert },
        { formantLfoOn, formantLfoRate, formantLfoSync, formantLfoShape, formantLfoDepth, formantLfoTrig, formantLfoFade, formantLfoPhase, formantLfoInvert },
        { grainLfoOn,   grainLfoRate,   grainLfoSync,   grainLfoShape,   grainLfoDepth,   grainLfoTrig,   grainLfoFade,   grainLfoPhase,   grainLfoInvert },
    };

    /** The destination and amount of one keyboard source. */
    struct SourceIds
    {
        const char* dest;
        const char* amount;
    };

    /** Indexed by ModSource: mod wheel, aftertouch, expression pedal. */
    inline constexpr SourceIds source[] = {
        { wheelDest, wheelAmt },
        { touchDest, touchAmt },
        { exprDest,  exprAmt },
    };
}

namespace grainlock
{
    enum class CaptureMode { hold = 0, live = 1 };
    // A choice list's length is part of the host contract (automation stores index / (count - 1)).
    // After the first release tag a list never changes length; a new option is a new parameter.
    enum class LfoShape    { sine = 0, triangle, square, sampleHold, saw, random };
    inline constexpr int numLfoShapes = 6;

    /** Where an LFO's cycle starts. free: runs on (and locks to the song when synced). note: the shared
        LFO restarts at every note. voice: each voice runs its own from its start. once: per voice,
        one cycle, then it holds its last value. */
    enum class LfoTrig     { free = 0, note, voice, once };
    inline constexpr int numLfoTrigs = 4;

    /** What a keyboard source moves. */
    enum class ModDest     { off = 0, vibrato, formant, grain, level };
    inline constexpr int numModDests = 5;

    enum class ModSource   { wheel = 0, touch, expression };
    inline constexpr int numModSources = 3;

    /** What ends a note. normal: the key (and the sustain pedal). latch: the next chord. onGrid: the
        key, but on the next grid line. full: the note's own length, whatever the key does. */
    enum class HoldMode    { normal = 0, latch, onGrid, full };
    inline constexpr int numHoldModes = 4;

    /** What each LFO moves. Also the index of that LFO everywhere (ParamID::lfo, EngineParams::lfos, ...). */
    enum class LfoTarget   { pitch = 0, formant, grainCycles };
    inline constexpr int numLfos = 3;

    inline constexpr int minCycles = 1;
    inline constexpr int maxCycles = 16;

    // Full-depth LFO swing per target.
    inline constexpr float lfoPitchRangeSemitones   = 1.0f;   // +/-100 cents
    inline constexpr float lfoFormantRangeSemitones = 12.0f;
    inline constexpr float lfoCyclesRange           = 8.0f;

    // Full-amount swing of a keyboard source per destination, and of the note envelope's tape stop.
    inline constexpr float sourceFormantRangeSemitones = 12.0f;
    inline constexpr float sourceCyclesRange           = 8.0f;
    inline constexpr float tapeStopFallSemitones       = 48.0f;

    /** Choice labels for the LFO sync parameters. Index 0 is free-running (Hz); the rest are note divisions. */
    const juce::StringArray& lfoSyncChoices();

    /** Choice labels for the LFO shape parameters, in LfoShape order: the one list every user shares. */
    const juce::StringArray& lfoShapeChoices();

    /** Choice labels for Offset Sync and Wait Sync. Index 0 is free (the ms knob); the rest are note
        values from 1/32 to 1/1. A value too long for its control is halved until it fits, so the grab
        stays on the grid. */
    const juce::StringArray& grabSyncChoices();

    /** Length in quarter-note beats of a grabSyncChoices() index, or 0 for free. */
    double grabSyncBeats (int syncIndex) noexcept;

    // The longest a synced Offset may reach back and a synced Wait may wait, in seconds. The input
    // memory is this Offset plus two seconds, at any sample rate.
    inline constexpr double maxOffsetSeconds = 1.2;
    inline constexpr double maxWaitSeconds   = 2.0;

    /** Threshold at the bottom of its range is off. */
    inline constexpr float thresholdOffDb = -80.0f;

    /** Choice labels for Hold Time: the note values of grabSyncChoices() without "Free". */
    const juce::StringArray& holdTimeChoices();

    /** Length in quarter-note beats of a holdTimeChoices() index. */
    double holdTimeBeats (int index) noexcept;

    /** Length of one LFO cycle in quarter-note beats for a sync index, or 0 for free-running. */
    double lfoSyncBeats (int syncIndex) noexcept;

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Rewrites a state saved before v0.2 (one shared LFO with a Target choice) so the old LFO's
        settings land on the LFO for the target it pointed at. States without the old IDs pass through. */
    void migrateLegacyLfoState (juce::XmlElement& state);

    /** Gives every parameter that a saved state does not mention a definite value, before the state is
        loaded. (JUCE leaves such a parameter at whatever the instance had, so an old preset loaded onto
        a live instance would keep that instance's newer settings.) The value is what the older version
        effectively did: Auto Gain off, because it did not exist; the default for everything else. */
    void fillMissingParameters (juce::XmlElement& state, const juce::AudioProcessorValueTreeState& apvts);

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
            std::atomic<float>* trig;
            std::atomic<float>* fade;
            std::atomic<float>* phase;
            std::atomic<float>* invert;
        };
        std::array<Lfo, numLfos> lfo;

        struct Source
        {
            std::atomic<float>* dest;
            std::atomic<float>* amount;
        };
        std::array<Source, numModSources> source;

        std::atomic<float> *envAttack, *envDecay, *envPitch, *envFormant, *envGrain, *tapeStop;
        std::atomic<float> *bendUp, *bendDown, *vibRate, *vibDepth;
        std::atomic<float> *grabAt, *wait, *waitSync, *offsetSync, *refreshSync;
        std::atomic<float> *snap, *threshold, *maxWait, *skipHiss, *gate, *gridGrabs, *skipChance, *feedback;
        std::atomic<float> *sustainPedal, *holdMode, *holdTime, *glideLegato, *glideRate, *polyGlide, *voices;

        std::atomic<float>* mix;
        std::atomic<float>* dryWhenIdle;
        std::atomic<float>* outGain;
        std::atomic<float>* bypass;

        std::atomic<float>* formantTrack;
        std::atomic<float>* autoGain;
    };
}
