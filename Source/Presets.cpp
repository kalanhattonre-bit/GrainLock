#include "Presets.h"
#include "Parameters.h"

namespace grainlock
{
    const std::vector<Preset>& factoryPresets()
    {
        using namespace ParamID;
        constexpr float hold = (float) (int) CaptureMode::hold;
        constexpr float live = (float) (int) CaptureMode::live;
        constexpr float sine = (float) (int) LfoShape::sine;
        constexpr float triangle = (float) (int) LfoShape::triangle;
        constexpr float sampleHold = (float) (int) LfoShape::sampleHold;
        constexpr float eighth = 7.0f;       // index of "1/8" in lfoSyncChoices()
        constexpr float sixteenth = 9.0f;    // index of "1/16"

        // 0.3
        constexpr float atKey = 1.0f;                                   // Grab At
        constexpr float perVoice = (float) (int) LfoTrig::voice;
        constexpr float latch = (float) (int) KeyUpMode::latch;
        constexpr float toGrid = (float) (int) KeyUpMode::toGrid;
        constexpr float lengthSixteenth = 2.0f;                         // index of "1/16" in noteLengthChoices()
        constexpr float byPitch = (float) (int) SpreadMode::byPitch;
        constexpr float anywhere = (float) (int) SpreadMode::random;

        static const std::vector<Preset> presets {
            { "Init", {} },

            // One wavelength, refreshed every 12 ms: speech stays readable but every syllable
            // lands on the note you play.
            { "Robot Voice", {
                { captureMode, live }, { grainCycles, 1.0f }, { smooth, 25.0f }, { refresh, 12.0f },
                { pitchLock, 1.0f }, { attack, 3.0f }, { decay, 200.0f }, { sustain, 100.0f },
                { release, 70.0f }, { velSens, 60.0f } } },

            // Each key freezes a fresh slice; zero attack and a short release gate it hard.
            { "Stutter Gate", {
                { captureMode, hold }, { grainCycles, 4.0f }, { smooth, 6.0f }, { attack, 0.0f },
                { decay, 150.0f }, { sustain, 100.0f }, { release, 30.0f }, { velSens, 80.0f } } },

            // Eight stacked cycles average into a smooth tone that drifts slowly with the source:
            // a gentle pitch drift plus an even slower formant swell.
            { "Drone Pad", {
                { captureMode, live }, { grainCycles, 8.0f }, { smooth, 35.0f }, { refresh, 250.0f },
                { attack, 900.0f }, { decay, 1500.0f }, { sustain, 85.0f }, { release, 2800.0f },
                { pitchLfoOn, 1.0f }, { pitchLfoShape, sine }, { pitchLfoRate, 0.25f }, { pitchLfoDepth, 12.0f },
                { formantLfoOn, 1.0f }, { formantLfoShape, triangle }, { formantLfoRate, 0.07f }, { formantLfoDepth, 15.0f },
                { velSens, 30.0f } } },

            // Grabs from a quarter second back so the hit is caught. S&H on the cycle count at 1/16
            // (Pitch Lock off, so each step also jumps the loop length) and on pitch at 1/8.
            { "Glitch Drums", {
                { captureMode, hold }, { offset, 250.0f }, { grainCycles, 4.0f }, { pitchLock, 0.0f },
                { smooth, 3.0f }, { attack, 0.0f }, { decay, 180.0f }, { sustain, 40.0f }, { release, 60.0f },
                { grainLfoOn, 1.0f }, { grainLfoShape, sampleHold }, { grainLfoSync, sixteenth }, { grainLfoDepth, 70.0f },
                { pitchLfoOn, 1.0f }, { pitchLfoShape, sampleHold }, { pitchLfoSync, eighth }, { pitchLfoDepth, 25.0f },
                { velSens, 70.0f } } },

            // Formant down 5 semitones for a bigger, darker throat; soft vibrato on pitch and a slow
            // vowel drift on formant; play 4-note chords.
            { "Formant Choir", {
                { captureMode, live }, { grainCycles, 4.0f }, { formant, -5.0f }, { smooth, 30.0f },
                { refresh, 40.0f }, { attack, 220.0f }, { decay, 600.0f }, { sustain, 90.0f },
                { release, 1100.0f }, { mono, 0.0f },
                { pitchLfoOn, 1.0f }, { pitchLfoShape, sine }, { pitchLfoRate, 5.2f }, { pitchLfoDepth, 6.0f },
                { formantLfoOn, 1.0f }, { formantLfoShape, sine }, { formantLfoRate, 0.15f }, { formantLfoDepth, 12.0f },
                { velSens, 50.0f } } },

            // 0.3 presets from here on. The five above are as they were in 0.2.

            // For drum loops: the loop starts on the hit nearest the key (within 40 ms either side),
            // not on whatever came before it.
            { "Beat Catcher", {
                { captureMode, hold }, { grabAt, atKey }, { snap, 40.0f }, { grainCycles, 4.0f }, { smooth, 4.0f },
                { attack, 0.0f }, { decay, 220.0f }, { sustain, 70.0f }, { release, 80.0f }, { velSens, 70.0f } } },

            // Re-grabs on every eighth note of the song, leaves a quarter of them out, and lets each
            // note go on the next sixteenth. Notes alternate left and right.
            { "Grid Slicer", {
                { captureMode, live }, { refreshSync, eighth }, { gridGrabs, 1.0f }, { skipChance, 25.0f },
                { grainCycles, 3.0f }, { smooth, 8.0f }, { attack, 0.0f }, { release, 60.0f },
                { keyUpMode, toGrid }, { noteLength, lengthSixteenth }, { spread, 60.0f }, { velSens, 60.0f } } },

            // The frozen sound goes back into what is grabbed next, softened by Diffuse, so a chord
            // keeps blooming after the input has moved on.
            { "Feedback Bloom", {
                { captureMode, live }, { refresh, 60.0f }, { grainCycles, 4.0f }, { smooth, 30.0f },
                { feedback, 75.0f }, { diffuse, 45.0f }, { lowCut, 120.0f }, { tilt, -1.5f }, { width, 40.0f },
                { attack, 150.0f }, { decay, 800.0f }, { sustain, 90.0f }, { release, 1800.0f }, { velSens, 40.0f } } },

            // A choir whose tone follows the key (higher notes are brighter), each voice with its own
            // late vibrato, low notes to the left and high to the right; letting go slows it down
            // like a tape.
            { "Tape Choir", {
                { captureMode, live }, { grainCycles, 4.0f }, { formant, -3.0f }, { formantTrack, 1.0f }, { smooth, 30.0f },
                { refresh, 40.0f }, { tapeStop, 1.0f }, { attack, 120.0f }, { decay, 600.0f }, { sustain, 90.0f }, { release, 900.0f },
                { pitchLfoOn, 1.0f }, { pitchLfoShape, sine }, { pitchLfoRate, 5.5f }, { pitchLfoDepth, 8.0f },
                { pitchLfoTrig, perVoice }, { pitchLfoFade, 400.0f },
                { spread, 55.0f }, { spreadMode, byPitch }, { width, 35.0f }, { velSens, 50.0f } } },

            // Play a chord and let go: it stays until the next chord. Wide, slowly wandering, a little hollow.
            { "Latch Drone", {
                { captureMode, live }, { keyUpMode, latch }, { refresh, 350.0f }, { grainCycles, 8.0f }, { smooth, 40.0f },
                { drift, 60.0f }, { spread, 70.0f }, { spreadMode, anywhere }, { width, 50.0f }, { hollow, 25.0f }, { diffuse, 35.0f },
                { attack, 1200.0f }, { decay, 1500.0f }, { sustain, 90.0f }, { release, 3000.0f }, { velSens, 20.0f } } },

            // For a voice: a key pressed in a gap waits for the next sound instead of freezing the
            // silence, and breaths and "sss" are never grabbed.
            { "Breath Guard", {
                { captureMode, live }, { threshold, -45.0f }, { maxWait, 300.0f }, { skipHiss, 1.0f },
                { grainCycles, 2.0f }, { smooth, 20.0f }, { refresh, 30.0f },
                { attack, 8.0f }, { decay, 250.0f }, { sustain, 100.0f }, { release, 250.0f }, { velSens, 50.0f } } },
        };
        return presets;
    }

    void applyPreset (juce::AudioProcessorValueTreeState& state, const Preset& preset)
    {
        auto setNormalised = [] (juce::RangedAudioParameter& p, float normalised)
        {
            p.beginChangeGesture();
            p.setValueNotifyingHost (normalised);
            p.endChangeGesture();
        };

        for (const char* id : ParamID::all)
            if (auto* p = state.getParameter (id))
                setNormalised (*p, p->getDefaultValue());

        for (const auto& v : preset.values)
            if (auto* p = state.getParameter (v.id))
                setNormalised (*p, p->convertTo0to1 (v.value));

        state.state.setProperty ("presetName", juce::String (preset.name), nullptr);
    }
}
