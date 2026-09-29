#include "Presets.h"
#include "Parameters.h"

namespace grainlock
{
    const std::vector<Preset>& factoryPresets()
    {
        static const std::vector<Preset> presets {
            { "Init", {} },
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
