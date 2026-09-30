#pragma once

#include "Controls.h"
#include "../Parameters.h"

namespace grainlock::ui
{
    /** The controls of one LFO: on/off, rate, depth, sync and shape. */
    class LfoPage final : public juce::Component
    {
    public:
        LfoPage (juce::AudioProcessorValueTreeState& state, const ParamID::LfoIds& ids);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** Called from the UI tick: dims Rate while Sync picks a note division. */
        void refresh();

    private:
        juce::AudioProcessorValueTreeState& state;
        ParamID::LfoIds ids;

        PillToggle power;
        Knob rate, depth;
        juce::ComboBox sync;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> syncAttachment;
        SegmentedControl shape;
        juce::Rectangle<int> syncCaption, shapeCaption;
    };

    /** PITCH / FORMANT / GRAIN tabs. Each shows a light while its LFO is on; clicking picks which
        LFO's controls are shown. All three LFOs run at the same time whichever tab is open. */
    class LfoTabs final : public juce::Component
    {
    public:
        std::function<void (int)> onSelect;

        void setSelected (int index);
        void setActive (int index, bool isOn);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        int tabAt (juce::Point<float> position) const;

        int selected = 0;
        int hovered = -1;
        std::array<bool, numLfos> active {};
    };
}
