#pragma once

#include "Controls.h"
#include "../Parameters.h"

namespace grainlock::ui
{
    /** The controls of one LFO, on the same thirteen cells as every other row: on/off, rate, depth,
        sync, shape, where its cycle starts (trigger), fade-in, phase and invert. */
    class LfoPage final : public juce::Component
    {
    public:
        LfoPage (juce::AudioProcessorValueTreeState& state, const ParamID::LfoIds& ids);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** Called from the UI tick: fades back the controls that do nothing as things are set. */
        void refresh();

    private:
        juce::Rectangle<int> cell (int firstCell, int span) const;

        juce::AudioProcessorValueTreeState& state;
        ParamID::LfoIds ids;

        PillToggle power;
        Knob rate, depth;
        ChoiceBox sync;
        SegmentedControl shape;
        ChoiceBox trigger;
        Knob fade, phase;
        PillToggle invert;
        juce::Rectangle<int> shapeCaption;
    };
}
