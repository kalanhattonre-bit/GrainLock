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
