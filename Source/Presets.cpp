#include "Presets.h"
#include "Parameters.h"

namespace grainlock
{
    const std::vector<Preset>& factoryPresets()
    {
        using namespace ParamID;
        constexpr float hold = (float) (int) CaptureMode::hold;
        constexpr float live = (float) (int) CaptureMode::live;

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

            // Eight stacked cycles average into a smooth tone that drifts slowly with the source.
            { "Drone Pad", {
                { captureMode, live }, { grainCycles, 8.0f }, { smooth, 35.0f }, { refresh, 250.0f },
                { attack, 900.0f }, { decay, 1500.0f }, { sustain, 85.0f }, { release, 2800.0f },
                { lfoShape, (float) (int) LfoShape::sine }, { lfoTarget, (float) (int) LfoTarget::pitch },
                { lfoRate, 0.25f }, { lfoDepth, 12.0f }, { velSens, 30.0f } } },

            // Grabs from a quarter second back so the hit is caught; S&H on the cycle count,
            // synced to 1/16, with Pitch Lock off so each step also jumps the loop length.
            { "Glitch Drums", {
                { captureMode, hold }, { offset, 250.0f }, { grainCycles, 4.0f }, { pitchLock, 0.0f },
                { smooth, 3.0f }, { attack, 0.0f }, { decay, 180.0f }, { sustain, 40.0f }, { release, 60.0f },
                { lfoShape, (float) (int) LfoShape::sampleHold }, { lfoTarget, (float) (int) LfoTarget::grainCycles },
                { lfoSync, 9.0f }, { lfoDepth, 70.0f }, { velSens, 70.0f } } },

            // Formant down 5 semitones for a bigger, darker throat; soft vibrato; play 4-note chords.
            { "Formant Choir", {
                { captureMode, live }, { grainCycles, 4.0f }, { formant, -5.0f }, { smooth, 30.0f },
                { refresh, 40.0f }, { attack, 220.0f }, { decay, 600.0f }, { sustain, 90.0f },
                { release, 1100.0f }, { mono, 0.0f },
                { lfoShape, (float) (int) LfoShape::sine }, { lfoTarget, (float) (int) LfoTarget::pitch },
                { lfoRate, 5.2f }, { lfoDepth, 6.0f }, { velSens, 50.0f } } },
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
