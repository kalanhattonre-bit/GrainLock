#pragma once

#include "PluginProcessor.h"
#include "ui/GrainLookAndFeel.h"
#include "ui/MainPanel.h"

class GrainLockEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit GrainLockEditor (GrainLockProcessor&);
    ~GrainLockEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    GrainLockProcessor& audioProcessor;
    grainlock::ui::GrainLookAndFeel lookAndFeel;   // declared before the panel so it outlives it
    grainlock::ui::MainPanel panel;
    bool constructed = false;   // resized() only saves the size once the constructor has set it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainLockEditor)
};
