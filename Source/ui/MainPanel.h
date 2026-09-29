#pragma once

#include "Controls.h"
#include "GrainDisplay.h"

class GrainLockProcessor;

namespace grainlock::ui
{
    /** The whole interface at its base size (780 x 440). The editor scales it to the window. */
    class MainPanel final : public juce::Component
    {
    public:
        explicit MainPanel (GrainLockProcessor& processor);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** Called about 30 times a second. frame is null when the audio thread sent nothing new. */
        void tick (const ScopeFrame* frame);

    private:
        struct Section
        {
            juce::String title;
            juce::Rectangle<int> bounds;
        };

        void layoutRow (juce::Rectangle<int> area, std::initializer_list<juce::Component*> cells);
        void refreshPresetBox();
        void stepPreset (int delta);
        juce::String statusText() const;
        float plainValue (const char* id) const;

        GrainLockProcessor& processor;
        juce::AudioProcessorValueTreeState& state;

        // Top bar
        ChevronButton previousPreset { false }, nextPreset { true };
        PresetButton presetBox;
        SegmentedControl captureMode;

        GrainDisplay display;

        // FREEZE
        Knob grain, smooth, offset, refresh;
        PillToggle pitchLock;

        // VOICE
        Knob tune, fine, formant, glide;
        PillToggle mono;

        // OUTPUT
        Knob mix, gain;
        PillToggle dryWhenIdle;

        // ENVELOPE
        Knob attack, decay, sustain, release, velSens;

        // LFO
        Knob lfoRate, lfoDepth;
        juce::ComboBox lfoSync;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lfoSyncAttachment;
        SegmentedControl lfoShape, lfoTarget;
        juce::Rectangle<int> syncCaption, shapeCaption, targetCaption, captureCaption;

        KeyStrip keys;

        std::array<Section, 5> sections;
        ScopeFrame lastFrame;
        juce::String shownPresetName;
    };
}
